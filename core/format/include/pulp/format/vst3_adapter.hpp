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

#include <pulp/format/processor.hpp>

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

    // Parameter output: snapshot values before process to detect plugin-side changes
    std::vector<float> param_snapshot_;
};

} // namespace pulp::format::vst3
