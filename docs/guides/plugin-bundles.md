# Multi-Plugin Bundles

A **bundle** packages many distinct plugins into ONE binary per format — a
single `.clap` / `.vst3` that a host scans once and lists as N plugins (the
shape Expert Sleepers' Silent Way suite uses). Pulp supports this without
regressing the single-plugin path: one plugin per binary stays the default (see
[opinionated defaults](../reference/opinionated-defaults.md#plugin-packaging)),
and bundling is opt-in.

## When to bundle

- You ship a *suite* of related plugins and want one install, one scan entry,
  one code-signed artifact (e.g. a control-voltage toolkit, a channel-strip set).
- You want the plugins to share code/assets in a single binary.

If you ship one plugin, use [`pulp_add_plugin`](../reference/cmake.md#pulp_add_plugin)
— don't reach for a bundle.

## Anatomy

A bundle target is declared with
[`pulp_add_plugin_bundle`](../reference/cmake.md#pulp_add_plugin_bundle). Each
format gets one entry translation unit that registers every plugin via the
bundle macros. The format build functions automatically prefer a
`<fmt>_bundle_entry.cpp` over the single-plugin `<fmt>_entry.cpp`.

### CLAP — `clap_bundle_entry.cpp`

One module entry symbol; its factory enumerates every plugin.

```cpp
#include "my_plugins.hpp"
#include <pulp/format/clap_entry.hpp>

PULP_CLAP_BUNDLE_PLUGIN(Gain,  my::create_gain)
PULP_CLAP_BUNDLE_PLUGIN(Width, my::create_width)
PULP_CLAP_BUNDLE_ENTRY()               // exactly one, after the plugins
```

Each plugin's id/name/category come from its `PluginDescriptor`. Every plugin
needs a unique `bundle_id`; `create_plugin(id)` resolves by id, and a duplicate
id is dropped from enumeration rather than advertised-but-unreachable.

### VST3 — `vst3_bundle_entry.cpp`

One `GetPluginFactory()`; it lists one class per plugin.

```cpp
#include "my_plugins.hpp"
#include <pulp/format/vst3_entry.hpp>

static const Steinberg::FUID kGainUID(0x…, 0x…, 0x…, 0x…);   // stable, unique
static const Steinberg::FUID kWidthUID(0x…, 0x…, 0x…, 0x…);

PULP_VST3_BUNDLE_PLUGIN(Gain,  my::create_gain,  {.id = "com.you.suite.gain"})
PULP_VST3_BUNDLE_PLUGIN(Width, my::create_width, {.id = "com.you.suite.width"})

PULP_VST3_FACTORY_BEGIN("You", "https://you.example", "mailto:support@you.example")
    PULP_VST3_BUNDLE_CLASS(Gain,  kGainUID,  "Gain",  Steinberg::Vst::PlugType::kFx, "1.0.0")
    PULP_VST3_BUNDLE_CLASS(Width, kWidthUID, "Width", Steinberg::Vst::PlugType::kFx, "1.0.0")
PULP_VST3_FACTORY_END
```

The `Ident` token (`Gain`, `Width`) links each `_BUNDLE_PLUGIN` to its
`_BUNDLE_CLASS` at compile time — a mismatch fails to compile, so the pair
cannot silently desync.

### CMakeLists.txt

```cmake
pulp_add_plugin_bundle(MySuite
    FORMATS      CLAP VST3
    BUNDLE_NAME  "MySuite"
    BUNDLE_ID    "com.you.suite"
    VERSION      "1.0.0"
    MANUFACTURER "You"
    SOURCES      gain.cpp width.cpp     # omit for header-only plugins
)
```

A complete, buildable two-plugin example lives in
`examples/combined-bundle-demo` (header-only plugins, CLAP + VST3), with an
artifact test that dlopens the built `.clap` and asserts it exposes two plugins.

## Format support

| Format | Bundles | Notes |
|--------|---------|-------|
| CLAP   | ✅ | One `clap_entry`; factory enumerates N plugins. |
| VST3   | ✅ | One `GetPluginFactory`; one class per plugin. |
| AU / AUv3 | ⏳ | An AU `.component` bundle needs a multi-component `Info.plist` (an `AudioComponents` array with N entries) and all N `AUSDK_COMPONENT_ENTRY` factory symbols exported. Tracked as a follow-up slice; requesting `AU` from `pulp_add_plugin_bundle` is a configure-time error today rather than a silently-single-plugin binary. |
| AAX    | ⏳ | Same multi-component packaging work as AU. |

## How it works

All plugins register into a shared, keyed registry
(`pulp::format::register_plugin(PluginRegistration{…})`). Only the bundle macros
publish keyed entries; the single-plugin macros use a legacy global slot and
leave the keyed table empty, so the single-plugin contract is byte-for-byte
unchanged. See the [`clap`](../../.agents/skills/clap/SKILL.md),
[`vst3`](../../.agents/skills/vst3/SKILL.md), and
[`auv2`](../../.agents/skills/auv2/SKILL.md) skills for the per-format macro
details and gotchas.
