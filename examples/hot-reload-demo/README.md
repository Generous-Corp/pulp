# Hot-Reload Demo — live DSP hot-swap in a DAW

This example is a **reloadable plugin**: the part a host loads (the *shell*) and
the DSP it runs (the *logic*) are compiled separately. You can recompile the DSP
and have it swap into the running plugin **while audio keeps playing** — no
reload, no dropout. It is the DAW-integration proof for Pulp's DSP hot-reload
(`pulp::format::reload`).

```
host (REAPER / Logic / standalone)
        loads
          ▼
  Pulp Hot-Reload Demo  ── the SHELL (ReloadableShell)
   • owns the audio entry, the StateStore, the RT-safe hot-swap slot
   • watches  ~/.pulp/hot-reload-demo/logic.dylib
   • dlopens + gates + swaps it on change
          ▲ swaps in
          │
   logic.dylib  ── the DSP you EDIT (logic_tremolo.cpp)
```

## Files

| File | Role |
|------|------|
| `logic_tremolo.cpp` | The hot-swappable DSP. **Edit this.** A tremolo; flip `kWaveform` sine↔square to hear a live morph. |
| `hot_reload_shell.hpp` | The shell factory (`create_hot_reload_shell`) — a `ReloadableShell` pointed at the logic path. |
| `clap_entry.cpp` / `vst3_entry.cpp` / `main.cpp` | CLAP / VST3 / standalone wrappers around the shell. |
| `rebuild_logic.sh` | Recompiles + republishes the logic so a loaded plugin hot-swaps it. |

## Build

```bash
cmake --build build --target HotReloadDemo_CLAP HotReloadDemo_Standalone hot-reload-demo-logic
```

The logic build publishes `logic.dylib` to `~/.pulp/hot-reload-demo/` (the path
the shell watches). The shell mirrors the logic's parameter contract (`Depth`,
`Rate`) at load — that contract is fixed; only the DSP behind it hot-swaps.

## Hot-reload it in REAPER

1. Install the CLAP and rescan:
   ```bash
   cp -R "build/CLAP/Pulp Hot-Reload Demo.clap" ~/Library/Audio/Plug-Ins/CLAP/
   ```
   In REAPER: *Options → Preferences → Plug-ins → CLAP → Re-scan*. The plugin
   appears as **Pulp Hot-Reload Demo (Pulp)**.
2. Put it on a track with audio playing through it (any source). You'll hear a
   smooth sine tremolo.
3. Edit `logic_tremolo.cpp` — change `kWaveform` from `Wave::Sine` to
   `Wave::Square` — and run:
   ```bash
   examples/hot-reload-demo/rebuild_logic.sh
   ```
4. Within ~150 ms the tremolo morphs to a hard square chop. **The plugin was
   never reloaded** and audio never stopped.

> The swap is **click-free**: the slot runs the old and new DSP in parallel for
> a short window (~12 ms) and mixes old→new along a smoothstep ramp, so neither
> the swap instant nor the fade end produces a discontinuity. The retired DSP is
> freed on the control thread, never on the audio thread (see
> `ProcessorHotSwapSlot`'s crossfade + `reclaim()`).

## Or without a DAW

```bash
./build/examples/hot-reload-demo/HotReloadDemo   # standalone; opens the default device
```
Then edit + `rebuild_logic.sh` as above. (Audio plays out your default device.)

## How it stays safe

The shell never loads or frees DSP on the audio thread. A background watcher
does the `dlopen`, and the swap is gated before any audio-visible change:

- **reload-ABI version** — refuses a logic built against a different entry-point ABI.
- **build fingerprint** — refuses a C++-ABI-incompatible build (different compiler/flags).
- **parameter contract** — refuses a logic whose automatable parameters differ.

A rejected reload leaves the current DSP playing untouched. The audio thread
takes a non-blocking lock for the whole `process()`; on swap contention it
passes one block through rather than ever blocking. See
`core/format/include/pulp/format/reload/`.
