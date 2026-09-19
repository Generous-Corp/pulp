# Agent efficiency audits

Pulp keeps small, evidence-backed checks for recurring agent mistakes. Each
check has an owner, a regression test, an evidence record, and a review date.
The review asks whether the failure still occurs and whether the check catches
real defects without adding noise. Retire a rule when its evidence disappears;
do not preserve it merely because it is already installed.

The current shell portability checks cover two silent zsh hazards: unbraced
`$name:path` expansions, and Bash-only `${PIPESTATUS[...]}` in zsh sessions.
Run them with:

```sh
python3 tools/scripts/shell_portability_check.py tools/ci scripts tools/scripts
```

The same checker accepts an ad-hoc command with `--command` or `--stdin`. The
repository-local Codex/Claude PreToolUse hooks use that advisory mode, so a
command typed directly by an agent receives the same warning as a tracked
script. It follows `PULP_AGENT_SHELL` or `SHELL` (`zsh`/`bash`) and stays
silent for unknown shells, so a valid Bash command is not reported as a zsh
defect. It is quote-aware and
does not treat single-quoted query data as shell syntax. The hooks always
return success; the regression gate and the user's shell remain authoritative.

The checker also catches a narrower reporting failure: a `cmake --build` or
`ctest` command whose result is sent through a final output consumer such as
`tail`, `head`, `grep`, `sed`, `awk`, or `tee`. Without `pipefail`, that
consumer's zero exit code can make a failed build or test run look successful;
this was observed in recent M3/M5 agent sessions. The advisory points to two
small repairs: enable `set -o pipefail` before the pipeline, or capture Bash
`${PIPESTATUS[0]}` / zsh `${pipestatus[1]}` on the immediately following line
before running another command. It recognizes an already-enabled `pipefail`
and the direct status-capture pattern, so it does not nag about a pipeline
whose producer status is being preserved. It intentionally does not attempt
to prove arbitrary shell control flow, and it does not flag unrelated command
output filters. Prefer retaining the full build/test log and filtering it only
after the command's status has been recorded when a short summary is useful.

Recent M5 evidence included a zsh `${PIPESTATUS[0]}` test that surfaced as
`unknown condition: -eq`, and `$base:core/...` / `$base:test/...` expansions
that silently lost the first character after the colon. These were outside
the tracked-file surface and are the reason command mode exists.

The worktree helper accepts both `PULP_WT_ROOT` and the fleet's existing
`PULP_WORKTREES_ROOT` spelling, with the former taking precedence. This keeps
age and budget inventory pointed at the actual M3/M5 worktree root. The
documented total-budget option remains report-only until an affirmative,
owner-and-process-aware cleanup classifier is implemented.

For a new recurring nuisance, record three independent root families, the
supported owner of the fix, a synthetic regression case, and a review date in
`tools/scripts/shell_portability_rules.json`. Keep the check narrow enough that
an agent can explain one actionable fix for every finding.

The local diff-coverage gate also protects its evidence lifecycle. A
`build-cov/` directory is reusable only when it carries a content identity of
the current worktree and a previous run reached a successful diff-coverage
result. The identity includes tracked diffs and non-ignored untracked-file
content; ignored generated/dependency inputs remain the responsibility of
their generator. The gate also refuses to publish if the worktree changes
during the run.
`tools/scripts/local_diff_cover.sh` removes an unproven or changed directory
before configuring. This addresses a measured failure mode in recent M3
sessions where stale objects and `.profraw` files made true coverage near 89%
appear as 22–36%. The identity file is written only after success, so an
interrupted run self-invalidates on the next attempt. Revisit or retire this
guard if the coverage toolchain begins providing an authoritative build/profile
identity of its own.

The recommended audit prompt is:

> Review a deduplicated, bounded sample of your recent development history.
> Record the owning system, evidence anchor, wasted calls/time, whether a
> supported route already existed, and the smallest prevention. Check for
> unnecessary full test runs, repeated configure/build work, SDK or CLI drift,
> merge-queue delays, permission loops, and false human blockers. Do not export
> transcript text or secrets. Recommend implementation only when the pattern
> appears in at least three independent root families and has a clear owner.

## Current next-goal slices

The Rust-native `pulp build` path now uses the same governed-build owner as the
source checkout's Shipyard lane. In a source checkout it invokes
`tools/ci/governed-build.sh`, which owns TartCI lease admission, heartbeat, and
release; in a generated or consumer checkout it applies the existing tier-0
memory/CPU bound and honors an inherited `PULP_BUILD_JOBS` share. Explicit
`-j`/`--parallel` values are caps, never permission to exceed the host share,
and are removed from the raw CMake invocation. Native and WAM/WCLAP builds use
the same planner and preserve child exit codes. This reuses the existing
governor rather than creating a second lease implementation.

The M3/M5 Shipyard handoff wedge is evidence-only for now. Pulp does not own
the canonical `GEN-*` validator, and changing `auto_handoff` before that
upstream validator is fixed would recreate unmanaged PRs. The bounded evidence
and canary requirements live in
`planning/friction/2026-09-18-shipyard-workstream-validator-evidence.md`.
