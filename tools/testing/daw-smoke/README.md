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

## Usage
```bash
python3 tools/testing/daw-smoke/reaper_smoke.py \
  --plugin-name "Pulp Hot-Reload Morph" \
  --format vst3 \
  --plugin-path "build/VST3/Pulp Hot-Reload Morph.vst3" \
  --watched-logic ~/.pulp/hot-reload-morph/logic.dylib \
  --initial-logic build/examples/hot-reload-morph/logic.dylib \
  --swap-logic    build/examples/hot-reload-morph/logic-harsh.dylib
```
Add `--check-config` to honor the opt-in toggle (SKIP unless enabled) — CI/gate use.

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
