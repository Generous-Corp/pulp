# DAW smoke (REAPER) — functional reload/editor verification

A real-DAW functional smoke for Pulp **reload / editor / format-adapter** changes.
It loads a Pulp plugin in REAPER, drives a hot-swap, and asserts the reload was
**accepted + applied** by scraping the plugin's reload log.

**This complements — never replaces — `auval` / `pluginval` / `clap-validator`.**
Those prove a plugin scans + loads. This proves a *functional* behavior actually
happens inside a host. Some bugs only appear in a host because the format adapter
injects state no headless path has (e.g. a synthesized bypass parameter that made
the reload contract gate reject every in-DAW reload — fixed 2026-07-04). See
[`docs/guides/daw-smoke.md`](../../../docs/guides/daw-smoke.md) for the full rules.

## When to run it (opt-in, NOT every build)
Default OFF. Enable per-machine in `~/.config/pulp/daw-smoke.toml`. Run it for
initial functional validation of reload/editor behavior (before asking a human to
test) and for risky reload/editor/format-adapter changes. Not trivial edits.

## Modes

### `reload` (default) — hot-swap a watched DSP artifact
```bash
python3 tools/testing/daw-smoke/reaper_smoke.py \
  --mode reload \
  --plugin-name "Pulp Hot-Reload Morph" \
  --format vst3 \
  --plugin-path "build/VST3/Pulp Hot-Reload Morph.vst3" \
  --watched-logic ~/.pulp/hot-reload-morph/logic.dylib \
  --initial-logic build/examples/hot-reload-morph/logic.dylib \
  --swap-logic    build/examples/hot-reload-morph/logic-harsh.dylib
```

### `live-plugin-swap` — swap a hosted plugin INSTANCE in a SignalGraph
Loads a Pulp host that hosts another plugin in a `SignalGraph`, then writes a
swap request to the path the host watches and asserts the swap **committed with
no dropout** by scraping the host's `[live-swap] committed` marker (a refusal
logs `live plugin swap refused` → FAIL).
```bash
python3 tools/testing/daw-smoke/reaper_smoke.py \
  --mode live-plugin-swap \
  --plugin-name "Pulp Graph Host" \
  --format vst3 \
  --plugin-path "build/VST3/Pulp Graph Host.vst3" \
  --watched-swap-request ~/.pulp/graph-host/swap-request.txt \
  --swap-target "com.example.replacement"
```
The CI-runnable, DAW-free mirror of this mode is the headless continuity test
`test/test_signal_graph_live_swap_continuity.cpp`, which drives the same
stage + `prepare_swap` commit through `process()` and asserts sample continuity
across the swap block for every hosted format (VST3/AU/CLAP/LV2). This REAPER
mode is the local-only in-host proof.

Add `--check-config` to honor the opt-in toggle (SKIP unless enabled) — CI/gate use.

## Audio etiquette
This launches REAPER with a Pulp plugin on a track and may press Play, so audio
can be active out of the default device during the run. The harness **announces**
the source + expected duration before launching, **caps** the run at `--timeout`
seconds, and **tears down** (kills REAPER) on every exit path.

Exit codes: `0` PASS · `1` FAIL (reload rejected / didn't apply) · `2` SKIP (REAPER
not installed) · `3` INCONCLUSIVE (flaky launch). **A SKIP is never a PASS.**

## Design guarantees
- **Zero pollution:** VST3/CLAP scan from a temp path via a fresh REAPER portable
  dir — never writes your REAPER config or `~/Library/Audio/Plug-Ins`. `--format au`
  installs to the real Components folder and always uninstalls on exit.
- **Cleanup always runs:** kills spawned REAPER, removes temp dirs, uninstalls any AU.
- **Headless-safe:** verifies by scraping the reload log, never `screencapture`
  (which returns black frames without a Screen-Recording grant). Use
  `render_to_png` for human-viewable visuals.

## Requirements
REAPER installed + licensed (local machines m1/m3/m5 already are). Absent → SKIP.
