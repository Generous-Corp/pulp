# Phase 3 Codecov Queue Pause Ledger

Last updated: 2026-05-01 EDT

This local ledger records the open `codecov` PR validation runs paused to free Namespace capacity for higher-priority work, plus the small-batch resume queue. Branches, PRs, commits, labels, and tracker comments stay intact; queued GitHub Actions validation attempts are cancellable and replaceable.

## Queue Policy

- Reopen Codecov validation in small batches; do not dump the full paused queue back into Namespace at once.
- While a batch is in flight, continue local-only tranche preparation, review, and failure triage.
- Merge `UNSTABLE` PRs only when GitHub branch protection shows all required checks are green and the PR can merge normally.
- For merge eligibility after a pause, refresh PR branches with `gh pr update-branch <pr>` so GitHub creates fresh `pull_request` check suites.
- Use `shipyard cloud run build <branch> --provider namespace --require-sha <sha> --no-wait` only as a Namespace smoke or targeted diagnostic; successful `workflow_dispatch` checks do not replace stale failed or cancelled `pull_request` contexts for branch protection.
- If one lane is slow or stuck, prefer `shipyard cloud retarget`/`shipyard cloud add-lane` so existing useful runs are preserved instead of cancelled wholesale.
- Branch protection currently requires the lower-case `linux`, `macos`, and
  `windows` contexts. The visible Namespace child jobs may appear as
  `Linux (x64) [namespace]`, `macOS (ARM64) [namespace]`, and
  `Windows (x64) [namespace]`; wait for the lower-case reusable-workflow
  wrappers to settle before declaring a PR mergeable.

## Snapshot Summary

- Open `codecov` PRs in snapshot: 73
- Queued/in-progress Codecov-branch workflow runs selected for cancellation: 193
- Selected workflows: Build and Test, Coverage, Sanitizer Tests

## Cancellation Result

- Cancellation pass completed on 2026-04-30 EDT.
- Cancellation requests submitted successfully for all 193 recorded runs.
- A few runs completed while cancellation was in flight; sticky queued
  runs for #1133 and #1134 required a second cancellation request.
- Final verification after GitHub settled: 0 queued or in-progress
  workflow runs remained for the open `codecov` PR branches in this
  snapshot.
- Expected side effect: cancelled validation attempts may leave PRs
  `BLOCKED` or `UNSTABLE` until the queue is reopened and fresh
  validation runs are dispatched.

## Resume Batches

### 2026-05-01 Batch 1: Build Gate Refill

Namespace capacity check showed no queued or in-progress GitHub workflow
runs for `danielraffel/pulp`, so the first refill batch replaced only the
cancelled `Build and Test` gates for recent Codecov PRs. Existing passing
Codecov/version/docs/license results were left intact.

| PR | Branch | Head | Workflow | New Run | Status at Dispatch |
| --- | --- | --- | --- | --- | --- |
| #1140 | `feature/status-ladder-coverage-643` | `bf87273e0534` | Build and Test | `25202620106` | in progress |
| #1141 | `feature/ship-appcast-coverage-644-next` | `2d2c119b4c8d` | Build and Test | `25202620128` | in progress |
| #1142 | `codex/package-tools-coverage-643` | `cb6fb9c4e9c3` | Build and Test | `25202620088` | in progress |
| #1143 | `feature/render-compute-coverage-646` | `2b2ccde36e63` | Build and Test | `25202620280` | in progress |

Next refill rule: when this batch drops below four active Codecov build
runs, dispatch the next merge-ready paused PRs by pinned SHA and update
this table before adding more local-only tranche PRs.

Follow-up correction: these `workflow_dispatch` runs all passed, but they
did not satisfy branch protection because stale failed or cancelled
`pull_request` check runs with the same required context names remained on
the PR head commits. The branches were then refreshed against `main` with
`gh pr update-branch` to create merge-eligible PR-event check suites:

| PR | Branch | Refreshed Head | Build Run | Coverage Run | Status |
| --- | --- | --- | --- | --- | --- |
| #1140 | `feature/status-ladder-coverage-643` | `f48a49e62ec3` | `25203370720` | `25203370713` | pending |
| #1141 | `feature/ship-appcast-coverage-644-next` | `bccfec4797d5` | `25203371862` | `25203371864` | pending |
| #1142 | `codex/package-tools-coverage-643` | `45dbdda453a3` | `25203373031` | `25203372990` | pending |
| #1143 | `feature/render-compute-coverage-646` | `14098f98d57e` | `25203374251` | `25203374250` | pending |

### 2026-05-01 Batch 2: Build Gate Refill

The first batch assigned cleanly on Namespace with no GitHub queued-run
backlog, so a second four-PR batch was queued behind it. This keeps the
active window warm without re-dispatching the full paused set.

| PR | Branch | Head | Workflow | New Run | Status at Dispatch |
| --- | --- | --- | --- | --- | --- |
| #1136 | `feature/package-registry-cache-fallback-643-next` | `46854e04fcf2` | Build and Test | `25202718222` | queued |
| #1137 | `feature/audio-platform-helper-coverage-640-next` | `5149c107a3ac` | Build and Test | `25202718230` | queued |
| #1138 | `feature/coverage-no-idle-guidance-641` | `6fea2fa4686a` | Build and Test | `25202718329` | queued |
| #1139 | `codex/events-phase3-coverage-tranche` | `a6237254b05d` | Build and Test | `25202718395` | queued |

Note: `shipyard cloud run build` printed older saved run IDs for #1136
and #1137, but `gh run list` confirmed the fresh workflow_dispatch run
IDs above.

### 2026-05-01 Batch 3: Build Gate Refill

A third build-only batch was queued after #1136-#1139. #1131 was not
queued because its `Diff coverage required` check is red and needs
coverage-gate triage before a build-only re-run can make it mergeable.

| PR | Branch | Head | Workflow | New Run | Status at Dispatch |
| --- | --- | --- | --- | --- | --- |
| #1132 | `codex/midi-sysex-sidecar-tests` | `2695097458c4` | Build and Test | `25202770455` | queued |
| #1133 | `feature/audio-channel-set-coverage-640-next` | `df227394cd7e` | Build and Test | `25202770462` | queued |
| #1134 | `codex/coverage-phase3-tranche-20260430095455` | `1dbd12f64ba0` | Build and Test | `25202770509` | queued |
| #1135 | `feature/signal-processor-chain-reset-coverage-645` | `89242ca0bfa5` | Build and Test | `25202770726` | queued |

Held for triage: #1131 `feature/audio-window-enumerator-coverage-640-next`
has passing `codecov/patch` but failing `Diff coverage required`, so it
likely needs a coverage workflow re-run or a patch rather than build-only
refill.

Follow-up correction: because build-only dispatches do not clear stale
PR-event contexts, the queued or running build-only dispatches for
#1132-#1139 were cancelled after the #1140-#1143 branch-refresh batch
started. Cancelled workflow_dispatch run IDs: `25202718222`,
`25202718230`, `25202718329`, `25202718395`, `25202770455`,
`25202770462`, `25202770509`, and `25202770726`.

### 2026-05-01 Batch 4: PR-Event Branch Refresh

After confirming the build-only dispatches were not merge-eligible
evidence, the next refill batch used `gh pr update-branch` so GitHub
created fresh required `pull_request` checks.

| PR | Branch | Refreshed Head | Build Run | Coverage Run | Status |
| --- | --- | --- | --- | --- | --- |
| #1135 | `feature/signal-processor-chain-reset-coverage-645` | `bc32ea5904b6` | `25203513486` | `25203513496` | pending |
| #1136 | `feature/package-registry-cache-fallback-643-next` | `476677d7b926` | `25203494171` | `25203494182` | pending |
| #1137 | `feature/audio-platform-helper-coverage-640-next` | `cc7fcaf98be8` | `25203494454` | `25203494423` | pending |
| #1138 | `feature/coverage-no-idle-guidance-641` | `72fa08f3553a` | `25203493849` | `25203493834` | pending |
| #1139 | `codex/events-phase3-coverage-tranche` | `df3f376fa716` | `25203493795` | `25203493796` | pending |

### 2026-05-01 Batch 5: PR-Event Branch Refresh

Namespace still had the earlier PR-event batch queued but not failing, so
the next low-risk stale build-matrix PRs were refreshed in a small batch.

| PR | Branch | Refreshed Head | Build Run | Coverage Run | Status |
| --- | --- | --- | --- | --- | --- |
| #1129 | `feature/midi-sysex-accumulator-coverage-645-next` | `a5ae96aa067c` | `25203575118` | `25203575127` | pending |
| #1132 | `codex/midi-sysex-sidecar-tests` | `8b403641dfd4` | `25203575435` | `25203575450` | pending |
| #1133 | `feature/audio-channel-set-coverage-640-next` | `a82513bedc13` | `25203575337` | `25203575346` | pending |
| #1134 | `codex/coverage-phase3-tranche-20260430095455` | `8c91ac2d0315` | `25203575338` | `25203575315` | pending |

Follow-up refresh after `main` advanced through #1141 and #1131:

| PR | Branch | Refreshed Head | Build Run | Coverage Run | Status |
| --- | --- | --- | --- | --- | --- |
| #1139 | `codex/events-phase3-coverage-tranche` | `539bbb76c41a` | `25204395492` | `25204395482` | pending; fresh PR-event checks queued with sanitizer run `25204395501` and IWYU run `25204395484` |

### 2026-05-01 Batch 6: PR-Event Branch Refresh

After #1139 merged and GitHub Actions pressure dropped to 18 active
runs, the next four patched/stale Codecov PRs were refreshed with
`gh pr update-branch` so branch protection gets fresh `pull_request`
contexts. Pressure after dispatch rose to 39 active runs, with 24 queued
and 13 in progress, so hold further refill until this batch drains.

| PR | Branch | Refreshed Head | Build Run | Coverage Run | Sanitizer Run | IWYU Run | Status |
| --- | --- | --- | --- | --- | --- | --- | --- |
| #1051 | `feature/signal-poly-math-coverage-645` | `0675a09ecabd` | `25207650057` | `25207650060` | `25207650064` | `25207650080` | queued/in progress |
| #1062 | `codex/coverage-midi-edge-644` | `4fdcb2605585` | `25207651042` | `25207651053` | `25207651046` | `25207651044` | queued/in progress |
| #1066 | `feature/signal-filter-meter-coverage-645` | `cd2aafe8be87` | `25207652324` | `25207652319` | `25207652379` | `25207652327` | queued/in progress |
| #1075 | `feature/cli-host-coverage-643` | `e3e7b0c6bedc` | `25207653425` | `25207653428` | `25207653415` | `25207653416` | queued/in progress |

## Merge Waves While Queue Is In Flight

These PRs were already green/mergeable when the paused queue was
reopened, so they were squash-merged without consuming additional
Namespace capacity. #1099 became conflicted after the preceding view
coverage merges and is held for a branch refresh.

| PR | Merge SHA | Result |
| --- | --- | --- |
| #988 | `9da94da8e4be` | already merged before this queue sweep; confirmed no remaining queue action needed |
| #989 | `a0f94f120b15` | already merged before this queue sweep; confirmed no remaining queue action needed |
| #1014 | `6cb97e612aee` | already merged before this queue sweep; confirmed no remaining queue action needed |
| #1065 | `a428365ded95` | merged |
| #1080 | `9c5a455d1cd9` | merged |
| #1081 | `bb8e9ba00a32` | merged |
| #1084 | `66c74123430a` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` contexts were green, only advisory macOS sanitizer lanes were still pending |
| #1087 | `c89bd020f53c` | merged |
| #1090 | `fd7ff5386993` | merged |
| #1091 | `4210ff8f2c14` | merged |
| #1092 | `72a6421776b3` | merged |
| #1093 | `6da560926231` | merged |
| #1094 | `4ffa26b1e3a9` | merged |
| #1095 | `296e521c97c9` | merged |
| #1098 | `c3aff2bf92cd` | merged |
| #1099 | pending | merge conflict after view merges; refresh branch before retry |
| #1100 | `bd4829234834` | merged |
| #1101 | `e22fb97b12e9` | merged |
| #1103 | `ac8f62ffccf2` | merged |
| #1105 | `cc02d524888c` | merged |
| #1106 | `3436fb6fa16b` | merged |
| #1107 | `ba2b79f69e1b` | merged |
| #1108 | `a7e21a107ba7` | merged |
| #1109 | `d19d71b1d938` | merged |
| #1110 | `02d32afb3fc1` | merged |
| #1111 | `5841a31279e6` | merged |
| #1112 | `fef94b10e89c` | merged |
| #1123 | `57ac9d3c3a70` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` contexts were green, only advisory coverage/sanitizer lanes were still pending |
| #1099 | `a18291ee6642` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` contexts were green, only advisory lanes were still pending |
| #1132 | `3d7fa34f380d` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` contexts were green, only advisory lanes were still pending |
| #1136 | `caf93e5f835f` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` contexts were green, only advisory lanes were still pending |
| #1141 | `49136be956d` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` contexts were green, only advisory macOS coverage/sanitizer lanes were still pending |
| #1131 | `0f4d38f6c30` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` contexts were green, only advisory macOS coverage/sanitizer lanes were still pending |
| #1120 | `dba48cb3f53c` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` contexts were green, only advisory macOS coverage/sanitizer lanes were still pending |
| #1140 | `3695a6af7163` | merged from `CLEAN`; required `linux`, `macos`, and `windows` contexts, diff coverage, and Codecov patch were green |
| #1085 | `5aac29496436` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` contexts, diff coverage, and Codecov patch were green, only advisory macOS sanitizer lanes were still pending |
| #1138 | `44cff0532848` | merged from `CLEAN`; durable coverage guide now codifies the no-idle Phase 3 loop and Namespace-default validation posture |
| #1082 | `b0903cd8ed4b` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` contexts plus Codecov patch were green, only advisory macOS sanitizer/coverage lanes were still pending |
| #1135 | `44e67d52cbfc` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` contexts plus Codecov patch/diff coverage were green, only advisory macOS sanitizer lanes were still pending |
| #1139 | `d701d4ae8d45` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` wrappers plus Codecov patch were green, only advisory macOS coverage/sanitizer lanes were still pending |
| #1134 | `a01ca7410faa` | merged from `CLEAN`; required `linux`, `macos`, and `windows` wrappers, Codecov patch, diff coverage, coverage lanes, and sanitizer lanes were green |
| #1119 | `0bf8f64aeb8b` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` wrappers plus Codecov patch were green, only advisory macOS coverage was still pending |
| #1102 | `b3df384ab0de` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` wrappers, Codecov patch, and coverage lanes were green, only advisory macOS sanitizer lanes were still pending |
| #1142 | `ed34a23d9e4d` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` wrappers plus Codecov patch were green, only advisory macOS coverage was still pending |
| #1071 | `160e76ec7c86` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` wrappers plus Codecov patch were green, only advisory macOS coverage/sanitizer lanes were still pending |
| #1045 | `2e4a0ed2e256` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` wrappers plus Codecov patch were green, only advisory macOS coverage/sanitizer lanes were still pending |
| #1137 | `ea731cbf365c` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` wrappers, Codecov patch, and diff coverage were green, only advisory macOS sanitizer lanes were still pending |
| #1143 | `d36fddc2cd9f` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` wrappers, Codecov patch, diff coverage, and platform build/coverage lanes were green, only advisory macOS UBSan was still pending |
| #1075 | `c34f11f7138c` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` wrappers plus Codecov patch were green. Red Windows coverage job was advisory artifact-upload plumbing: tests, Cobertura existence check, and Codecov upload succeeded |
| #1133 | `e1a22a1ffd93` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` wrappers, Codecov patch, diff coverage, and coverage/build lanes were green, only advisory macOS ASan/UBSan jobs were still pending |
| #1086 | `0300ba207577` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` wrappers plus Codecov patch were green, Windows/Linux coverage and platform build lanes were green, pending macOS coverage/sanitizers were advisory |
| #1074 | `5898bf057163` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` wrappers, Codecov patch, diff coverage, coverage lanes, and platform build lanes were green, only advisory macOS sanitizer jobs were still pending |

## Conflict And Failure Triage

| PR | Branch | New Head | Status |
| --- | --- | --- | --- |
| #1099 | `feature/view-layout-widgets-coverage-493` | `66decf5958e8` | Merged as `a18291ee6642` after required `linux`, `macos`, and `windows` wrappers passed; advisory lanes were still pending. |
| #1084 | `feature/runtime-text-diff-coverage-641` | `cb63e51416ed` | Merged as `66c74123430a` after required `linux`, `macos`, and `windows` wrappers passed; advisory macOS sanitizer lanes were still pending. |
| #1089 | `feature/view-file-browser-coverage-493` | `86cbe4ca12e9` | Conflict resolved and pushed. The first refreshed run failed `Enforce version & skill sync`; a metadata-only empty commit added `Version-Bump: sdk=skip reason="test-only file browser coverage; no SDK release surface change"`. Local worker validated skill-sync, version-bump with required-bump enforcement, and `git diff --check origin/main...HEAD`. Fresh PR checks are queued. |
| #1131 | `feature/audio-window-enumerator-coverage-640-next` | `f5af90f4f5af` | Pushed coverage-harness fix for Windows `.exe` object discovery in `scripts/run_coverage.sh`; `bash -n`, `test_run_coverage.py`, skill-sync report, version-bump report, and `git diff --check` passed. Local CMake validation was not rerun after the harness patch because the laptop filesystem had only about 150-211 MiB free; the earlier excerpt binary validation was already green and the platform-sensitive coverage fix now needs CI/Namespace. |
| #1137 | `feature/audio-platform-helper-coverage-640-next` | `e4ea28dfc2e0` | Pushed a test isolation fix after macOS Namespace exposed a parallel CTest temp-dir collision in `test_cli_projects_registry.cpp`. Focused local `pulp-test-cli-projects-registry "add_project falls back to directory basename when no name hint"` passed; skill-sync/version-bump reports and `git diff --check` passed. Merged as `ea731cbf365c` after required wrappers, Codecov patch, and diff coverage passed; advisory macOS sanitizer lanes were still pending. |
| #1079 | `feature/volume-detector-coverage-642` | `5efc687e53a0` | Conflict resolved after #1045 landed overlapping service-discovery coverage. Kept the non-duplicated lifecycle coverage from #1079, dropped the now-duplicated backend registration failure test, validated `pulp-test-network-service-discovery`, focused `[issue-642]`, broad `NSD|MountedVolumeListChangeDetector|LockingAsyncUpdater` CTest, skill-sync report, version-bump report, and `git diff --check`, then force-with-lease pushed. Fresh PR checks are queued. |

### Live Queue Audit 2026-05-01 02:12 EDT

- Merge-now set: none.
- Watch first: #1123, #1099, #1132, #1142, #1143. These are mergeable at
  the Git level, have no real failing checks, and are waiting on the
  underlying Namespace/build or coverage legs before the lower-case
  required aliases can go green.
- #1075 was the only fresh actionable gate failure in this sweep
  (`Enforce version & skill sync` after the coverage workflow patch);
  it was fixed with metadata-only head `4469868e669b`.
- Older PRs with red lower-case `linux`/`macos`/`windows` contexts mostly
  reflect the deliberate queue-cancellation pause and need future
  PR-event branch refreshes in small batches, not immediate code patches.
- Queue pressure at the audit: roughly 72 active Codecov-like workflow
  runs across 25 branches, so do not refill Namespace until the current
  batch drains.

### Live Queue Audit 2026-05-01 02:55 EDT

- Open `codecov` PRs: 44.
- Merge-now set: none. All open Codecov PRs currently report
  `mergeStateStatus=BLOCKED`.
- GitHub Actions pressure: 60 queued workflow runs and 4 in progress for
  `danielraffel/pulp`, so do not add another PR-event refresh batch yet.
- Watch first: #1143, #1142, #1139, #1137, #1140. #1143 has Linux,
  Windows, diff coverage, Codecov patch, and advisory coverage/sanitizer
  lanes green, but its required `macOS (ARM64) [namespace]` build wrapper
  is still queued. #1142 is similarly waiting on macOS build and macOS
  coverage. #1139/#1137 are still waiting on macOS Namespace lanes, with
  #1137 also waiting on the Linux build wrapper.
- Local-only progress continues while the queue drains. Workers were
  assigned to refresh `local/phase3-merge-cobertura-extra-643`,
  `local/phase3-lcov-cobertura-extra-643`, and
  `local/phase3-run-python-coverage-extra-643` against current
  `origin/main`; no push, PR, or Namespace dispatch until capacity returns.

### Live Queue Audit 2026-05-01 03:17 EDT

- Open `codecov` PRs: 43 after #1120 merged.
- Merge-now set: none. A fresh merge-candidate query returned no
  `CLEAN`/`UNSTABLE` open Codecov PRs.
- GitHub Actions pressure remained high at 58 queued and 5 in progress,
  so no additional PR-event refresh batch was added.
- #1143 remained blocked only on the queued `macOS (ARM64) [namespace]`
  build job. A dry-run `shipyard cloud retarget --pr 1143 --target macOS
  --provider github-hosted --workflow build` showed a clean single-job
  retarget plan, but `--apply` could not cancel the queued job because
  the current `gh` token lacks `actions:write`. Leave the existing
  Namespace lane intact unless a human cancels it manually.

### Live Queue Audit 2026-05-01 03:33 EDT

- #1138 merged as `44cff0532848`; tracker comments posted to #641 and
  #493.
- Open `codecov` PRs: 40 after #1138 merged.
- Merge-now set: none. A fresh merge-candidate query returned no
  `CLEAN`/`UNSTABLE` open Codecov PRs.
- GitHub Actions pressure after the small refill batch: 50 active
  Codecov-like workflow runs, with 45 queued and 5 in progress.
- Watch first: #1143 and #1142 are still blocked on old queued macOS
  Namespace jobs. Keep those existing useful runs rather than pushing
  branch-refresh noise that would invalidate already-green lanes.
- Refill batch added because pressure had dropped below the earlier
  58-72 queued range:

| PR | Branch | Refreshed Head | Build Run | Coverage Run | Sanitizer Run | IWYU Run | Status |
| --- | --- | --- | --- | --- | --- | --- | --- |
| #1045 | `feature/events-service-discovery-coverage-642-next6` | `bfc1d7f981c6` | `25206704717` | `25206704705` | `25206704724` | `25206704727` | queued/in progress |
| #1071 | `feature/background-scanner-restart-coverage-493` | `beacb534d34b` | `25206704899` | `25206704914` | `25206704918` | `25206704896` | queued/in progress |
| #1072 | `feature/design-import-edge-coverage-493` | `2d78c1d52a77` | `25206705148` | `25206705135` | `25206705153` | `25206705144` | queued/in progress |
| #1074 | `feature/audio-format-registry-compressed-640` | `12bed93ae611` | `25206705263` | `25206705261` | `25206705389` | `25206705221` | queued/in progress |

### Merge Update 2026-05-01 03:38 EDT

- #1082 merged as `b0903cd8ed4b` after required `linux`, `macos`,
  and `windows` wrappers, Codecov patch, and the relevant platform build
  lanes were green. Advisory macOS sanitizer/coverage jobs were still
  queued at merge time.
- Tracker comments posted to #641 and #646.
- Open `codecov` PRs: 39 immediately after the merge; fresh
  merge-candidate poll returned none.
- Subsequent queue poll: 57 active Codecov-like workflow runs, with 51
  queued and 6 in progress. Do not add another Namespace/PR-event refill
  batch yet; continue local-only prep and merge monitoring.

### Merge Update 2026-05-01 03:56 EDT

- #1135 merged as `44e67d52cbfc` after required `linux`, `macos`,
  and `windows` wrappers, Codecov patch, and diff coverage were green.
  Advisory macOS sanitizer jobs were still queued at merge time.
- Tracker comments posted to #641 and #645.
- Open `codecov` PRs: 38 immediately after the merge.

### Merge Update 2026-05-01 04:08 EDT

- #1139 merged as `d701d4ae8d45` after required `linux`, `macos`,
  and `windows` wrappers, Namespace build lanes, and Codecov patch were
  green. Pending macOS coverage and sanitizer jobs were advisory.
- Tracker comments posted to #641 and #642.
- Queue pressure immediately before the merge was 18 active GitHub
  workflow runs, with 17 queued and 1 in progress, so capacity is low
  enough for another small refill batch rather than a full queue dump.

### Merge Update 2026-05-01 04:30 EDT

- #1134 merged as `a01ca7410faa` after required `linux`, `macos`,
  and `windows` wrappers, Codecov patch, diff coverage, coverage lanes,
  and sanitizer lanes were green.
- #1119 merged as `0bf8f64aeb8b` after required `linux`, `macos`,
  and `windows` wrappers plus Codecov patch were green. Pending macOS
  coverage was advisory.
- Tracker comments posted to #641 for both merges and to #645 for #1134.

### Merge And Refill Update 2026-05-01 05:12 EDT

- #1137 merged as `ea731cbf365c` from `UNSTABLE` after required
  `linux`, `macos`, and `windows` wrappers, Codecov patch, and diff
  coverage were green. Pending macOS ASan/UBSan jobs were advisory.
  Tracker comments were posted to #641 and #640.
- #1184 opened from the previously local memory-message-channel tranche
  as `feature/memory-message-channel-coverage-641` at
  `065ef90edbd3`. It was labeled `codecov`; PR-event checks are queued
  and explicit Namespace build dispatch `25209211899` was started with
  `shipyard cloud run build feature/memory-message-channel-coverage-641
  --provider namespace --require-sha HEAD --no-wait`.
- #1185 opened from the previously local binding tranche as
  `feature/state-binding-coverage-641` at `c1e5bd595244`. It was
  rebased onto `origin/main`, revalidated locally with
  `pulp-test-binding`, focused `Binding` CTest, skill-sync report,
  version-bump report, and `git diff --check`, then labeled `codecov`.
  Explicit Namespace build dispatch `25209337122` is queued/running.
- The first #1184 `shipyard pr` attempt used the unskipped local target
  config and stopped before PR creation when SSH Windows timed out. The
  retry used the documented Namespace-first pattern:
  `shipyard pr --skip-target mac --skip-target ubuntu --skip-target
  windows`, followed by the explicit Namespace dispatch above.

### Refill Update 2026-05-01 05:21 EDT

- #1186 opened from the text-layout worker tranche as
  `feature/canvas-text-layout-coverage-641` at `d4d40aa77928`. It was
  rechecked locally with `pulp-test-text-shaper`, focused text-shaper
  CTest, skill-sync report, version-bump report, and `git diff --check`,
  then labeled `codecov`. Explicit Namespace build dispatch
  `25209419094` is queued/running.
- #1187 opened from the SVG worker tranche as
  `feature/canvas-svg-coverage-641` at `7e4e8c5fabdd`. It was rechecked
  locally with `pulp-test-svg`, exact `^SvgImage ` CTest, skill-sync
  report, version-bump report, and `git diff --check`, then labeled
  `codecov`. Explicit Namespace build dispatch `25209443645` is
  queued/running.
- AssetManager worker output is held local-only because #1125 is already
  open on `feature/view-asset-manager-coverage-493-next` for the same
  surface.

### Merge Update 2026-05-01 05:23 EDT

- #1143 merged as `d36fddc2cd9f` after required `linux`, `macos`, and
  `windows` wrappers, Codecov patch, diff coverage, and platform
  build/coverage lanes were green. Pending macOS UBSan was advisory.
- Tracker comments posted to #641 and #646.

### Merge Update 2026-05-01 05:27 EDT

- #1075 merged as `c34f11f7138c` after required `linux`, `macos`, and
  `windows` wrappers plus Codecov patch were green. The red Windows
  coverage job was advisory artifact-upload plumbing: coverage suite,
  Cobertura existence check, and Codecov upload succeeded; only the
  Cobertura artifact upload step failed.
- Tracker comments posted to #641 and #643.

### Merge Update 2026-05-01 05:33 EDT

- #1133 merged as `e1a22a1ffd93` after required `linux`, `macos`, and
  `windows` wrappers, Codecov patch, diff coverage, and coverage/build
  lanes were green. Pending macOS ASan/UBSan jobs were advisory.
- Tracker comments posted to #641 and #640.

### Refill Update 2026-05-01 05:35 EDT

- #1188 opened from the table-model tranche as
  `feature/view-table-model-coverage-493` at `9700d0aada15`. It was
  rebased onto current `origin/main`, revalidated locally with
  `pulp-test-table-model`, focused table-model CTest, skill-sync report,
  version-bump report, and `git diff --check`, then labeled `codecov`.
  Explicit Namespace build dispatch `25209808806` is queued/running.
- Tracker comments posted to #641 and #493.

### Refill Update 2026-05-01 05:48 EDT

- #1189 opened from the fast-math tranche as
  `feature/signal-fast-math-boundary-coverage-645` at `55a7e26b9415`.
  It was rebased/current with `origin/main`, revalidated locally with
  `pulp-test-fast-math`, focused fast-math CTest, skill-sync report,
  version-bump report, and `git diff --check`, then labeled `codecov`.
  Explicit Namespace build dispatch `25210105555` is queued/running.
- Tracker comments posted to #641 and #645.

### Merge Update 2026-05-01 05:55 EDT

- #1086 merged as `0300ba207577` after required `linux`, `macos`, and
  `windows` wrappers plus Codecov patch were green. Windows/Linux
  coverage and platform build lanes were green; pending macOS coverage
  and macOS sanitizer jobs were advisory.
- Tracker comments posted to #641 and #640.
- Generated `build*` directories were removed from completed sibling
  coverage worktrees only, recovering local free disk from roughly
  248 MiB to roughly 3.7 GiB. Source worktrees and branches were left
  intact.

### Merge Update 2026-05-01 06:01 EDT

- #1074 merged as `5898bf057163` after required `linux`, `macos`, and
  `windows` wrappers, Codecov patch, diff coverage, coverage lanes, and
  platform build lanes were green. Pending macOS sanitizer jobs were
  advisory.
- Tracker comments posted to #641 and #640.

### Refill Update 2026-05-01 06:10 EDT

- #1190 opened from the path-to-SDF tranche as
  `feature/path-to-sdf-coverage-641` at `c4b62563589a`. It was already
  refreshed against current `origin/main`, revalidated locally with
  `pulp-test-path-to-sdf`, focused `path_to_sdf` CTest, skill-sync
  report, version-bump report, and `git diff --check`, then labeled
  `codecov`. Explicit Namespace build dispatch `25210687152` is
  queued/running.
- Follow-up: the first Windows Namespace run exposed a pre-existing
  `pulp-test-hot-reload` watcher timing issue under parallel CTest, so
  #1190 was rebased onto `origin/main` at `7b78de80` and pushed at
  `69aa03601cb8` with deterministic hot-reload temp paths/content waits
  plus a CTest resource lock. Local validation passed
  `cmake --build build --target pulp-test-hot-reload -j4`,
  `./build/test/pulp-test-hot-reload "[view][hotreload]"`, focused
  `HotReloader` CTest, `pulp-test-path-to-sdf`, focused `path_to_sdf`
  CTest, skill-sync report, version-bump report, and `git diff --check`.
  Fresh PR-event checks are queued under Build and Test run
  `25211537622`.
- Tracker comment posted to #641.

### Merge Update 2026-05-01 06:12 EDT

- #1189 merged as `e60cc129153c` after required `linux`, `macos`, and
  `windows` wrappers, Namespace platform jobs, and Codecov patch were
  green. Pending macOS coverage and sanitizer jobs were advisory.
- Tracker comments posted to #641 and #645.

### Refill Update 2026-05-01 06:15 EDT

- #1191 opened from the SVG path widget tranche as
  `feature/svg-path-widget-coverage-493` at `476ca6ac5639`. It was
  rebased onto current `origin/main`, revalidated locally with
  `pulp-test-svg-path-widget`, focused `svg-path|SvgPath` CTest,
  skill-sync report, version-bump report, `git diff --check`, and
  `git diff --check origin/main...HEAD`, then labeled `codecov`.
  Explicit Namespace build dispatch `25210821512` is queued/running.
- Tracker comments posted to #641 and #493.

### Merge Update 2026-05-01 06:26 EDT

- #1129 merged as `9c43e9fedae2` after required `linux`, `macos`, and
  `windows` wrappers, Namespace platform jobs, Codecov patch, diff
  coverage, and coverage lanes were green. Pending macOS ASan/TSan jobs
  were advisory.
- Tracker comments posted to #641 and #645.

### Merge Update 2026-05-01 06:30 EDT

- #1066 merged as `7b78de802f38` after required `linux`, `macos`, and
  `windows` wrappers, Namespace platform jobs, and Codecov patch were
  green. Pending macOS coverage/UBSan/TSan jobs were advisory.
- Tracker comments posted to #641 and #645.

## Real Diff-Gap Patch Queue

These PRs were inspected after the pause. The failures are not just stale
branch-protection checks; the repo diff-coverage gate reported concrete
uncovered changed lines. Assigned items have workers patching their own
branches in separate worktrees.

| PR | Branch | Gap | Current Action |
| --- | --- | --- | --- |
| #1123 | `feature/host-scan-cache-coverage-493-next` | `core/host/src/scan_cache.cpp` lines `184-188,195,198` | patched/pushed head `fbb4aa88dc10`; merged as `57ac9d3c3a70` after required wrappers passed |
| #1120 | `feature/descriptor-validation-coverage-493-next` | `core/format/src/descriptor_validation.cpp` lines `41-43,69-72` | patched/pushed head `5f4e686b8022`; merged as `dba48cb3f53c` after required wrappers passed |
| #1119 | `feature/state-undo-history-coverage-641-next` | `core/state/include/pulp/state/edit_history.hpp` lines `47-49` | patched/pushed head `e6c0736326ca`; merged as `0bf8f64aeb8b` after required wrappers passed |
| #1102 | `feature/midi-running-status-coverage-645-next` | `core/midi/src/running_status.cpp` lines `92-97` | patched/pushed head `a412d3c88316`; CI queued |
| #1086 | `feature/audio-hotplug-coverage-640` | `core/audio/include/pulp/audio/device.hpp` lines `89-94,116-121,125-126` | patched/pushed head `77b98ed1a2e8`; merged as `0300ba207577` after required wrappers and Codecov patch passed |
| #1085 | `feature/audio-load-measurer-coverage-640` | `core/audio/include/pulp/audio/load_measurer.hpp` lines `35-38,40-41,43-47,49,78-79` | patched/pushed head `9dbd9544a65b`; merged as `5aac29496436` after required wrappers passed |
| #1082 | `feature/render-loop-coverage-646` | `core/render/src/render_loop.cpp` and `core/render/src/render_loop_state.hpp` lifecycle/state lines | patched/pushed head `c53953830003`; merged as `b0903cd8ed4b` after required wrappers and Codecov patch passed |
| #1066 | `feature/signal-filter-meter-coverage-645` | `core/signal/include/pulp/signal/multi_channel_meter.hpp` line `151` | patched/pushed head `cd2aafe8be87`; merged as `7b78de802f38` after required wrappers, Namespace lanes, and Codecov patch passed |
| #1062 | `codex/coverage-midi-edge-644` | `core/midi/include/pulp/midi/message.hpp` factory/masking lines | patched/pushed head `8a9bd9efc2ba`; CI queued |
| #1051 | `feature/signal-poly-math-coverage-645` | `core/signal/include/pulp/signal/poly_math.hpp` lines `51-55` | patched/pushed head `0a202abb8816`; CI queued |
| #1075 | `feature/cli-host-coverage-643` | diff gate passed; failure was Windows coverage summary upload plumbing, then fresh skill-sync gate required a `ci` skill bypass for `.github/workflows/coverage.yml` | patched/pushed head `4469868e669b`; merged as `c34f11f7138c` after required wrappers and Codecov patch passed. Red Windows coverage job was advisory artifact-upload plumbing; the coverage suite, Cobertura existence check, and Codecov upload succeeded. |

Additional local-only workers started while Namespace was saturated:

| Branch | Worktree | Scope | Status |
| --- | --- | --- | --- |
| `local/phase3-local-ci-extra-643` | `/Users/danielraffel/Code/pulp-local-ci-extra-643` | improve `tools/local-ci/local_ci.py` focused coverage via `tools/local-ci/test_local_ci_extra.py` | completed locally at `75f495983db9`; focused coverage now reports 70%; no push/CI |
| `local/phase3-version-bump-extra-643` | `/Users/danielraffel/Code/pulp-version-bump-extra-643` | improve `tools/scripts/version_bump_check.py` focused coverage via `tools/scripts/test_version_bump_check_extra.py` | rebased locally at `f5fa93577560`; focused coverage still reports 99%; no push/CI |
| `local/phase3-compat-sync-extra-643` | `/Users/danielraffel/Code/pulp-compat-sync-extra-643` | improve `tools/scripts/compat_sync_check.py` focused coverage via `tools/scripts/test_compat_sync_check_extra.py` | completed locally at `7107bacc732a`; focused coverage now reports 99%; no push/CI |
| `local/phase3-audit-top-level-coverage-643` | `/Users/danielraffel/Code/pulp-audit-top-level-coverage-643` | improve `tools/audit.py` focused coverage via `tools/scripts/test_audit_top_level.py` | completed locally at `0b4eece8745e`; focused coverage now reports 100%; no push/CI |
| `local/phase3-pulp-sandbox-extra-643` | `/Users/danielraffel/Code/pulp-pulp-sandbox-extra-643` | improve `tools/sandbox-e2e/pulp_sandbox.py` focused coverage via `tools/sandbox-e2e/test_pulp_sandbox_unit.py` | completed locally at `3032cfd0f752`; focused coverage now reports 100%; no push/CI |

Local disk note: after completed Codecov worktrees pushed their fixes, their
generated `build/` directories plus stale generated build outputs from
older Codecov worktrees were removed. Source worktrees and branches were
left intact. Free space recovered from about `192 MiB` to about `4.7 GiB`.

## Local-Only Work Prepared During Pause

These branches were prepared after the Namespace pause began. They have
not been pushed, PR'd, or dispatched to Namespace.

| Branch | Head | Scope | Files | Local Validation | Resume Action |
| --- | --- | --- | --- | --- | --- |
| `local/phase3-docs-generate-coverage-643` | `7a582ae4` | #643 tooling tranche for `tools/docs_generate.py` paths | `tools/scripts/test_docs_generate.py` | Refreshed against current `origin/main` at `7b78de80`; `python3 tools/scripts/test_docs_generate.py` reports 13 tests; `python3 tools/docs_generate.py check` reports `docs-generate: OK`; temp-venv focused `run_python_coverage.py --pattern 'tools/scripts/test_docs_generate.py'` passed 13 tests and reported 97% for `tools/docs_generate.py`; skill-sync report; version-bump report; `git diff --check`. System `python3` lacked coverage, so the worker used a temporary venv outside the repo and removed it afterward. | Hold local-only while Namespace pressure is high; when capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-list-limitations-coverage-643` | `a77a5b99` | #643 tooling tranche for `tools/list_limitations.py` paths | `tools/scripts/test_list_limitations.py` | Refreshed against current `origin/main` at `7b78de80`; `python3 tools/scripts/test_list_limitations.py` reports 8 tests; `python3 tools/test_list_limitations.py` reports 6 tests; `python3 tools/list_limitations.py` rendered 21 items across 13 capabilities; temp-venv focused `run_python_coverage.py --pattern tools/scripts/test_list_limitations.py` passed 8 tests and reported 98% for `tools/list_limitations.py`; skill-sync report; version-bump report; `git diff --check`. System `python3` lacked coverage, so the worker used a temporary venv outside the repo and removed it afterward. | Hold local-only while Namespace pressure is high; when capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-check-format-validation-coverage-643` | `d970e805` | #643 tooling tranche for `tools/check_format_validation.py` paths | `tools/scripts/test_check_format_validation.py` | Refreshed against current `origin/main`; `python3 tools/scripts/test_check_format_validation.py` reports 9 tests; `python3 tools/scripts/test_run_python_coverage.py` reports 23 tests; temp-venv focused `run_python_coverage.py --pattern tools/scripts/test_check_format_validation.py` passed and reported 97% for `tools/check_format_validation.py`; skill-sync report; version-bump report; `git diff --check`. System `python3` lacked `coverage.py >= 7.10`, so the worker used a temporary venv outside the repo. | Hold local-only while Namespace pressure is high; when capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-bench-diff-coverage-643` | `ffdf970d` | #643 tooling tranche for `tools/scripts/bench_diff.py` paths | `tools/scripts/test_bench_diff.py` | Refreshed against current `origin/main`; `python3 tools/scripts/test_bench_diff.py` reports 8 tests; `python3 tools/scripts/test_run_python_coverage.py` reports 23 tests; temp-venv focused `run_python_coverage.py --pattern tools/scripts/test_bench_diff.py` passed 8 tests and reported 98% for `tools/scripts/bench_diff.py`; skill-sync report; version-bump report; `git diff --check`. System `python3` lacked `coverage.py >= 7.10`, so the worker used a temporary venv outside the repo. | Hold local-only while Namespace pressure is high; when capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-check-docs-consistency-coverage-643` | `0d2c0515` | #643 tooling tranche for `tools/check-docs-consistency.py` paths | `tools/scripts/test_check_docs_consistency.py` | Refreshed against current `origin/main` at `7b78de80`; `python3 tools/scripts/test_check_docs_consistency.py` reports 9 tests; `python3 tools/test_check_docs_consistency.py` reports 9 tests; `python3 tools/check-docs-consistency.py` checked 5 accessibility platforms and 150 statuses; temp-venv focused `run_python_coverage.py --pattern tools/scripts/test_check_docs_consistency.py` passed 9 tests and reported 98% for `tools/check-docs-consistency.py`; skill-sync report; version-bump report; `git diff --check`; `git diff --check origin/main..HEAD`. System `python3` lacked coverage, so the worker used a temporary venv outside the repo and removed it afterward. | Hold local-only while Namespace pressure is high; when capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-mkdocs-hooks-coverage-643` | `84dcf913` | #643 tooling tranche for `tools/mkdocs_hooks.py` paths | `tools/scripts/test_mkdocs_hooks.py` | Refreshed against current `origin/main` at `7b78de80`; `python3 tools/scripts/test_mkdocs_hooks.py` reports 6 tests; `python3 tools/scripts/test_run_python_coverage.py` reports 23 tests; temp-venv focused `run_python_coverage.py --pattern tools/scripts/test_mkdocs_hooks.py` passed 6 tests and reported 96% for `tools/mkdocs_hooks.py`; skill-sync report; version-bump report; `git diff --check`. System `python3` lacked coverage, so the focused run used a temporary venv outside the repo and removed it afterward. | Hold local-only while Namespace pressure is high; when capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-encode-binary-data-coverage-643` | `24f0cd57` | #643 tooling tranche for `tools/cmake/scripts/encode_binary_data.py` paths | `tools/scripts/test_encode_binary_data.py` | Refreshed against current `origin/main` at `7b78de80`; `python3 tools/scripts/test_encode_binary_data.py` reports 5 tests; `python3 tools/scripts/test_run_python_coverage.py` reports 23 tests; temp-venv focused `run_python_coverage.py --pattern tools/scripts/test_encode_binary_data.py` passed 5 tests and reported 96% for `tools/cmake/scripts/encode_binary_data.py`; skill-sync report; version-bump report; `git diff --check`. System `python3` lacked coverage, so the worker used a temporary venv outside the repo and removed it afterward. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-add-component-coverage-643` | `fdc82d93` | #643 tooling tranche for `tools/add-component.py` paths | `tools/scripts/test_add_component.py` | Refreshed against current `origin/main` at `7b78de80`; `python3 tools/scripts/test_add_component.py` reports 8 tests; `python3 tools/scripts/test_run_python_coverage.py` reports 23 tests; temp-venv focused `run_python_coverage.py --pattern tools/scripts/test_add_component.py` passed and reported 98% for `tools/add-component.py`; skill-sync report; version-bump report; `git diff --check`. System `python3` lacked coverage, so the worker used a temporary venv outside the repo and removed it afterward. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-audit-top-level-coverage-643` | `0b4eece8` | #643 tooling tranche for `tools/audit.py` paths | `tools/scripts/test_audit_top_level.py` | `python3 -m unittest tools.scripts.test_audit_top_level`; venv-backed branch coverage reports 100% for `tools/audit.py`; commit-scoped skill-sync report; commit-scoped version-bump report; `git diff --check`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-pulp-sandbox-extra-643` | `3032cfd0` | #643 tooling tranche for `tools/sandbox-e2e/pulp_sandbox.py` paths | `tools/sandbox-e2e/test_pulp_sandbox_unit.py` | `uv run --with pytest --with pytest-cov python -m pytest tools/sandbox-e2e/test_pulp_sandbox_unit.py -q`; pytest-cov reports 100% for `tools/sandbox-e2e/pulp_sandbox.py`; commit-scoped skill-sync report; commit-scoped version-bump report; `git diff --check`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-embed-js-coverage-643` | `5978b3ce` | #643 tooling tranche for `core/view/js/embed_js.py` paths | `tools/scripts/test_embed_js.py` | Rebased cleanly onto `origin/main`; `python3 tools/scripts/test_embed_js.py`; `python3 tools/scripts/test_run_python_coverage.py`; venv-backed `run_python_coverage.py --pattern tools/scripts/test_embed_js.py` reports 100% for target; skill-sync report; version-bump report; `git diff --check`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-run-swift-coverage-extra-643` | `2cd63c7f` | #643 tooling tranche for `tools/scripts/run_swift_coverage.py` paths | `tools/scripts/test_run_swift_coverage_extra.py` | Rebased cleanly onto `origin/main`; `python3 tools/scripts/test_run_swift_coverage_extra.py` reports 9 tests; `python3 tools/scripts/test_run_swift_coverage.py` reports 5 tests; `python3 tools/scripts/test_run_python_coverage.py` reports 23 tests; venv-backed coverage over both Swift coverage test files reports 96% for target; skill-sync report; version-bump report; `git diff --check`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-android-target-coverage-643` | `d533edb3` | #643 tooling tranche for `tools/local-ci/android_target.py` paths | `tools/scripts/test_android_target.py` | Rebased cleanly onto `origin/main`; `python3 tools/scripts/test_android_target.py` reports 16 tests; `python3 tools/scripts/test_run_python_coverage.py` reports 23 tests; venv-backed `run_python_coverage.py --pattern tools/scripts/test_android_target.py` reports 100% for target; skill-sync report; version-bump report; `git diff --check`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-validate-hosts-coverage-643` | `424abc00` | #643 tooling tranche for `tools/deps/validate_hosts.py` paths | `tools/scripts/test_validate_hosts.py` | Rebased cleanly onto `origin/main`; `python3 tools/scripts/test_validate_hosts.py`; `python3 tools/scripts/test_run_python_coverage.py`; venv-backed `run_python_coverage.py --pattern tools/scripts/test_validate_hosts.py` reports 100% for target; skill-sync report; version-bump report; `git diff --check`. No SSH or VM work was run. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-compat-sync-extra-643` | `b21ac426` | #643 tooling tranche for `tools/scripts/compat_sync_check.py` paths | `tools/scripts/test_compat_sync_check_extra.py` | `python3 tools/scripts/test_compat_sync_check_extra.py`; `python3 tools/scripts/test_compat_sync_check.py`; `python3 tools/scripts/test_run_python_coverage.py`; venv-backed `run_python_coverage.py --pattern tools/scripts/test_compat_sync_check.py --pattern tools/scripts/test_compat_sync_check_extra.py` reports 87% for target; skill-sync report; version-bump report; `git diff --check`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-build-migration-index-extra-643` | `41758867` | #643 tooling tranche for `tools/scripts/build_migration_index.py` paths | `tools/scripts/test_build_migration_index_extra.py` | Rebased cleanly onto `origin/main`; `python3 tools/scripts/test_build_migration_index.py`; `python3 tools/scripts/test_build_migration_index_extra.py`; `python3 tools/scripts/test_run_python_coverage.py`; venv-backed `run_python_coverage.py --pattern tools/scripts/test_build_migration_index.py --pattern tools/scripts/test_build_migration_index_extra.py` reports 100% for target; skill-sync report; version-bump report; `git diff --check`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-docs-sync-check-extra-643` | `b7ca2001` | #643 tooling tranche for `tools/scripts/docs_sync_check.py` paths | `tools/scripts/test_docs_sync_check_extra.py` | Rebased cleanly onto `origin/main`; `python3 tools/scripts/test_docs_sync_check.py`; `python3 tools/scripts/test_docs_sync_check_extra.py`; `python3 tools/scripts/test_run_python_coverage.py`; venv-backed `run_python_coverage.py --pattern tools/scripts/test_docs_sync_check.py --pattern tools/scripts/test_docs_sync_check_extra.py` reports 99% for target; skill-sync report; version-bump report; `git diff --check`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-coverage-tier-check-extra-643` | `0ae74d1e` | #643 tooling tranche for `tools/scripts/coverage_tier_check.py` paths | `tools/scripts/test_coverage_tier_check_extra.py` | Rebased cleanly onto `origin/main`; `python3 tools/scripts/test_coverage_tier_check.py`; `python3 tools/scripts/test_coverage_tier_check_extra.py`; `python3 tools/scripts/test_run_python_coverage.py`; venv-backed `run_python_coverage.py --pattern tools/scripts/test_coverage_tier_check.py --pattern tools/scripts/test_coverage_tier_check_extra.py` reports 100% for target; skill-sync report; version-bump report; `git diff --check`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-skill-sync-extra-643` | `4e9a6200` | #643 tooling tranche for `tools/scripts/skill_sync_check.py` paths | `tools/scripts/test_skill_sync_check_extra.py` | Rebased cleanly onto `origin/main` at `5aac2949`; `python3 tools/scripts/test_skill_sync_check_extra.py`; `python3 tools/scripts/test_skill_sync_check.py`; `python3 tools/scripts/test_gates.py`; venv-backed `run_python_coverage.py --pattern tools/scripts/test_skill_sync_check.py --pattern tools/scripts/test_skill_sync_check_extra.py` reports 94% for target; skill-sync report; version-bump report; `git diff --check`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-version-bump-extra-643` | `8650d404` | #643 tooling tranche for `tools/scripts/version_bump_check.py` paths | `tools/scripts/test_version_bump_check_extra.py` | Rebased cleanly onto `origin/main`; `python3 tools/scripts/test_version_bump_check_extra.py -v`; `python3 tools/scripts/test_gates.py`; `python3 tools/scripts/test_run_python_coverage.py`; venv-backed `run_python_coverage.py --pattern tools/scripts/test_gates.py --pattern tools/scripts/test_version_bump_check_extra.py` reports 92% for target; skill-sync report; version-bump report; `git diff --check`. Branch is ahead by two local commits. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-auto-release-extra-643` | `2916819c` | #643 tooling tranche for `tools/scripts/auto_release_decision.py` CLI and parser edges | `tools/scripts/test_auto_release_decision_extra.py` | Rebased cleanly onto `origin/main`; `python3 tools/scripts/test_auto_release_decision.py`; `python3 tools/scripts/test_auto_release_decision_extra.py`; `python3 tools/scripts/test_run_python_coverage.py`; venv-backed `run_python_coverage.py --pattern tools/scripts/test_auto_release_decision.py --pattern tools/scripts/test_auto_release_decision_extra.py` reports 100% for target; skill-sync report; version-bump report; `git diff --check`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-iwyu-annotate-extra-643` | `d38042fa` | #643 tooling tranche for `tools/scripts/iwyu_annotate.py` edges | `tools/scripts/test_iwyu_annotate_extra.py` | Rebased cleanly onto `origin/main` at `5aac2949`; `python3 tools/scripts/test_iwyu_annotate.py`; `python3 tools/scripts/test_iwyu_annotate_extra.py`; `python3 tools/scripts/test_run_python_coverage.py`; venv-backed `run_python_coverage.py --pattern tools/scripts/test_iwyu_annotate.py --pattern tools/scripts/test_iwyu_annotate_extra.py` reports 100% for target; skill-sync report; version-bump report; `git diff --check`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-coverage-diff-comment-extra-643` | `8e71b196` | #643 tooling tranche for `tools/scripts/coverage_diff_comment.py` CLI and report-rendering edges | `tools/scripts/test_coverage_diff_comment_extra.py` | Rebased cleanly onto `origin/main` at `5aac2949`; `python3 tools/scripts/test_coverage_diff_comment.py`; `python3 tools/scripts/test_coverage_diff_comment_extra.py`; `python3 tools/scripts/test_run_python_coverage.py`; venv-backed `run_python_coverage.py --pattern tools/scripts/test_coverage_diff_comment.py --pattern tools/scripts/test_coverage_diff_comment_extra.py` reports 100% for target; skill-sync report; version-bump report; `git diff --check`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-cmajor-external-extra-643` | `e6d19e61` | #643 tooling tranche for `tools/scripts/cmajor_external.py` edges | `tools/scripts/test_cmajor_external_extra.py` | Rebased cleanly onto `origin/main`; `python3 tools/scripts/test_cmajor_external.py`; `python3 tools/scripts/test_cmajor_external_extra.py`; venv-backed `run_python_coverage.py --pattern tools/scripts/test_cmajor_external.py --pattern tools/scripts/test_cmajor_external_extra.py` reports 100% for target; focused `doctor` and `generate --dry-run`; skill-sync report; version-bump report; `git diff --check`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-jsfx-subset-extra-643` | `cf003816` | #643 tooling tranche for `tools/scripts/jsfx_subset.py` parser and human-output edges | `tools/scripts/test_jsfx_subset_extra.py` | Rebased cleanly onto `origin/main`; `python3 tools/scripts/test_jsfx_subset.py`; `python3 tools/scripts/test_jsfx_subset_extra.py`; focused `jsfx_subset.py doctor` checks; `python3 tools/scripts/test_run_python_coverage.py`; venv-backed `run_python_coverage.py --pattern tools/scripts/test_jsfx_subset.py --pattern tools/scripts/test_jsfx_subset_extra.py` reports 100% for target; skill-sync report; version-bump report; `git diff --check`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-package-cli-extra-643` | `60cf6a5d` | #643 tooling tranche for `tools/scripts/package_cli.py` cache, rpath, and macOS packaging edges | `tools/scripts/test_package_cli_extra.py` | Rebased cleanly onto `origin/main`; `python3 tools/scripts/test_package_cli.py`; `python3 tools/scripts/test_package_cli_extra.py`; `python3 tools/scripts/test_run_python_coverage.py`; venv-backed `run_python_coverage.py --pattern tools/scripts/test_package_cli.py --pattern tools/scripts/test_package_cli_extra.py` reports 98% for target; skill-sync report; version-bump report; `git diff --check`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-resolve-runs-on-extra-643` | `fb2fae2a` | #643 tooling tranche for `tools/scripts/resolve_runs_on.py` edges | `tools/scripts/test_resolve_runs_on_extra.py` | Rebased cleanly onto `origin/main`; `python3 tools/scripts/test_resolve_runs_on.py`; `python3 tools/scripts/test_resolve_runs_on_extra.py`; `python3 tools/scripts/test_run_python_coverage.py`; venv-backed `run_python_coverage.py --pattern tools/scripts/test_resolve_runs_on.py --pattern tools/scripts/test_resolve_runs_on_extra.py` reports 100% for target; skill-sync report; version-bump report; `git diff --check`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-merge-cobertura-extra-643` | `34f1c29d` | #643 tooling tranche for `tools/scripts/merge_cobertura.py` edges | `tools/scripts/test_merge_cobertura_extra.py` | Rebased cleanly onto `origin/main` at `5aac2949`; `python3 tools/scripts/test_merge_cobertura_extra.py`; `python3 tools/scripts/test_merge_cobertura.py`; venv-backed `run_python_coverage.py --pattern tools/scripts/test_merge_cobertura*.py` reports 100% for target; skill-sync report; version-bump report; `git diff --check`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-lcov-cobertura-extra-643` | `ee1cfe11` | #643 tooling tranche for `tools/scripts/lcov_cobertura.py` edges | `tools/scripts/test_lcov_cobertura_extra.py` | Rebased cleanly onto `origin/main` at `5aac2949`; `python3 tools/scripts/test_lcov_cobertura_extra.py`; `python3 tools/scripts/test_lcov_cobertura.py`; venv-backed `run_python_coverage.py --pattern tools/scripts/test_lcov_cobertura*.py` reports 98% for target; skill-sync report; version-bump report; `git diff --check`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-run-python-coverage-extra-643` | `50d82ab6` | #643 tooling tranche for `tools/scripts/run_python_coverage.py` edges | `tools/scripts/test_run_python_coverage_extra.py` | Rebased cleanly onto `origin/main` at `5aac2949`; `python3 tools/scripts/test_run_python_coverage_extra.py`; `python3 tools/scripts/test_run_python_coverage.py`; venv-backed `run_python_coverage.py --pattern tools/scripts/test_run_python_coverage.py --pattern tools/scripts/test_run_python_coverage_extra.py` reports 99% for target; version-bump report; `git diff --check`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-cli-sync-check-extra-643` | `88d9c911` | #643 tooling tranche for `tools/scripts/cli_sync_check.py` edges | `tools/scripts/test_cli_sync_check_extra.py` | Rebased cleanly onto `origin/main` at `5aac2949`; `python3 tools/scripts/test_cli_sync_check_extra.py`; `python3 tools/scripts/test_cli_sync_check.py`; venv-backed `run_python_coverage.py --pattern tools/scripts/test_cli_sync_check.py --pattern tools/scripts/test_cli_sync_check_extra.py` reports 98% for target; skill-sync report; version-bump report; `git diff --check`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-local-ci-extra-643` | `51b4b23f` | #643 tooling tranche for `tools/local-ci/local_ci.py` helper edges | `tools/local-ci/test_local_ci_extra.py` | Rebased cleanly onto `origin/main`; added a low-risk mocked remote-probe coverage test; `python3 tools/local-ci/test_local_ci_extra.py`; `python3 tools/local-ci/test_local_ci.py`; `python3 tools/scripts/test_run_python_coverage.py`; venv-backed `run_python_coverage.py --pattern tools/local-ci/test_local_ci.py --pattern tools/local-ci/test_local_ci_extra.py` reports 71% for target; skill-sync report; version-bump report; `git diff --check`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-deps-audit-extra-643` | `108f02a5` | #643 tooling tranche for `tools/deps/audit.py` edges | `tools/deps/test_audit_extra.py` | Rebased cleanly onto `origin/main` at `5aac2949`; `python3 -m unittest tools.deps.test_audit_extra -v`; `python3 -m unittest tools.deps.test_audit -v`; venv-backed `run_python_coverage.py --pattern tools/deps/test_audit.py --pattern tools/deps/test_audit_extra.py` reports 99% for target; skill-sync report; version-bump report; `git diff --check`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-pulp-sandbox-extra-643` | `2b8f2d38` | #643 tooling tranche for `tools/sandbox-e2e/pulp_sandbox.py` edges | `tools/sandbox-e2e/test_pulp_sandbox_unit.py` | Rebased cleanly onto `origin/main` at `5aac2949`; `pytest tools/sandbox-e2e/test_pulp_sandbox_unit.py` reports 17 passed; focused coverage reports 100% for `tools/sandbox-e2e/pulp_sandbox.py`; skill-sync report; version-bump report; `git diff --check`. Older duplicate ledger entry is superseded by this single refreshed local commit. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-freshness-extra-643` | `2a6b7316` | #643 tooling tranche for `tools/packages/freshness_check.py` edges | `tools/packages/test_freshness_check_extra.py` | Rebased cleanly onto `origin/main` at `caf93e5f`; folded useful freshness-only coverage from `local/phase3-pkg-freshness-extra-643`; `python3 tools/packages/test_freshness_check_extra.py`; `python3 tools/packages/test_package_validation_tools.py`; focused coverage for `tools/packages/freshness_check.py` reports 99%; skill-sync report with package skip trailer; version-bump report; `git diff --check`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-pkg-freshness-extra-643` | `55bee1c5` | #643 alternate tooling tranche for `tools/packages/freshness_check.py` edges | `tools/packages/test_freshness_check_extra.py` | Superseded by `local/phase3-freshness-extra-643`; do not open separately because the alternate branch drags unrelated LV2 coverage files against current `origin/main`. | Ignore unless a future manual comparison is needed. |
| `local/phase3-validate-registry-extra-643` | `2c742ed5` | #643 tooling tranche for `tools/packages/validate_registry.py` edges | `tools/scripts/test_validate_registry_extra.py` | Rebased cleanly onto `origin/main` at `5aac2949`; `python3 tools/scripts/test_validate_registry_extra.py` reports 7 tests OK; venv-backed `run_python_coverage.py --pattern tools/scripts/test_validate_registry_extra.py --pattern tools/scripts/test_package_cli.py` reports 99% for `tools/packages/validate_registry.py` and 92% for `tools/scripts/package_cli.py`; skill-sync report; version-bump report; `git diff --check`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-workflow-lint-coverage-643` | `23de06c4` | #643 static workflow guard tranche for `.github/workflows/workflow-lint.yml` invariants | `tools/scripts/test_workflow_lint.py` | Created locally from `origin/main` at `b0903cd8`; `python3 tools/scripts/test_workflow_lint.py`; skill-sync report; version-bump report; `git diff --check`. No workflow edit was needed. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-codecov-config-edges-643` | `34b841fa` | #643 static coverage-config guard tranche for `codecov.yml` invariants | `tools/scripts/test_codecov_config.py` | Created locally from `origin/main` at `b0903cd8`; `python3 tools/scripts/test_codecov_config.py` reports 13 tests; venv-backed `run_python_coverage.py --pattern tools/scripts/test_codecov_config.py`; skill-sync report; version-bump report; `git diff --check`. `codecov.yml` unchanged. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-ruleset-drift-config-643` | `3f961e54` | #643 static ruleset-drift guard tranche for branch-protection JSON/workflow invariants | `tools/scripts/test_ruleset_drift_config.py` | Created locally from `origin/main` at `b0903cd8`; `python3 tools/scripts/test_ruleset_drift_config.py` reports 6 tests; skill-sync report; version-bump report; `git diff --check`. Coverage run was attempted with the base Python but skipped because that interpreter has `coverage.py < 7.10` and the shared venv was unavailable in this worktree. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `feature/signal-fast-math-boundary-coverage-645` | `55a7e26b` | #645 signal tranche for `core/signal/include/pulp/signal/fast_math.hpp` branch cutoffs | `test/test_fast_math.cpp` | Refreshed against current `origin/main`; `cmake --build build --target pulp-test-fast-math -j4`; `./build/test/pulp-test-fast-math` passed 62 assertions in 14 test cases; `ctest --test-dir build -R 'fast-math\|FastMath' --output-on-failure` passed 14/14; skill-sync report; version-bump report; `git diff --check`. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1189, and explicit Namespace build dispatch `25210105555` passed required wrappers/platform lanes. | Merged as `e60cc129153c`; tracker comments posted to #641 and #645. |
| `local/phase3-rectangle-list-edges-641` | `7d2b2639` | #641 canvas tranche for `core/canvas/include/pulp/canvas/rectangle_list.hpp` empty/no-op edges | `test/test_rectangle_list.cpp` | Refreshed against current `origin/main` at `e60cc129`; `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug`; `cmake --build build --target pulp-test-rectangle-list -j4`; `./build/test/pulp-test-rectangle-list` passed 60 assertions in 16 test cases; `ctest --test-dir build -R 'rectangle-list\|RectangleList\|Rect ' --output-on-failure` passed 16/16; skill-sync report; version-bump report; `git diff --check`. Local configure disabled Skia/text shaping because optional `external/skia-build` was unavailable; focused rectangle-list validation passed. | Hold local-only while Namespace pressure is high; when capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-table-model-edges-493` | `3f774e36` | #493 view tranche for `core/view/src/table_model.cpp` and `core/view/include/pulp/view/table_model.hpp` bookkeeping edges | `test/test_table_model.cpp` | Created locally from `origin/main` at `b0903cd8`; `cmake --build build --target pulp-test-table-model -j4`; `./build/test/pulp-test-table-model`; `ctest --test-dir build -R 'table-model\|TableModel\|sort_by\|toggle_sort\|add_column' --output-on-failure`; skill-sync report; version-bump report; `git diff --check`. The CTest regex also matched nearby `SimpleTableModel` tests, but the direct `pulp-test-table-model` binary passed first. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-theme-io-validation-493` | `8ffe39c2` | #493 view tranche for `core/view/src/theme.cpp` and `core/view/include/pulp/view/theme.hpp` validation/file edge paths | `test/test_theme.cpp` | Refreshed against `origin/main` at `0300ba20`; `cmake --build build --target pulp-test-theme -j4`; `./build/test/pulp-test-theme` passed 82 assertions in 17 test cases; `ctest --test-dir build -R '^Color from hex$|^Theme |^Dark theme|^Light theme|^Pro audio theme|^Motion tokens' --output-on-failure` passed 17/17; skill-sync report; version-bump report; `git diff --check`. Build used local cached Mbed TLS after FetchContent network clone stalled; Skia/text shaping unavailable locally but theme validation passed. | Hold local-only while Namespace pressure is high; when capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-websocket-channel-edges-641` | `33e89b5b` | #641 runtime tranche for `core/runtime/src/websocket_channel.cpp` frame and handshake edge paths | `test/test_websocket_channel.cpp` | Refreshed against current `origin/main`; `cmake --build build --target pulp-test-websocket-channel -j4`; `./build/test/pulp-test-websocket-channel` passed 62 assertions in 11 test cases; `ctest --test-dir build -R 'websocket\|WebSocket' --output-on-failure` passed 11/11; skill-sync report; version-bump report; `git diff --check`. | Hold local-only while Namespace pressure is high; when capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `feature/svg-path-widget-coverage-493` | `476ca6ac` | #493 view tranche for `core/view/src/svg_path_widget.cpp` parser and degenerate path edges | `test/test_svg_path_widget.cpp` | Rebased onto current `origin/main` at `e60cc129`; `cmake --build build --target pulp-test-svg-path-widget -j4`; `./build/test/pulp-test-svg-path-widget` passed 133 assertions in 21 test cases; `ctest --test-dir build -R 'svg-path\|SvgPath' --output-on-failure` passed 21/21; skill-sync report; version-bump report; `git diff --check`; `git diff --check origin/main...HEAD`. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1191, and explicit Namespace build dispatch `25210821512` is queued/running. | Monitor #1191; merge when required wrappers, Codecov patch, and diff coverage are green, or patch if Namespace reports a failure. |
| `local/phase3-eq-curve-view-edges-493` | `cec00003` | #493 view tranche for `core/view/src/eq_curve_view.cpp` range, hit-test, and drag callback edges | `test/test_phase9_widgets.cpp` | Refreshed against current `origin/main`; `cmake --build build --target pulp-test-phase9-widgets -j4`; `./build/test/pulp-test-phase9-widgets '[view][eq_curve]'` passed 32 assertions in 4 test cases; `ctest --test-dir build -R 'EqCurve\|eq_curve\|phase9-widgets' --output-on-failure` passed 4/4; skill-sync report; version-bump report; `git diff --check`. | Hold local-only while Namespace pressure is high; when capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-attributed-string-edges-641` | `c955057b` | #641 canvas tranche for `core/canvas/src/attributed_string.cpp` layout and span edge paths | `core/canvas/src/attributed_string.cpp`, `test/test_attributed_string.cpp` | Rebased cleanly onto `origin/main` at `d701d4ae`; includes a real fix for tab delimiter skipping after a word-break. `cmake --build build --target pulp-test-attributed-string -j4`; `./build/test/pulp-test-attributed-string` passed 97 assertions in 22 test cases; `ctest --test-dir build -R 'attributed-string\|AttributedString' --output-on-failure` passed 9/9; skill-sync report; version-bump report; `git diff --check`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-sdf-atlas-cache-edges-641` | `a4720af3` | #641 canvas tranche for `core/canvas/src/sdf_atlas_cache.cpp` lifecycle, rebuild, and age-eviction edges | `test/test_sdf_atlas_cache.cpp` | Refreshed against current `origin/main`; `cmake --build build --target pulp-test-sdf-atlas-cache -j4`; `./build/test/pulp-test-sdf-atlas-cache` passed 58 assertions in 9 test cases; `ctest --test-dir build -R 'sdf-atlas-cache\|SdfAtlasCache' --output-on-failure` passed 9/9; skill-sync report; version-bump report; `git diff --check`. | Hold local-only while Namespace pressure is high; when capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-image-convolution-edges-641` | `3d31df05` | #641 canvas tranche for `core/canvas/include/pulp/canvas/image_convolution.hpp` construction, stride, clamp, and standard-kernel edges | `test/test_image_convolution.cpp` | Refreshed against current `origin/main`; `cmake --build build --target pulp-test-image-convolution -j4`; `./build/test/pulp-test-image-convolution` passed 111 assertions in 12 test cases; `ctest --test-dir build -R 'ImageConvolutionKernel\|Identity kernel\|Gaussian blur\|Blur reduces contrast\|Apply on null\|Alpha channel\|Standard kernel\|Sharpen kernel' --output-on-failure` passed 12/12; skill-sync report; version-bump report; `git diff --check`. | Hold local-only while Namespace pressure is high; when capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-sdf-software-renderer-edges-641` | `23fdf6db` | #641 canvas tranche for `core/canvas/include/pulp/canvas/sdf_software_renderer.hpp` empty-atlas, degenerate-quad, clipping, source-bound, and max-alpha paths | `test/test_sdf_software_renderer.cpp` | Refreshed against current `origin/main`; `cmake --build build --target pulp-test-sdf-software-renderer -j4`; `./build/test/pulp-test-sdf-software-renderer` passed 1,044 assertions in 7 test cases; `ctest --test-dir build -R 'software SDF render' --output-on-failure` passed 7/7; skill-sync report; version-bump report; `git diff --check`. | Hold local-only while Namespace pressure is high; when capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `feature/path-to-sdf-coverage-641` | `69aa0360` | #641 canvas tranche for `core/canvas/src/path_to_sdf.cpp` dimension, spread, and mask-threshold guard edges, plus hot-reload test hardening after Windows Namespace exposed a watcher timing race | `test/test_path_to_sdf.cpp`, `test/test_hot_reload.cpp`, `test/CMakeLists.txt` | Rebased onto current `origin/main` at `7b78de80`; `cmake --build build --target pulp-test-path-to-sdf -j4`; `./build/test/pulp-test-path-to-sdf` passed 532 assertions in 6 test cases; `ctest --test-dir build -R 'path_to_sdf' --output-on-failure` passed 6/6; `cmake --build build --target pulp-test-hot-reload -j4`; `./build/test/pulp-test-hot-reload "[view][hotreload]"` passed 16 assertions in 5 test cases; `ctest --test-dir build -R 'HotReloader' --output-on-failure -j4` passed 5/5 with serialized watcher tests; skill-sync report; version-bump report; `git diff --check`. Fresh PR-event Build and Test run `25211537622` is queued/running. | Monitor #1190; merge when required wrappers, Codecov patch, and diff coverage are green, or patch if Namespace reports another failure. |
| `local/phase3-sdf-text-edges-641` | `1a333cc0` | #641 canvas tranche for `core/canvas/include/pulp/canvas/sdf_text.hpp` snap, zero-base, wrapper, and scaled-quad helper edges | `test/test_sdf_text.cpp` | Created locally from `origin/main` at `d701d4ae`; `cmake --build build --target pulp-test-sdf-text -j4`; `./build/test/pulp-test-sdf-text` passed 45 assertions in 8 test cases; `ctest --test-dir build -R 'snap_pen\|build_text_quads\|named SDF text wrappers' --output-on-failure` passed 8/8; skill-sync report; version-bump report; `git diff --check`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-sdf-atlas-edges-641` | `2f94fe28` | #641 canvas tranche for `core/canvas/src/sdf_atlas.cpp` default-state, invalid-build, packing, and duplicate-codepoint edges | `test/test_sdf_atlas.cpp` | Created locally from `origin/main` at `d701d4ae`; `cmake --build build --target pulp-test-sdf-atlas -j4`; `./build/test/pulp-test-sdf-atlas` passed 100 assertions in 10 test cases; `ctest --test-dir build -R 'SdfAtlas\|SDF pen' --output-on-failure` passed 19/19 including adjacent cache entries; skill-sync report; version-bump report; `git diff --check`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-msdf-psdf-edges-641` | `b7c5c2c6` | #641 canvas tranche for `core/canvas/src/msdf_atlas.cpp` and PSDF vector-fallback threshold edges | `test/test_msdf_atlas.cpp`, `test/test_psdf_atlas.cpp` | Rebased cleanly onto `origin/main` at `0bf8f64a`; `cmake --build build --target pulp-test-msdf-atlas pulp-test-psdf-atlas -j4`; `./build/test/pulp-test-msdf-atlas` passed 73 assertions in 10 test cases; `./build/test/pulp-test-psdf-atlas` passed 12 assertions in 2 test cases; `ctest --test-dir build -R 'MsdfAtlas\|PsdfAtlas\|vector_fallback' --output-on-failure` passed 12/12; skill-sync report; version-bump report; `git diff --check`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-canvas-fallback-edges-641` | `cf949ced` | #641 canvas tranche for `core/canvas/include/pulp/canvas/canvas.hpp` fallback drawing paths | `test/test_canvas.cpp` | Refreshed against current `origin/main`; `cmake --build build --target pulp-test-canvas -j4`; `./build/test/pulp-test-canvas '[canvas][fallback]'` passed 62 assertions in 4 test cases; `./build/test/pulp-test-canvas` passed 1,260 assertions in 30 test cases; `ctest --test-dir build -R 'Canvas fallback\|SDF fallback' --output-on-failure` passed 5/5; skill-sync report; version-bump report; `git diff --check`. Broader ledger regex that also included `pulp-test-canvas` matched unrelated `pulp-test-canvas-widget_NOT_BUILT`; an attempted widget build hit local disk exhaustion, but the actual fallback cases passed. | Hold local-only while Namespace pressure is high; when capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-app-framework-edges-493` | `d3002876` | #493 view tranche for `core/view/src/app_framework.cpp` shortcut, key mapping, menu, toolbar, and settings edges | `test/test_app_framework.cpp` | Refreshed against current `origin/main`; `cmake --build build --target pulp-test-app-framework -j4`; `./build/test/pulp-test-app-framework` passed 101 assertions in 27 test cases; `ctest --test-dir build -R 'KeyShortcut\|KeyMapping\|MenuBar\|NativeToolbar\|AppSettings' --output-on-failure` passed 27/27; skill-sync report; version-bump report; `git diff --check`. Initial build attempts hit local disk pressure and passed after cleanup. Existing warnings remain around missing optional SDK/assets, third-party CMake deprecations, and pre-existing missing-field initializer warnings. | Hold local-only while Namespace pressure is high; when capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `feature/state-binding-coverage-641` | `c1e5bd59` | #641 state tranche for `core/state/include/pulp/state/binding.hpp` gesture, polling, reset, and undo edges | `test/test_binding.cpp` | Rebased cleanly onto `origin/main` at `ea731cbf`; `cmake --build build --target pulp-test-binding -j4`; `./build/test/pulp-test-binding` passed 55 assertions in 14 test cases; `ctest --test-dir build -R 'Binding' --output-on-failure` passed 13/13; skill-sync report; version-bump report; `git diff --check`. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1185, and explicit Namespace build dispatch `25209337122` is queued/running. | Monitor #1185; merge when required wrappers, Codecov patch, and diff coverage are green, or patch if Namespace reports a failure. |
| `local/phase3-system-volume-edges-640` | `5711d7e2` | #640 audio tranche for `core/audio/src/system_volume.cpp` Linux `amixer` command edges | `test/test_system_volume.cpp`, `test/CMakeLists.txt` | Refreshed against `origin/main` at `e1a22a1f`; `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug`; `cmake --build build --target pulp-test-system-volume -j4`; `./build/test/pulp-test-system-volume` passed 4 assertions in 4 test cases; `ctest --test-dir build -R 'system volume\|system mute\|system-volume' --output-on-failure` passed 4/4; skill-sync report; version-bump report; `git diff --check`. The fake-`amixer` assertions are Linux-gated, so local macOS only exercised the inert non-Linux path. | Hold local-only while Namespace pressure is high; when capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`; Linux-gated fake-`amixer` assertions need Namespace before merge. |
| `feature/memory-message-channel-coverage-641` | `065ef90e` | #641 runtime tranche for `core/runtime/src/memory_message_channel.cpp` delivery, callback replacement, and close lifecycle edges | `test/test_memory_message_channel.cpp`, `test/CMakeLists.txt` | Rebased cleanly onto `origin/main` at `ea731cbf`; previous local validation: `cmake --build build --target pulp-test-memory-message-channel -j4`; `./build/test/pulp-test-memory-message-channel` passed 24 assertions in 6 test cases; `ctest --test-dir build -R 'MemoryMessageChannel\|memory-message-channel' --output-on-failure` passed 6/6; skill-sync report; version-bump report; `git diff --check`. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1184, and explicit Namespace build dispatch `25209211899` is queued/running. | Monitor #1184; merge when required wrappers, Codecov patch, and diff coverage are green, or patch if Namespace reports a failure. |
| `feature/canvas-text-layout-coverage-641` | `d4d40aa7` | #641 canvas tranche for `core/canvas/src/text_layout.cpp` fallback layout, hit-test, index-position, and parallelogram edges | `test/test_text_shaper.cpp` | Rebased/current with `origin/main`; `cmake --build build --target pulp-test-text-shaper -j4`; `./build/test/pulp-test-text-shaper` passed 62 assertions in 20 test cases; `ctest --test-dir build -R 'text-shaper\|TextShaper\|layout_paragraph\|GlyphArrangement\|Parallelogram' --output-on-failure` passed 19/19; skill-sync report; version-bump report; `git diff --check`. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1186, and explicit Namespace build dispatch `25209419094` is queued/running. | Monitor #1186; merge when required wrappers, Codecov patch, and diff coverage are green, or patch if Namespace reports a failure. |
| `feature/canvas-svg-coverage-641` | `7e4e8c5f` | #641 canvas tranche for `core/canvas/src/svg.cpp` file, invalid-rasterize, render, and move-assignment edges | `test/test_svg.cpp` | Rebased/current with `origin/main`; `cmake --build build-svg --target pulp-test-svg -j4`; `./build-svg/test/pulp-test-svg` passed 40 assertions in 10 test cases; `ctest --test-dir build-svg -R '^SvgImage ' --output-on-failure` passed 10/10; skill-sync report; version-bump report; `git diff --check`. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1187, and explicit Namespace build dispatch `25209443645` is queued/running. | Monitor #1187; merge when required wrappers, Codecov patch, and diff coverage are green, or patch if Namespace reports a failure. |
| `feature/view-table-model-coverage-493` | `9700d0aa` | #493 view tranche for `core/view/src/table_model.cpp` and `core/view/include/pulp/view/table_model.hpp` bookkeeping edges | `test/test_table_model.cpp` | Rebased onto `origin/main` at `e1a22a1f`; `cmake --build build --target pulp-test-table-model -j4`; `./build/test/pulp-test-table-model` passed 51 assertions in 11 test cases; `ctest --test-dir build -R 'table-model\|TableModel\|sort_by\|toggle_sort\|add_column' --output-on-failure` passed 9/9; skill-sync report; version-bump report; `git diff --check`. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1188, and explicit Namespace build dispatch `25209808806` is queued/running. | Monitor #1188; merge when required wrappers, Codecov patch, and diff coverage are green, or patch if Namespace reports a failure. |
| `local/phase3-asset-manager-coverage-493` | `57106c8b` | #493 view tranche for `core/view/src/asset_manager.cpp` shader/blob/image cache and missing-resource edges | `test/test_asset_manager.cpp` | Worker validated locally with `cmake --build build --target pulp-test-asset-manager`; `./build/test/pulp-test-asset-manager` passed 66 assertions in 25 test cases; `ctest --test-dir build -R 'AssetManager\|Theme' --output-on-failure` passed 25/25; skill-sync report; version-bump report; `git diff --check origin/main..HEAD`. | Hold local-only until #1125 settles because #1125 is already open on the AssetManager surface. Use as follow-up or conflict-aware supplement if #1125 leaves gaps or fails. |

## Cancelled/Paused Runs

| PR | State | Branch | Head | Runs | Title |
| --- | --- | --- | --- | --- | --- |
| #1045 | UNKNOWN | `feature/events-service-discovery-coverage-642-next6` | `5783c552714e` | `Build and Test 25168056508`<br>`Coverage 25168033097`<br>`Sanitizer Tests 25168032974`<br>`Build and Test 25168032955` | test(events): cover service discovery edge paths |
| #1051 | UNKNOWN | `feature/signal-poly-math-coverage-645` | `98e7ace40ae6` | `Build and Test 25172061996`<br>`Coverage 25172040856`<br>`Build and Test 25172040818`<br>`Sanitizer Tests 25172040812` | fix(signal): handle degenerate polynomial roots |
| #1062 | UNKNOWN | `codex/coverage-midi-edge-644` | `7c532a7414f1` | `Build and Test 25170865514`<br>`Build and Test 25170844844`<br>`Sanitizer Tests 25170844747`<br>`Coverage 25170844738` | test(midi): cover factory data byte bounds |
| #1066 | UNKNOWN | `feature/signal-filter-meter-coverage-645` | `facbf9038b58` | `Build and Test 25170500099`<br>`Coverage 25170474161`<br>`Sanitizer Tests 25170474151`<br>`Build and Test 25170474021` | fix(signal): clamp meter process channel counts |
| #1071 | UNKNOWN | `feature/background-scanner-restart-coverage-493` | `f959d20f76ce` | `Build and Test 25171074566`<br>`Sanitizer Tests 25171040147`<br>`Coverage 25171040113`<br>`Build and Test 25171040103` | test(host): cover background scanner restart after cancel |
| #1072 | UNKNOWN | `feature/design-import-edge-coverage-493` | `858791335582` | `Build and Test 25172032962`<br>`Build and Test 25172003397`<br>`Sanitizer Tests 25172003385`<br>`Coverage 25172003360` | test(view): cover design import edge paths |
| #1074 | UNKNOWN | `feature/audio-format-registry-compressed-640` | `40c55f8c4896` | `Build and Test 25170667740`<br>`Coverage 25170647578`<br>`Build and Test 25170647546`<br>`Sanitizer Tests 25170647524` | test(audio): cover compressed reader rejection |
| #1075 | UNKNOWN | `feature/cli-host-coverage-643` | `098de603de19` | `Build and Test 25170662783`<br>`Build and Test 25170655086`<br>`Sanitizer Tests 25170655057`<br>`Coverage 25170655030` | test(cli): cover host wrapper edge paths |
| #1078 | UNKNOWN | `feature/runtime-gzip-header-coverage-641` | `7fccf9b22503` | `Build and Test 25170874594`<br>`Build and Test 25170861703`<br>`Sanitizer Tests 25170861699`<br>`Coverage 25170861659` | test(runtime): cover gzip header rejection paths |
| #1079 | UNKNOWN | `feature/volume-detector-coverage-642` | `1de5e8cf3798` | `Build and Test 25171421848`<br>`Coverage 25171405107`<br>`Sanitizer Tests 25171405054`<br>`Build and Test 25171405053` | test(events): cover volume detector edge paths |
| #1082 | UNKNOWN | `feature/render-loop-coverage-646` | `5343f39d1e4e` | `Build and Test 25172222764`<br>`Coverage 25172198359`<br>`Build and Test 25172198322`<br>`Sanitizer Tests 25172198309` | test(render): cover render loop edge paths |
| #1083 | UNKNOWN | `feature/platform-registry-coverage-640` | `2a8289dfc54d` | `Build and Test 25171427827`<br>`Sanitizer Tests 25171410018`<br>`Build and Test 25171409959`<br>`Coverage 25171409933` | test(platform): cover registry fallback stubs |
| #1084 | UNKNOWN | `feature/runtime-text-diff-coverage-641` | `56b7cdffd2c0` | `Build and Test 25171676839`<br>`Coverage 25171657125`<br>`Build and Test 25171657123`<br>`Sanitizer Tests 25171657090` | test(runtime): cover text diff edge paths |
| #1085 | UNKNOWN | `feature/audio-load-measurer-coverage-640` | `580de2c8b8b7` | `Build and Test 25170103165`<br>`Coverage 25170077440`<br>`Sanitizer Tests 25170077403`<br>`Build and Test 25170077397` | fix(audio): guard load measurer invalid timing inputs |
| #1086 | UNKNOWN | `feature/audio-hotplug-coverage-640` | `53664657b7f2` | `Build and Test 25171357108`<br>`Coverage 25171343389`<br>`Build and Test 25171343386`<br>`Sanitizer Tests 25171343376` | test(audio): cover hotplug callback edges |
| #1088 | UNKNOWN | `feature/events-async-helper-coverage-642` | `c02c36484186` | `Build and Test 25166064687`<br>`Sanitizer Tests 25166050540`<br>`Coverage 25166050279`<br>`Build and Test 25166050259` | test(events): cover async helper edge paths |
| #1096 | UNKNOWN | `feature/render-pure-coverage-646` | `0f6b5fff46a1` | `Build and Test 25171762144`<br>`Coverage 25171747201`<br>`Build and Test 25171747177`<br>`Sanitizer Tests 25171747166` | test(render): cover gpu graph helper state paths |
| #1097 | UNKNOWN | `feature/view-toolbar-coverage-493` | `db18ab5dad36` | `Build and Test 25172317712`<br>`Coverage 25172293881`<br>`Sanitizer Tests 25172293727`<br>`Build and Test 25172293720` | test(view): cover toolbar and breadcrumb interactions |
| #1102 | UNKNOWN | `feature/midi-running-status-coverage-645-next` | `bf4e0d35c7d3` | `Build and Test 25172829315`<br>`Build and Test 25172819367`<br>`Sanitizer Tests 25172819312`<br>`Coverage 25172819311` | fix(midi): clear stale running status on undefined system bytes |
| #1104 | UNKNOWN | `feature/cli-create-coverage-643` | `21bf715c69dd` | `Build and Test 25166284714`<br>`Build and Test 25166269351`<br>`Coverage 25166269330`<br>`Sanitizer Tests 25166269324` | test(cli): cover create fail-fast edge paths |
| #1112 | UNKNOWN | `feature/state-properties-coverage-641-next2` | `314c970a36f8` | `Build and Test 25164008645` | test(state): cover state tree listener detach edges |
| #1113 | UNKNOWN | `feature/runtime-expression-coverage-641-next` | `448e92021bb5` | `Build and Test 25164081023`<br>`Coverage 25164051339`<br>`Build and Test 25164051319`<br>`Sanitizer Tests 25164051266` | test(runtime): cover expression evaluator edge paths |
| #1114 | UNKNOWN | `feature/runtime-file-library-coverage-641-next` | `796e36a2896a` | `Build and Test 25164337154`<br>`Build and Test 25164286882`<br>`Sanitizer Tests 25164286847`<br>`Coverage 25164286830` | test(runtime): cover file and library helper edges |
| #1115 | UNKNOWN | `feature/runtime-license-analytics-coverage-641-next` | `67157a45fc68` | `Build and Test 25164886652`<br>`Coverage 25164835133`<br>`Sanitizer Tests 25164835113`<br>`Build and Test 25164835092` | test(runtime): cover license and analytics edges |
| #1116 | UNKNOWN | `feature/signal-interpolator-coverage-645-next` | `7832eae87eaa` | `Build and Test 25165535849`<br>`Build and Test 25165511969`<br>`Coverage 25165511967`<br>`Sanitizer Tests 25165511951` | test(signal): cover interpolator endpoints and impulses |
| #1117 | BLOCKED | `feature/lcov-cobertura-coverage-643-next` | `7c5d7d67dbfc` | `Build and Test 25165561249`<br>`Coverage 25165524012`<br>`Build and Test 25165524010` | test(ci): cover lcov cobertura edge paths |
| #1118 | UNKNOWN | `feature/osc-bundle-coverage-641-next` | `6735bc0d519b` | `Build and Test 25165563643`<br>`Build and Test 25165526241`<br>`Coverage 25165526234`<br>`Sanitizer Tests 25165526222` | test(osc): cover malformed bundle edges |
| #1119 | UNKNOWN | `feature/state-undo-history-coverage-641-next` | `34efa649b9fd` | `Build and Test 25165878118`<br>`Sanitizer Tests 25165848583`<br>`Coverage 25165848571`<br>`Build and Test 25165848554` | test(state): cover undo history edge cases |
| #1120 | UNKNOWN | `feature/descriptor-validation-coverage-493-next` | `14a965de6152` | `Build and Test 25166025406`<br>`Coverage 25165994906`<br>`Sanitizer Tests 25165994899`<br>`Build and Test 25165994884` | test(format): cover descriptor validation edges |
| #1121 | UNKNOWN | `feature/audio-buffering-reader-coverage-640-next` | `363a1692a771` | `Build and Test 25166210546`<br>`Build and Test 25166195268`<br>`Coverage 25166195230`<br>`Sanitizer Tests 25166195212` | test(audio): cover buffering reader tail lifecycle paths |
| #1122 | UNKNOWN | `feature/render-draw-batcher-coverage-646-next` | `c1a8b031c54d` | `Build and Test 25166218297`<br>`Sanitizer Tests 25166203865`<br>`Build and Test 25166203856`<br>`Coverage 25166203850` | test(render): cover draw batcher merge blockers |
| #1123 | UNKNOWN | `feature/host-scan-cache-coverage-493-next` | `c1792a45fe1b` | `Build and Test 25166249254`<br>`Sanitizer Tests 25166234957`<br>`Build and Test 25166234954`<br>`Coverage 25166234934` | test: cover host scan cache fallback paths |
| #1125 | UNKNOWN | `feature/view-asset-manager-coverage-493-next` | `be13ce28b68f` | `Build and Test 25167200088`<br>`Coverage 25167178443`<br>`Sanitizer Tests 25167178429`<br>`Build and Test 25167178420` | test(view): cover asset manager edge paths |
| #1126 | UNKNOWN | `feature/view-frame-clock-coverage-493-next` | `e8dca439b850` | `Build and Test 25167592036`<br>`Coverage 25167561161`<br>`Sanitizer Tests 25167561146`<br>`Build and Test 25167561113` | test(view): cover frame clock lifecycle edges |
| #1127 | UNKNOWN | `feature/render-texture-atlas-coverage-646-next` | `7b6a7bfc4f8b` | `Build and Test 25167987526`<br>`Sanitizer Tests 25167952725`<br>`Coverage 25167952691`<br>`Build and Test 25167952688` | test(render): cover texture atlas full-capacity edges |
| #1128 | UNKNOWN | `feature/audio-workgroup-coverage-640-next` | `e4875168d49b` | `Build and Test 25168404842`<br>`Build and Test 25168364432`<br>`Coverage 25168364417`<br>`Sanitizer Tests 25168364375` | test(audio): cover workgroup fallback lifecycle |
| #1129 | UNKNOWN | `feature/midi-sysex-accumulator-coverage-645-next` | `f81378693df2` | `Build and Test 25168440441`<br>`Build and Test 25168411404`<br>`Coverage 25168411387`<br>`Sanitizer Tests 25168411385` | test(midi): cover sysex accumulator tail paths |
| #1131 | UNKNOWN | `feature/audio-window-enumerator-coverage-640-next` | `36d5bd605414` | `Build and Test 25169301265`<br>`Build and Test 25169274684`<br>`Coverage 25169274675`<br>`Sanitizer Tests 25169274591` | test(audio): cover excerpt window boundary arithmetic |
| #1132 | UNKNOWN | `codex/midi-sysex-sidecar-tests` | `2695097458c4` | `Build and Test 25169342502`<br>`Build and Test 25169328452`<br>`Coverage 25169328381`<br>`Sanitizer Tests 25169328284` | test(midi): cover sysex sidecar lifecycle |
| #1133 | UNKNOWN | `feature/audio-channel-set-coverage-640-next` | `df227394cd7e` | `Build and Test 25169456361`<br>`Coverage 25169437661`<br>`Sanitizer Tests 25169437655`<br>`Build and Test 25169437642` | test(audio): cover channel set edge paths |
| #1134 | UNKNOWN | `codex/coverage-phase3-tranche-20260430095455` | `1dbd12f64ba0` | `Build and Test 25171425473`<br>`Build and Test 25170099336`<br>`Coverage 25170099278`<br>`Sanitizer Tests 25170099265` | test(signal): cover meter reset and clip edges |
| #1135 | UNKNOWN | `feature/signal-processor-chain-reset-coverage-645` | `89242ca0bfa5` | `Build and Test 25172582711`<br>`Build and Test 25172550916`<br>`Sanitizer Tests 25172550903`<br>`Coverage 25172550884` | test(signal): cover processor chain reset edges |
| #1136 | UNKNOWN | `feature/package-registry-cache-fallback-643-next` | `46854e04fcf2` | `Build and Test 25173056813`<br>`Sanitizer Tests 25173043882`<br>`Coverage 25173043809`<br>`Build and Test 25173043787` | test(cli): cover package registry cache fallbacks |
| #1137 | UNKNOWN | `feature/audio-platform-helper-coverage-640-next` | `5149c107a3ac` | `Build and Test 25173070502`<br>`Sanitizer Tests 25173053372`<br>`Coverage 25173053207`<br>`Build and Test 25173053201` | test(platform): cover progress parser delimiter edges |
| #1138 | BLOCKED | `feature/coverage-no-idle-guidance-641` | `6fea2fa4686a` | `Build and Test 25173469013`<br>`Coverage 25173434715`<br>`Build and Test 25173434684` | docs: codify coverage no-idle loop |
| #1139 | BLOCKED | `codex/events-phase3-coverage-tranche` | `a6237254b05d` | `Build and Test 25173850903`<br>`Sanitizer Tests 25173826530`<br>`Build and Test 25173826521`<br>`Coverage 25173826490` | test(events): cover ipc error state edges |
| #1140 | BLOCKED | `feature/status-ladder-coverage-643` | `bf87273e0534` | `Build and Test 25173851676`<br>`Coverage 25173829139`<br>`Build and Test 25173829138` | test(tools): cover status ladder checker |
| #1141 | BLOCKED | `feature/ship-appcast-coverage-644-next` | `2d2c119b4c8d` | `Build and Test 25173929796`<br>`Build and Test 25173909885`<br>`Sanitizer Tests 25173909871`<br>`Coverage 25173909805` | test(ship): cover appcast malformed optional fields |
| #1142 | BLOCKED | `codex/package-tools-coverage-643` | `cb6fb9c4e9c3` | `Build and Test 25174428148`<br>`Build and Test 25174415212`<br>`Coverage 25174415161` | test(packages): cover validation tool edge paths |
| #1143 | BLOCKED | `feature/render-compute-coverage-646` | `2b2ccde36e63` | `Build and Test 25174471464`<br>`Sanitizer Tests 25174429661`<br>`Coverage 25174429510`<br>`Build and Test 25174429474` | test(render): cover gpu compute pool bookkeeping |

## Resume Checklist

1. Confirm Namespace capacity is available.
2. Re-fetch each PR branch and verify the remote branch still points to the recorded head SHA.
3. Re-dispatch in small batches, starting with the most merge-ready PRs.
4. After each batch, merge only PRs with required checks green and `mergeStateStatus` clean enough for normal merge.
5. Update this ledger with resumed run IDs and merge SHAs.
