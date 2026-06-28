---
name: prototype-loop
description: Enter leveraged-prototype focus mode — focus marker plus normal watch/rebuild loop for visual-parity work and batch upstream framework gap-fixes.
---

# `/prototype-loop` — Leveraged-prototype focus mode

Use the `prototype-loop` skill for the full playbook. This slash command is the Claude Code entry point — it confirms the focus platform with the user, then orchestrates the loop.

## What this command does

1. Asks the user to confirm the focus platform (macOS / Linux / Windows). Defaults to the auto-detected host.
2. Persists the focus marker via `pulp loop --platform=<...>` (or runs `--no-watch` first if the user wants to set the marker without entering the watch loop yet).
3. Runs the normal watch + rebuild loop after persisting the focus marker; use separate screenshot tooling when visual proof is needed.
4. Prompts the user to run `pulp loop --off` and `shipyard pr` (or `pulp pr`) before landing the consumer-side PR.

## Step-by-step

### 1. Confirm the focus platform

```bash
pulp loop --status
```

Read the auto-detected host from the output. If the user wants to override, ask "Which platform do you want to focus on? (macos / linux / windows)" and pass `--platform=<value>`.

### 2. Optionally analyze the consumer's bundle

If the user is doing porting / parity work and hasn't analyzed the consumer's React bundle yet:

> "Have you run `pulp-css-analyze` against this consumer's bundle? It produces a coverage report identifying unmapped CSS props with occurrence counts — the ideal input for filing framework issues."

If they say no, point them at the analyzer. Until the analyzer is lifted into the shared Pulp tooling, the Spectr-side copy lives at `spectr/native-react/tools/pulp-css-analyze`.

### 3. File framework issues from the analyzer output

Coach the user through issue-filing per the skill's Step 2:
- One-line title.
- Occurrence count.
- Acceptance criteria.
- Bridge-fn signature suggestion.
- Cross-link to the analyzer report.

### 4. Enter focus mode

```bash
pulp loop --platform=<chosen> --test
```

Add `--validate`, `--run`, `--target` per the user's needs. The watch loop kicks off — every save triggers rebuild + tests using the current project build configuration.

`pulp loop` uses the current project's build configuration. The persisted marker is advisory for surrounding tooling; it does not rewrite the build graph by itself.

### 5. Optional: ar-swap helper

If the user wants to locally prototype a framework patch from another worktree:

> "The ar-swap helper that validates header/library ABI before splicing is deferred. Until it lands, build the framework patch in another worktree and splice manually with `ar -r`. `nm -gU` the patched object first to verify the symbol set."

### 6. Optional: PR-state monitor

If the user is waiting on multiple upstream PRs to merge:

> "The `--watch-issues N1,N2,...` PR-state monitor is deferred. Until then, run this in a side terminal:
> ```
> watch -n 60 'gh pr list --state merged --search "924 OR 927" --json number,title,mergedAt'
> ```"

### 7. Land

When the user is ready to land:

```bash
pulp loop --off                  # restore cross-platform mode
shipyard pr                      # full cross-platform validation + merge on green
```

Remind the user: "`shipyard pr` validates cross-platform regardless of focus mode, but exit `pulp loop` first so the marker matches reality."

## See also

- Skill: `.agents/skills/prototype-loop/SKILL.md`
- Docs: `docs/guides/focus-mode.md`
