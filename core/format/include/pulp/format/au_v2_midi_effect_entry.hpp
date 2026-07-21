#pragma once

// AU v2 MIDI-effect entry point generator
// For MIDI processors that receive MIDI and produce MIDI with no audio path
// (`aumi`, kAudioUnitType_MIDIProcessor) — transposers, arpeggiators, chord
// generators, note filters, humanizers.
//
// Usage (in one .cpp file per AU MIDI-effect plugin):
//   #include "my_midi_effect.hpp"
//   #include <pulp/format/au_v2_midi_effect_entry.hpp>
//   PULP_AU_MIDI_EFFECT(MyMidiEffectAU, my_namespace::create_my_midi_effect)
//
// Three surfaces must agree or the component is invalid:
//   1. `descriptor().category == PluginCategory::MidiEffect`
//   2. the `aumi` component type — `pulp_add_plugin(CATEGORY MidiEffect)`
//      resolves it, or a hand-written Info.plist.au declares it
//   3. `PULP_AU_MIDI_EFFECT` here
//
// The factory is the load-bearing half of (3): a factory only dispatches the
// selectors its lookup table carries, so registering through the plain
// `ausdk::AUBaseFactory` makes every host `MusicDeviceMIDIEvent` call return
// -4 (unimpErr) even though the adapter class implements `HandleMIDIEvent`.
// `AUMIDIEffectFactory`'s `AUMIDILookup` carries MIDIEvent + SysEx, which is
// exactly a MIDI processor's selector set (StartNote / StopNote belong to an
// instrument, not to an `aumi`).

// Apple-only: wraps AudioUnitSDK. No-op on non-Apple so the header stays
// self-contained on the Linux header-hygiene check.
#if defined(__APPLE__)

#include <pulp/format/au_v2_midi_processor.hpp>
#include <pulp/format/registry.hpp>
#include <AudioUnitSDK/AUPlugInDispatch.h>
#include <AudioUnitSDK/ComponentBase.h>

// Generate an AU v2 MIDI-effect entry point.
// ClassName determines the factory function name: ClassNameFactory, which must
// match the `factoryFunction` in the bundle's Info.plist (CMake derives it as
// `<cmake-target>AUFactory`).
#define PULP_AU_MIDI_EFFECT(ClassName, factory_fn) \
    PULP_REGISTER_PLUGIN(factory_fn) \
    class ClassName : public pulp::format::au::PulpAUMidiProcessor { \
    public: \
        explicit ClassName(AudioComponentInstance ci) \
            : PulpAUMidiProcessor(ci) {} \
    }; \
    AUSDK_COMPONENT_ENTRY(ausdk::AUMIDIEffectFactory, ClassName)

// ── Multi-plugin bundle variant ──────────────────────────────────────────
// One binary hosts many MIDI processors (`aumi`). Binds each component to its
// own factory lexically (via the factory-taking PulpAUMidiProcessor ctor), so
// processor construction never consults the legacy global slot, and registers a
// KEYED entry (which never touches that global). Expand once per component; N
// per binary is legal. The trailing arg is a PluginRegistration braced-init
// carrying id + AU codes (WITHOUT `.factory` — it is bound from factory_fn and
// force-assigned, so a stray `.factory` would be ignored); it is variadic so
// its internal commas survive the preprocessor.
#define PULP_AU_BUNDLE_MIDI_EFFECT(ClassName, factory_fn, ...) \
    namespace { \
        struct ClassName##_Registrar { \
            ClassName##_Registrar() { \
                ::pulp::format::PluginRegistration reg __VA_ARGS__; \
                reg.factory = factory_fn; \
                ::pulp::format::register_plugin(reg); \
            } \
        } ClassName##_registrar_; \
    } \
    class ClassName : public pulp::format::au::PulpAUMidiProcessor { \
    public: \
        explicit ClassName(AudioComponentInstance ci) \
            : PulpAUMidiProcessor(ci, factory_fn) {} \
    }; \
    AUSDK_COMPONENT_ENTRY(ausdk::AUMIDIEffectFactory, ClassName)

#endif // defined(__APPLE__)
