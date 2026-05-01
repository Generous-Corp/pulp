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

## Merge Waves While Queue Is In Flight

These PRs were already green/mergeable when the paused queue was
reopened, so they were squash-merged without consuming additional
Namespace capacity. #1099 became conflicted after the preceding view
coverage merges and is held for a branch refresh.

| PR | Merge SHA | Result |
| --- | --- | --- |
| #1065 | `a428365ded95` | merged |
| #1080 | `9c5a455d1cd9` | merged |
| #1081 | `bb8e9ba00a32` | merged |
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
| #1141 | `49136be956d` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` contexts were green, only advisory macOS coverage/sanitizer lanes were still pending |

## Conflict And Failure Triage

| PR | Branch | New Head | Status |
| --- | --- | --- | --- |
| #1099 | `feature/view-layout-widgets-coverage-493` | `66decf5958e8` | Conflict resolved and pushed; normal PR workflows are running. Local worker validated `pulp-test-view-layout-widgets`, `pulp-test-phase9-widgets`, and `pulp-test-property-list` directly. |
| #1084 | `feature/runtime-text-diff-coverage-641` | `cb63e51416ed` | Conflict resolved and pushed; normal PR workflows are running. Local worker validated `pulp-test-runtime-utils "[runtime][text-diff]"` and matching CTest cases. |
| #1089 | `feature/view-file-browser-coverage-493` | `86cbe4ca12e9` | Conflict resolved and pushed. The first refreshed run failed `Enforce version & skill sync`; a metadata-only empty commit added `Version-Bump: sdk=skip reason="test-only file browser coverage; no SDK release surface change"`. Local worker validated skill-sync, version-bump with required-bump enforcement, and `git diff --check origin/main...HEAD`. Fresh PR checks are queued. |
| #1131 | `feature/audio-window-enumerator-coverage-640-next` | `f5af90f4f5af` | Pushed coverage-harness fix for Windows `.exe` object discovery in `scripts/run_coverage.sh`; `bash -n`, `test_run_coverage.py`, skill-sync report, version-bump report, and `git diff --check` passed. Local CMake validation was not rerun after the harness patch because the laptop filesystem had only about 150-211 MiB free; the earlier excerpt binary validation was already green and the platform-sensitive coverage fix now needs CI/Namespace. |
| #1137 | `feature/audio-platform-helper-coverage-640-next` | `e4ea28dfc2e0` | Pushed a test isolation fix after macOS Namespace exposed a parallel CTest temp-dir collision in `test_cli_projects_registry.cpp`. Focused local `pulp-test-cli-projects-registry "add_project falls back to directory basename when no name hint"` passed; skill-sync/version-bump reports and `git diff --check` passed. Fresh PR checks are queued. |

## Real Diff-Gap Patch Queue

These PRs were inspected after the pause. The failures are not just stale
branch-protection checks; the repo diff-coverage gate reported concrete
uncovered changed lines. Assigned items have workers patching their own
branches in separate worktrees.

| PR | Branch | Gap | Current Action |
| --- | --- | --- | --- |
| #1123 | `feature/host-scan-cache-coverage-493-next` | `core/host/src/scan_cache.cpp` lines `184-188,195,198` | patched/pushed head `fbb4aa88dc10`; CI queued |
| #1120 | `feature/descriptor-validation-coverage-493-next` | `core/format/src/descriptor_validation.cpp` lines `41-43,69-72` | patched/pushed head `5f4e686b8022`; CI queued |
| #1119 | `feature/state-undo-history-coverage-641-next` | `core/state/include/pulp/state/edit_history.hpp` lines `47-49` | patched/pushed head `e6c0736326ca`; CI queued |
| #1102 | `feature/midi-running-status-coverage-645-next` | `core/midi/src/running_status.cpp` lines `92-97` | patched/pushed head `a412d3c88316`; CI queued |
| #1086 | `feature/audio-hotplug-coverage-640` | `core/audio/include/pulp/audio/device.hpp` lines `89-94,116-121,125-126` | patched/pushed head `77b98ed1a2e8`; CI queued |
| #1085 | `feature/audio-load-measurer-coverage-640` | `core/audio/include/pulp/audio/load_measurer.hpp` lines `35-38,40-41,43-47,49,78-79` | patched/pushed head `9dbd9544a65b`; CI queued |
| #1082 | `feature/render-loop-coverage-646` | `core/render/src/render_loop.cpp` and `core/render/src/render_loop_state.hpp` lifecycle/state lines | patched/pushed head `c53953830003`; CI queued. Worker labeled `pulp-test-render-loop` tests with `coverage` and validated focused CMake/CTest plus skill/version checks. |
| #1066 | `feature/signal-filter-meter-coverage-645` | `core/signal/include/pulp/signal/multi_channel_meter.hpp` line `151` | patched/pushed head `bf60924fc794`; CI queued |
| #1062 | `codex/coverage-midi-edge-644` | `core/midi/include/pulp/midi/message.hpp` factory/masking lines | patched/pushed head `8a9bd9efc2ba`; CI queued |
| #1051 | `feature/signal-poly-math-coverage-645` | `core/signal/include/pulp/signal/poly_math.hpp` lines `51-55` | patched/pushed head `0a202abb8816`; CI queued |
| #1075 | `feature/cli-host-coverage-643` | diff gate passed; failure was Windows coverage summary upload plumbing | patched/pushed head `49d950e5e470`; CI queued. Worker made coverage summary artifact uploads advisory after confirming Windows coverage, Cobertura XML, Codecov upload, and diff gate had already completed. |

Additional local-only workers started while Namespace was saturated:

| Branch | Worktree | Scope | Status |
| --- | --- | --- | --- |
| `local/phase3-local-ci-extra-643` | `/Users/danielraffel/Code/pulp-local-ci-extra-643` | improve `tools/local-ci/local_ci.py` focused coverage via `tools/local-ci/test_local_ci_extra.py` | worker active; no push/CI |
| `local/phase3-version-bump-extra-643` | `/Users/danielraffel/Code/pulp-version-bump-extra-643` | improve `tools/scripts/version_bump_check.py` focused coverage via `tools/scripts/test_version_bump_check_extra.py` | completed locally at `b1d78296aee2`; focused coverage now reports 99%; no push/CI |
| `local/phase3-compat-sync-extra-643` | `/Users/danielraffel/Code/pulp-compat-sync-extra-643` | improve `tools/scripts/compat_sync_check.py` focused coverage via `tools/scripts/test_compat_sync_check_extra.py` | completed locally at `7107bacc732a`; focused coverage now reports 99%; no push/CI |

Local disk note: after completed Codecov worktrees pushed their fixes, their
generated `build/` directories plus stale generated build outputs from
older Codecov worktrees were removed. Source worktrees and branches were
left intact. Free space recovered from about `192 MiB` to about `4.7 GiB`.

## Local-Only Work Prepared During Pause

These branches were prepared after the Namespace pause began. They have
not been pushed, PR'd, or dispatched to Namespace.

| Branch | Head | Scope | Files | Local Validation | Resume Action |
| --- | --- | --- | --- | --- | --- |
| `local/phase3-docs-generate-coverage-643` | `1f1b0bbf` | #643 tooling tranche for `tools/docs_generate.py` paths | `tools/scripts/test_docs_generate.py` | `python3 tools/scripts/test_docs_generate.py`; `python3 tools/scripts/test_run_python_coverage.py`; venv-backed `run_python_coverage.py --pattern tools/scripts/test_docs_generate.py` reports 97% for target; skill-sync report; version-bump report; `git diff --check`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-list-limitations-coverage-643` | `9040b518` | #643 tooling tranche for `tools/list_limitations.py` paths | `tools/scripts/test_list_limitations.py` | `python3 tools/scripts/test_list_limitations.py`; `python3 tools/test_list_limitations.py`; `python3 tools/scripts/test_run_python_coverage.py`; venv-backed `run_python_coverage.py --pattern tools/scripts/test_list_limitations.py` reports 98% for target; skill-sync report; version-bump report; `git diff --check`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-check-format-validation-coverage-643` | `454d09a9` | #643 tooling tranche for `tools/check_format_validation.py` paths | `tools/scripts/test_check_format_validation.py` | `python3 tools/scripts/test_check_format_validation.py`; `python3 tools/scripts/test_run_python_coverage.py`; venv-backed `run_python_coverage.py --pattern tools/scripts/test_check_format_validation.py` reports 97% for target; skill-sync report; version-bump report; `git diff --check`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-bench-diff-coverage-643` | `d37ba4a2` | #643 tooling tranche for `tools/scripts/bench_diff.py` paths | `tools/scripts/test_bench_diff.py` | `python3 tools/scripts/test_bench_diff.py`; `python3 tools/scripts/test_run_python_coverage.py`; venv-backed `run_python_coverage.py --pattern tools/scripts/test_bench_diff.py` reports 98% for target; skill-sync report; version-bump report; `git diff --check`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-check-docs-consistency-coverage-643` | `46291395` | #643 tooling tranche for `tools/check-docs-consistency.py` paths | `tools/scripts/test_check_docs_consistency.py` | `python3 tools/scripts/test_check_docs_consistency.py`; `python3 tools/scripts/test_run_python_coverage.py`; venv-backed `run_python_coverage.py --pattern tools/scripts/test_check_docs_consistency.py` reports 98% for target; skill-sync report; version-bump report; `git diff --check`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-mkdocs-hooks-coverage-643` | `b1dc34ed` | #643 tooling tranche for `tools/mkdocs_hooks.py` paths | `tools/scripts/test_mkdocs_hooks.py` | `python3 tools/scripts/test_mkdocs_hooks.py`; `python3 tools/scripts/test_run_python_coverage.py`; venv-backed `run_python_coverage.py --pattern tools/scripts/test_mkdocs_hooks.py` reports 96% for target; skill-sync report; version-bump report; `git diff --check`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-encode-binary-data-coverage-643` | `e0e49d7e` | #643 tooling tranche for `tools/cmake/scripts/encode_binary_data.py` paths | `tools/scripts/test_encode_binary_data.py` | `python3 tools/scripts/test_encode_binary_data.py`; `python3 tools/scripts/test_run_python_coverage.py`; venv-backed `run_python_coverage.py --pattern tools/scripts/test_encode_binary_data.py` reports 96% for target; skill-sync report; version-bump report; `git diff --check`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-add-component-coverage-643` | `872bd5a5` | #643 tooling tranche for `tools/add-component.py` paths | `tools/scripts/test_add_component.py` | `python3 tools/scripts/test_add_component.py`; `python3 tools/scripts/test_run_python_coverage.py`; venv-backed `run_python_coverage.py --pattern tools/scripts/test_add_component.py` reports 98% for target; skill-sync report; version-bump report; `git diff --check`; `git diff --check origin/main...HEAD`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-audit-top-level-coverage-643` | `ce08d897` | #643 tooling tranche for `tools/audit.py` paths | `tools/scripts/test_audit_top_level.py` | `python3 tools/scripts/test_audit_top_level.py`; `python3 tools/scripts/test_run_python_coverage.py`; venv-backed `run_python_coverage.py --pattern tools/scripts/test_audit_top_level.py` reports 90% for target; skill-sync report; version-bump report; `git diff --check`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-embed-js-coverage-643` | `d375778b` | #643 tooling tranche for `core/view/js/embed_js.py` paths | `tools/scripts/test_embed_js.py` | `python3 tools/scripts/test_embed_js.py`; `python3 tools/scripts/test_run_python_coverage.py`; venv-backed `run_python_coverage.py --pattern tools/scripts/test_embed_js.py` reports 100% for target; skill-sync report; version-bump report; `git diff --cached --check`; `git diff --check origin/main..HEAD`; `git diff --check`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-run-swift-coverage-extra-643` | `17c78f01` | #643 tooling tranche for `tools/scripts/run_swift_coverage.py` paths | `tools/scripts/test_run_swift_coverage_extra.py` | `python3 tools/scripts/test_run_swift_coverage_extra.py`; `python3 tools/scripts/test_run_swift_coverage.py`; `python3 tools/scripts/test_run_python_coverage.py`; venv-backed `run_python_coverage.py --pattern tools/scripts/test_run_swift_coverage.py --pattern tools/scripts/test_run_swift_coverage_extra.py` reports 96% for target; skill-sync report; version-bump report; `git diff --check`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-android-target-coverage-643` | `f8161902` | #643 tooling tranche for `tools/local-ci/android_target.py` paths | `tools/scripts/test_android_target.py` | `python3 tools/scripts/test_android_target.py`; `python3 tools/scripts/test_run_python_coverage.py`; venv-backed `run_python_coverage.py --pattern tools/scripts/test_android_target.py` reports 100% for target; skill-sync report; version-bump report; `git diff --check`; staged `git diff --cached --check`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-validate-hosts-coverage-643` | `c70debfa` | #643 tooling tranche for `tools/deps/validate_hosts.py` paths | `tools/scripts/test_validate_hosts.py` | `python3 tools/scripts/test_validate_hosts.py`; `python3 tools/scripts/test_run_python_coverage.py`; venv-backed `run_python_coverage.py --pattern tools/scripts/test_validate_hosts.py` reports 97% for target; skill-sync report; version-bump report; `git diff --check HEAD~1..HEAD`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-compat-sync-extra-643` | `b21ac426` | #643 tooling tranche for `tools/scripts/compat_sync_check.py` paths | `tools/scripts/test_compat_sync_check_extra.py` | `python3 tools/scripts/test_compat_sync_check_extra.py`; `python3 tools/scripts/test_compat_sync_check.py`; `python3 tools/scripts/test_run_python_coverage.py`; venv-backed `run_python_coverage.py --pattern tools/scripts/test_compat_sync_check.py --pattern tools/scripts/test_compat_sync_check_extra.py` reports 87% for target; skill-sync report; version-bump report; `git diff --check`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-build-migration-index-extra-643` | `1f61728a` | #643 tooling tranche for `tools/scripts/build_migration_index.py` paths | `tools/scripts/test_build_migration_index_extra.py` | `python3 tools/scripts/test_build_migration_index.py`; `python3 tools/scripts/test_build_migration_index_extra.py`; `python3 tools/scripts/test_run_python_coverage.py`; venv-backed `run_python_coverage.py --pattern tools/scripts/test_build_migration_index.py --pattern tools/scripts/test_build_migration_index_extra.py` reports 100% for target; skill-sync report; version-bump report; `git diff --check HEAD~1 HEAD`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-docs-sync-check-extra-643` | `b9204849` | #643 tooling tranche for `tools/scripts/docs_sync_check.py` paths | `tools/scripts/test_docs_sync_check_extra.py` | `python3 tools/scripts/test_docs_sync_check.py`; `python3 tools/scripts/test_docs_sync_check_extra.py`; `python3 tools/scripts/test_run_python_coverage.py`; venv-backed `run_python_coverage.py --pattern tools/scripts/test_docs_sync_check.py --pattern tools/scripts/test_docs_sync_check_extra.py` reports 99% for target; skill-sync report; version-bump report; `git diff --check origin/main..HEAD`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-coverage-tier-check-extra-643` | `ce7ec28d` | #643 tooling tranche for `tools/scripts/coverage_tier_check.py` paths | `tools/scripts/test_coverage_tier_check_extra.py` | `python3 tools/scripts/test_coverage_tier_check_extra.py`; `python3 tools/scripts/test_coverage_tier_check.py`; `PYTHONPATH=tools/scripts python3 -m unittest tools.scripts.test_coverage_tier_check tools.scripts.test_coverage_tier_check_extra`; `python3 tools/scripts/test_run_python_coverage.py`; venv-backed `run_python_coverage.py --pattern tools/scripts/test_coverage_tier_check.py --pattern tools/scripts/test_coverage_tier_check_extra.py` reports 100% for target; skill-sync report; version-bump report; `git diff --check`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-skill-sync-extra-643` | `0c7b8b41` | #643 tooling tranche for `tools/scripts/skill_sync_check.py` paths | `tools/scripts/test_skill_sync_check_extra.py` | `python3 tools/scripts/test_skill_sync_check_extra.py`; `python3 tools/scripts/test_skill_sync_check.py`; `python3 tools/scripts/test_run_python_coverage.py`; venv-backed `run_python_coverage.py --pattern tools/scripts/test_skill_sync_check.py --pattern tools/scripts/test_skill_sync_check_extra.py` reports 94% for target; skill-sync report; version-bump report; `git diff --check`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-version-bump-extra-643` | `bfbe4af8` | #643 tooling tranche for `tools/scripts/version_bump_check.py` paths | `tools/scripts/test_version_bump_check_extra.py` | `python3 tools/scripts/test_version_bump_check_extra.py`; `python3 tools/scripts/test_run_python_coverage.py`; venv-backed `run_python_coverage.py --pattern tools/scripts/test_gates.py --pattern tools/scripts/test_version_bump_check_extra.py` reports 80% for target; skill-sync report; version-bump report; `git diff --check`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-auto-release-extra-643` | `d901c863` | #643 tooling tranche for `tools/scripts/auto_release_decision.py` CLI and parser edges | `tools/scripts/test_auto_release_decision_extra.py` | `python3 tools/scripts/test_auto_release_decision_extra.py`; `python3 tools/scripts/test_auto_release_decision.py`; `python3 tools/scripts/test_run_python_coverage.py`; venv-backed `run_python_coverage.py --pattern tools/scripts/test_auto_release_decision.py --pattern tools/scripts/test_auto_release_decision_extra.py` reports 100% for target; skill-sync report; version-bump report; `git diff --check`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-iwyu-annotate-extra-643` | `537c5a77` | #643 tooling tranche for `tools/scripts/iwyu_annotate.py` edges | `tools/scripts/test_iwyu_annotate_extra.py` | `python3 tools/scripts/test_iwyu_annotate_extra.py`; `python3 tools/scripts/test_iwyu_annotate.py`; `python3 tools/scripts/test_run_python_coverage.py`; venv-backed `run_python_coverage.py --pattern tools/scripts/test_iwyu_annotate.py --pattern tools/scripts/test_iwyu_annotate_extra.py` reports 100% for target; tranche-scoped skill-sync report; tranche-scoped version-bump report; `git diff --check`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-coverage-diff-comment-extra-643` | `3723aaf5` | #643 tooling tranche for `tools/scripts/coverage_diff_comment.py` CLI and report-rendering edges | `tools/scripts/test_coverage_diff_comment_extra.py` | `python3 tools/scripts/test_coverage_diff_comment_extra.py`; `python3 tools/scripts/test_coverage_diff_comment.py`; `python3 tools/scripts/test_run_python_coverage.py`; venv-backed `run_python_coverage.py --pattern tools/scripts/test_coverage_diff_comment.py --pattern tools/scripts/test_coverage_diff_comment_extra.py` reports 100% for target; skill-sync report; version-bump report; `git diff --check`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-cmajor-external-extra-643` | `19db68f4` | #643 tooling tranche for `tools/scripts/cmajor_external.py` edges | `tools/scripts/test_cmajor_external_extra.py` | `python3 tools/scripts/test_cmajor_external_extra.py`; `python3 tools/scripts/test_cmajor_external.py`; venv-backed `python tools/scripts/test_run_python_coverage.py`; venv-backed `run_python_coverage.py --pattern tools/scripts/test_cmajor_external.py --pattern tools/scripts/test_cmajor_external_extra.py` reports 100% for target; tranche-scoped skill-sync report; tranche-scoped version-bump report; `git diff --check HEAD^ HEAD`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-jsfx-subset-extra-643` | `fc4735b7` | #643 tooling tranche for `tools/scripts/jsfx_subset.py` parser and human-output edges | `tools/scripts/test_jsfx_subset_extra.py` | `python3 tools/scripts/test_jsfx_subset_extra.py`; `python3 tools/scripts/test_jsfx_subset.py`; `python3 tools/scripts/test_run_python_coverage.py`; venv-backed `run_python_coverage.py --pattern tools/scripts/test_jsfx_subset.py --pattern tools/scripts/test_jsfx_subset_extra.py` reports 100% for target; skill-sync report; version-bump report; `git diff --check`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-package-cli-extra-643` | `af178f3a` | #643 tooling tranche for `tools/scripts/package_cli.py` cache, rpath, and macOS packaging edges | `tools/scripts/test_package_cli_extra.py` | `python3 tools/scripts/test_package_cli_extra.py`; `python3 tools/scripts/test_package_cli.py`; `python3 tools/scripts/test_run_python_coverage.py`; venv-backed `run_python_coverage.py --pattern tools/scripts/test_package_cli.py --pattern tools/scripts/test_package_cli_extra.py` reports 98% for target; skill-sync report; version-bump report; `git diff --check`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-resolve-runs-on-extra-643` | `edaba26e` | #643 tooling tranche for `tools/scripts/resolve_runs_on.py` edges | `tools/scripts/test_resolve_runs_on_extra.py` | `python3 tools/scripts/test_resolve_runs_on_extra.py`; `python3 tools/scripts/test_resolve_runs_on.py`; `python3 tools/scripts/test_run_python_coverage.py`; venv-backed `run_python_coverage.py --pattern tools/scripts/test_resolve_runs_on.py --pattern tools/scripts/test_resolve_runs_on_extra.py` reports 100% for target; tranche-scoped skill-sync report; tranche-scoped version-bump report; `git diff --check HEAD~1 HEAD`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-merge-cobertura-extra-643` | `3ac2719e` | #643 tooling tranche for `tools/scripts/merge_cobertura.py` edges | `tools/scripts/test_merge_cobertura_extra.py` | `python3 tools/scripts/test_merge_cobertura_extra.py`; `python3 tools/scripts/test_merge_cobertura.py`; `python3 tools/scripts/test_run_python_coverage.py`; venv-backed `run_python_coverage.py --pattern tools/scripts/test_merge_cobertura.py --pattern tools/scripts/test_merge_cobertura_extra.py` reports 100% for target; tranche-scoped skill-sync report; tranche-scoped version-bump report; `git diff --check HEAD~1..HEAD`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-lcov-cobertura-extra-643` | `1f9b6c7c` | #643 tooling tranche for `tools/scripts/lcov_cobertura.py` edges | `tools/scripts/test_lcov_cobertura_extra.py` | `python3 tools/scripts/test_lcov_cobertura_extra.py`; `python3 tools/scripts/test_lcov_cobertura.py`; `python3 tools/scripts/test_run_python_coverage.py`; venv-backed `run_python_coverage.py --pattern tools/scripts/test_lcov_cobertura.py --pattern tools/scripts/test_lcov_cobertura_extra.py`; focused coverage report shows 98% for target; skill-sync report; version-bump report; `git diff --check`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-run-python-coverage-extra-643` | `5a10f330` | #643 tooling tranche for `tools/scripts/run_python_coverage.py` edges | `tools/scripts/test_run_python_coverage_extra.py` | `python3 tools/scripts/test_run_python_coverage_extra.py`; `python3 tools/scripts/test_run_python_coverage.py`; venv-backed `run_python_coverage.py --pattern tools/scripts/test_run_python_coverage_extra.py`; direct coverage report over existing + extra tests shows 99% for target; version-bump report; tranche-scoped `git diff --check`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-cli-sync-check-extra-643` | `1b1d19e1` | #643 tooling tranche for `tools/scripts/cli_sync_check.py` edges | `tools/scripts/test_cli_sync_check_extra.py` | `python3 tools/scripts/test_cli_sync_check_extra.py`; `python3 tools/scripts/test_cli_sync_check.py`; `python3 tools/scripts/test_run_python_coverage.py`; venv-backed `run_python_coverage.py --pattern tools/scripts/test_cli_sync_check.py --pattern tools/scripts/test_cli_sync_check_extra.py` reports 98% for target; tranche-scoped skill-sync report; version-bump report; `git diff --check`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-local-ci-extra-643` | `e07612c8` | #643 tooling tranche for `tools/local-ci/local_ci.py` helper edges | `tools/local-ci/test_local_ci_extra.py` | `python3 tools/local-ci/test_local_ci_extra.py`; `python3 tools/local-ci/test_local_ci.py`; `python3 tools/scripts/test_run_python_coverage.py`; venv-backed `run_python_coverage.py --pattern tools/local-ci/test_local_ci.py --pattern tools/local-ci/test_local_ci_extra.py` reports 68% for target; skill-sync report; version-bump report; `git diff --check`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-deps-audit-extra-643` | `0b94d497` | #643 tooling tranche for `tools/deps/audit.py` edges | `tools/deps/test_audit_extra.py` | `python3 -m unittest tools.deps.test_audit_extra -v`; `python3 -m unittest tools.deps.test_audit -v`; `python3 -m unittest tools.scripts.test_run_python_coverage -v`; venv-backed `run_python_coverage.py --pattern tools/deps/test_audit.py --pattern tools/deps/test_audit_extra.py` reports 99% for target; tranche-scoped skill-sync report; tranche-scoped version-bump report; `git diff --check`; `git diff --cached --check`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-pulp-sandbox-extra-643` | `a585b261` | #643 tooling tranche for `tools/sandbox-e2e/pulp_sandbox.py` edges | `tools/sandbox-e2e/test_pulp_sandbox_unit.py` | venv-backed `python -m pytest tools/sandbox-e2e/test_pulp_sandbox_unit.py -q` reports 10 passed; venv-backed coverage run over `tools/sandbox-e2e` reports 91% for `pulp_sandbox.py`; worktree clean. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-freshness-extra-643` | `07bbd1d` | #643 tooling tranche for `tools/packages/freshness_check.py` edges | `tools/packages/test_freshness_check_extra.py` | `python3 -m unittest tools/packages/test_package_validation_tools.py tools/packages/test_freshness_check_extra.py` reports 14 tests OK; focused branch coverage for `tools/packages/freshness_check.py` improved from 75% to 98%; `git diff --check --cached`; worktree clean after commit. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-pkg-freshness-extra-643` | `55bee1c5` | #643 alternate tooling tranche for `tools/packages/freshness_check.py` edges | `tools/packages/test_freshness_check_extra.py` | `python3 tools/packages/test_freshness_check_extra.py`; `python3 tools/packages/test_package_validation_tools.py`; focused branch coverage for `tools/packages/freshness_check.py` reports 99%. | Compare against `local/phase3-freshness-extra-643`; choose or merge the better test set before opening one freshness PR. |
| `local/phase3-validate-registry-extra-643` | `2915248b` | #643 tooling tranche for `tools/packages/validate_registry.py` edges | `tools/scripts/test_validate_registry_extra.py` | `python3 tools/scripts/test_validate_registry_extra.py` reports 7 tests OK; venv-backed `run_python_coverage.py --pattern tools/scripts/test_validate_registry_extra.py` exits 0; focused coverage for `tools/packages/validate_registry.py` reports 99%. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |

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
