# Focus Mode (`pulp loop`)

> Status: **experimental** — Slice 1 of [#940](https://github.com/danielraffel/pulp/issues/940). The CLI surface, persistence model, and watch loop are in place. The `--ar-swap-from` and `--watch-issues` helpers, plus the lifted `@pulp/css-adapt` / `pulp-css-analyze` / `extract-html-bundle` packages, ship in follow-up slices ([#946](https://github.com/danielraffel/pulp/issues/946), [#947](https://github.com/danielraffel/pulp/issues/947), [#948](https://github.com/danielraffel/pulp/issues/948)).

## Background

On 2026-04-28, while porting Spectr's WebView editor to native via `@pulp/react` ([#924](https://github.com/danielraffel/pulp/issues/924), spectr#28), we accidentally ran a tight dev loop that closed 5 framework gaps in ~2 hours:

1. An AOT analyzer (`pulp-css-analyze`) walked the consumer's React bundle and produced a coverage report identifying unmapped CSS props with occurrence counts.
2. We filed an umbrella issue + 6 sub-issues, each with concrete acceptance criteria, a bridge-fn signature suggestion, and occurrence evidence from the analyzer.
3. We locally prototyped one issue via ar-swap into the pinned SDK to validate the shape before upstream picked it up. That prototyping surfaced an additional gap preemptively.
4. A merge-only PR-state monitor (`gh pr list` polling) fired on each upstream PR's state flip.
5. Upstream agents picked up all 6 sub-issues; PRs merged within 2 hours, auto-released as v0.54.0–v0.56.0.
6. Bumped the consumer's SDK pin from `0.52.0 → 0.56.0` in one shot. Massive visible parity gain confirmed via screenshot.

`pulp loop` codifies this loop as an explicit, opt-in mode.

## When to use focus mode

- **Visual / behavioral parity work.** You're porting a UI to Pulp and want to see "does this match" feedback every save.
- **Gap-finding.** You suspect there are framework gaps and want to enumerate them before filing issues.
- **Multi-PR upstream coordination.** You're filing a batch of framework PRs and want a tight loop to validate each as it merges.
- **Slow cross-platform configure cost.** Skia/Dawn/threejs FetchContent are taking minutes per cycle and you only need to validate on one platform during iteration.

## When **not** to use focus mode

- **Bugfixes that touch platform-specific code.** A cross-platform build is the cheapest correctness check; don't trade it away.
- **Final landing.** Always exit focus mode (or run `shipyard pr` / `pulp pr`, which validates cross-platform regardless) before merging.
- **Refactors that span subsystems.** The single-platform configure can mask a build break on a sibling platform.

## How focus mode works

`pulp loop` does three things:

1. **Records intent.** Writes `[loop] focus_platform = "macos"` (or `linux` / `windows`) to `~/.pulp/config.toml`. The marker is the explicit signal — if it's missing, the CLI is in cross-platform mode.
2. **Drives a watch loop.** Reuses the same `WatchOptions` plumbing as `pulp dev` (build → optional test → optional validate → optional relaunch) but pinned to the focus platform's toolchain.
3. **Surfaces deferred slices.** `--watch-issues` and `--ar-swap-from` are accepted as flags today and print "deferred" hints pointing to the follow-up issues, so the surface is forward-compatible.

## CLI surface

```bash
pulp loop                          # Enter focus mode on the auto-detected host
pulp loop --platform=macos --test  # Pin to macOS + run tests on every save
pulp loop --status                 # Print current focus state
pulp loop --off                    # Restore cross-platform mode
```

Full flag reference: [`docs/reference/cli.md#loop`](../reference/cli.md#loop).

## Recommended playbook

The leveraged-prototype loop has five steps. Each leans on tooling that either ships today or arrives in a named follow-up slice.

### 1. AOT-analyze the consumer's bundle

Walk the consumer's already-built JS / React bundle with `pulp-css-analyze` (lifting in Slice 4, [#948](https://github.com/danielraffel/pulp/issues/948)). The output is a coverage report listing unmapped CSS props with occurrence counts.

Until Slice 4 lands, run the Spectr-side copy at `tools/pulp-css-analyze` in any consumer using `@pulp/react`.

### 2. File framework issues with the right shape

Each gap deserves its own issue. The shape that worked on 2026-04-28:

- **One-line title** — e.g. "Label.font_family_ accessor missing from public surface".
- **Occurrence count** — "Used in 14 places across the Spectr bundle (see analyzer report)".
- **Acceptance criteria** — concrete, testable.
- **Bridge-fn signature suggestion** — "`label.set_font_family(string)` mirroring `set_font()`".
- **Cross-link to the analyzer report** that surfaced it.

Filing 6 well-shaped issues with concrete signatures cuts the framework-author's ramp-up cost dramatically.

### 3. Locally prototype one issue via ar-swap

Pick the simplest gap. Build the framework patch in another worktree, then `pulp loop --ar-swap-from <ref>` (Slice 2, [#946](https://github.com/danielraffel/pulp/issues/946)) swaps the changed `.o` files into the pinned SDK's static archive without doing a full SDK release cycle.

The ar-swap helper validates header/library ABI before swapping and refuses on vtable mismatch — that's the lesson learned from the `Label::font_family_` ABI-mismatch trap during the 2026-04-28 session.

Until Slice 2 lands, do the ar-swap by hand in your local SDK and verify the vtable manually.

### 4. Monitor upstream PR state flips

`pulp loop --watch-issues 924,927,931,...` (Slice 3, [#947](https://github.com/danielraffel/pulp/issues/947)) polls `gh pr list` for state flips on PRs referencing the named issues. The notification fires when each PR transitions to `MERGED`.

Until Slice 3 lands, run `gh pr list --search "924 OR 927 OR ..." --json state,number,title --jq '.[] | select(.state == "MERGED")'` in a side terminal.

### 5. Bump the SDK pin and validate cross-platform

After the upstream batch merges and auto-releases, bump the consumer-side SDK pin in one shot. Then:

1. **Run `pulp loop --off`** — restore cross-platform mode.
2. **Run `shipyard pr` (or `pulp pr`)** — full cross-platform validation gates the merge.

This is the "single-platform for iterating, cross-platform for landing" separation made explicit.

## Persistence + state

The focus marker is `[loop] focus_platform` in `~/.pulp/config.toml`. Read it via `pulp loop --status`. Clear it via `pulp loop --off`.

If `PULP_HOME` points elsewhere (e.g. `PULP_HOME=/tmp/test-home pulp loop --status`), the marker lives under that home — useful in tests.

## Why this is Pulp-specific

The pattern works because Pulp has:

- A pinned-SDK consumer model (`find_package(Pulp X.Y.Z)`).
- An auto-release flow (PR merge → tagged SDK release in <1h).
- A clean bridge surface (`setX`/`createX`) that's analyzer-friendly.
- Multi-target Shipyard CI that handles cross-platform validation at the framework PR boundary, freeing consumers to iterate on one platform.

Frameworks without these traits won't benefit equally.

## See also

- [`pulp dev`](../reference/cli.md#dev) — the cross-platform sibling. Same watch-loop plumbing, no focus-mode marker.
- [`shipyard pr` / `pulp pr`](../reference/cli.md#pr) — the ship path that always validates cross-platform.
- [`.agents/skills/prototype-loop/SKILL.md`](../../.agents/skills/prototype-loop/SKILL.md) — agent-facing playbook.
- [`.claude/commands/prototype-loop.md`](../../.claude/commands/prototype-loop.md) — Claude Code slash command.
