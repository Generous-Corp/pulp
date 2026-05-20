# Coverage Compliance Status

Last reviewed: 2026-04-30 02:20 EDT

This is the durable tracker for the repo-wide coverage compliance
program under `#641`.

## Current Phase

- Phase 1 - Representation / Codecov truth: complete.
- Phase 2 - Gap planning: complete enough to execute from the tracker
  map and tranche issues.
- Phase 3 - Gap closure: active. The work is now small, focused
  coverage PRs that move represented components toward their tier
  targets.

The authoritative live tracker remains `#641`. The high-leverage
host/format/view tracker remains `#493`. Component tranche trackers are:

- `#640` audio / platform
- `#642` events
- `#643` CLI / tools
- `#644` ship
- `#645` MIDI / signal
- `#646` render
- `#493` host / format / view

## Goal

Get Codecov as close as practical to the intended first-party local
source surface, then use that corrected baseline to plan and close the
real measured test gaps.

Target tiers remain unchanged in `ci/coverage-targets.yaml`:

- `80%`: `audio`, `format`, `host`, `midi`, `signal`, `platform`
- `70%`: `render`, `view`, `cli`
- `50%`: `events`, `runtime`, `state`, `canvas`, `osc`, `ship`, `tools`

## Latest Complete Codecov Snapshot

Latest complete Codecov `main` report observed while updating this doc:

- commit: `e9c994d1bb2c42f1a50e4775a5886b6348808fb0`
- PR: `#851` - `test(signal): cover meter channel-count guards`
- state: complete
- overall tracked coverage: `48.40%`
- covered lines: `34,608 / 71,492`
- missed lines: `35,511`
- partial lines: `1,373`
- files: `560`
- sessions: `4`

Current component coverage from the same Codecov API snapshot:

| Component | Coverage | Target | Gap |
| --- | ---: | ---: | ---: |
| `audio` | `37.20%` | `80%` | `42.80` |
| `platform` | `38.95%` | `80%` | `41.05` |
| `host` | `44.16%` | `80%` | `35.84` |
| `cli` | `40.84%` | `70%` | `29.16` |
| `format` | `51.92%` | `80%` | `28.08` |
| `view` | `45.17%` | `70%` | `24.83` |
| `midi` | `55.72%` | `80%` | `24.28` |
| `render` | `64.43%` | `70%` | `5.57` |
| `signal` | `75.40%` | `80%` | `4.60` |
| `tools` | `46.93%` | `50%` | `3.07` |

Components currently above their configured Phase 3 tier floor in this
snapshot include `canvas`, `events`, `runtime`, `state`, `osc`, and
`ship`. Non-tier reporting components include `android`, `apple`,
`linux`, `windows`, `dsl`, and `inspect`.

## Queue State

The active queue is intentionally fluid during Phase 3. Treat `#641`
and the component tracker comments as the live source of truth, then
verify with GitHub before reporting or merging:

```bash
gh pr list --repo danielraffel/pulp --label codecov --state open \
  --json number,title,headRefOid,mergeStateStatus,mergeable,updatedAt,url
```

Do not preserve stale point-in-time queue lists in this document. When
a queue snapshot is useful, post it as a tracker comment with concrete
dates, PR numbers, and head SHAs.

## Phase 3 Operating Loop

Phase 3 should not become a wait-for-CI loop. Keep this cycle running
until the finish criteria below are met:

1. Monitor open `codecov` PRs.
2. If a PR is `CLEAN` and checks are complete, squash merge it manually,
   delete the branch, and update `#641` plus the component tracker.
3. If a check fails, inspect the failed job logs, patch the branch, push,
   and comment with the root cause and fix.
4. If all open PRs are only pending or queued, immediately continue the
   next focused coverage tranche in a separate worktree.
5. Keep tranche scope small: one subsystem slice, focused local
   validation, Codecov patch/diff proof, tracker link, then PR.

Use `shipyard pr` as the PR orchestrator. As of 2026-05-20, Namespace
macOS routing is disabled for cost control; macOS CI runs on the
self-hosted `pulp-build` runners, while Linux/Windows advisory legs use
GitHub-hosted runners unless repo variables say otherwise. Do not pass
`runner_provider=namespace` for Phase 3 coverage PRs.

Subagents are useful for this loop when their write scopes are
disjoint. One agent can monitor merge/failure state while other agents
prepare non-overlapping coverage tranches in separate worktrees.

## Phase 3 Finish Line

Phase 3 is done when the planned represented-surface tranches have been
implemented or explicitly deferred, and the remaining gaps are
documented rather than silently ignored.

Practical finish criteria for the current plan:

- `audio`, `platform`, `host`, `format`, `midi`, and `signal` move
  materially toward the `80%` tier, with any hardware-bound platform
  shims either covered through deterministic seams or documented as
  deferred.
- `view`, `render`, and `cli` move toward the `70%` tier, prioritizing
  large represented files before small tail cleanup.
- `tools` crosses the `50%` infrastructure floor, then further tooling
  slices are handled only when they are high leverage or support the
  coverage system itself.
- `canvas`, `events`, `runtime`, `state`, `osc`, and `ship` stay above
  their `50%` infrastructure floor while ordinary feature work continues
  to satisfy the required diff-coverage gate.
- Deferred surfaces remain explicit in the tracker set: authored
  JavaScript (`#659`), Node bindings plus shell/PowerShell
  classification (`#657`), optional native surfaces (`#737`), and
  mobile runtime / simulator coverage (`#77`).

## Next Tranche Order

Use the latest complete Codecov file ranking and the tracker issues to
choose the next small PR. As of the `c05384aa` snapshot, the highest
remaining misses are concentrated in:

- `#640`: `audio` / `platform`, especially platform device shims and
  deterministic platform helpers.
- `#493`: `host` / `format` / `view`, especially real slot adapters,
  `widget_bridge.cpp`, and macOS view-host surfaces.
- `#643`: `cli` / `tools`, especially large command modules and
  low-coverage Python helpers.

## Recent Phase 3 Tranches

| Date | Branch | SHA | Scope | Local validation | PR state | Next action |
| --- | --- | --- | --- | --- | --- | --- |
| 2026-05-20 | `feature/phase3-codecov-batch-755` | `062de20e0` | Runtime `ScopedNoAlloc` nested-depth and thread-local guard coverage in `pulp-test-runtime-utils`; Shipyard mac target restored to local validation after the Namespace cutover. | `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF`; `cmake --build build --target pulp-test-runtime-utils -j$(sysctl -n hw.ncpu)`; `./build/test/pulp-test-runtime-utils "[runtime][scoped_no_alloc]"`; skill-sync report; version-bump report; `git diff --check`. | PR #2473 opened; pending GitHub queue. | Monitor #2473 required checks and merge on green. |
- `#645`: `midi` / `signal`, with `signal` close to the 80% tier and
  `midi` still led by platform MIDI shims and MPE tracker paths.
- `#646`: `render`, now close to the 70% tier and best handled through
  GPU-off-safe helper targets when possible.

Keep the tranche size small: one subsystem slice, local focused
validation, Codecov patch/diff proof, tracker update, then merge when
GitHub branch protection is green and `mergeStateStatus` is `CLEAN`.

## Control-Plane Invariants

- Every top-level `core/*` directory must have matching subsystem
  entries in `codecov.yml` `flags:` and `component_management:`.
- Platform axes must match live repo path conventions, including
  `core/**/win/**` for Windows.
- Swift upload artifacts must be repo-relative LCOV with `SF:` paths
  rooted under `apple/Sources/**`.
- Android Kotlin coverage must run on the always-on Coverage workflow,
  not only on path-gated Android workflow commits.
- Upload-only flags stay `os-linux`, `os-macos`, and `os-windows`.
- `codecov.yml` `ignore:` must stay aligned with
  `scripts/run_coverage.sh` `COVERAGE_IGNORE_REGEX`.
- The authoritative program tracker is `#641`, not the older language
  umbrella `#568`.

## Validation Commands

For doc-only refreshes:

```bash
tools/check-docs.sh
PULP_ENFORCE_PREPUSH=1 python3 tools/scripts/skill_sync_check.py --base origin/main --mode=report
python3 tools/scripts/version_bump_check.py --base origin/main --config tools/scripts/versioning.json --mode=report
git diff --check
```
