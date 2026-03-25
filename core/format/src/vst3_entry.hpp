#pragma once

// VST3 plugin factory and entry point generation
// Creates the GetPluginFactory() export that DAWs call to discover plugins

#include "vst3_adapter.hpp"
#include <public.sdk/source/main/pluginfactory.h>

namespace pulp::format::vst3 {

// Macro to declare a VST3 plugin entry point
// Usage in plugin's main.cpp:
//   PULP_VST3_ENTRY(MyProcessor, create_my_processor,
//       "com.mycompany.myplugin",
//       "My Plugin", "Fx", "My Company",
//       "1.0.0", "mailto:me@example.com")

#define PULP_VST3_ENTRY(ProcessorClass, FactoryFn, BundleId, Name, Category, Vendor, Version, URL) \
    static Steinberg::FUID processor_uid = Steinberg::FUID::fromTUID( \
        {BundleId[0], BundleId[1], BundleId[2], BundleId[3], \
         BundleId[4], BundleId[5], BundleId[6], BundleId[7], \
         BundleId[8], BundleId[9], BundleId[10], BundleId[11], \
         BundleId[12], BundleId[13], BundleId[14], BundleId[15]}); \
    \
    BEGIN_FACTORY_DEF(Vendor, URL, "mailto:" URL) \
        DEF_CLASS2(INLINE_UID_FROM_FUID(processor_uid), \
            PClassInfo::kManyInstances, \
            kVstAudioEffectClass, \
            Name, \
            Vst::kDistributable, \
            Category, \
            Version, \
            kVstVersionString, \
            [](void*) -> Steinberg::FUnknown* { \
                return static_cast<Steinberg::Vst::IAudioProcessor*>( \
                    new pulp::format::vst3::PulpVst3Processor(FactoryFn)); \
            }) \
    END_FACTORY

} // namespace pulp::format::vst3
