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

// ── Multi-plugin bundle: one VST3 binary, many plugins ───────────────────
// VST3's factory natively lists multiple classes, so a bundle is structured
// exactly like Steinberg's own multi-class factory: define each plugin's create
// function at file scope with PULP_VST3_BUNDLE_PLUGIN, then list them between
// PULP_VST3_FACTORY_BEGIN / _END. Each create function binds its OWN
// ProcessorFactory lexically (PulpVst3Processor already takes it), so no global
// lookup happens on any instantiation path — the same model as the AU bundle
// macros. A keyed registry entry is also recorded per plugin for enumeration
// and per-plugin editor-asset resolution (see registry.hpp).
//
// Usage:
//   PULP_VST3_BUNDLE_PLUGIN(Foo, create_foo, {.id="com.x.foo"})
//   PULP_VST3_BUNDLE_PLUGIN(Bar, create_bar, {.id="com.x.bar"})
//   PULP_VST3_FACTORY_BEGIN("My Co", "https://x.com", "mailto:info@x.com")
//       PULP_VST3_BUNDLE_CLASS(Foo, kFooUID, "Foo", "Fx", "1.0.0")
//       PULP_VST3_BUNDLE_CLASS(Bar, kBarUID, "Bar", "Fx", "1.0.0")
//   PULP_VST3_FACTORY_END
//
// `Ident` links a _PLUGIN to its _CLASS: the same token names the generated
// create-function symbol, so a mismatch fails to COMPILE (undefined symbol) —
// the pair cannot silently desync. factory_fn is the single source of truth,
// force-assigned onto the keyed entry's `.factory`; do NOT set `.factory` in the
// braced-init (ignored), same rule as the AU bundle macros. The trailing
// PluginRegistration init is variadic so its brace commas survive the
// preprocessor.

// File-scope: generate `_pulp_vst3_create_<Ident>` bound to factory_fn, and
// register the keyed entry. Place ABOVE the factory block, once per plugin.
#define PULP_VST3_BUNDLE_PLUGIN(Ident, factory_fn, ...) \
    static Steinberg::FUnknown* _pulp_vst3_create_##Ident(void*) { \
        return static_cast<Steinberg::Vst::IAudioProcessor*>( \
            new pulp::format::vst3::PulpVst3Processor(factory_fn)); \
    } \
    namespace { \
        struct Ident##_Vst3Registrar { \
            Ident##_Vst3Registrar() { \
                ::pulp::format::PluginRegistration reg __VA_ARGS__; \
                reg.factory = factory_fn; \
                ::pulp::format::register_plugin(reg); \
            } \
        } Ident##_vst3_registrar_; \
    }

#define PULP_VST3_FACTORY_BEGIN(vendor, url, email) \
    BEGIN_FACTORY_DEF(vendor, url, email)

// Inside the factory block: list one class per plugin. `Ident` must match a
// PULP_VST3_BUNDLE_PLUGIN above (same generated create symbol).
#define PULP_VST3_BUNDLE_CLASS(Ident, uid, name, category, version) \
    DEF_CLASS2(INLINE_UID_FROM_FUID(uid), \
        PClassInfo::kManyInstances, \
        kVstAudioEffectClass, \
        name, \
        0, \
        category, \
        version, \
        kVstVersionString, \
        _pulp_vst3_create_##Ident)

#define PULP_VST3_FACTORY_END END_FACTORY
