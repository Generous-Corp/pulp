# DSP Hot-Reload

Recompile a plugin's DSP and have it swap into the **running** plugin — while the
host keeps the instance and the audio stream alive. No reload, no rescan, no
dropout. This is the audio analogue of the JS/UI hot-reload Pulp already does for
scripted widgets: edit, rebuild, hear the change in well under a second.

> Status: the engine and the DAW-integration shell are in `core/format/reload/`.
> Verified live in REAPER (CLAP). The shell is format-agnostic, so the same
> mechanism applies to every format adapter (see [Format & platform
> coverage](#format--platform-coverage)).

## Is this the swap you want?

Pulp has two different "change the audio without a dropout" tools. They sound
similar but do different jobs — pick by *what is being swapped*:

- **DSP hot-reload (this page)** swaps a plugin's **own DSP** — the code *you*
  wrote and compiled. You rebuild your DSP (or push a signed update to a plugin
  already in the field) and the running plugin adopts it without the host
  reloading it. This is the one that needs signing/trust, because it is *loading
  new code*.

- **Live graph editing** is for the other case: when your Pulp plugin isn't a
  single effect but a **chain that hosts other, already-installed plugins**
  (say a VST3 EQ → an AU compressor → a CLAP reverb) wired together inside one
  Pulp plugin. Pulp can change that chain *while it plays* — re-wire the
  connections, adjust a block — without a gap. That's a separate feature; see
  the [Signal graph](../reference/signal-graph.md) and
  [Hosting](hosting.md) guides.

Rule of thumb: swapping *your own* DSP → hot-reload (here). Re-arranging or
replacing *hosted third-party* plugins in a chain → live graph editing.

### Why only hot-reload carries signing/trust

The signing, capability, and revocation machinery exists on **this** page's
feature and **not** on live graph editing — on purpose, and the reason is simple:

- **Hot-reload loads new code.** It replaces a plugin's own DSP (and scripted UI)
  with a freshly compiled build, or a build pushed to a plugin already in the
  field. New code arriving into a running plugin is exactly what has to be
  verified, so it is gated by signing (opt-in `require_signed`), capabilities,
  and a revocation list.
- **Live graph editing loads no new code.** It only re-arranges, or substitutes,
  plugins the machine **already installed and already trusts** — the same trust
  boundary that let Pulp host them in the first place. Nothing new is fetched or
  verified because nothing new arrives.

And the two never cross in a rack: a **hosted** third-party plugin has no Pulp
reload path — it is not built from Pulp's reloadable shell, so it has no watcher
and nothing (a host, a rack, a script) can reload *its* DSP or UI through Pulp.
The reload mechanism only ever touches a Pulp plugin that opted into the
reloadable shell, and there it stays behind that plugin's own `require_signed`
gate. Hosting a plugin in a chain does not open a door to reloading it.

## How it works

A reloadable plugin is split into two halves:

```
host (REAPER / Logic / standalone)
        loads
          ▼
  ReloadableShell  ── the SHELL (a pulp::format::Processor)
   • owns the audio entry, the StateStore, and the RT-safe hot-swap slot
   • watches a logic shared library on disk
   • dlopens + gates + (click-free) swaps it on change
          ▲ swaps in
          │
  logic.dylib  ── the DSP you EDIT (exports the reload ABI)
```

- The **shell** (`pulp::format::reload::ReloadableShell`) is an ordinary
  `Processor`. The format adapters (VST3 / AU / AUv3 / CLAP / Standalone) wrap it
  exactly like any other plugin. It owns the audio entry point, the host's
  `StateStore`, the RT-safe `ProcessorHotSwapSlot`, and a background watcher
  thread.
- The **logic library** is the DSP, compiled separately as a shared library that
  exports the reload ABI via the `PULP_RELOAD_LOGIC(...)` macro. The shell
  `dlopen`s it and swaps the new `Processor` into the slot on change.

### Threading & RT-safety

- `process()` runs on the audio thread and only ever calls the slot's RT-safe
  `process()` — a non-blocking try-lock; on swap contention it passes one block
  through. It never loads, allocates, or frees.
- A single **background watcher thread** does the `dlopen` + gated swap. The
  slot's writer lock proves no audio reader is inside the old DSP before it is
  destroyed **on the control thread** — never on the audio thread.
- On swap the slot runs the old and new DSP in parallel for a short window
  (~12 ms) and mixes old→new along a **smoothstep crossfade** (zero slope at both
  ends), so the swap is click-free. The retired processor is freed on the control
  thread via `reclaim()` (lock-free hand-off from the audio thread).

### The gates (fail-closed)

Every candidate is gated **before** any audio-visible swap; a rejected reload
leaves the current DSP playing untouched:

1. **reload-ABI version** — refuses a logic built against a different entry-point ABI.
2. **build fingerprint** — refuses a C++-ABI-incompatible build (different compiler/flags).
3. **parameter contract** — refuses a logic whose automatable parameters differ.

The **parameter contract and reported latency are frozen at load** — the host has
already cached them. Only the DSP behind a stable contract hot-swaps; changing the
parameter set or latency needs a full plugin reload.

## Building a reloadable plugin

### 1. The logic (the DSP you edit)

Write a `Processor` and export it with `PULP_RELOAD_LOGIC`:

```cpp
#include <pulp/format/processor.hpp>
#include <pulp/format/reload/reload_abi.hpp>

class MyDsp final : public pulp::format::Processor { /* ... define_parameters / prepare / process ... */ };

PULP_RELOAD_LOGIC(new MyDsp())   // exports abi-version / fingerprint / create / destroy
```

Build it with the `pulp_add_reload_logic()` CMake helper, publishing it to the
path the shell will watch:

```cmake
pulp_add_reload_logic(my-plugin-logic
    SOURCES     my_dsp.cpp
    OUTPUT_NAME logic
    PUBLISH_DIR "$ENV{HOME}/.pulp/my-plugin")   # the watched path
```

### 2. The shell (what the host loads)

The shell factory returns a `ReloadableShell` pointed at the logic path
(`PULP_RELOAD_LOGIC_PATH` overrides it at runtime):

```cpp
#include <pulp/format/reload/reloadable_shell.hpp>

inline std::unique_ptr<pulp::format::Processor> create_my_plugin() {
    return std::make_unique<pulp::format::reload::ReloadableShell>(
        std::string(std::getenv("HOME")) + "/.pulp/my-plugin/logic.dylib");
}
```

Wire it into the formats with the usual `pulp_add_plugin()` + the convention
entry files (`clap_entry.cpp`, `vst3_entry.cpp`, `main.cpp`).

### 3. The dev loop

Build the logic target to publish a fresh copy; the watcher hot-swaps it within
~150 ms. The example ships a `rebuild_logic.sh` that rebuilds + republishes
through the same CMake build (so the build fingerprint matches the shell):

```bash
# edit my_dsp.cpp, then:
cmake --build build --target my-plugin-logic
# a host with the plugin loaded hot-swaps the DSP — no reload, no dropout
```

## Hot-reloading the UI too (thin logic)

A reload can swap the **editor** as well as the DSP: `ReloadableShell::create_view()`
forwards to the active logic, so a logic that overrides `create_view()` brings its
own UI, and `set_on_reloaded()` lets a host rebuild the editor after each swap (the
UI follows the DSP).

But a logic that builds UI links `pulp::view`, and the host already has it — static
-linking would put **two copies** of `pulp::view` in the process (duplicate ObjC
classes, unsafe). So build UI-bearing logic **thin**:

```cmake
pulp_add_reload_logic(my-logic SOURCES my_dsp.cpp RESOLVE_FROM_HOST)  # no SDK archives
# every host that loads it must export its SDK symbols:
pulp_reload_host(MyPlugin_Standalone)   # and _CLAP / _VST3, the capture tool, etc.
```

`RESOLVE_FROM_HOST` links no SDK archives and resolves `pulp::*` from the host at
`dlopen` (one SDK copy in the process); `pulp_reload_host()` exports the host's
symbols so that resolution succeeds. (DSP-only logic doesn't pull `pulp::view`, so
the default static model is fine there — use thin whenever the logic builds UI.)

> **Thin reload is validated for *executable* hosts** (standalone apps, tools).
> A DAW loads a VST3/CLAP *bundle* with `RTLD_LOCAL`, so the bundle's exported
> symbols are not in the scope the thin logic binds against at `dlopen` — in-DAW
> UI hot-reload via thin logic is unproven (likely needs the bundle to publish
> SDK symbols into the loader's global scope; platform- and host-dependent). For
> in-DAW hot-reload today, use the **DSP-only static** model
> (`examples/hot-reload-demo`, REAPER-validated); UI hot-reload is a standalone
> capability until the in-bundle symbol path is proven.

## Worked examples

- **`examples/hot-reload-demo/`** — a DSP-only reloadable plugin: a tremolo logic
  + the shell (CLAP / VST3 / Standalone) + `rebuild_logic.sh`. Load the CLAP in
  REAPER, play audio, flip `kWaveform` from `Sine` to `Square`, run
  `rebuild_logic.sh`, and the tremolo morphs live and click-free.
- **`examples/hot-reload-morph/`** — one reload swaps **both** the DSP *and* the
  editor (blue "WARM" sine tremolo ↔ red "HARSH" square chop) via thin logic +
  `create_view` forwarding. A headless capture tool renders each version's editor
  (PNG) and DSP (WAV) through a real swap as proof.

See each example's `README.md` for the step-by-step.

## Format & platform coverage

The shell is a plain `Processor`, so DSP hot-reload works in **any format** the
host loads it as — VST3, AU v2, AU v3, CLAP, and the standalone app all wrap the
same `Processor::process()` path the slot drives. CLAP is verified live in
REAPER; the other formats share the identical code path and build through the
same `pulp_add_plugin()` declaration.

The mechanism is `dlopen`-based, so it targets native desktop hosts (macOS /
Windows / Linux). It does **not** apply to the Web/WASM target, which has no
`dlopen`; the web story for live iteration is the JS/TS UI hot-reload in
`pulp-render` (see [the rendering strategy](../reference/modules.md)). DSP-level
hot-swap on WASM would need module-instance swapping and is out of scope here.

## Reference

- Engine: `core/format/include/pulp/format/reload/`
  (`reload_abi.hpp`, `processor_hotswap_slot.hpp`, `reload_transaction.hpp`,
  `reload_controller.hpp`, `reloadable_shell.hpp`).
- CMake: [`pulp_add_reload_logic`](../status/cmake-functions.yaml).
- Tests: `test/test_hotswap_slot.cpp`, `test/test_reload_transaction.cpp`,
  `test/test_reload_controller.cpp`, `test/test_reloadable_shell.cpp`.
