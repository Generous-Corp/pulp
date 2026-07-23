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
#include <pulp/format/registry.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>
#include <pulp/signal/scoped_flush_denormals.hpp>

#include <array>
#include <cstring>
#include <span>

namespace pulp::format::au {

namespace {
// Number of AU output elements to hand the MusicDeviceBase base constructor.
// The base ctor stores the count and CreateElements() materialises exactly that
// many output elements, so the count MUST be known before the base runs — hence
// a throwaway probe of the registered factory here (instrument construction is
// not on the audio thread, so the extra allocation is harmless). Falls back to a
// single output bus if no factory is registered.
UInt32 registry_output_element_count(ProcessorFactory factory) {
    if (!factory) return 1;
    auto probe = factory();
    if (!probe) return 1;
    const auto& desc = probe->descriptor();
    const std::size_t declared = desc.output_buses.size();
    const std::size_t count = instrument_output_element_count(desc);
    if (declared > count) {
        // instrument_output_element_count clamps to BusBufferSet::kMaxBuses
        // because Render() only fills that many output views. Advertising the
        // extra elements would leave permanently silent buses in the host, so
        // surface the truncation instead of dropping the buses silently.
        runtime::log_warn(
            "AU v2 instrument: descriptor declares {} output buses but the AU "
            "render path supports at most {}; clamping to {} output element(s). "
            "Output buses beyond the cap are dropped and would never receive "
            "audio.",
            declared, count, count);
    }
    return static_cast<UInt32>(count);
}
}  // namespace

PulpAUInstrument::PulpAUInstrument(AudioComponentInstance ci)
    : PulpAUInstrument(ci, registered_factory())
{
}

PulpAUInstrument::PulpAUInstrument(AudioComponentInstance ci, ProcessorFactory factory)
    : MusicDeviceBase(ci, /*numInputs=*/0,
                      /*numOutputs=*/registry_output_element_count(factory),
                      /*numGroups=*/0)
{
    if (factory) {
        processor_ = factory();
        if (processor_) {
            processor_->set_state_store(&store_);
            processor_->define_parameters(store_);
            // Cache the immutable descriptor once so the render path can view its
            // bus-name strings without copying it per block (which would allocate
            // on the audio thread).
            descriptor_ = processor_->descriptor();

            // Materialise the output elements now (guarded/idempotent — DoInitialize
            // calls CreateElements() again later as a no-op) so each declared bus's
            // channel count is set from the descriptor before the host queries
            // stream formats. The default AU IO element is stereo; only a bus that
            // declares a different width needs adjusting, and for non-interleaved
            // float only mChannelsPerFrame changes.
            CreateElements();
            const UInt32 n_elements = Outputs().GetNumberOfElements();
            for (UInt32 e = 0;
                 e < n_elements && e < descriptor_.output_buses.size(); ++e) {
                const int ch = descriptor_.output_buses[e].default_channels;
                if (ch <= 0) continue;
                AudioStreamBasicDescription asbd = GetOutput(e)->GetStreamFormat();
                if (asbd.mChannelsPerFrame != static_cast<UInt32>(ch)) {
                    asbd.mChannelsPerFrame = static_cast<UInt32>(ch);
                    GetOutput(e)->SetStreamFormat(asbd);
                }
            }

            // Resolve host accommodations once so GetLatency() can apply
            // the same policy-gated clamp as the effect adapter.
            const auto host_info = detect_host_info();
            host_quirks_ = resolved_quirks(host_info.type, host_info.version);

            for (const auto& param : store_.all_params()) {
                Globals()->SetParameter(
                    static_cast<AudioUnitParameterID>(param.id),
                    param.range.default_value);
            }

            // Editor -> host parameter path: gesture brackets + value-change
            // notification, wired once through the shared AU v2 bridge.
            wire_host_parameter_bridge(store_, GetComponentInstance(),
                                               ui_push_listener_);
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
        ScopedHostParamWrite host_write;
        store_.set_value_rt(static_cast<state::ParamID>(inID), inValue);
        return noErr;
    }
    return MusicDeviceBase::SetParameter(inID, inScope, inElement, inValue,
                                         inBufferOffsetInFrames);
}

OSStatus PulpAUInstrument::GetParameterList(AudioUnitScope inScope,
                                            AudioUnitParameterID* outParameterList,
                                            UInt32& outNumParameters)
{
    return fill_parameter_list(store_, inScope, outParameterList,
                                       outNumParameters);
}

OSStatus PulpAUInstrument::GetParameterInfo(AudioUnitScope inScope,
                                            AudioUnitParameterID inParameterID,
                                            AudioUnitParameterInfo& outParameterInfo)
{
    // false: this adapter serves neither GetParameterValueStrings nor the
    // ParameterStringFromValue / ...ValueFromString properties, so advertising
    // ValuesHaveStrings would point the host at a door nobody answers.
    return fill_parameter_info(store_, inScope, inParameterID,
                               outParameterInfo,
                               /*advertise_value_strings=*/false);
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
        ctx->owner_alive = owner_alive_.capture();
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

    // Pre-reserve each output bus's channel-pointer vector to its declared width
    // so the per-block resize in Render() never grows a vector on the audio
    // thread. Only the elements the host actually created are reserved.
    const UInt32 n_out = Outputs().GetNumberOfElements();
    for (UInt32 e = 0; e < n_out && e < output_bus_ptrs_.size(); ++e) {
        const UInt32 ch = GetOutput(e)->GetStreamFormat().mChannelsPerFrame;
        output_bus_ptrs_[e].reserve(ch == 0 ? 2 : ch);
    }

    runtime::log_info("AU v2 instrument: initialized at {} Hz, {} output bus(es)",
                      GetOutput(0)->GetStreamFormat().mSampleRate,
                      Outputs().GetNumberOfElements());
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

    audio::BufferView<const float> input_view;

    midi::MidiBuffer midi_in, midi_out;
    while (auto ev = midi_in_queue_.try_pop())  // lock-free drain
        midi_in.add(*ev);
    midi_in.sort();

    ProcessContext ctx = make_render_process_context(
        GetOutput(0)->GetStreamFormat().mSampleRate,
        static_cast<int>(inNumberFrames));

    apply_host_callbacks_to_process_context(ctx, *this, playhead_prev_);

    // Instruments have no audio input; the single input bus view is inactive so
    // process(ProcessBuffers&) projects a null main input.
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

    // Build one output bus view per AU output element. The AU host renders each
    // output element with its own DoRenderBus/PrepareBuffer, but a single Render()
    // is expected to fill ALL of them (NeedsToRender gates the re-render for the
    // remaining buses pulled at the same timestamp — see AUBase::RenderBus). So we
    // prepare and fill every output element here. Bus name/index/role metadata
    // comes from the cached descriptor via build_output_bus_infos, so a multi-out
    // processor that overrides process(ProcessBuffers&) sees stable bus identity.
    //
    // Each bus is pre-zeroed BEFORE process(): a processor that only implements the
    // simple main-in/main-out process() writes just the main bus, so aux buses must
    // read silence rather than uninitialised memory. A host that leaves an aux bus
    // disconnected still gets a prepared (silent) element here without disturbing
    // the bus->buffer index mapping.
    std::array<ProcessBusBufferInfo, kMaxOutputBuses> infos{};
    const std::size_t n_declared =
        build_output_bus_infos(descriptor_, infos.data(), kMaxOutputBuses);

    std::array<ProcessBusBufferView<float>, kMaxOutputBuses> output_buses{};
    std::size_t routed = 0;
    const UInt32 n_elements = Outputs().GetNumberOfElements();
    for (UInt32 e = 0; e < n_elements && routed < kMaxOutputBuses; ++e) {
        AudioBufferList& bl = GetOutput(e)->PrepareBuffer(inNumberFrames);
        const UInt32 ch = bl.mNumberBuffers;
        auto& ptrs = output_bus_ptrs_[e];
        ptrs.resize(ch);
        for (UInt32 c = 0; c < ch; ++c) {
            ptrs[c] = static_cast<float*>(bl.mBuffers[c].mData);
            if (ptrs[c] != nullptr)
                std::memset(ptrs[c], 0, sizeof(float) * inNumberFrames);
        }
        ProcessBusBufferInfo info =
            (e < n_declared)
                ? infos[e]
                : ProcessBusBufferInfo{"Aux Out", e, BusDirection::Output,
                                       BusRole::Aux, static_cast<int>(ch), true,
                                       ch > 0};
        info.active = ch > 0;
        output_buses[routed] = {
            .info = info,
            .buffer = audio::BufferView<float>(ptrs.data(), ch, inNumberFrames),
        };
        ++routed;
    }

    ProcessBuffers process_buffers{
        ProcessBusBufferSet<const float>{std::span(input_buses)},
        ProcessBusBufferSet<float>{
            std::span(output_buses.data(), routed)},
    };

    // Audio-thread render: the Processor's process() must neither allocate nor
    // take a blocking lock. Bracket it in ScopedNoAlloc so the RT interposition
    // guard (test/native_components/rt_intercept_test_support.cpp) traps any
    // regression in test builds. Mirrors the AU-v2 effect adapter.
    {
        pulp::runtime::ScopedNoAlloc no_alloc_guard;
        processor_->process(process_buffers, midi_in, midi_out, ctx);
    }

    return noErr;
}

OSStatus PulpAUInstrument::SaveState(CFPropertyListRef* outData)
{
    auto result = MusicDeviceBase::SaveState(outData);
    if (result != noErr) return result;
    if (!processor_) return kAudioUnitErr_Uninitialized;
    return save_pulp_state(store_, *processor_, outData);
}

OSStatus PulpAUInstrument::RestoreState(CFPropertyListRef plist)
{
    auto result = MusicDeviceBase::RestoreState(plist);
    if (result != noErr) return result;
    if (!processor_) return kAudioUnitErr_Uninitialized;
    // store_ is the source of truth (GetParameter reads it) — no Globals
    // mirror to update.
    return restore_pulp_state(store_, *processor_, plist);
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
