---
name: daw-smoke
description: Real-DAW (REAPER) functional smoke for reload/editor/format-adapter changes — opt-in, scoped, headless-safe, zero-pollution
---

# daw-smoke — functional reload/editor verification in a real DAW (REAPER)

Use this when you need proof that a **reload / editor / format-adapter** behavior
actually works **inside a host**, beyond what `auval` / `pluginval` /
`clap-validator` give (those prove scan+load, not functional behavior).

Harness: `tools/testing/daw-smoke/reaper_smoke.py` (+ `insert_and_float.lua`).
Full rules: `docs/guides/daw-smoke.md`. CLAUDE.md has the one-paragraph policy.

## When to reach for it
- A headless / standalone / `render_to_png` capture PASSES but you're not sure it
  works in a host. It usually doesn't tell the whole story — see the gotcha below.
- Reload/editor/format-adapter change, especially **before asking a human to test**.
- Opt-in (default OFF), enabled per-machine in `~/.config/pulp/daw-smoke.toml`; a
  ship gate only when `enabled` + `gate` + the diff hits the allowlist. NOT every
  build. `DAW-Smoke: skip reason="..."` trailer bypasses a single commit.

## Gotchas (the expensive lessons)
- **Headless capture hides adapter-only bugs.** The format adapter injects state no
  headless path has. 2026-07-04: it *synthesizes a Bypass parameter* in a host, so
  the reload param-contract gate rejected every in-DAW reload (`parameter contract
  differs`) while every headless capture passed. Only REAPER surfaced it (fixed
  `4a6e048f4`: the contract gate now excludes an adapter-synthesized bypass). If you
  change reload/editor/adapter code, a real-DAW smoke is the check that catches this.
- **`screencapture` returns BLACK frames** in an agent/SSH context (no Screen-
  Recording TCC grant). Do NOT verify visually via screencapture. Verify by scraping
  the plugin reload log from REAPER's captured stdout (`swapped DSP` = applied,
  `reload rejected` + `reject-diff` = fail, `loaded initial logic` = it loaded). Use
  `render_to_png` (e.g. examples/hot-reload-morph capture) for human-viewable visuals.
- **REAPER launch is flaky** (scan dialogs, stuck instances, first-run per portable
  dir). Mitigate: fresh temp portable dir per run, a pre-warm scan launch you kill,
  `pkill -9 -x REAPER` stragglers, timeouts, and an INCONCLUSIVE outcome — never a
  spurious FAIL. A `STATUS: None`/no-FX-load run is INCONCLUSIVE, not PASS.
- **Never pollute the user's machine.** VST3/CLAP scan from a TEMP path (portable
  `reaper.ini` `vstpath`/`clap_path`), never `~/Library/Audio/Plug-Ins`. AU can't be
  scanned from a custom path, so `--format au` installs to the real Components folder
  and MUST uninstall on exit. Cleanup ALWAYS runs (finally): kill REAPER, rm temp
  dirs, uninstall AU. Prefer VST3/CLAP — the reload path is format-agnostic (shared
  editor pump), so they validate the same code the owner runs as AU in Logic.
- **SKIP is never PASS.** Exit codes: 0 PASS / 1 FAIL / 2 SKIP (REAPER absent) /
  3 INCONCLUSIVE. A gate must treat SKIP/INCONCLUSIVE as not-passed.
- **REAPER license is a secret** (`~/.config/pulp/secrets/reaper-license.txt`,
  personal non-commercial) — never commit, echo, or bake into a TartCI golden. Local
  Macs (m1/m3/m5) are already serialized; local is the primary lane.
