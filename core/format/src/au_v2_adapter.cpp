// Audio Unit v2 adapter for Pulp
// Uses Apple's AudioUnitSDK (Apache 2.0) for proper AU v2 hosting contract
// Subclasses AUEffectBase and overrides ProcessBufferLists for multi-channel

#include <AudioUnitSDK/AUPlugInDispatch.h>
#include <AudioToolbox/AudioUnitUtilities.h>
#include <AudioToolbox/AudioToolbox.h>  // kAudioUnitProperty_CocoaUI, AudioUnitCocoaViewInfo

#include <mach/mach_time.h>

#include <pulp/format/au_v2_adapter.hpp>
#include <pulp/format/quirk_apply.hpp>
#include <pulp/format/detail/playhead_diff.hpp>
#include <pulp/format/plugin_state_io.hpp>
#include <pulp/format/registry.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>

#include <cstring>
#include <limits>

namespace pulp::format::au {

// Cross-TU Cocoa-view hook (see au_v2_adapter.hpp). Hidden visibility keeps it
// per-loaded-image so two Pulp AU components in one host don't share state.
// Installed by au_v2_cocoa_view.mm's static-init when a *_AU target links it
// (PULP_AU_GUI); null for CLAP / Standalone / headless builds of pulp-format.
#if defined(__clang__) || defined(__GNUC__)
__attribute__((visibility("hidden")))
#endif
CocoaViewInfoFiller g_cocoa_view_info_filler = nullptr;

// Parameter IDs for the AU system map 1:1 from Pulp ParamIDs
// AU uses AudioUnitParameterID (UInt32) which matches our state::ParamID

midi::MidiEvent decode_midi_event(uint8_t inStatus,
                                  uint8_t inChannel,
                                  uint8_t inData1,
                                  uint8_t inData2) noexcept
{
    // AUMIDIBase::MIDIEvent (AudioUnitSDK 1.4 AUMIDIBase.h) splits the
    // wire-format status byte unconditionally:
    //
    //     strippedStatus = inStatus & 0xF0   // top nibble  -> our inStatus
    //     channel        = inStatus & 0x0F   // low nibble  -> our inChannel
    //     HandleMIDIEvent(strippedStatus, channel, ...)
    //
    // The split is the SAME for every status byte the host delivers to
    // this callback — channel-voice (0x80-0xEF) AND system (0xF0-0xFF).
    // For system common (0xF1-0xF7) and system realtime (0xF8-0xFF), the
    // low nibble carries the system-message subtype rather than a channel,
    // but the bit-layout reassembly is identical: status = top | low.
    //
    // The previous "is_system → return inStatus unchanged" special case
    // turned every system message into 0xF0 (sysex start), so MIDI clock
    // / start / stop / continue / song-position / quarter-frame all
    // arrived at the Processor with the wrong status byte. Codex review
    // on PR #638 caught this; the unit test in test_au_v2_effect.cpp now
    // mirrors the SDK splitting so the regression cannot reappear.
    const uint8_t status_byte =
        static_cast<uint8_t>((inStatus & 0xF0) | (inChannel & 0x0F));
    midi::MidiEvent ev{
        choc::midi::ShortMessage(status_byte, inData1, inData2),
        /*sample_offset=*/0,
        /*timestamp=*/0.0,
    };
    return ev;
}

PulpAUEffect::PulpAUEffect(AudioComponentInstance ci)
    : AUMIDIEffectBase(ci, /*inProcessesInPlace=*/true)
{
    auto factory = registered_factory();
    if (factory) {
        processor_ = factory();
        if (processor_) {
            processor_->set_state_store(&store_);
            processor_->define_parameters(store_);

            // Resolve host accommodations once (host-quirks plan, P3) via
            // the runtime policy (PULP_HOST_QUIRKS env / API).
            const auto host_info = detect_host_info();
            host_quirks_ = resolved_quirks(host_info.type, host_info.version);

            // synthesize_bypass_parameter (host-quirks P3d): inject an
            // automatable Bypass when the plugin declared none (before the
            // AU param list is built from the store), then detect it so
            // ProcessBufferLists can honor it with a pass-through.
            maybe_synthesize_bypass(store_, host_quirks_);
            for (const auto& p : store_.all_params()) {
                if (p.name == "Bypass" && p.range.step >= 1.0f &&
                    p.range.min == 0.0f && p.range.max == 1.0f) {
                    bypass_param_id_ = p.id;
                    break;
                }
            }

            // Wire gesture callbacks for undo grouping support.
            store_.set_gesture_callbacks(
                [this](state::ParamID id) {
                    AudioUnitEvent event;
                    event.mEventType = kAudioUnitEvent_BeginParameterChangeGesture;
                    event.mArgument.mParameter.mAudioUnit = GetComponentInstance();
                    event.mArgument.mParameter.mParameterID = static_cast<AudioUnitParameterID>(id);
                    event.mArgument.mParameter.mScope = kAudioUnitScope_Global;
                    event.mArgument.mParameter.mElement = 0;
                    AUEventListenerNotify(nullptr, nullptr, &event);
                },
                [this](state::ParamID id) {
                    AudioUnitEvent event;
                    event.mEventType = kAudioUnitEvent_EndParameterChangeGesture;
                    event.mArgument.mParameter.mAudioUnit = GetComponentInstance();
                    event.mArgument.mParameter.mParameterID = static_cast<AudioUnitParameterID>(id);
                    event.mArgument.mParameter.mScope = kAudioUnitScope_Global;
                    event.mArgument.mParameter.mElement = 0;
                    AUEventListenerNotify(nullptr, nullptr, &event);
                });

            // Set defaults in AU parameter system at construction time so hosts
            // can inspect them before Initialize() is called.
            for (const auto& param : store_.all_params()) {
                Globals()->SetParameter(
                    static_cast<AudioUnitParameterID>(param.id),
                    param.range.default_value);
            }
        }
    }
}

OSStatus PulpAUEffect::GetParameterList(AudioUnitScope inScope,
                                        AudioUnitParameterID* outParameterList,
                                        UInt32& outNumParameters)
{
    if (inScope != kAudioUnitScope_Global) {
        outNumParameters = 0;
        return noErr;
    }

    outNumParameters = static_cast<UInt32>(store_.param_count());
    if (outParameterList) {
        auto params = store_.all_params();
        for (std::size_t i = 0; i < params.size(); ++i) {
            outParameterList[i] = static_cast<AudioUnitParameterID>(params[i].id);
        }
    }
    return noErr;
}

OSStatus PulpAUEffect::GetParameterInfo(AudioUnitScope inScope,
                                        AudioUnitParameterID inParameterID,
                                        AudioUnitParameterInfo& outParameterInfo)
{
    if (inScope != kAudioUnitScope_Global)
        return kAudioUnitErr_InvalidParameter;

    const auto* param = store_.info(static_cast<state::ParamID>(inParameterID));
    if (!param) return kAudioUnitErr_InvalidParameter;

    outParameterInfo.flags = kAudioUnitParameterFlag_IsWritable
                           | kAudioUnitParameterFlag_IsReadable
                           | kAudioUnitParameterFlag_HasCFNameString;

    CFStringRef name = CFStringCreateWithCString(
        kCFAllocatorDefault, param->name.c_str(), kCFStringEncodingUTF8);
    outParameterInfo.cfNameString = name;
    strlcpy(reinterpret_cast<char*>(outParameterInfo.name),
            param->name.c_str(), sizeof(outParameterInfo.name));

    outParameterInfo.minValue = param->range.min;
    outParameterInfo.maxValue = param->range.max;
    outParameterInfo.defaultValue = param->range.default_value;

    if (param->unit == "dB") {
        outParameterInfo.unit = kAudioUnitParameterUnit_Decibels;
    } else if (param->unit == "Hz") {
        outParameterInfo.unit = kAudioUnitParameterUnit_Hertz;
    } else if (param->unit == "%") {
        outParameterInfo.unit = kAudioUnitParameterUnit_Percent;
    } else if (param->range.step >= 1.0f
               && param->range.min == 0.0f
               && param->range.max == 1.0f) {
        outParameterInfo.unit = kAudioUnitParameterUnit_Boolean;
    } else {
        outParameterInfo.unit = kAudioUnitParameterUnit_Generic;
    }

    return noErr;
}

OSStatus PulpAUEffect::GetParameterValueStrings(AudioUnitScope inScope,
                                                AudioUnitParameterID inParameterID,
                                                CFArrayRef* outStrings)
{
    if (inScope != kAudioUnitScope_Global)
        return kAudioUnitErr_InvalidParameter;

    const auto* param = store_.info(static_cast<state::ParamID>(inParameterID));
    if (!param) return kAudioUnitErr_InvalidParameter;

    if (param->range.step < 1.0f || !param->to_string)
        return kAudioUnitErr_InvalidPropertyValue;

    int count = static_cast<int>((param->range.max - param->range.min) / param->range.step) + 1;
    CFMutableArrayRef strings = CFArrayCreateMutable(kCFAllocatorDefault, count, &kCFTypeArrayCallBacks);
    for (int i = 0; i < count; ++i) {
        float value = param->range.min + i * param->range.step;
        auto str = param->to_string(value);
        CFStringRef cfStr = CFStringCreateWithCString(
            kCFAllocatorDefault, str.c_str(), kCFStringEncodingUTF8);
        CFArrayAppendValue(strings, cfStr);
        CFRelease(cfStr);
    }
    *outStrings = strings;
    return noErr;
}

OSStatus PulpAUEffect::GetPropertyInfo(AudioUnitPropertyID inID, AudioUnitScope inScope,
                                       AudioUnitElement inElement, UInt32& outDataSize,
                                       bool& outWritable)
{
    if (inID == kPulpEditorContextProperty) {
        if (inScope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;
        if (inElement != 0) return kAudioUnitErr_InvalidElement;
        outDataSize = sizeof(PulpEditorContext);
        outWritable = false;
        return noErr;
    }
    if (inID == kAudioUnitProperty_CocoaUI && g_cocoa_view_info_filler) {
        if (inScope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;
        outDataSize = sizeof(AudioUnitCocoaViewInfo);
        outWritable = false;
        return noErr;
    }
    return AUMIDIEffectBase::GetPropertyInfo(inID, inScope, inElement, outDataSize, outWritable);
}

OSStatus PulpAUEffect::GetProperty(AudioUnitPropertyID inID, AudioUnitScope inScope,
                                   AudioUnitElement inElement, void* outData)
{
    if (inID == kPulpEditorContextProperty) {
        if (inScope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;
        if (inElement != 0) return kAudioUnitErr_InvalidElement;
        if (!outData) return kAudioUnitErr_InvalidProperty;
        auto* ctx = static_cast<PulpEditorContext*>(outData);
        ctx->processor = processor_.get();
        ctx->store = &store_;
        return noErr;
    }
    if (inID == kAudioUnitProperty_CocoaUI && g_cocoa_view_info_filler) {
        if (inScope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;
        if (!outData) return kAudioUnitErr_InvalidProperty;
        return g_cocoa_view_info_filler(outData) ? noErr : kAudioUnitErr_InvalidProperty;
    }
    return AUMIDIEffectBase::GetProperty(inID, inScope, inElement, outData);
}

OSStatus PulpAUEffect::Initialize()
{
    auto result = AUEffectBase::Initialize();
    if (result != noErr) return result;

    if (processor_) {
        PrepareContext ctx;
        ctx.sample_rate = GetSampleRate();
        ctx.max_buffer_size = GetMaxFramesPerSlice();
        ctx.input_channels = static_cast<int>(GetNumberOfChannels());
        ctx.output_channels = static_cast<int>(GetNumberOfChannels());
        processor_->prepare(ctx);

        for (const auto& param : store_.all_params()) {
            auto au_id = static_cast<AudioUnitParameterID>(param.id);
            float value = Globals()->GetParameter(au_id);
            store_.set_value(param.id, value);
        }
    }

    runtime::log_info("AU v2: initialized with {} channels at {} Hz",
                      GetNumberOfChannels(), GetSampleRate());
    return noErr;
}

void PulpAUEffect::Cleanup()
{
    if (processor_) processor_->release();
    AUEffectBase::Cleanup();
}

OSStatus PulpAUEffect::HandleMIDIEvent(UInt8 inStatus, UInt8 inChannel,
                                       UInt8 inData1, UInt8 inData2,
                                       UInt32 inStartFrame)
{
    auto ev = decode_midi_event(static_cast<uint8_t>(inStatus),
                                static_cast<uint8_t>(inChannel),
                                static_cast<uint8_t>(inData1),
                                static_cast<uint8_t>(inData2));
    ev.sample_offset = static_cast<int32_t>(inStartFrame);
    std::lock_guard lock(midi_mutex_);
    pending_midi_.add(ev);
    return noErr;
}

OSStatus PulpAUEffect::HandleSysEx(const UInt8* inData, UInt32 inLength)
{
    if (!inData || inLength == 0) return noErr;
    std::vector<uint8_t> bytes(inData, inData + inLength);
    std::lock_guard lock(midi_mutex_);
    // AU v2 SysEx doesn't surface a per-event sample offset at this SDK
    // layer; deliver at block start so plugins see it on the leading edge
    // of the current process() block.
    pending_midi_.add_sysex(std::move(bytes), /*sample_offset=*/0);
    return noErr;
}

OSStatus PulpAUEffect::ProcessBufferLists(AudioUnitRenderActionFlags& ioActionFlags,
                                          const AudioBufferList& inBuffer,
                                          AudioBufferList& outBuffer,
                                          UInt32 inFramesToProcess)
{
    if (!processor_) {
        for (UInt32 i = 0; i < outBuffer.mNumberBuffers; ++i) {
            memset(outBuffer.mBuffers[i].mData, 0,
                   outBuffer.mBuffers[i].mDataByteSize);
        }
        return noErr;
    }

    // Host → plugin: pull current parameter values into the store before
    // processing. Also snapshot them so we can detect plugin-driven changes
    // after process() returns.
    auto params = store_.all_params();
    param_snapshot_.resize(params.size());
    for (std::size_t i = 0; i < params.size(); ++i) {
        auto au_id = static_cast<AudioUnitParameterID>(params[i].id);
        float value = GetParameter(au_id);
        store_.set_value(params[i].id, value);
        param_snapshot_[i] = value;
    }

    UInt32 in_channels = inBuffer.mNumberBuffers;
    UInt32 out_channels = outBuffer.mNumberBuffers;

    input_ptrs_.resize(in_channels);
    output_ptrs_.resize(out_channels);

    for (UInt32 i = 0; i < in_channels; ++i) {
        input_ptrs_[i] = static_cast<const float*>(inBuffer.mBuffers[i].mData);
    }
    for (UInt32 i = 0; i < out_channels; ++i) {
        output_ptrs_[i] = static_cast<float*>(outBuffer.mBuffers[i].mData);
    }

    // synthesize_bypass_parameter (host-quirks P3d): when the Bypass param
    // (declared or synthesized) is engaged, copy main input → main output
    // (null-guarded), zero any output channel without a matching input, and
    // skip the Processor — mirrors the VST3/CLAP pass-through. The value was
    // pulled into the store from GetParameter() above.
    if (bypass_param_id_ != 0 && store_.get_value(bypass_param_id_) >= 0.5f) {
        for (UInt32 ch = 0; ch < out_channels; ++ch) {
            float* dst = output_ptrs_[ch];
            if (dst == nullptr) continue;
            if (ch < in_channels && input_ptrs_[ch] != nullptr) {
                std::memcpy(dst, input_ptrs_[ch], sizeof(float) * inFramesToProcess);
            } else {
                std::memset(dst, 0, sizeof(float) * inFramesToProcess);
            }
        }
        // Drain + DISCARD any MIDI queued by HandleMIDIEvent/HandleSysEx
        // while bypassed. Without this the queue grows for the whole bypass
        // window and floods the processor with stale notes/CCs the instant
        // bypass turns off (Codex review on #3246). A bypassed plugin is a
        // wire — inbound MIDI is dropped with the block, matching the VST3
        // path (which leaves MIDI buffers empty on the bypass short-circuit).
        {
            std::lock_guard lock(midi_mutex_);
            pending_midi_ = midi::MidiBuffer{};
        }
        return noErr;
    }

    audio::BufferView<const float> input_view(
        input_ptrs_.data(), in_channels, inFramesToProcess);
    audio::BufferView<float> output_view(
        output_ptrs_.data(), out_channels, inFramesToProcess);

    midi::MidiBuffer midi_in, midi_out;
    // Drain MIDI events queued by HandleMIDIEvent/HandleSysEx on the host's
    // MIDI delivery thread. Mirrors the AU v2 instrument adapter's pattern
    // (``core/format/src/au_v2_instrument.cpp``) so ``aumf``-typed effects
    // can actually receive inbound MIDI. ``aufx``-typed effects still get
    // here, but the host never routes MIDI to them — pending_midi_ will
    // simply stay empty.
    {
        std::lock_guard lock(midi_mutex_);
        midi_in = std::move(pending_midi_);
        pending_midi_ = midi::MidiBuffer{};
    }
    midi_in.sort();

    ProcessContext ctx;
    ctx.sample_rate = GetSampleRate();
    ctx.num_samples = static_cast<int>(inFramesToProcess);
    ctx.process_mode = ProcessMode::Realtime;
    ctx.render_speed_hint = RenderSpeedHint::Realtime;

    // Item 1.3 — populate transport fields from the host callbacks the
    // host installed via kAudioUnitProperty_HostCallbacks. The
    // AudioUnitSDK wraps these as `CallHostBeatAndTempo` /
    // `CallHostMusicalTimeLocation` / `CallHostTransportState`. They
    // are render-thread-only; calling outside ProcessBufferLists
    // returns `kAudioUnitErr_CannotDoInCurrentContext`. AU has no
    // dedicated frame-rate or loop callback — `frame_rate` stays
    // `FrameRate::unknown` and the loop range falls back to the
    // transport callback's outCycleStartBeat / outCycleEndBeat (only
    // valid when outIsCycling is true).
    {
        Float64 beat = 0.0;
        Float64 tempo = 0.0;
        if (CallHostBeatAndTempo(&beat, &tempo) == noErr) {
            ctx.position_beats = beat;
            if (tempo > 0.0) ctx.tempo_bpm = tempo;
        }

        UInt32 delta_samples = 0;
        Float32 ts_num = 0.0f;
        UInt32 ts_denom = 0;
        Float64 current_measure_downbeat = 0.0;
        if (CallHostMusicalTimeLocation(&delta_samples, &ts_num, &ts_denom,
                                        &current_measure_downbeat) == noErr) {
            if (ts_num > 0.0f) {
                ctx.time_sig_numerator = static_cast<int>(ts_num);
            }
            if (ts_denom > 0) {
                ctx.time_sig_denominator = static_cast<int>(ts_denom);
            }
        }

        Boolean is_playing = false;
        Boolean transport_state_changed = false;
        Float64 current_sample_in_timeline = 0.0;
        Boolean is_cycling = false;
        Float64 cycle_start = 0.0;
        Float64 cycle_end = 0.0;
        if (CallHostTransportState(&is_playing, &transport_state_changed,
                                   &current_sample_in_timeline, &is_cycling,
                                   &cycle_start, &cycle_end) == noErr) {
            ctx.is_playing = (is_playing != 0);
            ctx.position_samples =
                static_cast<int64_t>(current_sample_in_timeline);
            ctx.is_looping = (is_cycling != 0);
            if (ctx.is_looping) {
                ctx.loop_start_beats = cycle_start;
                ctx.loop_end_beats = cycle_end;
            }
            // `is_recording` is not exposed by HostCallback_GetTransportState;
            // the v2 GetTransportState2 callback adds it but AUBase's
            // CallHostTransportState wrapper only calls the v1 proc. Leave
            // `is_recording` at the default false sentinel — plug-ins that
            // need recording state must use AU v3 today.
        }

        // Host clock for video sync. AU doesn't surface a "host time"
        // callback, but `mach_absolute_time` ticks the same monotonic
        // clock the host renders against on Apple platforms, so it is
        // the closest analogue. Convert ticks to nanoseconds via
        // `mach_timebase_info` (cached on first use).
        static mach_timebase_info_data_t timebase = {0, 0};
        if (timebase.denom == 0) mach_timebase_info(&timebase);
        if (timebase.denom != 0) {
            const uint64_t now = mach_absolute_time();
            ctx.host_time_ns = static_cast<int64_t>(
                (now * timebase.numer) / timebase.denom);
        }

        pulp::format::detail::derive_bar_from_beats(ctx);
    }

    pulp::format::detail::compute_playhead_changes(ctx, playhead_prev_);

    // Phase 3 — uniform param-events contract + RT-safety guard. AU v2 has no
    // scheduled-parameter event source, so the queue is empty (host params flow
    // via store_ as before); set it anyway so a Processor always sees a non-null
    // queue. Wrap ONLY the process call in ScopedNoAlloc — the preamble above
    // (param snapshot, pointer-vector resizes) legitimately allocates.
    param_events_.clear();
    processor_->set_param_events(&param_events_);
    {
        pulp::runtime::ScopedNoAlloc no_alloc_guard;
        processor_->process(output_view, input_view, midi_in, midi_out, ctx);
    }

    // Plugin → host: diff params against the pre-process snapshot and push
    // any plugin-side changes back to the host's parameter system. Without
    // this loop, plugin-driven automation (e.g. a modulated parameter that
    // the plugin writes to its store during process()) would never reach the
    // host's automation lane, mirror widgets, or generic UI.
    // Matches the VST3 and CLAP adapters' snapshot/diff pattern; workstream
    // 01 slice 1.3.
    for (std::size_t i = 0; i < params.size(); ++i) {
        float post = store_.get_value(params[i].id);
        if (post == param_snapshot_[i]) continue;
        auto au_id = static_cast<AudioUnitParameterID>(params[i].id);
        SetParameter(au_id, post);
        AudioUnitEvent event;
        std::memset(&event, 0, sizeof(event));
        event.mEventType = kAudioUnitEvent_ParameterValueChange;
        event.mArgument.mParameter.mAudioUnit = GetComponentInstance();
        event.mArgument.mParameter.mParameterID = au_id;
        event.mArgument.mParameter.mScope = kAudioUnitScope_Global;
        event.mArgument.mParameter.mElement = 0;
        AUEventListenerNotify(nullptr, nullptr, &event);
    }

    // Item 3.11 — push latency / tail change notifications the processor
    // flagged during process(). AU v2 hosts watch
    // kAudioUnitProperty_Latency and kAudioUnitProperty_TailTime via
    // PropertyListeners; PropertyChanged is the canonical SDK call
    // that wakes them. Safe to call from the render callback (AU SDK
    // marshals through the host's property-listener queue).
    if (processor_->consume_latency_changed_flag()) {
        PropertyChanged(kAudioUnitProperty_Latency,
                        kAudioUnitScope_Global, 0);
    }
    if (processor_->consume_tail_changed_flag()) {
        PropertyChanged(kAudioUnitProperty_TailTime,
                        kAudioUnitScope_Global, 0);
    }

    return noErr;
}

OSStatus PulpAUEffect::SaveState(CFPropertyListRef* outData)
{
    auto result = AUEffectBase::SaveState(outData);
    if (result != noErr) return result;

    if (!processor_) return kAudioUnitErr_Uninitialized;
    auto data = plugin_state_io::serialize(store_, *processor_);
    CFDataRef cfData = CFDataCreate(kCFAllocatorDefault,
                                    data.data(),
                                    static_cast<CFIndex>(data.size()));
    if (cfData) {
        CFMutableDictionaryRef dict = nullptr;
        if (*outData && CFGetTypeID(*outData) == CFDictionaryGetTypeID()) {
            dict = CFDictionaryCreateMutableCopy(
                kCFAllocatorDefault, 0,
                static_cast<CFDictionaryRef>(*outData));
        } else {
            dict = CFDictionaryCreateMutable(kCFAllocatorDefault,
                                             0,
                                             &kCFTypeDictionaryKeyCallBacks,
                                             &kCFTypeDictionaryValueCallBacks);
        }

        if (*outData) {
            CFRelease(*outData);
        }

        CFDictionarySetValue(dict, CFSTR("pulp-state"), cfData);
        *outData = dict;
        CFRelease(cfData);
    }
    return noErr;
}

OSStatus PulpAUEffect::RestoreState(CFPropertyListRef plist)
{
    auto result = AUEffectBase::RestoreState(plist);
    if (result != noErr) return result;

    if (CFGetTypeID(plist) == CFDictionaryGetTypeID()) {
        auto dict = static_cast<CFDictionaryRef>(plist);
        auto cfData = static_cast<CFDataRef>(
            CFDictionaryGetValue(dict, CFSTR("pulp-state")));
        if (cfData && CFGetTypeID(cfData) == CFDataGetTypeID()) {
            auto* bytes = CFDataGetBytePtr(cfData);
            auto length = CFDataGetLength(cfData);
            if (!processor_) return kAudioUnitErr_Uninitialized;
            if (!plugin_state_io::deserialize({bytes, static_cast<size_t>(length)},
                                              store_, *processor_)) {
                return kAudioUnitErr_InvalidPropertyValue;
            }

            for (const auto& param : store_.all_params()) {
                Globals()->SetParameter(
                    static_cast<AudioUnitParameterID>(param.id),
                    store_.get_value(param.id));
            }
        }
    }
    return noErr;
}

bool PulpAUEffect::SupportsTail()
{
    return true;
}

Float64 PulpAUEffect::GetTailTime()
{
    if (!processor_) return 0.0;
    auto tail = processor_->descriptor().tail_samples;
    if (tail < 0) return std::numeric_limits<Float64>::infinity();
    return tail > 0 ? static_cast<Float64>(tail) / GetSampleRate() : 0.0;
}

Float64 PulpAUEffect::GetLatency()
{
    if (!processor_) return 0.0;
    // clamp_latency_to_nonneg (host-quirks P3): route the existing clamp
    // through the quirk so PULP_HOST_QUIRKS=off reports raw latency too.
    int latency = reported_latency_samples(processor_->latency_samples(), host_quirks_);
    return GetSampleRate() > 0 ? static_cast<Float64>(latency) / GetSampleRate() : 0.0;
}

} // namespace pulp::format::au
