// Audio Unit v2 MIDI-processor adapter for Pulp.
// Component type `aumi` (kAudioUnitType_MIDIProcessor): MIDI in, MIDI out, no
// audio processing. Built on the AudioUnitSDK's MusicDeviceBase — see the class
// comment in au_v2_midi_processor.hpp for why that base, that entry factory,
// and that element shape.

#include <AudioUnitSDK/AUOutputElement.h>
#include <AudioUnitSDK/AUPlugInDispatch.h>
#include <AudioToolbox/AudioToolbox.h>  // kAudioUnitProperty_CocoaUI, AudioUnitCocoaViewInfo

#include <pulp/format/au_v2_midi_processor.hpp>
#include <pulp/format/registry.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>
#include <pulp/signal/scoped_flush_denormals.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <span>

namespace pulp::format::au {

namespace {

// The `aumi` audio output element exists only so the host has something to
// render — rendering is what drives the MIDI processing. It carries no signal.
constexpr UInt32 kSilentOutputElements = 1;

}  // namespace

PulpAUMidiProcessor::PulpAUMidiProcessor(AudioComponentInstance ci)
    : PulpAUMidiProcessor(ci, registered_factory())
{
}

PulpAUMidiProcessor::PulpAUMidiProcessor(AudioComponentInstance ci,
                                         ProcessorFactory factory)
    : MusicDeviceBase(ci, /*numInputs=*/0,
                      /*numOutputs=*/kSilentOutputElements, /*numGroups=*/0)
{
    if (!factory) return;
    processor_ = factory();
    if (!processor_) return;

    processor_->set_state_store(&store_);
    processor_->define_parameters(store_);
    // Cache the immutable descriptor once: descriptor() returns by value and
    // allocates its std::string members, so the render path must never call it.
    descriptor_ = processor_->descriptor();

    const auto host_info = detect_host_info();
    host_quirks_ = resolved_quirks(host_info.type, host_info.version);

    // Honour a plugin-DECLARED bypass control. Nothing is synthesized here (see
    // the bypass_param_id_ member comment).
    for (const auto& p : store_.all_params()) {
        if (state::is_bypass_param(p)) {
            bypass_param_id_ = p.id;
            break;
        }
    }

    for (const auto& param : store_.all_params()) {
        Globals()->SetParameter(static_cast<AudioUnitParameterID>(param.id),
                                param.range.default_value);
    }

    // Editor -> host parameter path: gesture brackets + value-change
    // notification, wired once through the shared AU v2 bridge.
    wire_host_parameter_bridge(store_, GetComponentInstance(),
                                       ui_push_listener_);
}

OSStatus PulpAUMidiProcessor::GetParameterList(AudioUnitScope inScope,
                                               AudioUnitParameterID* outParameterList,
                                               UInt32& outNumParameters)
{
    return fill_parameter_list(store_, inScope, outParameterList,
                                       outNumParameters);
}

OSStatus PulpAUMidiProcessor::GetParameterInfo(AudioUnitScope inScope,
                                               AudioUnitParameterID inParameterID,
                                               AudioUnitParameterInfo& outParameterInfo)
{
    return fill_parameter_info(store_, inScope, inParameterID,
                               outParameterInfo,
                               /*advertise_value_strings=*/true);
}

OSStatus PulpAUMidiProcessor::GetParameterValueStrings(AudioUnitScope inScope,
                                                       AudioUnitParameterID inParameterID,
                                                       CFArrayRef* outStrings)
{
    return fill_parameter_value_strings(store_, inScope, inParameterID,
                                                outStrings);
}

OSStatus PulpAUMidiProcessor::GetParameter(AudioUnitParameterID inID,
                                           AudioUnitScope inScope,
                                           AudioUnitElement inElement,
                                           Float32& outValue)
{
    if (inScope == kAudioUnitScope_Global &&
        store_.info(static_cast<state::ParamID>(inID)) != nullptr) {
        outValue = store_.get_value(static_cast<state::ParamID>(inID));
        return noErr;
    }
    return MusicDeviceBase::GetParameter(inID, inScope, inElement, outValue);
}

OSStatus PulpAUMidiProcessor::SetParameter(AudioUnitParameterID inID,
                                           AudioUnitScope inScope,
                                           AudioUnitElement inElement,
                                           Float32 inValue,
                                           UInt32 inBufferOffsetInFrames)
{
    if (inScope == kAudioUnitScope_Global &&
        store_.info(static_cast<state::ParamID>(inID)) != nullptr) {
        ScopedHostParamWrite host_write;
        store_.set_value_rt(static_cast<state::ParamID>(inID), inValue);
        return noErr;
    }
    return MusicDeviceBase::SetParameter(inID, inScope, inElement, inValue,
                                         inBufferOffsetInFrames);
}

OSStatus PulpAUMidiProcessor::GetPropertyInfo(AudioUnitPropertyID inID,
                                              AudioUnitScope inScope,
                                              AudioUnitElement inElement,
                                              UInt32& outDataSize,
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
    // MIDI output is definitional for an `aumi` — always advertised, never
    // gated on descriptor().produces_midi. The AudioUnitSDK implements neither
    // property, so both are handled here.
    if (inID == kAudioUnitProperty_MIDIOutputCallbackInfo) {
        if (inScope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;
        outDataSize = sizeof(CFArrayRef);
        outWritable = false;  // read: host queries the output name list
        return noErr;
    }
    if (inID == kAudioUnitProperty_MIDIOutputCallback) {
        if (inScope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;
        outDataSize = sizeof(AUMIDIOutputCallbackStruct);
        outWritable = true;  // write: host installs the delivery callback
        return noErr;
    }
    // Per-value string conversion. The host passes the target ParamID inside
    // the in/out struct (not via inElement), so support is advertised at global
    // scope and the specific parameter is validated in GetProperty.
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
    return MusicDeviceBase::GetPropertyInfo(inID, inScope, inElement,
                                            outDataSize, outWritable);
}

OSStatus PulpAUMidiProcessor::GetProperty(AudioUnitPropertyID inID,
                                          AudioUnitScope inScope,
                                          AudioUnitElement inElement,
                                          void* outData)
{
    if (inID == kPulpEditorContextProperty) {
        if (inScope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;
        if (inElement != 0) return kAudioUnitErr_InvalidElement;
        if (!outData) return kAudioUnitErr_InvalidProperty;
        auto* ctx = static_cast<PulpEditorContext*>(outData);
        ctx->processor = processor_.get();
        ctx->store = &store_;
        ctx->owner_alive = owner_alive_.capture();
        return noErr;
    }
    if (inID == kAudioUnitProperty_CocoaUI && g_cocoa_view_info_filler) {
        if (inScope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;
        if (!outData) return kAudioUnitErr_InvalidProperty;
        return g_cocoa_view_info_filler(outData) ? noErr
                                                 : kAudioUnitErr_InvalidProperty;
    }
    if (inID == kAudioUnitProperty_MIDIOutputCallbackInfo) {
        if (inScope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;
        if (!outData) return kAudioUnitErr_InvalidProperty;
        *static_cast<CFArrayRef*>(outData) =
            make_midi_output_names(descriptor_.name.c_str());
        return noErr;
    }
    if (inID == kAudioUnitProperty_MIDIOutputCallback) {
        if (inScope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;
        return midi_output_callback_.reflect(outData);
    }
    if (inID == kAudioUnitProperty_ParameterStringFromValue) {
        if (inScope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;
        return parameter_string_from_value(store_, outData);
    }
    if (inID == kAudioUnitProperty_ParameterValueFromString) {
        if (inScope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;
        return parameter_value_from_string(store_, outData);
    }
    return MusicDeviceBase::GetProperty(inID, inScope, inElement, outData);
}

OSStatus PulpAUMidiProcessor::SetProperty(AudioUnitPropertyID inID,
                                          AudioUnitScope inScope,
                                          AudioUnitElement inElement,
                                          const void* inData, UInt32 inDataSize)
{
    if (inID == kAudioUnitProperty_MIDIOutputCallback) {
        if (inScope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;
        return midi_output_callback_.publish(inData, inDataSize);
    }
    return MusicDeviceBase::SetProperty(inID, inScope, inElement, inData,
                                        inDataSize);
}

OSStatus PulpAUMidiProcessor::Initialize()
{
    auto result = MusicDeviceBase::Initialize();
    if (result != noErr) return result;

    if (processor_) {
        PrepareContext ctx;
        ctx.sample_rate = GetOutput(0)->GetStreamFormat().mSampleRate;
        ctx.max_buffer_size = GetMaxFramesPerSlice();
        // A MIDI processor has no audio buses; the Processor is prepared with
        // zero channels so any audio scratch it allocates is sized honestly.
        ctx.input_channels = 0;
        ctx.output_channels = 0;
        processor_->prepare(ctx);
    }

    // Pre-size the per-block MIDI buffers so the render drain and the
    // Processor's own appends never allocate. Capacity-limited: appends past
    // the reserved bound drop instead of growing the underlying vectors.
    midi_in_.reserve(kMaxEventsPerBlock, kMaxSysexPerBlock, kMaxSysexPayloadBytes);
    midi_out_.reserve(kMaxEventsPerBlock, kMaxSysexPerBlock, kMaxSysexPayloadBytes);
    midi_in_.set_realtime_capacity_limit(true);
    midi_out_.set_realtime_capacity_limit(true);

    runtime::log_info("AU v2 MIDI processor: initialized at {} Hz",
                      GetOutput(0)->GetStreamFormat().mSampleRate);
    return noErr;
}

void PulpAUMidiProcessor::Cleanup()
{
    if (processor_) processor_->release();
    MusicDeviceBase::Cleanup();
}

bool PulpAUMidiProcessor::StreamFormatWritable(AudioUnitScope scope,
                                               AudioUnitElement element)
{
    (void)element;
    return scope == kAudioUnitScope_Output;
}

bool PulpAUMidiProcessor::CanScheduleParameters() const noexcept
{
    return true;
}

OSStatus PulpAUMidiProcessor::HandleMIDIEvent(UInt8 inStatus, UInt8 inChannel,
                                              UInt8 inData1, UInt8 inData2,
                                              UInt32 inStartFrame)
{
    auto ev = decode_midi_event(static_cast<uint8_t>(inStatus),
                                static_cast<uint8_t>(inChannel),
                                static_cast<uint8_t>(inData1),
                                static_cast<uint8_t>(inData2));
    ev.sample_offset = static_cast<int32_t>(inStartFrame);
    midi_in_queue_.try_push(ev);  // lock-free; dropped if full (flood backstop)
    return noErr;
}

OSStatus PulpAUMidiProcessor::HandleSysEx(const UInt8* inData, UInt32 inLength)
{
    if (!inData || inLength == 0) return noErr;
    // Lock-free, bounded copy. AU v2 SysEx carries no per-event sample offset
    // at this SDK layer; it is delivered at block start. A payload longer than
    // the chunk is truncated.
    SysexChunk chunk;
    chunk.length = static_cast<uint16_t>(
        std::min<UInt32>(inLength, static_cast<UInt32>(chunk.bytes.size())));
    std::memcpy(chunk.bytes.data(), inData, chunk.length);
    sysex_in_queue_.try_push(chunk);
    return noErr;
}

bool PulpAUMidiProcessor::bypass_engaged() const noexcept
{
    return bypass_param_id_ != 0 && store_.get_value(bypass_param_id_) >= 0.5f;
}

OSStatus PulpAUMidiProcessor::Render(AudioUnitRenderActionFlags& ioActionFlags,
                                     const AudioTimeStamp& inTimeStamp,
                                     UInt32 inNumberFrames)
{
    (void)inTimeStamp;

    // The Processor and its scratch were sized in prepare() to
    // GetMaxFramesPerSlice(). Rejecting an oversized render is the canonical AU
    // response; well-behaved hosts never exceed the advertised maximum.
    if (inNumberFrames > GetMaxFramesPerSlice())
        return kAudioUnitErr_TooManyFramesToProcess;

    // The output element carries no musical signal. Zero it so the host never
    // reads uninitialised memory, and label the block silent so a host that
    // honours the flag can skip it — an `aumi` genuinely produces silence.
    AudioBufferList& out_bl = GetOutput(0)->PrepareBuffer(inNumberFrames);
    for (UInt32 c = 0; c < out_bl.mNumberBuffers; ++c) {
        if (auto* data = static_cast<float*>(out_bl.mBuffers[c].mData))
            std::memset(data, 0, sizeof(float) * inNumberFrames);
    }
    ioActionFlags |= kAudioUnitRenderAction_OutputIsSilence;

    if (!processor_) return noErr;

    // Flush denormals for the whole audio-callback body, restoring the host's
    // prior FP mode on scope exit. See docs/guides/dsp-threading.md.
    pulp::signal::ScopedFlushDenormals flush_denormals;

    midi_in_.clear();
    midi_in_.clear_sysex();
    midi_out_.clear();
    midi_out_.clear_sysex();

    // Drain the lock-free queues HandleMIDIEvent / HandleSysEx filled.
    // Wait-free on the audio thread. add_sysex_copy (not add_sysex(vector))
    // copies into the buffer's pre-reserved realtime payload pool rather than
    // allocating a fresh heap vector per message.
    while (auto ev = midi_in_queue_.try_pop())
        midi_in_.add(*ev);
    while (auto sx = sysex_in_queue_.try_pop())
        midi_in_.add_sysex_copy(sx->bytes.data(), sx->length,
                                /*sample_offset=*/0);
    midi_in_.sort();

    if (bypass_engaged()) {
        // A bypassed MIDI processor is a WIRE: the inbound stream is copied to
        // the output untouched and process() is skipped. Discarding the stream
        // (the audio-effect bypass shape, where the plugin's own output is what
        // gets suppressed) would silence every instrument downstream of the
        // bypassed slot.
        for (const auto& ev : midi_in_) midi_out_.add(ev);
        for (const auto& sx : midi_in_.sysex()) {
            midi_out_.add_sysex_copy(sx.data.data(), sx.data.size(),
                                     sx.sample_offset, sx.timestamp);
        }
    } else {
        ProcessContext ctx = make_render_process_context(
            GetOutput(0)->GetStreamFormat().mSampleRate,
            static_cast<int>(inNumberFrames));
        apply_host_callbacks_to_process_context(ctx, *this, playhead_prev_);

        param_events_.clear();
        processor_->set_param_events(&param_events_);

        // An `aumi` exposes NO audio buses to the Processor: both views are
        // empty and zero-channel. The main OUTPUT bus is still marked ACTIVE,
        // which is load-bearing — `Processor::process(ProcessBuffers&)`'s
        // default projection returns early when `main_output()` is null, so an
        // inactive output bus would mean the plugin's `process()` never runs at
        // all and the MIDI effect would silently do nothing.
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
                .buffer = audio::BufferView<const float>{},
            },
        }};
        std::array<ProcessBusBufferView<float>, 1> output_buses{{
            {
                .info = {
                    .name = "Audio Out",
                    .index = 0,
                    .direction = BusDirection::Output,
                    .role = BusRole::Main,
                    .declared_channels = 0,
                    .optional = true,
                    .active = true,
                },
                .buffer = audio::BufferView<float>{},
            },
        }};
        ProcessBuffers process_buffers{
            ProcessBusBufferSet<const float>{std::span(input_buses)},
            ProcessBusBufferSet<float>{std::span(output_buses)},
        };
        {
            pulp::runtime::ScopedNoAlloc no_alloc_guard;
            processor_->process(process_buffers, midi_in_, midi_out_, ctx);
        }
    }

    // Return trigger / momentary params (panic, reset, tap) to their default
    // now that the Processor has observed this block. Done on BOTH paths so a
    // panic raised while bypassed settles with this block instead of firing on
    // the next active one. GetParameter reads the store directly, so the host
    // sees the control settle automatically.
    store_.reset_triggers_rt();

    // Deliver the block's outbound MIDI to the host. Outside any ScopedNoAlloc
    // scope because the callback is host code we don't control; the packet list
    // itself is built into pre-reserved storage. The (callback, userData) pair
    // is read with a single acquire-load, so a concurrent SetProperty on the
    // main thread can never hand us a torn pair. Packet timestamps are sample
    // offsets within the block, clamped to it; the host callback receives the
    // current render time as its base.
    const auto* midi_cb = midi_output_callback_.load();
    if (midi_cb != nullptr && midi_cb->callback != nullptr &&
        (midi_out_.size() > 0 || midi_out_.sysex_size() > 0)) {
        const MIDIPacketList* list =
            midi_out_packet_builder_.build(midi_out_, inNumberFrames);
        if (list != nullptr) {
            midi_cb->callback(midi_cb->user_data, &CurrentRenderTime(),
                              /*midiOutNum=*/0, list);
        }
    }

    if (processor_->consume_latency_changed_flag())
        PropertyChanged(kAudioUnitProperty_Latency, kAudioUnitScope_Global, 0);
    // A MIDI processor has no audio tail (SupportsTail() stays false), so a
    // flagged tail change has nothing to republish — consume it anyway so the
    // flag does not stay latched forever.
    (void)processor_->consume_tail_changed_flag();

    return noErr;
}

OSStatus PulpAUMidiProcessor::SaveState(CFPropertyListRef* outData)
{
    auto result = MusicDeviceBase::SaveState(outData);
    if (result != noErr) return result;
    if (!processor_) return kAudioUnitErr_Uninitialized;
    return save_pulp_state(store_, *processor_, outData);
}

OSStatus PulpAUMidiProcessor::RestoreState(CFPropertyListRef plist)
{
    auto result = MusicDeviceBase::RestoreState(plist);
    if (result != noErr) return result;
    if (!processor_) return kAudioUnitErr_Uninitialized;
    // store_ is the source of truth (GetParameter reads it) — no Globals mirror
    // to update.
    return restore_pulp_state(store_, *processor_, plist);
}

Float64 PulpAUMidiProcessor::GetLatency()
{
    if (!processor_) return 0.0;
    // A MIDI processor that delays events (a quantizer, a humanizer with
    // lookahead) reports it here so the host can compensate. Clamped
    // non-negative unless host-quirk policy filters that out.
    const int latency =
        reported_latency_samples(processor_->latency_samples(), host_quirks_);
    double sr = 0.0;
    try {
        sr = GetOutput(0)->GetStreamFormat().mSampleRate;
    } catch (...) {
        sr = 0.0;
    }
    return sr > 0.0 ? static_cast<Float64>(latency) / sr : 0.0;
}

} // namespace pulp::format::au
