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
