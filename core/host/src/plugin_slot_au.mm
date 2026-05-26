// Audio Unit v2 plugin slot (macOS only).
//
// Loads an AU v2 effect/instrument via AudioComponent APIs, exposes the
// PluginSlot interface, and drives audio through AudioUnitRender. Scope
// for Phase 1: stereo in/out, parameter metadata, bypass, basic
// activation. MIDI routing to instruments is Phase 2; editor view,
// state save/restore via ClassInfo, and sidechain ports land later.
//
// Identified by {componentType, componentSubType, componentManufacturer}
// encoded into PluginInfo.unique_id as "TYPE:SUBT:MANU" (OSType 4CCs).
// Scanner fills this in via scanner_au.mm.

#include <pulp/host/plugin_slot.hpp>
#include <pulp/runtime/log.hpp>

#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>
#include <CoreAudioTypes/CoreAudioTypes.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <vector>

namespace pulp::host {
namespace {

bool parse_4cc_triplet(const std::string& id, OSType& t, OSType& s, OSType& m) {
    // Expect "XXXX:YYYY:ZZZZ" with each field exactly 4 ASCII characters.
    if (id.size() != 14 || id[4] != ':' || id[9] != ':') return false;
    auto pack = [](const char* p) -> OSType {
        return (static_cast<OSType>(static_cast<uint8_t>(p[0])) << 24)
             | (static_cast<OSType>(static_cast<uint8_t>(p[1])) << 16)
             | (static_cast<OSType>(static_cast<uint8_t>(p[2])) << 8)
             |  static_cast<OSType>(static_cast<uint8_t>(p[3]));
    };
    t = pack(id.data() + 0);
    s = pack(id.data() + 5);
    m = pack(id.data() + 10);
    return true;
}

class AuSlot final : public PluginSlot {
public:
    AuSlot(PluginInfo info, AudioUnit au) : info_(std::move(info)), au_(au) {}

    ~AuSlot() override {
        release();
        if (au_) {
            AudioComponentInstanceDispose(au_);
            au_ = nullptr;
        }
    }

    const PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return au_ != nullptr; }

    bool prepare(double sample_rate, int max_block_size) override {
        if (!au_) return false;
        if (prepared_) release();

        sample_rate_ = sample_rate;
        max_block_size_ = max_block_size;

        // 32-bit float, non-interleaved, 2 channels. Most AUs support this
        // natively; a fuller host negotiates per-scope/element.
        const int channels = std::max(1, info_.num_outputs > 0 ? info_.num_outputs : 2);
        AudioStreamBasicDescription asbd{};
        asbd.mSampleRate       = sample_rate;
        asbd.mFormatID         = kAudioFormatLinearPCM;
        asbd.mFormatFlags      = kAudioFormatFlagIsFloat
                               | kAudioFormatFlagIsNonInterleaved
                               | kAudioFormatFlagIsPacked;
        asbd.mBytesPerPacket   = sizeof(float);
        asbd.mFramesPerPacket  = 1;
        asbd.mBytesPerFrame    = sizeof(float);
        asbd.mChannelsPerFrame = static_cast<UInt32>(channels);
        asbd.mBitsPerChannel   = 32;

        // Set on both scopes so the AU knows what stream shape to expect.
        for (auto scope : {kAudioUnitScope_Input, kAudioUnitScope_Output}) {
            OSStatus st = AudioUnitSetProperty(
                au_, kAudioUnitProperty_StreamFormat, scope, 0,
                &asbd, sizeof(asbd));
            if (st != noErr) {
                runtime::log_error("AU: set StreamFormat failed (scope {}, status {})",
                                   static_cast<int>(scope), static_cast<int>(st));
                return false;
            }
        }

        UInt32 max_frames = static_cast<UInt32>(max_block_size);
        AudioUnitSetProperty(
            au_, kAudioUnitProperty_MaximumFramesPerSlice,
            kAudioUnitScope_Global, 0,
            &max_frames, sizeof(max_frames));

        // Install a render callback that pulls the host's current input
        // view; the callback reads from input_frames_ set in process().
        AURenderCallbackStruct cb{};
        cb.inputProc       = &AuSlot::render_callback;
        cb.inputProcRefCon = this;
        OSStatus st = AudioUnitSetProperty(
            au_, kAudioUnitProperty_SetRenderCallback,
            kAudioUnitScope_Input, 0,
            &cb, sizeof(cb));
        if (st != noErr) {
            runtime::log_warn("AU: SetRenderCallback failed (status {}) — instrument only?",
                              static_cast<int>(st));
        }

        st = AudioUnitInitialize(au_);
        if (st != noErr) {
            runtime::log_error("AU: AudioUnitInitialize failed (status {})",
                               static_cast<int>(st));
            return false;
        }

        num_channels_ = channels;
        prepared_ = true;
        cache_parameters();
        return true;
    }

    void release() override {
        if (!au_ || !prepared_) return;
        AudioUnitUninitialize(au_);
        prepared_ = false;
    }

    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>& input,
                 const midi::MidiBuffer& midi_in,
                 midi::MidiBuffer& /*midi_out*/,
                 const ParameterEventQueue& param_events,
                 int num_samples) override {
        if (!au_ || !prepared_) return;
        if (num_samples <= 0 || num_samples > max_block_size_) return;
        if (bypassed_.load(std::memory_order_relaxed)) {
            copy_or_zero(output, input, num_samples);
            return;
        }

        // Deliver MIDI input via MusicDeviceMIDIEvent, AU v2's sample-
        // accurate MIDI write path. Workstream 03 slice 3.6 — previously
        // midi_in was discarded, so hosted AU instruments received no
        // MIDI. Skips malformed messages (length outside [1..3] or no
        // status byte) to avoid polluting the plugin with garbage, same
        // validation the AUv3 adapter adopted in PR #179.
        for (auto it = midi_in.begin(); it != midi_in.end(); ++it) {
            const auto& me = *it;
            const auto& m = me.message;
            auto len = m.length();
            if (len < 1 || len > 3 || (m.data()[0] & 0x80) == 0) continue;
            // Clamp to [0, num_samples - 1]. SignalGraph::inject_midi
            // forwards caller-provided offsets without normalization, so
            // a >= num_samples value can sneak in; AU rejects or mistimes
            // such events. Fix per #191 review.
            int32_t offset = me.sample_offset;
            if (offset < 0) offset = 0;
            if (offset >= num_samples) offset = num_samples - 1;
            MusicDeviceMIDIEvent(
                au_,
                m.data()[0],
                len > 1 ? m.data()[1] : 0,
                len > 2 ? m.data()[2] : 0,
                static_cast<UInt32>(offset));
        }

        // Deliver host-automation events via AudioUnitScheduleParameters,
        // which is AU v2's sample-accurate parameter write path.
        for (const auto& pe : param_events) {
            AudioUnitParameterEvent aupe{};
            aupe.scope     = kAudioUnitScope_Global;
            aupe.element   = 0;
            aupe.parameter = (AudioUnitParameterID)pe.param_id;
            aupe.eventType = kParameterEvent_Immediate;
            aupe.eventValues.immediate.bufferOffset =
                (UInt32)std::max(0, pe.sample_offset);
            aupe.eventValues.immediate.value = pe.value;
            AudioUnitScheduleParameters(au_, &aupe, 1);
        }

        // Make the input pointers visible to the render callback.
        current_input_ = &input;
        current_frames_ = num_samples;

        const int channels = std::min(static_cast<int>(output.num_channels()),
                                      num_channels_);
        // Build an AudioBufferList pointing at the caller's output buffers.
        // Each channel is its own AudioBuffer since we declared
        // non-interleaved.
        std::vector<std::uint8_t> abl_storage(
            sizeof(AudioBufferList) + sizeof(AudioBuffer) * (channels - 1));
        auto* abl = reinterpret_cast<AudioBufferList*>(abl_storage.data());
        abl->mNumberBuffers = static_cast<UInt32>(channels);
        for (int c = 0; c < channels; ++c) {
            abl->mBuffers[c].mNumberChannels = 1;
            abl->mBuffers[c].mDataByteSize =
                static_cast<UInt32>(num_samples) * sizeof(float);
            abl->mBuffers[c].mData = output.channel_ptr(c);
        }

        AudioUnitRenderActionFlags flags = 0;
        AudioTimeStamp ts{};
        ts.mFlags       = kAudioTimeStampSampleTimeValid;
        ts.mSampleTime  = static_cast<Float64>(sample_time_);

        OSStatus st = AudioUnitRender(
            au_, &flags, &ts, 0,
            static_cast<UInt32>(num_samples), abl);
        if (st != noErr) {
            runtime::log_warn("AU: AudioUnitRender returned {}",
                              static_cast<int>(st));
        }
        sample_time_ += num_samples;
        current_input_  = nullptr;
        current_frames_ = 0;
    }

    std::vector<HostParamInfo> parameters() const override { return params_; }

    float get_parameter(uint32_t id) const override {
        if (!au_) return 0.0f;
        AudioUnitParameterValue v = 0.f;
        if (AudioUnitGetParameter(au_, id, kAudioUnitScope_Global, 0, &v) == noErr) {
            return static_cast<float>(v);
        }
        return 0.0f;
    }

    void set_parameter(uint32_t id, float normalized_value) override {
        if (!au_) return;
        AudioUnitSetParameter(au_, id, kAudioUnitScope_Global, 0,
                              static_cast<AudioUnitParameterValue>(normalized_value), 0);
    }

    void set_bypass(bool b) override { bypassed_.store(b, std::memory_order_relaxed); }
    bool is_bypassed() const override { return bypassed_.load(std::memory_order_relaxed); }

    std::vector<uint8_t> save_state() const override {
        if (!au_) return {};
        CFPropertyListRef plist = nullptr;
        UInt32 size = sizeof(plist);
        OSStatus st = AudioUnitGetProperty(
            au_, kAudioUnitProperty_ClassInfo,
            kAudioUnitScope_Global, 0, &plist, &size);
        if (st != noErr || !plist) return {};

        CFDataRef data = CFPropertyListCreateData(
            nullptr, plist,
            kCFPropertyListBinaryFormat_v1_0, 0, nullptr);
        CFRelease(plist);
        if (!data) return {};
        const UInt8* bytes = CFDataGetBytePtr(data);
        const CFIndex len = CFDataGetLength(data);
        std::vector<uint8_t> out(bytes, bytes + len);
        CFRelease(data);
        return out;
    }

    bool restore_state(const std::vector<uint8_t>& data) override {
        if (!au_ || data.empty()) return false;
        CFDataRef cfd = CFDataCreate(nullptr, data.data(),
                                     static_cast<CFIndex>(data.size()));
        if (!cfd) return false;
        CFErrorRef err = nullptr;
        CFPropertyListRef plist = CFPropertyListCreateWithData(
            nullptr, cfd, kCFPropertyListImmutable, nullptr, &err);
        CFRelease(cfd);
        if (!plist) {
            if (err) CFRelease(err);
            return false;
        }
        OSStatus st = AudioUnitSetProperty(
            au_, kAudioUnitProperty_ClassInfo,
            kAudioUnitScope_Global, 0, &plist, sizeof(plist));
        CFRelease(plist);
        return st == noErr;
    }

    bool has_editor() const override { return false; }      // Phase 4
#if defined(__GNUC__) || defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
    void* create_editor_view() override { return nullptr; } // Phase 4
    void destroy_editor_view() override {}                  // Phase 4
#if defined(__GNUC__) || defined(__clang__)
#  pragma clang diagnostic pop
#endif

    int latency_samples() const override {
        if (!au_) return 0;
        Float64 secs = 0.0;
        UInt32 size = sizeof(secs);
        if (AudioUnitGetProperty(
                au_, kAudioUnitProperty_Latency,
                kAudioUnitScope_Global, 0, &secs, &size) == noErr) {
            return static_cast<int>(secs * sample_rate_);
        }
        return 0;
    }
    int tail_samples() const override {
        if (!au_) return 0;
        Float64 secs = 0.0;
        UInt32 size = sizeof(secs);
        if (AudioUnitGetProperty(
                au_, kAudioUnitProperty_TailTime,
                kAudioUnitScope_Global, 0, &secs, &size) == noErr) {
            return static_cast<int>(secs * sample_rate_);
        }
        return 0;
    }

    // Item 4.5 — typed plugin introspection. Surface the AudioUnit
    // instance and its component description so callers can talk directly
    // to AudioToolbox / use vendor-specific properties.
    void accept(ExtensionsVisitor& visitor) const override {
        AudioUnitExtension ext;
        ext.component_instance = au_;
        // Re-parse the 'TYPE:SUBT:MANU' triplet we stored at load time.
        // parse_4cc_triplet lives in this file's anonymous namespace.
        OSType t = 0, s = 0, m = 0;
        if (parse_4cc_triplet(info_.unique_id, t, s, m)) {
            ext.type = static_cast<unsigned int>(t);
            ext.subtype = static_cast<unsigned int>(s);
            ext.manufacturer = static_cast<unsigned int>(m);
        }
        visitor.visit_audio_unit(*this, ext);
    }

private:
    static OSStatus render_callback(
        void* refcon,
        AudioUnitRenderActionFlags* /*flags*/,
        const AudioTimeStamp* /*ts*/,
        UInt32 /*bus*/,
        UInt32 frames,
        AudioBufferList* data)
    {
        auto* self = static_cast<AuSlot*>(refcon);
        if (!self || !self->current_input_ || !data) return noErr;
        const auto& in = *self->current_input_;
        const UInt32 count = data->mNumberBuffers;
        const int host_ch = static_cast<int>(in.num_channels());
        for (UInt32 i = 0; i < count; ++i) {
            auto* dst = static_cast<float*>(data->mBuffers[i].mData);
            if (!dst) continue;
            if (static_cast<int>(i) < host_ch) {
                const float* src = in.channel_ptr(i);
                std::memcpy(dst, src, sizeof(float) * frames);
            } else {
                std::memset(dst, 0, sizeof(float) * frames);
            }
        }
        return noErr;
    }

    void cache_parameters() {
        params_.clear();
        if (!au_) return;
        UInt32 size = 0;
        Boolean writable = false;
        if (AudioUnitGetPropertyInfo(
                au_, kAudioUnitProperty_ParameterList,
                kAudioUnitScope_Global, 0, &size, &writable) != noErr || size == 0) {
            return;
        }
        std::vector<AudioUnitParameterID> ids(size / sizeof(AudioUnitParameterID));
        if (AudioUnitGetProperty(
                au_, kAudioUnitProperty_ParameterList,
                kAudioUnitScope_Global, 0, ids.data(), &size) != noErr) {
            return;
        }
        for (auto pid : ids) {
            AudioUnitParameterInfo info{};
            UInt32 isize = sizeof(info);
            if (AudioUnitGetProperty(
                    au_, kAudioUnitProperty_ParameterInfo,
                    kAudioUnitScope_Global, pid, &info, &isize) != noErr) {
                continue;
            }
            HostParamInfo h;
            h.id = static_cast<uint32_t>(pid);
            h.min_value = static_cast<float>(info.minValue);
            h.max_value = static_cast<float>(info.maxValue);
            h.default_value = static_cast<float>(info.defaultValue);

            if (info.flags & kAudioUnitParameterFlag_HasCFNameString && info.cfNameString) {
                char buf[256] = {0};
                if (CFStringGetCString(info.cfNameString, buf, sizeof(buf),
                                       kCFStringEncodingUTF8)) {
                    h.name = buf;
                }
                if (info.flags & kAudioUnitParameterFlag_CFNameRelease) {
                    CFRelease(info.cfNameString);
                }
            } else {
                h.name = info.name;
            }
            // Map AudioUnitParameterFlags onto HostParamInfo::ParamFlags.
            const UInt32 f = info.flags;
            h.flags.read_only   = (f & kAudioUnitParameterFlag_IsReadable) != 0
                                && !(f & kAudioUnitParameterFlag_IsWritable);
            h.flags.hidden      = (f & kAudioUnitParameterFlag_NonRealTime) != 0;
            h.flags.automatable = !h.flags.read_only;
            // AU parameter unit kAudioUnitParameterUnit_Indexed marks discrete.
            h.flags.stepped     = (info.unit == kAudioUnitParameterUnit_Indexed
                                || info.unit == kAudioUnitParameterUnit_Boolean);
            h.flags.rampable    = !h.flags.stepped;
            h.flags.modulatable = false;  // AU has no per-voice mod concept.
            params_.push_back(std::move(h));
        }
    }

    static void copy_or_zero(audio::BufferView<float>& out,
                             const audio::BufferView<const float>& in,
                             int num_samples) {
        const size_t nch_out = out.num_channels();
        const size_t nch_in  = in.num_channels();
        for (size_t c = 0; c < nch_out; ++c) {
            auto* dst = out.channel_ptr(c);
            if (c < nch_in) {
                std::memcpy(dst, in.channel_ptr(c), sizeof(float) * num_samples);
            } else {
                std::memset(dst, 0, sizeof(float) * num_samples);
            }
        }
    }

    PluginInfo info_;
    AudioUnit au_ = nullptr;

    std::vector<HostParamInfo> params_;
    const audio::BufferView<const float>* current_input_ = nullptr;
    int current_frames_ = 0;

    double sample_rate_    = 48000.0;
    int    max_block_size_ = 0;
    int    num_channels_   = 2;
    bool   prepared_       = false;
    int64_t sample_time_   = 0;
    std::atomic<bool> bypassed_{false};
};

}  // namespace

std::unique_ptr<PluginSlot> load_au_plugin(const PluginInfo& info) {
    OSType type = 0, subtype = 0, manufacturer = 0;
    if (!parse_4cc_triplet(info.unique_id, type, subtype, manufacturer)) {
        runtime::log_error(
            "AU load: unique_id must be 'TYPE:SUBT:MANU' (OSType 4CCs), got '{}'",
            info.unique_id);
        return nullptr;
    }

    AudioComponentDescription desc{};
    desc.componentType = type;
    desc.componentSubType = subtype;
    desc.componentManufacturer = manufacturer;

    AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
    if (!comp) {
        runtime::log_error("AU load: AudioComponentFindNext failed for '{}'",
                           info.unique_id);
        return nullptr;
    }

    AudioComponentInstance instance = nullptr;
    OSStatus st = AudioComponentInstanceNew(comp, &instance);
    if (st != noErr || !instance) {
        runtime::log_error("AU load: AudioComponentInstanceNew failed (status {})",
                           static_cast<int>(st));
        return nullptr;
    }

    return std::make_unique<AuSlot>(info, instance);
}

}  // namespace pulp::host
