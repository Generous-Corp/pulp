// PulpGain VST3 entry point
// This file provides the GetPluginFactory() export that DAWs call
// Uses SingleComponentEffect pattern (combined processor + controller)

#ifndef DEVELOPMENT
#ifndef RELEASE
#ifndef NDEBUG
#define DEVELOPMENT 1
#else
#define RELEASE 1
#endif
#endif
#endif

#include "pulp_gain.hpp"
#include "../../core/format/src/vst3_adapter.hpp"

#include <public.sdk/source/main/pluginfactory.h>

// Unique ID for this plugin (generated, stable across versions)
static const Steinberg::FUID PulpGainProcessorUID(0x50554C50, 0x47414900, 0x00000001, 0x00000001);

// Factory creation function
static Steinberg::FUnknown* createPulpGainProcessor(void*) {
    return static_cast<Steinberg::Vst::IAudioProcessor*>(
        new pulp::format::vst3::PulpVst3Processor(pulp::examples::create_pulp_gain));
}

// Plugin Factory definition
BEGIN_FACTORY_DEF("Pulp",
                  "https://github.com/danielraffel/pulp",
                  "mailto:info@pulp.dev")

    // Single component effect: flag must be 0 (cannot be distributed)
    DEF_CLASS2(INLINE_UID_FROM_FUID(PulpGainProcessorUID),
        PClassInfo::kManyInstances,
        kVstAudioEffectClass,
        "PulpGain",
        0,  // single component — not distributable
        Steinberg::Vst::PlugType::kFx,
        "1.0.0",
        kVstVersionString,
        createPulpGainProcessor)

END_FACTORY
