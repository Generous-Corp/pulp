// Audio Unit v2 adapter for Pulp
// Uses Apple's AudioUnitSDK (Apache 2.0) for proper AU v2 hosting contract
// Subclasses AUEffectBase and overrides ProcessBufferLists for multi-channel

#include <AudioUnitSDK/AUPlugInDispatch.h>
#include <AudioToolbox/AudioUnitUtilities.h>
#include <AudioToolbox/AudioToolbox.h>  // kAudioUnitProperty_CocoaUI, AudioUnitCocoaViewInfo

#include <pulp/format/au_v2_adapter.hpp>
#include <pulp/format/quirk_apply.hpp>
#include <pulp/format/detail/param_host_sync.hpp>
#include <pulp/format/detail/playhead_diff.hpp>
#include <pulp/format/detail/audio_buffer_list_validation.hpp>
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

            // Editor -> host parameter path: gesture brackets + value-change
            // notification, wired once through the shared AU v2 bridge.
            wire_host_parameter_bridge(store_, GetComponentInstance(),
                                               ui_push_listener_);

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
    return fill_parameter_list(store_, inScope, outParameterList,
                                       outNumParameters);
}

OSStatus PulpAUEffect::GetParameterInfo(AudioUnitScope inScope,
                                        AudioUnitParameterID inParameterID,
                                        AudioUnitParameterInfo& outParameterInfo)
{
    return fill_parameter_info(store_, inScope, inParameterID,
                               outParameterInfo,
                               /*advertise_value_strings=*/true);
}

OSStatus PulpAUEffect::GetParameterValueStrings(AudioUnitScope inScope,
                                                AudioUnitParameterID inParameterID,
                                                CFArrayRef* outStrings)
{
    return fill_parameter_value_strings(store_, inScope, inParameterID,
                                                outStrings);
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
    // listener synchronously, where the host-write guard suppresses the echo
    // back to the host.
    if (inScope == kAudioUnitScope_Global &&
        store_.info(static_cast<state::ParamID>(inID)) != nullptr) {
        ScopedHostParamWrite host_write;
        store_.set_value_rt(static_cast<state::ParamID>(inID), inValue);
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
        ctx->owner_alive = owner_alive_.capture();
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
        // One MIDI output stream named after the plugin.
        *static_cast<CFArrayRef*>(outData) = make_midi_output_names(
            descriptor_.name.c_str());
        return noErr;
    }
    if (inID == kAudioUnitProperty_MIDIOutputCallback &&
        plugin_produces_midi()) {
        if (inScope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;
        // Reflect the currently published pair (acquire-load the snapshot).
        return midi_output_callback_.reflect(outData);
    }
    // Continuous-parameter display: value -> string. The host owns and releases
    // outString, so create it with a +1 retain. inValue == nullptr means "use
    // the parameter's current value" per the AU convention.
    if (inID == kAudioUnitProperty_ParameterStringFromValue) {
        if (inScope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;
        if (!outData) return kAudioUnitErr_InvalidProperty;
        return parameter_string_from_value(store_, outData);
    }
    // Continuous-parameter text entry: string -> value.
    if (inID == kAudioUnitProperty_ParameterValueFromString) {
        if (inScope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;
        if (!outData) return kAudioUnitErr_InvalidProperty;
        return parameter_value_from_string(store_, outData);
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
        return midi_output_callback_.publish(inData, inDataSize);
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

        // Size the bypass dry-delay line to the reported latency so a bypassed
        // block emits `input[n - latency]`, matching the host's PDC on the wet
        // path. Allocates here (off the render thread); a 0 latency leaves it a
        // zero-copy passthrough. Same policy the CLAP/VST3 adapters apply.
        bypass_.prepare(reported_latency_samples(
            processor_->latency_samples(), host_quirks_));
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
        detail::zero_audio_buffer_list(&outBuffer);
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

    const auto& input_format = Input(0).GetStreamFormat();
    const auto& output_format = Output(0).GetStreamFormat();
    const bool input_noninterleaved =
        (input_format.mFormatFlags & kAudioFormatFlagIsNonInterleaved) != 0;
    const bool output_noninterleaved =
        (output_format.mFormatFlags & kAudioFormatFlagIsNonInterleaved) != 0;
    const UInt32 expected_input_buffers = input_noninterleaved
        ? input_format.mChannelsPerFrame : UInt32{1};
    const UInt32 expected_output_buffers = output_noninterleaved
        ? output_format.mChannelsPerFrame : UInt32{1};
    const UInt32 input_channels_per_buffer = input_noninterleaved
        ? UInt32{1} : input_format.mChannelsPerFrame;
    const UInt32 output_channels_per_buffer = output_noninterleaved
        ? UInt32{1} : output_format.mChannelsPerFrame;
    const bool valid_input = detail::audio_buffer_list_shape_matches(
                                 &inBuffer, expected_input_buffers,
                                 input_channels_per_buffer) &&
        detail::audio_buffer_list_has_storage(
            &inBuffer, inFramesToProcess, input_format.mBytesPerFrame);
    const bool valid_output = detail::audio_buffer_list_shape_matches(
                                  &outBuffer, expected_output_buffers,
                                  output_channels_per_buffer) &&
        detail::audio_buffer_list_has_storage(
            &outBuffer, inFramesToProcess, output_format.mBytesPerFrame);
    if (!valid_input || !valid_output) {
        detail::zero_audio_buffer_list(&outBuffer);
        ioActionFlags |= kAudioUnitRenderAction_OutputIsSilence;
        processor_->set_sidechain(nullptr);
        store_.reset_triggers_rt();
        return noErr;
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
        // Copy main input → main output, latency-compensated through the shared
        // dry-delay line so the bypassed dry signal stays aligned with the
        // host's PDC on the wet path (channels beyond the boundary ceiling fall
        // back to an undelayed copy uniformly). Zero any output channel with no
        // matching input.
        boundary::render_bypass_passthrough(
            bypass_, output_ptrs_.data(), static_cast<int>(out_channels),
            input_ptrs_.data(), static_cast<int>(in_channels),
            static_cast<std::uint32_t>(inFramesToProcess));
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
    // bus->buffer mapping and behavior are unchanged. Bus names come from the
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
    // an active render. Leaving it set hands the host a full buffer labeled
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
    const auto* midi_cb = midi_output_callback_.load();
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
    return save_pulp_state(store_, *processor_, outData);
}

OSStatus PulpAUEffect::RestoreState(CFPropertyListRef plist)
{
    auto result = AUEffectBase::RestoreState(plist);
    if (result != noErr) return result;
    if (!processor_) return kAudioUnitErr_Uninitialized;
    result = restore_pulp_state(store_, *processor_, plist);
    if (result != noErr) return result;

    // Mirror the restored values into the AU's own parameter storage so a host
    // query that falls through our GetParameter override still reads the
    // preset rather than construction defaults.
    for (const auto& param : store_.all_params()) {
        Globals()->SetParameter(static_cast<AudioUnitParameterID>(param.id),
                                store_.get_value(param.id));
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
