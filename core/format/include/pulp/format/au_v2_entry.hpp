#pragma once

// Generic AU v2 entry point generator
// Plugin developers include this and call PULP_AU_PLUGIN() with their factory
// function and a class name. All AudioUnitSDK boilerplate is generated.
//
// Usage (in one .cpp file per AU plugin):
//   #include "my_processor.hpp"
//   #include <pulp/format/au_v2_entry.hpp>
//
//   // The class name determines the factory function: MyPluginAUFactory
//   // This must match the factoryFunction in Info.plist.au
//   PULP_AU_PLUGIN(MyPluginAU, my_namespace::create_my_processor)
//
// This generates:
//   - A class MyPluginAU : PulpAUEffect (the AudioUnit implementation)
//   - A factory function MyPluginAUFactory (for the Info.plist factoryFunction)
//   - Plugin registration via PULP_REGISTER_PLUGIN

// Apple-only: pulls in AudioUnitSDK. No-op on non-Apple so the header stays
// self-contained on the Linux header-hygiene check.
#if defined(__APPLE__)

#include <pulp/format/au_v2_adapter.hpp>
#include <pulp/format/registry.hpp>
#include <AudioUnitSDK/AUPlugInDispatch.h>
#include <AudioUnitSDK/ComponentBase.h>

// Generate an AU v2 entry point for an audio-only effect (`aufx`).
// ClassName determines the factory function name: ClassNameFactory
// factory_fn is the ProcessorFactory function that creates the Processor
//
// Uses AUBaseFactory: the component exposes the base AU selectors only. An
// `aufx` host never delivers MIDI, so no MusicDevice selectors are needed.
// If the plugin's descriptor sets `accepts_midi = true` (packaged `aumf`),
// use PULP_AU_MIDI_PLUGIN instead — see below.
#define PULP_AU_PLUGIN(ClassName, factory_fn) \
    PULP_REGISTER_PLUGIN(factory_fn) \
    class ClassName : public pulp::format::au::PulpAUEffect { \
    public: \
        explicit ClassName(AudioComponentInstance ci) : PulpAUEffect(ci) {} \
    }; \
    AUSDK_COMPONENT_ENTRY(ausdk::AUBaseFactory, ClassName)

// Generate an AU v2 entry point for a MIDI-receiving effect (`aumf`,
// kAudioUnitType_MusicEffect). Identical to PULP_AU_PLUGIN except the
// component is registered through AUMIDIEffectFactory, whose AUMIDILookup
// dispatches the MusicDevice MIDIEvent + SysEx selectors to the base class.
//
// PulpAUEffect already derives ausdk::AUMIDIEffectBase and implements
// HandleMIDIEvent / HandleSysEx, but with the plain AUBaseFactory those
// selectors are never wired, so the host's MusicDeviceMIDIEvent call returns
// -4 (unimpErr) and auval fails ("-4 IN CALL MusicDeviceMIDIEvent"). The
// factory is the ONLY difference — the lookup table, not the C++ class, is
// what carries the MIDI dispatch.
//
// Pairing rule (must match or the component is invalid): use this macro
// exactly when the bundle is typed `aumf` and the descriptor sets
// `accepts_midi = true`. Instruments (`aumu`) that also implement
// StartNote/StopNote use the instrument entry (AUMusicDeviceFactory) instead.
#define PULP_AU_MIDI_PLUGIN(ClassName, factory_fn) \
    PULP_REGISTER_PLUGIN(factory_fn) \
    class ClassName : public pulp::format::au::PulpAUEffect { \
    public: \
        explicit ClassName(AudioComponentInstance ci) : PulpAUEffect(ci) {} \
    }; \
    AUSDK_COMPONENT_ENTRY(ausdk::AUMIDIEffectFactory, ClassName)

// ── Multi-plugin bundle variants ─────────────────────────────────────────
// One binary hosts many plugins. Unlike the single-plugin macros above, these
// bind each component to its OWN factory lexically (via the factory-taking
// PulpAUEffect ctor), so instantiation never consults the legacy global slot,
// and they register a KEYED entry (which never touches that global). Expand
// once per component; N per binary is legal (AUSDK_COMPONENT_ENTRY generates a
// distinct ClassNameFactory each time). Pass PluginRegistration metadata
// (id + AU codes) so tools/enumeration and per-plugin editor assets resolve.
// The registration braced-init is passed as trailing __VA_ARGS__ so its
// internal commas (`{.id=…, .au_subtype=…}`) survive the preprocessor — parens
// don't protect brace commas, and an extra paren pair would form a GNU
// statement-expression. au_factory therefore comes BEFORE the init.
//
// `factory_fn` is the SINGLE source of truth: it binds the AU class ctor AND is
// force-assigned onto the registration's `.factory`, overwriting whatever the
// braced-init said. So the factory the host constructs (lexical ctor arg) and
// the factory `find_plugin`/`registered_plugins` report are provably the same
// function — a caller cannot desync them by passing a stray `.factory`. Callers
// therefore MUST NOT set `.factory` in the braced-init (it is ignored).
#define PULP_AU_BUNDLE_ENTRY_(ClassName, factory_fn, au_factory, ...) \
    namespace { \
        struct ClassName##_Registrar { \
            ClassName##_Registrar() { \
                ::pulp::format::PluginRegistration reg __VA_ARGS__; \
                reg.factory = factory_fn; \
                ::pulp::format::register_plugin(reg); \
            } \
        } ClassName##_registrar_; \
    } \
    class ClassName : public pulp::format::au::PulpAUEffect { \
    public: \
        explicit ClassName(AudioComponentInstance ci) \
            : PulpAUEffect(ci, factory_fn) {} \
    }; \
    AUSDK_COMPONENT_ENTRY(au_factory, ClassName)

// `aufx` component in a bundle. The trailing arg is a PluginRegistration
// braced-init WITHOUT `.factory` (bound from factory_fn), e.g.
//   PULP_AU_BUNDLE_PLUGIN(FooAU, create_foo,
//       {.id="com.x.foo", .au_type='aufx',
//        .au_subtype='Foo1', .au_manufacturer='Mfr '})
#define PULP_AU_BUNDLE_PLUGIN(ClassName, factory_fn, ...) \
    PULP_AU_BUNDLE_ENTRY_(ClassName, factory_fn, ausdk::AUBaseFactory, __VA_ARGS__)

// `aumf` (MusicEffect) component in a bundle.
#define PULP_AU_BUNDLE_MIDI_PLUGIN(ClassName, factory_fn, ...) \
    PULP_AU_BUNDLE_ENTRY_(ClassName, factory_fn, ausdk::AUMIDIEffectFactory, __VA_ARGS__)

#endif // defined(__APPLE__)
