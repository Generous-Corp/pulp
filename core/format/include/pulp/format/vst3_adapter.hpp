#pragma once

// VST3 Adapter for Pulp
// Implements Steinberg's VST3 interfaces by wrapping pulp::format::Processor
// Uses SingleComponentEffect pattern (combined processor + controller)
// Built from VST3 SDK headers (MIT license), not from any existing adapter

// VST3 SDK requires one of these to be defined
#ifndef NDEBUG
    #ifndef DEVELOPMENT
        #define DEVELOPMENT 1
    #endif
#else
    #ifndef RELEASE
        #define RELEASE 1
    #endif
#endif

// Must come first per VST3 SDK requirements
#include <public.sdk/source/vst/vstsinglecomponenteffect.h>

#include <pluginterfaces/vst/ivstparameterchanges.h>
#include <pluginterfaces/vst/ivstevents.h>
#include <pluginterfaces/base/ibstream.h>

#include <pulp/events/plugin_main_thread.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/format/host_quirks.hpp>
#include <pulp/format/detail/playhead_diff.hpp>
#include <pulp/state/parameter_event_queue.hpp>

#include <pluginterfaces/gui/iplugview.h>

namespace pulp::format::vst3 {

// The VST3 combined processor + controller
// Wraps a pulp::format::Processor for the VST3 host
class PulpVst3Processor : public Steinberg::Vst::SingleComponentEffect {
public:
    PulpVst3Processor(ProcessorFactory factory);

    // IPluginBase
    Steinberg::tresult PLUGIN_API initialize(Steinberg::FUnknown* context) override;
    Steinberg::tresult PLUGIN_API terminate() override;

    // IEditController — editor view creation
    Steinberg::IPlugView* PLUGIN_API createView(Steinberg::FIDString name) override;

    // IAudioProcessor
    Steinberg::tresult PLUGIN_API setBusArrangements(
        Steinberg::Vst::SpeakerArrangement* inputs, Steinberg::int32 numIns,
        Steinberg::Vst::SpeakerArrangement* outputs, Steinberg::int32 numOuts) override;

    Steinberg::tresult PLUGIN_API setupProcessing(
        Steinberg::Vst::ProcessSetup& setup) override;

    Steinberg::tresult PLUGIN_API setActive(Steinberg::TBool state) override;

    Steinberg::uint32 PLUGIN_API getLatencySamples() override;
    Steinberg::uint32 PLUGIN_API getTailSamples() override;

    Steinberg::tresult PLUGIN_API process(
        Steinberg::Vst::ProcessData& data) override;

    const state::ParameterEventQueue& last_input_param_events() const {
        return param_events_;
    }

    /// Returns the StateStore ParamID the adapter routes VST3's
    /// `kIsBypass` parameter to. `process()` short-circuits to
    /// pass-through when this parameter's value is >= 0.5. Returns 0
    /// when no bypass parameter is available.
    state::ParamID bypass_parameter_id() const { return bypass_param_id_; }

    // IComponent (state)
    Steinberg::tresult PLUGIN_API getState(Steinberg::IBStream* state) override;
    Steinberg::tresult PLUGIN_API setState(Steinberg::IBStream* state) override;

    static Steinberg::FUnknown* createInstance(void*);

private:
    ProcessorFactory factory_;
    std::unique_ptr<Processor> processor_;
    state::StateStore store_;

    // Working buffers
    std::vector<float*> input_ptrs_;
    std::vector<float*> output_ptrs_;
    // Second input bus routed to Processor::set_sidechain(). Only input
    // bus 1 is consumed; additional buses are ignored because the
    // Processor API exposes a single sidechain slot.
    std::vector<float*> sidechain_ptrs_;

    // Per-block MIDI buffers, reused across process() calls. Reserved +
    // realtime-capacity-limited in setupProcessing() so add()/add_sysex_copy()
    // never heap-allocate on the audio thread (they drop past the reserved
    // worst-case instead). Previously these were block-local, so the first
    // add() on any block carrying MIDI allocated.
    midi::MidiBuffer midi_in_;
    midi::MidiBuffer midi_out_;

    // Parameter output: snapshot values before process to detect plugin-side changes
    std::vector<float> param_snapshot_;
    state::ParameterEventQueue param_events_;

    // Cached parameter ID of the VST3 bypass parameter. 0 when none is
    // available, so process() never short-circuits.
    state::ParamID bypass_param_id_ = 0;

    // Host accommodations, resolved once in initialize() via the runtime
    // policy (env / API / compile default). Adapters consult these flags
    // instead of hardcoding DAW workarounds.
    HostQuirks quirks_{};

    // The processor is always prepare()'d with the descriptor-default
    // channel counts (see setupProcessing), so these cache what the
    // processor's buffers are sized for. When setBusArrangements accepts
    // an arrangement the processor does not natively support, process()
    // hands the processor only its prepared channel counts and zero-fills
    // the host's extra channels.
    int native_in_ = 0;
    int native_out_ = 0;
    bool silence_unsupported_active_ = false;

    // Block size the processor's scratch buffers were prepared for in
    // setupProcessing() (setup.maxSamplesPerBlock). process() clamps an
    // oversized render to this so a host that exceeds the advertised max
    // can't overrun the prepared buffers. 0 means "no limit set yet".
    int max_block_size_ = 0;

    // Previous-block transport snapshot used to derive the
    // `tempo_changed` / `time_sig_changed` / `transport_changed` flags
    // on `ProcessContext`. Default-constructed so the first process()
    // call after construction reports no changes.
    detail::PlayheadSnapshot playhead_prev_{};

    // MainThreadDispatcher backend token. Acquired in initialize(),
    // released in terminate(). On macOS this lets adapter-side and
    // view-side code marshal work onto the DAW's main thread via
    // `pulp::events::MainThreadDispatcher::call_async`.
    pulp::events::MainThreadDispatcher::Token main_thread_token_ = 0;
};

} // namespace pulp::format::vst3
