#pragma once

// AU v2 instrument entry point generator
// For synths/samplers that receive MIDI and produce audio (aumu type)
//
// Usage (in one .cpp file per AU instrument plugin):
//   #include "my_synth.hpp"
//   #include <pulp/format/au_v2_instrument_entry.hpp>
//   PULP_AU_INSTRUMENT(MySynthAU, my_namespace::create_my_synth)

// Apple-only: wraps AudioUnitSDK. No-op on non-Apple so the header stays
// self-contained on the Linux header-hygiene check.
#if defined(__APPLE__)

#include <pulp/format/au_v2_instrument.hpp>
#include <pulp/format/registry.hpp>
#include <AudioUnitSDK/AUPlugInDispatch.h>
#include <AudioUnitSDK/ComponentBase.h>

// Generate an AU v2 instrument entry point.
// ClassName determines the factory function name: ClassNameFactory
// factory_fn is the ProcessorFactory function
#define PULP_AU_INSTRUMENT(ClassName, factory_fn) \
    PULP_REGISTER_PLUGIN(factory_fn) \
    class ClassName : public pulp::format::au::PulpAUInstrument { \
    public: \
        explicit ClassName(AudioComponentInstance ci) : PulpAUInstrument(ci) {} \
    }; \
    AUSDK_COMPONENT_ENTRY(ausdk::AUMusicDeviceFactory, ClassName)

// ── Multi-plugin bundle variant ──────────────────────────────────────────
// One binary hosts many instruments (`aumu`). Binds each component to its own
// factory lexically (via the factory-taking PulpAUInstrument ctor), so the
// base-ctor output-element probe and processor construction never consult the
// legacy global slot, and registers a KEYED entry (which never touches that
// global). Expand once per component; N per binary is legal. The trailing arg
// is a PluginRegistration braced-init carrying id + AU codes (WITHOUT
// `.factory` — bound from factory_fn, see PULP_AU_BUNDLE_ENTRY_); it is
// variadic so its internal commas survive the preprocessor.
#define PULP_AU_BUNDLE_INSTRUMENT(ClassName, factory_fn, ...) \
    namespace { \
        struct ClassName##_Registrar { \
            ClassName##_Registrar() { \
                ::pulp::format::PluginRegistration reg __VA_ARGS__; \
                reg.factory = factory_fn; \
                ::pulp::format::register_plugin(reg); \
            } \
        } ClassName##_registrar_; \
    } \
    class ClassName : public pulp::format::au::PulpAUInstrument { \
    public: \
        explicit ClassName(AudioComponentInstance ci) \
            : PulpAUInstrument(ci, factory_fn) {} \
    }; \
    AUSDK_COMPONENT_ENTRY(ausdk::AUMusicDeviceFactory, ClassName)

#endif // defined(__APPLE__)
