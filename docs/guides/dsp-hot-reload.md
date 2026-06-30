# DSP Hot-Reload

Recompile a plugin's DSP and have it swap into the **running** plugin — while the
host keeps the instance and the audio stream alive. No reload, no rescan, no
dropout. This is the audio analogue of the JS/UI hot-reload Pulp already does for
scripted widgets: edit, rebuild, hear the change in well under a second.

> Status: the engine and the DAW-integration shell are in `core/format/reload/`.
> Verified live in REAPER (CLAP). The shell is format-agnostic, so the same
> mechanism applies to every format adapter (see [Format & platform
> coverage](#format--platform-coverage)).

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

## Worked example

`examples/hot-reload-demo/` is a complete reloadable plugin: a tremolo logic
(`logic_tremolo.cpp`) + the shell (CLAP / VST3 / Standalone) + `rebuild_logic.sh`.
Load the CLAP in REAPER, play audio, flip `kWaveform` from `Sine` to `Square`,
run `rebuild_logic.sh`, and the tremolo morphs live and click-free. See that
example's `README.md` for the step-by-step.

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
