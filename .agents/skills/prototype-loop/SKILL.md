---
name: prototype-loop
description: Leveraged-prototype dev loop (`pulp loop`) — single-platform iteration with AOT analyzer, ar-swap, and PR-state monitor. Use when porting an existing UI/bundle to Pulp, doing visual/behavioral parity work, or batching upstream framework gap fixes.
requires:
  tools:
    - gh
    - cmake
---

# Prototype Loop Skill

Codifies the leveraged-prototype dev loop documented in pulp [#940](https://github.com/danielraffel/pulp/issues/940). The loop closed 5 framework gaps in ~2 hours during the 2026-04-28 Spectr-on-`@pulp/react` parity session.

## When to use this skill

Trigger this skill when the user is doing any of:

- **Porting a UI to Pulp** and wants tight visual-feedback iteration (Spectr's WebView editor → native via `@pulp/react` is the canonical example).
- **Gap-finding** — they suspect there are framework gaps and want to enumerate them via an AOT analyzer.
- **Multi-PR upstream coordination** — they're filing a batch of framework PRs and want a tight loop while upstream merges land.
- Hitting **slow cross-platform configure cost** (Skia/Dawn/threejs FetchContent) on changes that only need single-platform validation during iteration.

Do **not** trigger for: bugfixes, cross-platform refactors, or final-landing flows. Those want the cross-platform default.

## When NOT to use this skill

- Pre-merge / landing the consumer's PR — always exit focus mode (or run `shipyard pr` / `pulp pr`) before landing. The ship path validates cross-platform regardless, but exiting focus mode keeps subsequent local iteration honest.
- Bugfixes that touch platform-specific code — a cross-platform build is the cheapest correctness check for those.
- Refactors that span subsystems — single-platform configure can mask a build break on a sibling platform.

## The loop in one breath

```
analyze → file → prototype (ar-swap) → monitor → bump → ship
```

Each step has tooling. The CLI loop is available now; deeper archive-splice, issue-monitoring, and analyzer-lift work should land separately.

## Current surface

| Surface | Status |
|---------|--------|
| `pulp loop` CLI + skill + slash command + docs | available |
| `pulp-ar-swap.sh` ABI-checked archive splice | deferred |
| `pulp loop --watch-issues` PR-monitor | deferred |
| Lift `@pulp/css-adapt`, `pulp-css-analyze`, `extract-html-bundle` | deferred |

## Step 1 — AOT analyze the consumer's bundle

Run `pulp-css-analyze` over the consumer's pre-built React bundle. The output is a coverage report listing unmapped CSS props with occurrence counts.

Reference output shape:
```
Unmapped CSS props (8 total):
  fontFamily            14×   examples: src/Label.tsx:23, src/Header.tsx:8, ...
  textAlign             6×    examples: src/Caption.tsx:12, ...
  ...
```

The occurrence counts are how you prioritize — file the `14×` issue first.

## Step 2 — File framework issues with the right shape

Each gap deserves its own issue. Use this shape:

- **One-line title** — e.g. "Label.font_family_ accessor missing from public surface".
- **Occurrence count** — "Used in 14 places across the Spectr bundle (see analyzer report URL)".
- **Acceptance criteria** — concrete, testable. "`label.set_font_family("Inter")` followed by `label.font_family() == "Inter"` returns true".
- **Bridge-fn signature suggestion** — "`label.set_font_family(string)` mirroring `set_font()`".
- **Cross-link to the analyzer report** that surfaced it.

Filing 6 well-shaped issues with concrete signatures cuts upstream ramp-up dramatically. If you're going to file an umbrella + sub-issues, link the sub-issues from the umbrella's body so the dashboard view is coherent.

## Step 3 — Enter focus mode (`pulp loop`)

```bash
pulp loop                       # auto-detect host platform
pulp loop --platform=macos      # explicit override
pulp loop --status              # report current state
pulp loop --off                 # restore cross-platform mode
```

The CLI persists `[loop] focus_platform = "..."` in `~/.pulp/config.toml`. Subsequent invocations stay pinned until explicitly cleared.

`--no-watch` flips state and exits without entering the watch loop — this is what tests use, and it's also useful when you want the marker but plan to drive builds yourself.

## Step 4 — Local prototype via ar-swap (Slice 2 — deferred, [#946](https://github.com/danielraffel/pulp/issues/946))

Pick the simplest gap from your filed issues. Build the framework patch in another worktree:

```bash
git worktree add ../pulp-fix-issue-X feat/fix-issue-X
cd ../pulp-fix-issue-X
cmake --build build --target pulp-view
```

Once Slice 2 ships, `pulp loop --ar-swap-from ../pulp-fix-issue-X` will splice the changed `.o` files into the pinned SDK's static archive *after* validating header/library ABI. The validator refuses on vtable mismatch — that's the trap that bit during the 2026-04-28 session with `Label::font_family_`.

Until Slice 2 lands, do the ar-swap by hand:
1. Build the patched object file in the other worktree.
2. `nm -gU` the object — make sure exported symbols match what your consumer's compile expects.
3. `ar -r <pinned-sdk>/lib/libpulp-view.a <patched.o>` — splice.
4. Visually validate via `pulp-screenshot` or `pulp loop`'s screencap.
5. **DELETE the local archive after validation.** Otherwise you'll forget it's spliced and ship a binary that doesn't match the upstream pin.

## Step 5 — Monitor upstream PR state flips (Slice 3 — deferred, [#947](https://github.com/danielraffel/pulp/issues/947))

Once Slice 3 ships:
```bash
pulp loop --watch-issues 924,927,931,932
```
will poll `gh pr list` for state flips on PRs referencing the named issues. It fires a notification when each PR transitions to `MERGED`.

Until then, run this in a side terminal:
```bash
watch -n 60 'gh pr list --state merged --search "924 OR 927 OR 931 OR 932" --json number,title,mergedAt'
```

## Step 6 — Bump SDK pin and validate cross-platform

After the upstream batch merges and auto-releases:

1. **Bump the consumer's SDK pin** in one shot (e.g. `0.52.0 → 0.56.0` if v0.53/v0.54/v0.55/v0.56 each came from one of the merged PRs). Update `pulp.toml` / `find_package(Pulp …)`.
2. **Run `pulp loop --off`** — restore cross-platform mode.
3. **Run `shipyard pr`** (or `pulp pr`) — full cross-platform validation gates the merge. This is the contract: focus mode for *iterating*, cross-platform for *landing*.

## Filing follow-up issues

When you discover a new framework gap *while* in focus mode, file it the same way as Step 2. Don't fix it locally and forget — the loop's discipline is upstream-first.

If you fixed something locally to keep iteration moving and the upstream issue isn't merged yet, leave a `// TODO(issue-NNN)` marker and a planning doc note in the consumer. The skill is "file framework issues *immediately*", not "fix and forget".

## Switching modes

- **Enter:** `pulp loop --platform=macos` → all subsequent rebuilds are single-platform.
- **Exit:** `pulp loop --off` → restores cross-platform mode.
- **Land:** `shipyard pr` (or `pulp pr`) → still runs full cross-platform validation before merge regardless of focus state.

The mode is *advisory* at the build layer today. The marker is read by tooling that wants to know "is the developer iterating or landing?", but it does not silently hide cross-platform breakage. Slice 1 keeps the marker semantically clean: "I am iterating, please don't surprise me with cross-platform configure cost."

## Common pitfalls

- **Forgetting to exit focus mode before landing** — `pulp loop --off` is one extra keystroke, but it's how you preserve the "single-platform for iterating, cross-platform for landing" separation. The ship path validates regardless, but the marker should match reality.
- **Locally hacking around a framework gap instead of filing it** — easy to fall into, especially when you're in flow. The skill prompts upstream issue-filing as the preferred path; resist the local-fix temptation.
- **Drifting past merged upstream PRs** — the `--watch-issues` monitor (Slice 3) is the structural answer. Until then, set a 5-minute timer or pin the `gh pr list` watch in a side terminal.
- **ar-swap leaving SDK in inconsistent state** — header vs `.a` vtable mismatch is the trap. Slice 2's helper refuses on mismatch; until then, `nm -gU` the patched object before splicing and delete the local archive after validation.
- **Multiple worktrees on different focus platforms** — the marker is per-`PULP_HOME`, so two worktrees pointing at the same `~/.pulp/config.toml` share the marker. If you're working on macOS in one worktree and want a Linux focus check in another, use `PULP_HOME=/tmp/pulp-linux pulp loop --platform=linux`.

## Slash command

The Claude Code slash command lives at [`.claude/commands/prototype-loop.md`](../../.claude/commands/prototype-loop.md). It asks the user to confirm the focus platform, then orchestrates the loop.

## Reference

- Issue: [#940 — Codify the leveraged-prototype dev loop](https://github.com/danielraffel/pulp/issues/940)
- Validation example: [pulp #924](https://github.com/danielraffel/pulp/issues/924) + spectr#28
- Original 2026-04-28 coverage report: spectr `feature/native-react-editor` branch, `planning/spectr-style-coverage-report.md`
- Companion docs: [`docs/guides/focus-mode.md`](../../docs/guides/focus-mode.md)
