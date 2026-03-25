#pragma once

// Generic VST3 entry point generator
// Plugin developers include this and call PULP_VST3_PLUGIN() with their factory
// function and a unique FUID. All VST3 boilerplate is generated automatically.
//
// Usage (in one .cpp file per plugin):
//   #include "my_processor.hpp"
//   #include <pulp/format/vst3_entry.hpp>
//
//   // Unique ID — generate once, never change across versions
//   static const Steinberg::FUID kMyPluginUID(0x12345678, 0x9ABCDEF0, 0x00000001, 0x00000001);
//
//   PULP_VST3_PLUGIN(kMyPluginUID, "My Plugin", "Fx", "My Company",
//                     "1.0.0", "https://example.com",
//                     my_namespace::create_my_processor)

#include <pulp/format/vst3_adapter.hpp>
#include <pulp/format/registry.hpp>

#include <public.sdk/source/main/pluginfactory.h>

// Generate a VST3 factory with a single plugin class.
// The plugin uses SingleComponentEffect (combined processor + controller).
#define PULP_VST3_PLUGIN(uid, name, category, vendor, version, url, factory_fn) \
    static Steinberg::FUnknown* _pulp_vst3_create(void*) { \
        return static_cast<Steinberg::Vst::IAudioProcessor*>( \
            new pulp::format::vst3::PulpVst3Processor(factory_fn)); \
    } \
    \
    BEGIN_FACTORY_DEF(vendor, url, "mailto:info@example.com") \
        DEF_CLASS2(INLINE_UID_FROM_FUID(uid), \
            PClassInfo::kManyInstances, \
            kVstAudioEffectClass, \
            name, \
            0, \
            category, \
            version, \
            kVstVersionString, \
            _pulp_vst3_create) \
    END_FACTORY
