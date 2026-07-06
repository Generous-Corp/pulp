// Audio Unit v2 instrument adapter for Pulp
// Uses Apple's AudioUnitSDK MusicDeviceBase for AU instruments (aumu type)
// Handles MIDI note events and renders audio output with no audio input

#include <AudioUnitSDK/AUPlugInDispatch.h>
#include <AudioUnitSDK/AUOutputElement.h>
#include <AudioToolbox/AudioToolbox.h>  // kAudioUnitProperty_CocoaUI, AudioUnitCocoaViewInfo
#include <mach/mach_time.h>

#include <pulp/format/au_v2_instrument.hpp>
#include <pulp/format/au_v2_adapter.hpp>  // kPulpEditorContextProperty, PulpEditorContext, fill_cocoa_view_info
#include <pulp/format/detail/playhead_diff.hpp>
#include <pulp/format/plugin_state_io.hpp>
#include <pulp/format/registry.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/signal/scoped_flush_denormals.hpp>

#include <array>
#include <cstring>

namespace pulp::format::au {

// Set while the HOST writes a parameter (SetParameter); the store's UI-push
// listener (fires inline on the same thread) skips echoing it back.
namespace { thread_local bool g_host_writing_param = false; }

PulpAUInstrument::PulpAUInstrument(AudioComponentInstance ci)
    : MusicDeviceBase(ci, /*numInputs=*/0, /*numOutputs=*/1, /*numGroups=*/0)
{
    auto factory = registered_factory();
    if (factory) {
        processor_ = factory();
        if (processor_) {
            processor_->set_state_store(&store_);
            processor_->define_parameters(store_);

            // Resolve host accommodations once so GetLatency() can apply
            // the same policy-gated clamp as the effect adapter.
            const auto host_info = detect_host_info();
            host_quirks_ = resolved_quirks(host_info.type, host_info.version);

            for (const auto& param : store_.all_params()) {
                Globals()->SetParameter(
                    static_cast<AudioUnitParameterID>(param.id),
                    param.range.default_value);
            }

            // Gesture begin/end → host Begin/EndParameterChangeGesture, so the
            // editor's drag start/stop bracket a clean automation pass (Touch/
            // Latch). Mirrors the AU v2 effect adapter — was missing here, so the
            // instrument recorded values without gesture boundaries.
            store_.set_gesture_callbacks(
                [this](state::ParamID id) {
                    AudioUnitEvent event;
                    std::memset(&event, 0, sizeof(event));
                    event.mEventType = kAudioUnitEvent_BeginParameterChangeGesture;
                    event.mArgument.mParameter.mAudioUnit = GetComponentInstance();
                    event.mArgument.mParameter.mParameterID =
                        static_cast<AudioUnitParameterID>(id);
                    event.mArgument.mParameter.mScope = kAudioUnitScope_Global;
                    event.mArgument.mParameter.mElement = 0;
                    AUEventListenerNotify(nullptr, nullptr, &event);
                },
                [this](state::ParamID id) {
                    AudioUnitEvent event;
                    std::memset(&event, 0, sizeof(event));
                    event.mEventType = kAudioUnitEvent_EndParameterChangeGesture;
                    event.mArgument.mParameter.mAudioUnit = GetComponentInstance();
                    event.mArgument.mParameter.mParameterID =
                        static_cast<AudioUnitParameterID>(id);
                    event.mArgument.mParameter.mScope = kAudioUnitScope_Global;
                    event.mArgument.mParameter.mElement = 0;
                    AUEventListenerNotify(nullptr, nullptr, &event);
                });

            // UI -> host parameter notification, on the editing (main) thread.
            // An editor ParameterEdit fires this; it notifies the host so it
            // re-reads via GetParameter and records automation. NOT
            // AudioUnitSetParameter-on-self and NEVER from the render thread.
            ui_push_listener_ = store_.add_listener(
                [this](state::ParamID id, float /*value*/) {
                    if (g_host_writing_param) return;
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
        }
    }
}

OSStatus PulpAUInstrument::GetParameter(AudioUnitParameterID inID,
                                        AudioUnitScope inScope,
                                        AudioUnitElement inElement, Float32& outValue)
{
    if (inScope == kAudioUnitScope_Global &&
        store_.info(static_cast<state::ParamID>(inID)) != nullptr) {
        outValue = store_.get_value(static_cast<state::ParamID>(inID));
        return noErr;
    }
    return MusicDeviceBase::GetParameter(inID, inScope, inElement, outValue);
}

OSStatus PulpAUInstrument::SetParameter(AudioUnitParameterID inID,
                                        AudioUnitScope inScope,
                                        AudioUnitElement inElement, Float32 inValue,
                                        UInt32 inBufferOffsetInFrames)
{
    if (inScope == kAudioUnitScope_Global &&
        store_.info(static_cast<state::ParamID>(inID)) != nullptr) {
        g_host_writing_param = true;
        store_.set_value_rt(static_cast<state::ParamID>(inID), inValue);
        g_host_writing_param = false;
        return noErr;
    }
    return MusicDeviceBase::SetParameter(inID, inScope, inElement, inValue,
                                         inBufferOffsetInFrames);
}

OSStatus PulpAUInstrument::GetParameterList(AudioUnitScope inScope,
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
        for (std::size_t i = 0; i < params.size(); ++i)
            outParameterList[i] = static_cast<AudioUnitParameterID>(params[i].id);
    }
    return noErr;
}

OSStatus PulpAUInstrument::GetParameterInfo(AudioUnitScope inScope,
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

    if (param->unit == "dB")
        outParameterInfo.unit = kAudioUnitParameterUnit_Decibels;
    else if (param->unit == "Hz")
        outParameterInfo.unit = kAudioUnitParameterUnit_Hertz;
    else
        outParameterInfo.unit = kAudioUnitParameterUnit_Generic;

    return noErr;
}

OSStatus PulpAUInstrument::GetPropertyInfo(AudioUnitPropertyID inID, AudioUnitScope inScope,
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
    return MusicDeviceBase::GetPropertyInfo(inID, inScope, inElement, outDataSize, outWritable);
}

OSStatus PulpAUInstrument::GetProperty(AudioUnitPropertyID inID, AudioUnitScope inScope,
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
    return MusicDeviceBase::GetProperty(inID, inScope, inElement, outData);
}

OSStatus PulpAUInstrument::Initialize()
{
    auto result = MusicDeviceBase::Initialize();
    if (result != noErr) return result;

    if (processor_) {
        PrepareContext ctx;
        ctx.sample_rate = GetOutput(0)->GetStreamFormat().mSampleRate;
        ctx.max_buffer_size = GetMaxFramesPerSlice();
        ctx.input_channels = 0;
        ctx.output_channels = static_cast<int>(
            GetOutput(0)->GetStreamFormat().mChannelsPerFrame);
        processor_->prepare(ctx);
        // No Globals->store pull: store_ is the single source of truth (already
        // holds defaults or RestoreState's values). Pulling Globals here would
        // clobber a restored preset with construction defaults.
    }

    runtime::log_info("AU v2 instrument: initialized at {} Hz",
                      GetOutput(0)->GetStreamFormat().mSampleRate);
    return noErr;
}

void PulpAUInstrument::Cleanup()
{
    if (processor_) processor_->release();
    MusicDeviceBase::Cleanup();
}

bool PulpAUInstrument::StreamFormatWritable(AudioUnitScope scope, AudioUnitElement element)
{
    (void)element;
    return scope == kAudioUnitScope_Output || scope == kAudioUnitScope_Input;
}

bool PulpAUInstrument::CanScheduleParameters() const noexcept
{
    return true;
}

OSStatus PulpAUInstrument::HandleNoteOn(UInt8 inChannel, UInt8 inNoteNumber,
                                        UInt8 inVelocity, UInt32 inStartFrame)
{
    auto me = midi::MidiEvent::note_on(inChannel, inNoteNumber, inVelocity);
    me.sample_offset = static_cast<int32_t>(inStartFrame);
    midi_in_queue_.try_push(me); // lock-free; dropped if full (flood backstop)
    return noErr;
}

OSStatus PulpAUInstrument::HandleNoteOff(UInt8 inChannel, UInt8 inNoteNumber,
                                         UInt8 inVelocity, UInt32 inStartFrame)
{
    auto me = midi::MidiEvent::note_off(inChannel, inNoteNumber, inVelocity);
    me.sample_offset = static_cast<int32_t>(inStartFrame);
    midi_in_queue_.try_push(me);
    return noErr;
}

OSStatus PulpAUInstrument::Render(AudioUnitRenderActionFlags& ioActionFlags,
                                  const AudioTimeStamp& inTimeStamp,
                                  UInt32 inNumberFrames)
{
    (void)ioActionFlags;
    (void)inTimeStamp;

    if (!processor_) return noErr;

    // Flush denormals to zero for the whole audio-callback body so quiet tails
    // in recursive filter/reverb/feedback state can't stall the host's audio
    // thread, then restore its prior FP mode on scope exit. See
    // docs/guides/dsp-threading.md "Numeric mode".
    pulp::signal::ScopedFlushDenormals flush_denormals;

    // No Globals->store pull: GetParameter/SetParameter are store-backed, so
    // host automation already landed in the store and process() reads it below.

    auto* output = GetOutput(0);
    AudioBufferList& outBL = output->PrepareBuffer(inNumberFrames);

    UInt32 out_channels = outBL.mNumberBuffers;
    output_ptrs_.resize(out_channels);
    for (UInt32 i = 0; i < out_channels; ++i)
        output_ptrs_[i] = static_cast<float*>(outBL.mBuffers[i].mData);

    audio::BufferView<float> output_view(
        output_ptrs_.data(), out_channels, inNumberFrames);

    audio::BufferView<const float> input_view;

    midi::MidiBuffer midi_in, midi_out;
    while (auto ev = midi_in_queue_.try_pop())  // lock-free drain
        midi_in.add(*ev);
    midi_in.sort();

    ProcessContext ctx = make_render_process_context(
        GetOutput(0)->GetStreamFormat().mSampleRate,
        static_cast<int>(inNumberFrames));

    apply_host_callbacks_to_process_context(ctx, *this, playhead_prev_);

    std::array<ProcessBusBufferView<const float>, 1> input_buses{{
        {
            .info = {
                .name = "Audio In",
                .index = 0,
                .direction = BusDirection::Input,
                .role = BusRole::Main,
                .declared_channels = 0,
                .optional = true,
                .active = false,
            },
            .buffer = input_view,
        },
    }};
    std::array<ProcessBusBufferView<float>, 1> output_buses{{
        {
            .info = {
                .name = "Audio Out",
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
        ProcessBusBufferSet<const float>{std::span(input_buses)},
        ProcessBusBufferSet<float>{std::span(output_buses)},
    };

    processor_->process(process_buffers, midi_in, midi_out, ctx);

    return noErr;
}

OSStatus PulpAUInstrument::SaveState(CFPropertyListRef* outData)
{
    auto result = MusicDeviceBase::SaveState(outData);
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

OSStatus PulpAUInstrument::RestoreState(CFPropertyListRef plist)
{
    auto result = MusicDeviceBase::RestoreState(plist);
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
            // store_ is the source of truth (GetParameter reads it) — no Globals
            // mirror to update.
        }
    }
    return noErr;
}

bool PulpAUInstrument::SupportsTail()
{
    return true;
}

Float64 PulpAUInstrument::GetTailTime()
{
    if (!processor_) return 0.0;
    const auto tail = processor_->descriptor().tail_samples;
    if (tail <= 0) return tail_samples_to_seconds(tail, 0.0);

    double sr = 0.0;
    try {
        sr = GetOutput(0)->GetStreamFormat().mSampleRate;
    } catch (...) {
        sr = 0.0;
    }
    return tail_samples_to_seconds(tail, sr);
}

Float64 PulpAUInstrument::GetLatency()
{
    if (!processor_) return 0.0;
    // Report the processor's latency for host PDC, clamped non-negative
    // unless the host quirk is filtered out. Instruments with lookahead
    // should get the same delay compensation as effects.
    // MusicDeviceBase has no GetSampleRate(); read it from the output
    // stream format like the render path, guarded for pre-config queries.
    int latency = reported_latency_samples(processor_->latency_samples(), host_quirks_);
    double sr = 0.0;
    try {
        sr = GetOutput(0)->GetStreamFormat().mSampleRate;
    } catch (...) {
        sr = 0.0;
    }
    return sr > 0.0 ? static_cast<Float64>(latency) / sr : 0.0;
}

} // namespace pulp::format::au
