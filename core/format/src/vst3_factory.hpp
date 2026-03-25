#pragma once

// VST3 Plugin Factory — simple, direct implementation
// Creates the IPluginFactory that DAWs query to instantiate plugins

#ifndef DEVELOPMENT
#ifndef RELEASE
#ifndef NDEBUG
#define DEVELOPMENT 1
#else
#define RELEASE 1
#endif
#endif
#endif

#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/base/funknown.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include "vst3_adapter.hpp"
#include <cstring>

namespace pulp::format::vst3 {

// Simple factory registration — one plugin per binary
struct PluginRegistration {
    Steinberg::TUID processor_uid;
    const char* name;
    const char* category;
    const char* vendor;
    const char* version;
    const char* url;
    ProcessorFactory factory;
};

// Set this before the factory is queried (typically via a macro in the plugin's source)
inline PluginRegistration& get_registration() {
    static PluginRegistration reg{};
    return reg;
}

} // namespace pulp::format::vst3

// Macro for plugin developers to declare their VST3 entry
// Place this in ONE .cpp file per plugin
#define PULP_DECLARE_VST3(uid_bytes, name, category, vendor, version, url, factory_fn) \
    namespace { \
        struct PulpVst3Init { \
            PulpVst3Init() { \
                auto& reg = pulp::format::vst3::get_registration(); \
                std::memcpy(reg.processor_uid, uid_bytes, 16); \
                reg.name = name; \
                reg.category = category; \
                reg.vendor = vendor; \
                reg.version = version; \
                reg.url = url; \
                reg.factory = factory_fn; \
            } \
        } _pulp_vst3_init; \
    }
