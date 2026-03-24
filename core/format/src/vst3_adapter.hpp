#pragma once

// VST3 Adapter for Pulp
// Implements Steinberg's VST3 interfaces by wrapping pulp::format::Processor
// Built from VST3 SDK headers (MIT license), not from any existing adapter

#include <pulp/format/processor.hpp>

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

#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivstparameterchanges.h>
#include <pluginterfaces/vst/ivstevents.h>
#include <pluginterfaces/base/ibstream.h>
#include <pluginterfaces/base/funknown.h>
#include <public.sdk/source/vst/vstaudioeffect.h>

namespace pulp::format::vst3 {

// Forward declarations
class PulpVst3Processor;
class PulpVst3Controller;

// The VST3 audio processor component
// Wraps a pulp::format::Processor for the VST3 host
class PulpVst3Processor : public Steinberg::Vst::AudioEffect {
public:
    PulpVst3Processor(ProcessorFactory factory);

    // IPluginBase
    Steinberg::tresult PLUGIN_API initialize(Steinberg::FUnknown* context) override;
    Steinberg::tresult PLUGIN_API terminate() override;

    // IAudioProcessor
    Steinberg::tresult PLUGIN_API setBusArrangements(
        Steinberg::Vst::SpeakerArrangement* inputs, Steinberg::int32 numIns,
        Steinberg::Vst::SpeakerArrangement* outputs, Steinberg::int32 numOuts) override;

    Steinberg::tresult PLUGIN_API setupProcessing(
        Steinberg::Vst::ProcessSetup& setup) override;

    Steinberg::tresult PLUGIN_API setActive(Steinberg::TBool state) override;

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
};

} // namespace pulp::format::vst3
