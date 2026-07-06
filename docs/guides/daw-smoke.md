# Real-DAW smoke (REAPER)

A functional smoke test that loads a Pulp plugin in **REAPER**, drives a hot-swap,
and asserts the reload was **accepted + applied** in a real host. It is **opt-in**
and scoped — not part of every build.

> Living document. The *rules* below (when to run, the path allowlist) are meant to
> evolve — edit this file as the practice matures.

## Why it exists (and why the validators aren't enough)

`auval` / `pluginval` / `clap-validator` (the [Plugin Install Policy](../../CLAUDE.md)
gate) prove a plugin **scans, loads, and passes format conformance**. They do **not**
prove that a *functional, interactive* behavior actually happens inside a host.

Some bugs only appear in a real host because the **format adapter injects state that
no headless / standalone / render-to-png path has**. The motivating case (2026-07-04):
the morph editor "wouldn't update live" in Logic while every headless capture passed.
Driving the morph VST3 in REAPER exposed the cause — the adapter *synthesizes a
Bypass parameter* in a host, so the reload param-contract gate rejected every in-DAW
reload (`parameter contract differs`). Headless paths have no adapter, no synthesized
bypass, so they couldn't see it. Fixed in `4a6e048f4`.

**Rule of thumb:** "passes the validators" ≠ "works in a host." For reload / editor /
format-adapter behavior, a real-DAW smoke is the check that catches the adapter-only
class of bug.

## When to run it — and when NOT to

**Opt-in, default OFF.** Enable per-machine in `~/.config/pulp/daw-smoke.toml`:

```toml
[daw-smoke]
enabled = true    # run the smoke at all (default false)
gate    = false   # make it a ship gate when it runs (default false)
strict  = false   # fail (not SKIP) if REAPER is unavailable (default false)
```

Even when enabled, run it **only** when BOTH hold:

1. The change touches a **reload / editor / format-adapter** surface — the allowlist
   (extend here as needed):
   - `core/format/**/reload/**`, `reloadable_shell.hpp`, `param_contract.hpp`
   - `core/format/**/view_bridge.*`, `gpu_host_select.hpp`, `plugin_view_host*`
   - the format adapters (`vst3_*`, `au_*`, `clap_*`) and editor view controllers
2. Functional confidence matters at this moment: **initial development of a behavior**
   (before asking a human to test — save their time) or a **risky** reload/editor change.

**Do NOT run it** on: every build, doc-only changes, non-reload code, trivial edits.

## As a ship gate — only at certain times

The smoke becomes a **gate** only when `enabled` AND `gate` are true AND the PR diff
hits the allowlist. Otherwise it is advisory / on-demand. Even as a gate it degrades
to a **loud SKIP** when REAPER is unavailable (unless `strict = true`). Bypass for a
single commit with a tip-commit trailer, mirroring the other gates:

```
DAW-Smoke: skip reason="..."
```

A **SKIP is never a PASS** — the runner uses distinct exit codes so a gate can tell
them apart: `0` PASS · `1` FAIL · `2` SKIP (REAPER not installed) · `3` INCONCLUSIVE.

## How to run it

```bash
python3 tools/testing/daw-smoke/reaper_smoke.py \
  --plugin-name "Pulp Hot-Reload Morph" --format vst3 \
  --plugin-path "build/VST3/Pulp Hot-Reload Morph.vst3" \
  --watched-logic ~/.pulp/hot-reload-morph/logic.dylib \
  --initial-logic build/examples/hot-reload-morph/logic.dylib \
  --swap-logic    build/examples/hot-reload-morph/logic-harsh.dylib \
  --check-config   # honor the opt-in toggle (SKIP unless enabled)
```

See [`tools/testing/daw-smoke/README.md`](../../tools/testing/daw-smoke/README.md).

## Guarantees (so it's safe to run anywhere)

- **No pollution.** VST3/CLAP scan from a temp path via a fresh REAPER portable dir —
  never writes your REAPER config or `~/Library/Audio/Plug-Ins`. `--format au`
  installs to the real Components folder and **always uninstalls** on exit.
- **Cleanup always runs** — kills spawned REAPER, removes temp dirs, uninstalls any AU.
- **Headless-safe.** Verifies by scraping the plugin reload log, **never**
  `screencapture` (which returns black frames without a Screen-Recording grant). Use
  `render_to_png` for human-viewable visuals.

## Local vs CI (TartCI)

- **Local machines are the primary lane.** Validate *before* building/PR; the local
  Macs already have REAPER serialized. This is the "don't waste a human's time" check.
- **TartCI VM is optional** (not wired in). If ever added, the golden must install +
  serialize REAPER at provision time from the license at
  `~/.config/pulp/secrets/reaper-license.txt` — a **personal, non-commercial** license
  that must **never** be committed or baked into a shared/public golden image.
