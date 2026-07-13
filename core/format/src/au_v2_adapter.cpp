// Audio Unit v2 adapter for Pulp
// Uses Apple's AudioUnitSDK (Apache 2.0) for proper AU v2 hosting contract
// Subclasses AUEffectBase and overrides ProcessBufferLists for multi-channel

#include <AudioUnitSDK/AUPlugInDispatch.h>
#include <AudioToolbox/AudioUnitUtilities.h>
#include <AudioToolbox/AudioToolbox.h>  // kAudioUnitProperty_CocoaUI, AudioUnitCocoaViewInfo

#include <mach/mach_time.h>

#include <pulp/format/au_v2_adapter.hpp>
#include <pulp/format/quirk_apply.hpp>
#include <pulp/format/detail/param_host_sync.hpp>
#include <pulp/format/detail/playhead_diff.hpp>
#include <pulp/format/plugin_state_io.hpp>
#include <pulp/format/parameter_text.hpp>
#include <pulp/format/registry.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>
#include <pulp/signal/scoped_flush_denormals.hpp>

#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>

namespace pulp::format::au {

// Set while the HOST is writing a parameter (SetParameter), so the store's
// UI-push listener — which fires inline on the same thread — skips echoing the
// change back to the host. Thread-local so the guard is scoped to the writing
// thread only.
namespace { thread_local bool g_host_writing_param = false; }

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
    // Returning inStatus unchanged for system messages would turn every
    // system message into 0xF0 (sysex start), so MIDI clock / start / stop /
    // continue / song-position / quarter-frame would arrive at the Processor
    // with the wrong status byte. The unit test in test_au_v2_effect.cpp
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
    : PulpAUEffect(ci, registered_factory())
{
}

PulpAUEffect::PulpAUEffect(AudioComponentInstance ci, ProcessorFactory factory)
    : AUMIDIEffectBase(ci, /*inProcessesInPlace=*/true)
{
    if (factory) {
        processor_ = factory();
        if (processor_) {
            processor_->set_state_store(&store_);
            processor_->define_parameters(store_);
            // Cache the immutable descriptor so the render path can view its bus
            // names without a per-block copy (which would allocate on the audio
            // thread).
            descriptor_ = processor_->descriptor();

            // Resolve host accommodations once via the runtime policy.
            const auto host_info = detect_host_info();
            host_quirks_ = resolved_quirks(host_info.type, host_info.version);

            // Inject an automatable Bypass when host-quirk policy requests
            // one and the plugin declared none. Do this before the AU param
            // list is built from the store, then detect it so
            // ProcessBufferLists can honor it with a pass-through.
            maybe_synthesize_bypass(store_, host_quirks_);
            for (const auto& p : store_.all_params()) {
                // Declared designation wins; falls back to the legacy
                // name/range heuristic for params that declare none.
                if (state::is_bypass_param(p)) {
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

            // UI → host parameter NOTIFICATION. When the editor edits a
            // parameter (ParameterEdit → store), tell the host its value
            // changed so it re-reads (via our GetParameter override) and records
            // automation. This is a plain AUEventListenerNotify — NOT
            // AudioUnitSetParameter on ourselves (which re-dispatches through the
            // AU and, observed in Logic, wedges the render on gesture release).
            //
            // Registered as an Audio listener so it runs INLINE, synchronously,
            // on whichever thread set the store: a UI edit fires it on the
            // message thread (correct); a host write (SetParameter) fires it with
            // g_host_writing_param set, so the echo is suppressed. The render
            // thread never writes the store (no reconcile), so this never
            // notifies from the render thread.
            ui_push_listener_ = store_.add_listener(
                [this](state::ParamID id, float /*value*/) {
                    if (g_host_writing_param) return; // host's own write — no echo
                    AudioUnitEvent ev;
                    std::memset(&ev, 0, sizeof(ev));
                    ev.mEventType = kAudioUnitEvent_ParameterValueChange;
                    ev.mArgument.mParameter.mAudioUnit = GetComponentInstance();
                    ev.mArgument.mParameter.mParameterID =
                        static_cast<AudioUnitParameterID>(id);
                    ev.mArgument.mParameter.mScope = kAudioUnitScope_Global;
                    ev.mArgument.mParameter.mElement = 0;
                    AUEventListenerNotify(nullptr, nullptr, &ev);
                },
                state::ListenerThread::Audio);

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

    // Advertise author-supplied value strings so the host queries our display
    // formatting. For DISCRETE params the host reads the enumerated list via
    // GetParameterValueStrings; for CONTINUOUS params it round-trips single
    // values through kAudioUnitProperty_ParameterStringFromValue /
    // ...ValueFromString (handled in GetProperty). Both are gated on to_string,
    // so a plugin that declares no converter keeps the host's stock numeric
    // display unchanged.
    if (param->to_string || !param->value_labels.empty()) {
        outParameterInfo.flags |= kAudioUnitParameterFlag_ValuesHaveStrings;
    }

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
    } else if (state::is_boolean_param(*param)) {
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

    if (!state::is_discrete_param(*param))
        return kAudioUnitErr_InvalidPropertyValue;

    const int count = static_cast<int>(state::param_value_count(*param));
    CFMutableArrayRef strings = CFArrayCreateMutable(kCFAllocatorDefault, count, &kCFTypeArrayCallBacks);
    for (int i = 0; i < count; ++i) {
        const float step = param->range.step > 0.0f ? param->range.step : 1.0f;
        float value = param->range.min + i * step;
        auto str = format_parameter_text(*param, value);
        CFStringRef cfStr = CFStringCreateWithCString(
            kCFAllocatorDefault, str.c_str(), kCFStringEncodingUTF8);
        CFArrayAppendValue(strings, cfStr);
        CFRelease(cfStr);
    }
    *outStrings = strings;
    return noErr;
}

OSStatus PulpAUEffect::GetParameter(AudioUnitParameterID inID, AudioUnitScope inScope,
                                    AudioUnitElement inElement, Float32& outValue)
{
    // Single source of truth: the host reads the plugin's StateStore directly
    // (matching the plain value range declared by GetParameterInfo). No separate
    // Globals copy, so nothing to reconcile and nothing to snap back.
    if (inScope == kAudioUnitScope_Global &&
        store_.info(static_cast<state::ParamID>(inID)) != nullptr) {
        outValue = store_.get_value(static_cast<state::ParamID>(inID));
        return noErr;
    }
    return AUMIDIEffectBase::GetParameter(inID, inScope, inElement, outValue);
}

OSStatus PulpAUEffect::SetParameter(AudioUnitParameterID inID, AudioUnitScope inScope,
                                    AudioUnitElement inElement, Float32 inValue,
                                    UInt32 inBufferOffsetInFrames)
{
    // Host-side write (automation playback, generic UI, preset recall) lands
    // straight in the store that process() reads. set_value_rt is RT-safe (the
    // host may call this from the render thread) and fires the inline Audio
    // listener synchronously, where g_host_writing_param suppresses the echo
    // back to the host.
    if (inScope == kAudioUnitScope_Global &&
        store_.info(static_cast<state::ParamID>(inID)) != nullptr) {
        g_host_writing_param = true;
        store_.set_value_rt(static_cast<state::ParamID>(inID), inValue);
        g_host_writing_param = false;
        return noErr;
    }
    return AUMIDIEffectBase::SetParameter(inID, inScope, inElement, inValue,
                                          inBufferOffsetInFrames);
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
    if (inID == kAudioUnitProperty_MIDIOutputCallbackInfo &&
        plugin_produces_midi()) {
        if (inScope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;
        outDataSize = sizeof(CFArrayRef);
        outWritable = false;  // read: host queries the output name list
        return noErr;
    }
    if (inID == kAudioUnitProperty_MIDIOutputCallback &&
        plugin_produces_midi()) {
        if (inScope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;
        outDataSize = sizeof(AUMIDIOutputCallbackStruct);
        outWritable = true;  // write: host installs the delivery callback
        return noErr;
    }
    // Per-value string conversion for CONTINUOUS parameters. The host passes
    // the target ParamID inside the in/out struct (not via inElement), so we
    // advertise support at global scope and validate the specific parameter in
    // GetProperty. Read-only from the host's perspective.
    if (inID == kAudioUnitProperty_ParameterStringFromValue) {
        if (inScope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;
        outDataSize = sizeof(AudioUnitParameterStringFromValue);
        outWritable = false;
        return noErr;
    }
    if (inID == kAudioUnitProperty_ParameterValueFromString) {
        if (inScope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;
        outDataSize = sizeof(AudioUnitParameterValueFromString);
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
    if (inID == kAudioUnitProperty_MIDIOutputCallbackInfo &&
        plugin_produces_midi()) {
        if (inScope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;
        if (!outData) return kAudioUnitErr_InvalidProperty;
        // One MIDI output stream named after the plugin. The host owns the array
        // and its element (it releases them), so create with +1 retain.
        CFStringRef name = CFStringCreateWithCString(
            kCFAllocatorDefault,
            processor_ ? processor_->descriptor().name.c_str() : "MIDI Out",
            kCFStringEncodingUTF8);
        CFStringRef values[1] = {name};
        CFArrayRef array = CFArrayCreate(kCFAllocatorDefault,
                                         reinterpret_cast<const void**>(values),
                                         1, &kCFTypeArrayCallBacks);
        if (name) CFRelease(name);  // the array retained it
        *static_cast<CFArrayRef*>(outData) = array;
        return noErr;
    }
    if (inID == kAudioUnitProperty_MIDIOutputCallback &&
        plugin_produces_midi()) {
        if (inScope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;
        if (!outData) return kAudioUnitErr_InvalidProperty;
        // Reflect the currently published pair (acquire-load the snapshot).
        auto* out = static_cast<AUMIDIOutputCallbackStruct*>(outData);
        const auto* pair =
            midi_output_callback_.load(std::memory_order_acquire);
        if (pair) {
            out->midiOutputCallback = pair->callback;
            out->userData = pair->user_data;
        } else {
            out->midiOutputCallback = nullptr;
            out->userData = nullptr;
        }
        return noErr;
    }
    // Continuous-parameter display: value -> string. The host owns and releases
    // outString, so create it with a +1 retain. inValue == nullptr means "use
    // the parameter's current value" per the AU convention.
    if (inID == kAudioUnitProperty_ParameterStringFromValue) {
        if (inScope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;
        if (!outData) return kAudioUnitErr_InvalidProperty;
        auto* sfv = static_cast<AudioUnitParameterStringFromValue*>(outData);
        const auto* param = store_.info(static_cast<state::ParamID>(sfv->inParamID));
        if (!param) return kAudioUnitErr_InvalidPropertyValue;
        const float value = sfv->inValue
            ? static_cast<float>(*sfv->inValue)
            : store_.get_value(static_cast<state::ParamID>(sfv->inParamID));
        const std::string text = format_parameter_text(*param, value);
        sfv->outString = CFStringCreateWithCString(
            kCFAllocatorDefault, text.c_str(), kCFStringEncodingUTF8);
        if (!sfv->outString) return kAudioUnitErr_InvalidPropertyValue;
        return noErr;
    }
    // Continuous-parameter text entry: string -> value.
    if (inID == kAudioUnitProperty_ParameterValueFromString) {
        if (inScope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;
        if (!outData) return kAudioUnitErr_InvalidProperty;
        auto* vfs = static_cast<AudioUnitParameterValueFromString*>(outData);
        const auto* param = store_.info(static_cast<state::ParamID>(vfs->inParamID));
        if (!param || !vfs->inString)
            return kAudioUnitErr_InvalidPropertyValue;
        char buf[256] = {0};
        if (!CFStringGetCString(vfs->inString, buf, sizeof(buf),
                                kCFStringEncodingUTF8))
            return kAudioUnitErr_InvalidPropertyValue;
        const auto parsed = parse_parameter_text(*param, buf);
        if (!parsed) return kAudioUnitErr_InvalidPropertyValue;
        vfs->outValue = *parsed;
        return noErr;
    }
    return AUMIDIEffectBase::GetProperty(inID, inScope, inElement, outData);
}

OSStatus PulpAUEffect::SetProperty(AudioUnitPropertyID inID, AudioUnitScope inScope,
                                   AudioUnitElement inElement, const void* inData,
                                   UInt32 inDataSize)
{
    // The host installs (or clears) the MIDI-output delivery callback here on the
    // main thread; the render thread reads it each block. Publish the
    // (callback, userData) pair atomically via a double-buffered slot so the
    // render thread never observes a torn pair (a fresh callback paired with a
    // stale userData). Write the inactive slot, then release-store the pointer to
    // it; the render side acquire-loads a single, internally-consistent pair.
    if (inID == kAudioUnitProperty_MIDIOutputCallback && plugin_produces_midi()) {
        if (inScope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;
        if (!inData || inDataSize < sizeof(AUMIDIOutputCallbackStruct))
            return kAudioUnitErr_InvalidPropertyValue;
        const auto* in = static_cast<const AUMIDIOutputCallbackStruct*>(inData);
        const std::uint8_t slot =
            midi_output_callback_write_slot_.load(std::memory_order_relaxed);
        midi_output_callback_slots_[slot].callback = in->midiOutputCallback;
        midi_output_callback_slots_[slot].user_data = in->userData;
        // Publish the freshly written slot, then flip the write cursor so the
        // next SetProperty writes the other slot (never the one the render thread
        // may still be reading).
        midi_output_callback_.store(&midi_output_callback_slots_[slot],
                                    std::memory_order_release);
        midi_output_callback_write_slot_.store(slot ^ 1,
                                               std::memory_order_relaxed);
        return noErr;
    }
    return AUMIDIEffectBase::SetProperty(inID, inScope, inElement, inData, inDataSize);
}

UInt32 PulpAUEffect::SupportedNumChannels(const AUChannelInfo** outInfo)
{
    if (!processor_) return 0;
    // Fill the per-instance member table so the returned pointer outlives the
    // call (the host reads it after we return) without per-call allocation.
    const UInt32 count =
        build_channel_info(processor_->descriptor(), channel_info_.data());
    if (outInfo) *outInfo = channel_info_.data();
    return count;
}

bool PulpAUEffect::plugin_produces_midi() const noexcept
{
    return processor_ && processor_->descriptor().produces_midi;
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
        // No reconcile state to seed and no Globals→store pull: store_ is the
        // single source of truth and already holds the current values (defaults
        // from define_parameters, or the values RestoreState wrote). Pulling
        // Globals here would clobber a restored preset with construction defaults.
    }

    // Pre-size the per-block MIDI buffers so the render drain loop appends
    // without heap allocation. Capacity-limited: add()/add_sysex_copy() drop
    // past the reserved bound instead of growing the underlying vectors
    // (matches the VST3/CLAP adapters).
    midi_in_.reserve(kMaxEventsPerBlock, kMaxSysexPerBlock, kMaxSysexPayloadBytes);
    midi_out_.reserve(kMaxEventsPerBlock, kMaxSysexPerBlock, kMaxSysexPayloadBytes);
    midi_in_.set_realtime_capacity_limit(true);
    midi_out_.set_realtime_capacity_limit(true);

    // Pre-size the per-channel pointer vectors so the render-time resize() never
    // grows capacity (and thus never allocates) in steady state. The render
    // block resizes to the host-supplied buffer count, which is at most the
    // configured channel count in AU's non-interleaved float model; reserving to
    // it up front turns the first render / reconfig resize into a no-op realloc.
    input_ptrs_.reserve(static_cast<std::size_t>(GetNumberOfChannels()));
    output_ptrs_.reserve(static_cast<std::size_t>(GetNumberOfChannels()));

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
    midi_in_queue_.try_push(ev); // lock-free; dropped if full (flood backstop)
    return noErr;
}

OSStatus PulpAUEffect::HandleSysEx(const UInt8* inData, UInt32 inLength)
{
    if (!inData || inLength == 0) return noErr;
    // Lock-free, bounded copy. AU v2 SysEx carries no per-event sample offset at
    // this SDK layer; it is delivered at block start. SysEx longer than the
    // chunk (rare — most CI/MTC/identity messages are tiny) is truncated.
    SysexChunk chunk;
    chunk.length = static_cast<uint16_t>(std::min<UInt32>(
        inLength, static_cast<UInt32>(chunk.bytes.size())));
    std::memcpy(chunk.bytes.data(), inData, chunk.length);
    sysex_in_queue_.try_push(chunk);
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

    // Flush denormals to zero for the whole audio-callback body so quiet tails
    // in recursive filter/reverb/feedback state can't stall the host's audio
    // thread, then restore its prior FP mode on scope exit. See
    // docs/guides/dsp-threading.md "Numeric mode".
    pulp::signal::ScopedFlushDenormals flush_denormals;

    // Max-frames contract guard (generic — protects EVERY Pulp AU plugin). The
    // Processor and all its scratch buffers were sized in prepare() to
    // GetMaxFramesPerSlice(). A render larger than that would overflow them and
    // corrupt DSP state (silence until re-init). The canonical AU response is
    // to reject the render; well-behaved hosts never exceed the advertised max,
    // so this never fires in normal playback. This also satisfies auval's
    // "Bad Max Frames — Render should fail" contract test.
    if (inFramesToProcess > GetMaxFramesPerSlice()) {
        return kAudioUnitErr_TooManyFramesToProcess;
    }

    // No host↔plugin parameter reconcile here any more. GetParameter/SetParameter
    // are overridden to read/write store_ directly (single source of truth), so
    // the host's parameter value IS the store value: host automation already
    // landed in the store via SetParameter, and process() reads it below. This
    // the render thread neither pulls, pushes, nor notifies host parameters,
    // which removes the snap-back and the gesture-release render stall the
    // per-block reconcile + on-thread host writes caused.

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

    // When the Bypass param is engaged, copy main input to main output
    // (null-guarded), zero any output channel without a matching input, and
    // skip the Processor. The value was pulled into the store from
    // GetParameter() above.
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
        // Drain and discard any MIDI queued by HandleMIDIEvent/HandleSysEx
        // while bypassed. Otherwise the queue grows for the whole bypass
        // window and floods the processor with stale notes/CCs the instant
        // bypass turns off. A bypassed plugin is a wire, so inbound MIDI is
        // dropped with the block.
        while (midi_in_queue_.try_pop()) {}
        while (sysex_in_queue_.try_pop()) {}
        // Trigger reset is a single-exit invariant: settle Reset/trigger params
        // even on the bypass short-circuit, so a panic/reset raised while
        // bypassed clears this block instead of firing on the next active one.
        // GetParameter reads the store directly, so the host sees it settled.
        store_.reset_triggers_rt();
        return noErr;
    }

    audio::BufferView<const float> input_view(
        input_ptrs_.data(), in_channels, inFramesToProcess);
    audio::BufferView<float> output_view(
        output_ptrs_.data(), out_channels, inFramesToProcess);

    // Reuse the member MIDI buffers (pre-reserved + capacity-limited in
    // Initialize); reset them rather than constructing new ones each block.
    // clear() empties the event list; clear_sysex() recycles the pooled sysex
    // payloads so last block's sysex does not leak into this one.
    midi::MidiBuffer& midi_in = midi_in_;
    midi::MidiBuffer& midi_out = midi_out_;
    midi_in.clear();
    midi_in.clear_sysex();
    midi_out.clear();
    midi_out.clear_sysex();
    // Drain the lock-free MIDI queues filled by HandleMIDIEvent / HandleSysEx.
    // Wait-free on the audio thread — no mutex. aumf-typed effects receive MIDI
    // here; aufx-typed effects simply find the queues empty. add_sysex_copy
    // (not add_sysex(vector)) copies into the buffer's pre-reserved realtime
    // payload pool instead of allocating a fresh heap vector per message.
    while (auto ev = midi_in_queue_.try_pop())
        midi_in.add(*ev);
    while (auto sx = sysex_in_queue_.try_pop())
        midi_in.add_sysex_copy(sx->bytes.data(), sx->length,
                               /*sample_offset=*/0);
    midi_in.sort();

    ProcessContext ctx = make_render_process_context(
        GetSampleRate(), static_cast<int>(inFramesToProcess));

    apply_host_callbacks_to_process_context(ctx, *this, playhead_prev_);

    // AU v2 has no scheduled-parameter event source, so this queue is empty
    // and host params flow via store_ as before. Set it anyway so a Processor
    // always sees a non-null queue. Only the process call is wrapped in
    // ScopedNoAlloc. The MIDI drain above no longer allocates (the buffers are
    // member buffers reset with clear()/clear_sysex(), and SysEx is copied into
    // the pre-reserved payload pool). The input_ptrs_/output_ptrs_ resize() in
    // the preamble is a no-op realloc in steady state — Initialize() reserves
    // both vectors to the channel count — so the only path that could still
    // allocate is a host that grows the buffer count beyond the configured
    // channels, which the AU non-interleaved float model does not do.
    param_events_.clear();
    processor_->set_param_events(&param_events_);

    // Input bus views: the main input (index 0), plus — when the descriptor
    // declares a sidechain input bus — an INACTIVE Sidechain view (index 1).
    // AUEffectBase pulls only the main input element, so the sidechain view stays
    // inactive/null: Processor::sidechain_input() returns nullptr gracefully
    // rather than exposing a bus the stock AU-effect render path cannot feed.
    // For the common single-input effect this is exactly one Main view, so the
    // bus->buffer mapping and behaviour are unchanged. Bus names come from the
    // cached descriptor (build_input_bus_infos), which owns the strings.
    std::array<ProcessBusBufferInfo, 2> in_infos{};
    const std::size_t n_in =
        build_input_bus_infos(descriptor_, in_infos.data(), in_infos.size());
    in_infos[0].declared_channels = static_cast<int>(in_channels);
    in_infos[0].active = input_view.num_channels() > 0;
    std::array<ProcessBusBufferView<const float>, 2> input_buses{};
    input_buses[0] = {.info = in_infos[0], .buffer = input_view};
    for (std::size_t i = 1; i < n_in; ++i) {
        input_buses[i] = {.info = in_infos[i],
                          .buffer = audio::BufferView<const float>{}};
    }

    std::array<ProcessBusBufferView<float>, 1> output_buses{{
        {
            .info = {
                .name = descriptor_.output_buses.empty()
                            ? std::string_view{"Audio Out"}
                            : std::string_view{descriptor_.output_buses[0].name},
                .index = 0,
                .direction = BusDirection::Output,
                .role = BusRole::Main,
                .declared_channels = static_cast<int>(out_channels),
                .optional = false,
                .active = output_view.num_channels() > 0,
            },
            .buffer = output_view,
        },
    }};
    ProcessBuffers process_buffers{
        ProcessBusBufferSet<const float>{std::span(input_buses.data(), n_in)},
        ProcessBusBufferSet<float>{std::span(output_buses)},
    };
    {
        pulp::runtime::ScopedNoAlloc no_alloc_guard;
        processor_->process(process_buffers, midi_in, midi_out, ctx);
    }

    // The host ORs kAudioUnitRenderAction_OutputIsSilence into ioActionFlags via
    // AUInputElement::PullInput whenever it renders silence upstream, and
    // AUEffectBase::Render never clears it afterwards. Overriding
    // ProcessBufferLists bypasses the stock implementation, which is the only
    // place the base class would have cleared the bit on behalf of a kernel that
    // produced output. A Processor may synthesize output from silence — a
    // generator, an oscillator, a DC/control-voltage source, a reverb tail — and
    // the adapter cannot know whether this one did, so the flag must not survive
    // an active render. Leaving it set hands the host a full buffer labelled
    // silent, and a host that honours the label substitutes digital silence.
    //
    // The bypass path above returns early and deliberately leaves the flag
    // alone: there the plugin really is a wire, so upstream silence is still
    // silence downstream.
    ioActionFlags &= ~kAudioUnitRenderAction_OutputIsSilence;

    // Return trigger / momentary params (panic, reset, tap) to their default
    // now that the Processor has observed this block. GetParameter reads the
    // store directly, so the host sees the control settle back automatically.
    store_.reset_triggers_rt();

    // Deliver any MIDI the Processor emitted this block to the host via the
    // installed kAudioUnitProperty_MIDIOutputCallback. Outside the ScopedNoAlloc
    // scope above because the callback is host code we don't control; the packet
    // list itself is built into pre-reserved storage (no allocation). The
    // (callback, userData) pair is read via a single atomic acquire-load so a
    // concurrent SetProperty on the main thread can never hand us a torn pair.
    // When no callback is installed (host hasn't wired the MIDI output, or this
    // is a plain audio effect), this is a cheap no-op. Packet timestamps are
    // sample offsets within the block, clamped to it; the host callback receives
    // the current render time as its base, the documented contract.
    const auto* midi_cb =
        midi_output_callback_.load(std::memory_order_acquire);
    if (midi_cb != nullptr && midi_cb->callback != nullptr &&
        (midi_out.size() > 0 || midi_out.sysex_size() > 0)) {
        const MIDIPacketList* list =
            midi_out_packet_builder_.build(midi_out, inFramesToProcess);
        if (list != nullptr) {
            midi_cb->callback(midi_cb->user_data,
                              &CurrentRenderTime(),
                              /*midiOutNum=*/0,
                              list);
        }
    }

    // No plugin→host parameter diff/push here. Because GetParameter reads the
    // store directly, any value the Processor wrote during process() is already
    // what the host will read — no Globals copy to update. (Live host-recording
    // of *processor-driven* parameter modulation, which needs an explicit change
    // notification, would be done from a main-thread pump, never a render-thread
    // notify; no current plugin drives parameters from process().)

    // Push latency / tail change notifications the processor flagged during
    // process(). AU v2 hosts watch kAudioUnitProperty_Latency and
    // kAudioUnitProperty_TailTime via PropertyListeners; PropertyChanged is
    // the canonical SDK call that wakes them. Safe to call from the render
    // callback because the AU SDK marshals through the host's listener queue.
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
    const auto tail = processor_->descriptor().tail_samples;
    if (tail <= 0) return tail_samples_to_seconds(tail, 0.0);
    return tail_samples_to_seconds(tail, GetSampleRate());
}

Float64 PulpAUEffect::GetLatency()
{
    if (!processor_) return 0.0;
    // Route the non-negative latency clamp through host-quirk policy so
    // disabling host quirks reports raw latency too.
    int latency = reported_latency_samples(processor_->latency_samples(), host_quirks_);
    return GetSampleRate() > 0 ? static_cast<Float64>(latency) / GetSampleRate() : 0.0;
}

} // namespace pulp::format::au
