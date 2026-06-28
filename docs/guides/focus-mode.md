# Focus Mode (`pulp loop`)

> Status: **experimental**. The CLI persists focus state and runs the normal
> watch loop; surrounding tooling can read the focus marker for
> single-platform iteration workflows.
>
> Current scope: the focus marker and normal watch loop are implemented today.
> Automatic PR-state monitoring and archive-splice helpers are still deferred;
> their flags are compatibility surfaces that print diagnostics and leave the
> manual playbook below as the current workflow.

## Background

While porting a WebView editor to native via `@pulp/react`, we accidentally
ran a tight dev loop that closed 5 framework gaps in about 2 hours:

1. A bundle analysis pass identified unmapped UI features with occurrence counts.
2. We filed an umbrella issue + 6 sub-issues, each with concrete acceptance criteria, a bridge-fn signature suggestion, and occurrence evidence from the analyzer.
3. We locally prototyped one issue in a sibling framework checkout to validate the shape before upstream picked it up. That prototyping surfaced an additional gap preemptively.
4. A merge-only PR-state monitor (`gh pr list` polling) fired on each upstream PR's state flip.
5. Upstream agents picked up all 6 sub-issues; PRs merged within 2 hours, auto-released as v0.54.0–v0.56.0.
6. Bumped the consumer's SDK pin from `0.52.0 → 0.56.0` in one shot. Massive visible parity gain confirmed via screenshot.

`pulp loop` codifies the available CLI part of this loop as an explicit,
opt-in mode. The broader analyzer, local-prototype, and upstream-monitoring
steps remain a documented manual playbook until their dedicated helpers land.

## When to use focus mode

- **Visual / behavioral parity work.** You're porting a UI to Pulp and want to see "does this match" feedback every save.
- **Gap-finding.** You suspect there are framework gaps and want to enumerate them before filing issues.
- **Multi-PR upstream coordination.** You're filing a batch of framework PRs and want a tight loop to validate each as it merges.
- **Single-platform iteration sessions.** You want a persistent marker that
  says this local loop is intentionally focused on one platform before you
  return to full validation.

## When **not** to use focus mode

- **Bugfixes that touch platform-specific code.** A cross-platform build is the cheapest correctness check; don't trade it away.
- **Final landing.** Always exit focus mode (or run `shipyard pr` / `pulp pr`, which validates cross-platform regardless) before merging.
- **Refactors that span subsystems.** A focused local loop can mask a build
  break on a sibling platform.

## How focus mode works

`pulp loop` does two things:

1. **Records intent.** Writes `[loop] focus_platform = "macos"` (or `linux` / `windows`) to `~/.pulp/config.toml`. The marker is the explicit signal — if it's missing, the CLI is in cross-platform mode.
2. **Drives a watch loop.** Reuses the same `WatchOptions` plumbing as
   `pulp dev` (build, optional test, optional validate, optional relaunch)
   using the current project's normal build configuration.

## CLI surface

```bash
pulp loop                          # Enter focus mode on the auto-detected host
pulp loop --platform=macos --test  # Mark macOS focus + run tests on every save
pulp loop --status                 # Print current focus state
pulp loop --off                    # Restore cross-platform mode
```

Full flag reference: [`docs/reference/cli.md#loop`](../reference/cli.md#loop).

## Recommended playbook

The leveraged-prototype loop has five steps. Today, `pulp loop` automates the
focus marker plus watch loop; the surrounding analyzer, local-prototype,
PR-monitor, and SDK-bump steps are still manual.

### 1. Analyze the consumer's bundle

Walk the consumer's already-built JS / React bundle with the project's
available analysis tooling. The useful output is a coverage report listing
unmapped UI features with occurrence counts and source locations.

### 2. File framework issues with the right shape

Each gap deserves its own issue. The shape that worked on 2026-04-28:

- **One-line title** — e.g. "Label.font_family_ accessor missing from public surface".
- **Occurrence count** — "Used in 14 places across the Spectr bundle (see analyzer report)".
- **Acceptance criteria** — concrete, testable.
- **Bridge-fn signature suggestion** — "`label.set_font_family(string)` mirroring `set_font()`".
- **Cross-link to the analyzer report** that surfaced it.

Filing 6 well-shaped issues with concrete signatures cuts the framework-author's ramp-up cost dramatically.

### 3. Locally prototype one issue

Pick the simplest gap. Build the framework patch in another worktree and run
the consumer against that local framework build. Keep ABI-sensitive changes
small and validate the patched SDK before treating the result as proof.

### 4. Monitor upstream PR state flips

Automatic `pulp loop --watch-issues` monitoring is deferred today. The current
CLI recognizes the flag, prints a diagnostic, and continues the normal loop.

Use GitHub issue and PR searches to watch the upstream batch manually. The
important signal is when each PR linked to a filed gap merges and the SDK
release that contains it is available to the consumer.

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
