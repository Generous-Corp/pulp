# Coming from JUCE

A short guide for plugin authors moving a JUCE project to Pulp.
Covers the patterns that differ most often, plus the things Pulp
gets right by default that you'd otherwise have to remember to add.

If you've never written a JUCE plugin: this guide is also a
useful "what footguns are NOT in Pulp" tour.

## The big picture

| | JUCE | Pulp |
|---|---|---|
| License | GPL / commercial split | MIT, single license |
| Language | C++17 + Projucer-generated headers | Modern C++20, plain CMake |
| Build system | Projucer / CMake (since 6.x) | CMake everywhere |
| UI | LookAndFeel + paint(Graphics&) | JS / WebView / Skia-backed `pulp::view::View` |
| Audio API | `AudioProcessor::processBlock` | `Processor::process(BufferView, BufferView, MidiBuffer, MidiBuffer, ProcessContext)` |
| Params | `AudioProcessorValueTreeState` | `StateStore` + `ListenerToken` |
| Inspector | Melatonin Inspector add-on | `Cmd+I` built in, plus TCP IPC for AI tools |

The audio thread contract is the same in both frameworks (don't
allocate, don't lock, don't block on main). Pulp catches more
violations for you at debug time — see "[DSP threading](dsp-threading.md)".

## Build & run

```
# JUCE (with the CMake API)
cmake -S . -B build
cmake --build build
open build/MyPlugin_artefacts/Standalone/MyPlugin.app

# Pulp
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
open build/MyPlugin_artefacts/Standalone/MyPlugin.app
# Or, with the Pulp CLI:
pulp run                  # build and launch
pulp run --watch          # rebuild + relaunch
pulp dev --run MyPlugin   # standalone host
pulp build --watch        # incremental build
```

Watch-mode rebuilds (the equivalent of "save in your IDE, hit
play") are first-class — there's no extra plugin or VS Code task.

## Validation: built in, not an extra

The biggest one-line story:

```
juce ⏵  install pluginval / clap-validator / auval separately,
        glue them into your CI yaml by hand, hope you remember
        to bump versions in lockstep with the SDK.

pulp ⏵  `pulp validate`. Discovers installed validators, reports
        missing/broken tools clearly, and hard-fails when a validator
        rejects the bundle. `--strict` makes missing validators fail CI.
        `--screenshot` is available for visual diffs.
```

`pulp validate` discovers **pluginval + clap-validator + auval** from
the host environment and reports which validators ran, were missing,
or were broken. There is also a *hard*
"no install without validation" policy in `pulp ship` — see
[Shipping Guide](shipping.md).

If you were running pluginval manually and patching for `strictness=10`
mismatches: that's `--strict` in Pulp.

## Inspector: Cmd+I, baked in

JUCE: drop in Melatonin Inspector, pay for the licensing, restart
the host. Pulp:

```
Cmd+I   →  Inspector overlay on the running plugin window.
pulp inspect MyPlugin  →  CLI driver from an external script.
```

The overlay shows:

* `View::id` tree with bounds, opacity, transform
* `StateStore` parameter values + recent changes (ring buffer of 100)
* Console (`log_info` / `log_warn` / `log_error`) live tail
* Performance snapshot per frame
* `DirtyTracker::debug_overlay` repaint flash
* Live constants (see below)

It also speaks JSON-RPC over a local TCP port — that's how `pulp inspect`
drives it, and it's how AI tools like the design import skill drive it.

## `PULP_LIVE_CONSTANT(value, min, max)`

JUCE: `JUCE_LIVE_CONSTANT(3.14)` lets you scrub a literal at runtime.
Pulp: `PULP_LIVE_CONSTANT(3.14, 0.0, 10.0)` — same idea, with min/max
hints so the inspector can show a sane slider range. Registers a
sliderable constant via `LiveConstantRegistry` and returns the current
value; the registration is one-shot (mutex + first-call alloc), so
keep these in UI/init code, not inside RT-critical hot loops.

```cpp
auto cutoff = PULP_LIVE_CONSTANT(440.0f, 20.0f, 20000.0f);
filter.set_cutoff(cutoff);
```

In a debug build, the inspector's *Live Constants* panel lists every
call site and lets you drag a slider — your DSP picks up the new
value on the next block.

## Migrating parameters

JUCE pattern (AudioProcessorValueTreeState):

```cpp
parameters.createAndAddParameter(...);
parameters.addParameterListener("gain", this);
// And remember to removeParameterListener in the destructor.
```

Pulp pattern (StateStore + ListenerToken):

```cpp
void MyPlugin::define_parameters(StateStore& store) {
    store.add_parameter({.id = kGainId, .name = "Gain", .unit = "dB",
                         .range = {-60.0f, 12.0f, 0.0f}});
}

// In the editor or wherever subscribes:
listener_token_ = store.add_listener(
    [this](ParamID id, float value) { /* react */ },
    ListenerThread::Main);
// No need to unregister — token's destructor removes the listener.
```

The token is the cleanup. Forget the token, the listener is gone
when the owner is gone. Forget JUCE's `removeParameterListener`,
your callback fires after `this` was destroyed.

### MIDI Learn And Parameter Maps

If your JUCE project has a MIDI-learn layer that maps incoming controller
messages to `AudioProcessorValueTreeState` parameters, port that to
`pulp::state::MidiParameterMap`. It is a plugin-owned `(channel, cc) ->
ParamID` map with an `arm_learn()` mode, so the next incoming CC can bind to the
target parameter and subsequent CCs write the parameter in the normalized domain.

That covers parameter learn. It is separate from "learn the next MIDI note and
store it as a setting" workflows, which should use a small note-listener helper
or project-local state until Pulp grows a generic note/control learn utility.

## Migrating microtuning

JUCE does not provide one built-in microtuning stack, so projects usually carry
their own Scala loader, tuning table, or direct ODDSound MTS-ESP client calls.
Pulp maps those to the provider-neutral `pulp::midi::TuningProvider` API:

- Direct `.scl` / `.kbm` file loading maps to `ScalaTuningProvider`, enabled
  with `PULP_ENABLE_SCALA_TUNING=ON` in a Pulp source build, or
  `pulp add sst-tuning-library` plus `pulp_enable_midi_tuning_provider(... SCALA)`
  in a project that consumes an installed Pulp SDK.
- Direct ODDSound `libMTSClient.h` calls map to `MtsEspTuningProvider`, enabled
  with `PULP_ENABLE_MTS_ESP=ON` in a Pulp source build, or `pulp add mts-esp`
  plus `pulp_enable_midi_tuning_provider(... MTS_ESP)` in a project that
  consumes an installed Pulp SDK.
- Products that support both local tuning files and a DAW/session-wide MTS-ESP
  master should use `MtsEspFallbackTuningProvider` so an active MTS source wins
  while the local Scala provider remains the fallback.

The project-import flow records this as `integration_requirements`: detected
MTS calls request `mts-esp`, detected `.scl` / `.kbm` assets request
`sst-tuning-library` and are copied into the scaffold, and `.tun` assets are
copied into the scaffold but marked for review because direct `.tun` parsing is
not part of the Scala provider.

Imported scaffolds should include `cmake/pulp-packages.cmake`, call
`pulp_add_plugin(...)`, and then call:

```cmake
pulp_enable_midi_tuning_provider(MySynth MTS_ESP SCALA)
```

That works with either an installed SDK or a source-tree Pulp checkout. The
third-party sources are still fetched only when the project opts in with
`pulp add`.

Keep file parsing and MTS SysEx parsing off the audio callback. Voice code should
query `TuningProvider::note_to_frequency()` at note-on or at the same retuning
cadence the original JUCE project used.

## Migrating structured state

Pulp separates owned tree structure from structured leaf values:

- ValueTree-like records with children map to `StateTree` nodes plus
  `add_child()` for owned child records.
- var-style arrays/objects map to `PropertyValue` arrays and string-keyed
  objects via `make_property_array()` and `make_property_object()`.
- Scalars keep the existing `std::variant` shape: `bool`, `int64_t`, `double`,
  and `std::string` still work with `tree->set("key", value)` and typed getters.

```cpp
auto root = StateTree::create("PluginState");
root->set("ui", make_property_object({
    {"theme", std::string("dark")},
    {"recent", make_property_array({std::string("Init"), std::string("Wide")})},
}));

auto osc = StateTree::create("Oscillator");
osc->set("id", std::string("osc1"));
osc->set("extra", make_property_object({
    {"wave", std::string("saw")},
    {"weights", make_property_array({1.0, 0.5, 0.25})},
}));
root->add_child(osc);
```

That mapping preserves arrays, objects, and nested primitive data during JSON
serialization and StateTree sync without importing framework-specific names into
the Pulp API. Direct node-as-property storage is intentionally staged rather
than omitted: embedding node references in `PropertyValue` would make ownership
ambiguous and could create hidden aliasing/cycles across `deep_copy()`,
`clone_synced()`, and cross-process sync.

## Audio thread: snapshot, don't atomic-load-per-sample

```cpp
// JUCE — common (incorrect) pattern:
for (int s = 0; s < numSamples; ++s) {
    out[s] = in[s] * *params.getRawParameterValue("gain");
}

// Pulp — block-local snapshot:
const float gain = state().get_value(kGainId);
for (int s = 0; s < n; ++s) {
    out[s] = in[s] * gain;
}
```

Call `get_value()` once per parameter at the top of `process()` and
read from the local inside the per-sample loop. For several parameters,
use `state().snapshot(ids)` and keep reading from the returned local
array. See [DSP threading](dsp-threading.md) for the full contract.
Pulp's `ScopedNoAlloc` debug guard marks `Processor::process` so
allocation-on-RT is catchable by debug allocator hooks.

## macOS AU cache refresh

JUCE devs: when AU validation gets stuck because macOS cached a
stale `Info.plist`, you reach for:

```
killall -9 AudioComponentRegistrar
```

Pulp wraps the same fix:

```
pulp doctor --au-cache
pulp doctor --au-cache --dry-run
```

## Windows: static MSVC runtime by default

JUCE: you ship a plugin and a user reports `vcruntime140.dll not
found`. You go searching for the right Visual C++ Redistributable.

Pulp: `CMAKE_MSVC_RUNTIME_LIBRARY = MultiThreaded$<$<CONFIG:Debug>:Debug>`
is set in the framework's root CMakeLists.txt. The runtime is
statically linked into your binary. End users don't need the
redist installed.

If you want the dynamic runtime back, override with
`-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL` at configure time.

## Different defaults from JUCE

* **Styling.** Pulp's visual layer is JS + Skia. Restyle by editing
  the JS bundle or by using `pulp design` to drive Stitch / Figma /
  Pencil / v0 imports. Most JUCE LookAndFeel patterns map to a CSS
  theme.
* **Host-specific wrapper checks.** The same `Processor` runs in
  VST3 / AU / CLAP / LV2 / Standalone. Prefer descriptor fields and
  framework host-quirk handling over plugin-side `wrapperType_*`
  conditionals.
* **Debug logging.** Use `pulp::runtime::log_info` / `log_warn` /
  `log_error`; the formatter is designed for audio-thread-safe logging.

## Already ahead of JUCE (you don't have to add these)

* `pulp validate` runs pluginval + clap-validator + auval, has
  `--strict`, validator-discovery preflight, `--screenshot` diffing,
  and a no-install-without-validation policy.
* `pulp run` / `pulp dev` give you JUCE's "standalone for fast
  iteration" without any project file boilerplate.
* `pulp inspect` + `Cmd+I` cover Melatonin Inspector and more —
  bundled, no licensing.
* `pulp clean` covers the JUCE-era "`rm -rf build`" reflex without
  losing your CMake cache.

## See also

* [Putting a Pulp UI in a JUCE plugin](juce-embed.md) — the *other* direction:
  keep your JUCE DSP and swap only the UI (import an existing plugin, start from
  the template, or use the adapter directly).
* [DSP threading](dsp-threading.md) — the audio-thread contract.
* sudara, *"Big List of JUCE Tips and Tricks"* —
  https://melatonin.dev/blog/the-big-list-of-juce-tips-and-tricks/
