# Coverage Compliance Status

Last reviewed: 2026-05-21 03:50 PDT

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
`runner_provider=namespace` for Phase 3 coverage PRs, and do not re-run
stale pre-cutover Namespace jobs. Rebase or push a fresh branch SHA so
the current workflow and runner labels apply.

Subagents are useful for this loop when their write scopes are
disjoint. One agent can monitor merge/failure state while other agents
prepare non-overlapping coverage tranches in separate worktrees.

## Recent Phase 3 Tranches

| Date | Branch | SHA | Scope | Local validation | PR state | Next action |
| --- | --- | --- | --- | --- | --- | --- |
| 2026-05-20 | `feature/phase3-codecov-batch-756` | `35c20f5e0` | Signal helper edges: DelayLine fractional wraparound; LadderFilter reset equivalence and nonpositive buffer no-ops; SVF, WaveShaper, and NoiseGate nonpositive buffer no-ops; Panner equal-power sign/energy invariant. | `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF`; `cmake --build build --target pulp-test-signal -j$(sysctl -n hw.ncpu)`; `./build/test/pulp-test-signal "[coverage][phase3-batch756]"`; skill-sync report; version-bump report; `git diff --check`. | Local commit pending PR. | Ship after rebase validation. |
| 2026-05-20 | `feature/phase3-codecov-batch-755` | `d035c202` merge | Runtime `ScopedNoAlloc` nested-depth and thread-local guard coverage in `pulp-test-runtime-utils`; Shipyard mac target restored to local validation after the Namespace cutover. | `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF`; `cmake --build build --target pulp-test-runtime-utils -j$(sysctl -n hw.ncpu)`; `./build/test/pulp-test-runtime-utils "[runtime][scoped_no_alloc]"`; skill-sync report; version-bump report; `git diff --check`. | PR #2473 merged. | None. |

## Active Tranche Ledger

Keep only active, concrete tranche state here; post queue snapshots to
the tracker issues instead of preserving stale lists.

| Date | Branch | Head | Scope | Local validation | PR state | Next action |
| --- | --- | --- | --- | --- | --- | --- |
| 2026-05-21 | `feature/phase3-codecov-tools-batch-771` | `31435f1d7` | `#643` local-CI state path helpers: assert `prepared_dir()` uses the configured local-CI state root, closing the last accessor gap in `tools/local-ci/state_paths.py`. Focused local-CI helper coverage now reports `state_paths.py`, `git_helpers.py`, `github_workflows.py`, `io_utils.py`, `job_queue.py`, `normalize.py`, `provenance.py`, and `targets.py` at 100%. | `python3 tools/local-ci/test_state_paths.py`; `/tmp/pulp-pycoverage-venv/bin/python tools/scripts/run_python_coverage.py --pattern 'tools/local-ci/test_state_paths.py' --pattern 'tools/local-ci/test_git_helpers.py' --pattern 'tools/local-ci/test_io_utils.py' --pattern 'tools/local-ci/test_job_queue.py' --pattern 'tools/local-ci/test_normalize.py' --pattern 'tools/local-ci/test_provenance.py' --pattern 'tools/local-ci/test_targets.py' --pattern 'tools/local-ci/test_footprint.py' --pattern 'tools/local-ci/test_github_workflows.py'`; `git diff --check`. | Local only, not pushed. | Continue accumulating tools/CI coverage fixes toward the next 24-36 fix PR while monitoring open PR checks/comments through REST. |
| 2026-05-21 | `feature/phase3-codecov-tools-batch-771` | `b5f6520ef` | `#643` release Skia fetcher tooling: cover the empty architecture-subdirectory normalization path so `fetch_skia_for_release.py` exercises the no-file-moved branch before reporting a missing-library layout error. Focused coverage now reports `tools/scripts/fetch_skia_for_release.py` at 100%. | `python3 tools/scripts/test_fetch_skia_for_release.py`; `/tmp/pulp-pycoverage-venv/bin/python tools/scripts/run_python_coverage.py --pattern 'tools/scripts/test_fetch_skia_for_release.py' --pattern 'tools/scripts/test_fetch_skia_for_release_extra.py'`; `git diff --check`. | Local only, not pushed. | Continue accumulating tools/CI coverage fixes toward the next 24-36 fix PR while monitoring open PR checks/comments through REST. |
| 2026-05-21 | `feature/phase3-codecov-tools-batch-771` | `c1c82a448` | `#643` macOS reroute watcher tooling: cover the script `__main__` path by running the file with patched CLI arguments and a handled subprocess interrupt. Focused coverage now reports `tools/scripts/macos_reroute_watcher.py` at 100%. | `python3 tools/scripts/test_macos_reroute_watcher.py`; `/tmp/pulp-pycoverage-venv/bin/python tools/scripts/run_python_coverage.py --pattern 'tools/scripts/test_macos_reroute_watcher.py'`; `git diff --check`. | Local only, not pushed. | Continue accumulating tools/CI coverage fixes toward the next 24-36 fix PR while monitoring open PR checks/comments through REST. |
| 2026-05-21 | `feature/phase3-codecov-tools-batch-771` | `822cfc7b1` | `#643` LLDB debug formatter tooling: add fake LLDB value-object tests for atomic float extraction across libc++/libstdc++/legacy layouts, parameter/range/state/geometry/color/listener summaries, BufferView empty/sparkline/error paths, and LLDB registration commands. Focused coverage now reports `tools/debug/pulp_lldb.py` at 100%. | `python3 tools/scripts/test_pulp_lldb.py`; `/tmp/pulp-pycoverage-venv/bin/python tools/scripts/run_python_coverage.py --pattern 'tools/scripts/test_pulp_lldb.py'`; `git diff --check`. | Local only, not pushed. | Continue accumulating tools/CI coverage fixes toward the next 24-36 fix PR while monitoring open PR checks/comments through REST. |
| 2026-05-21 | `feature/phase3-codecov-tools-batch-771` | `5be729d9a` | `#643` Node ABI gate: add in-process unit tests for repo/git helpers, comment stripping, class-body parsing, nested parameter/default normalization, virtual declaration canonicalization, mismatch rendering, surface check failures, CLI mode behavior, and script entrypoint handling. Focused coverage now reports `tools/scripts/node_abi_gate.py` at 100% while preserving the existing subprocess integration tests. | `python3 tools/scripts/test_node_abi_gate.py`; `/tmp/pulp-pycoverage-venv/bin/python tools/scripts/run_python_coverage.py --pattern 'tools/scripts/test_node_abi_gate.py'`; `git diff --check`. | Local only, not pushed. | Continue accumulating tools/CI coverage fixes toward the next 24-36 fix PR while monitoring open PR checks/comments through REST. |
| 2026-05-21 | `feature/phase3-codecov-tools-batch-771` | `2386703a3` | `#643` coverage tooling: add deterministic branch/error-path tests for `coverage_tier_check.py` and `coverage_diff_comment.py`, including YAML config rejection, git diff failures, Cobertura parser edge cases, empty changed-line aggregates, direct glob classification, extensionless source filtering, required/advisory comment rendering, and script `__main__` entrypoints. Focused coverage now reports both modules at 100%. | `python3 tools/scripts/test_coverage_tier_check.py`; `python3 tools/scripts/test_coverage_diff_comment.py`; `/tmp/pulp-pycoverage-venv/bin/python tools/scripts/run_python_coverage.py --pattern 'tools/scripts/test_coverage_diff_comment.py' --pattern 'tools/scripts/test_coverage_tier_check.py'`; import-validation strict/tests; skill-sync report; version-bump report; `git diff --check refs/remotes/origin/main..HEAD`. | Local only, not pushed. | Continue accumulating tools/CI coverage fixes toward the next 24-36 fix PR while monitoring open PR checks/comments through REST. |
| 2026-05-21 | `feature/phase3-codecov-tools-batch-771` | `9bf4d7c81` | `#643` local-CI tools: add and extend deterministic tests for extracted helper modules. Focused coverage now reports `tools/local-ci/github_workflows.py`, `tools/local-ci/io_utils.py`, `tools/local-ci/job_queue.py`, `tools/local-ci/normalize.py`, `tools/local-ci/provenance.py`, `tools/local-ci/state_paths.py`, and `tools/local-ci/targets.py` at 100%; `tools/local-ci/footprint.py` remains 97% with a defensive tail return that coverage.py still marks unreachable. | `python3 tools/local-ci/test_normalize.py`; `python3 tools/local-ci/test_targets.py`; `python3 tools/local-ci/test_job_queue.py`; `python3 tools/local-ci/test_provenance.py`; `python3 tools/local-ci/test_state_paths.py`; `python3 tools/local-ci/test_github_workflows.py`; `python3 tools/local-ci/test_io_utils.py`; `/tmp/pulp-pycoverage-venv/bin/python tools/scripts/run_python_coverage.py --pattern 'tools/local-ci/test_state_paths.py' --pattern 'tools/local-ci/test_git_helpers.py' --pattern 'tools/local-ci/test_io_utils.py' --pattern 'tools/local-ci/test_job_queue.py' --pattern 'tools/local-ci/test_normalize.py' --pattern 'tools/local-ci/test_provenance.py' --pattern 'tools/local-ci/test_targets.py' --pattern 'tools/local-ci/test_footprint.py' --pattern 'tools/local-ci/test_github_workflows.py'`; import-validation strict/tests; skill-sync report; version-bump report; `git diff --check`. | Local only, not pushed. | Continue accumulating tools/CI coverage fixes toward the next 24-36 fix PR while monitoring open PR checks/comments through REST. |
| 2026-05-21 | `feature/phase3-codecov-tools-batch-771` | `e181b83e6` | `#643` tools: extend the local tools tranche with deterministic branch/error-path tests for `source_tree_pollution_check.py`, `verify_cobertura_xml.py`, `compat_aggregate.py`, `docs_noise_lint.py`, `check_format_validation.py`, `fetch_skia_for_release.py`, `version_bump_surfaces.py`, `tools/packages/freshness_check.py`, and `tools/check-docs-consistency.py`. Focused coverage now reports `auto_release_decision.py`, `bench_diff.py`, `check_shipyard_pin.py`, `resolve_runs_on.py`, `source_tree_pollution_check.py`, `validation_profile_select.py`, `verify_cobertura_xml.py`, `compat_aggregate.py`, `docs_noise_lint.py`, `check_format_validation.py`, `version_bump_surfaces.py`, `tools/packages/freshness_check.py`, `tools/packages/validate_registry.py`, and `tools/check-docs-consistency.py` at 100%; `fetch_skia_for_release.py` remains 99% with one coverage.py partial branch on a `try:` target. | `python3 tools/scripts/test_source_tree_pollution_check.py`; `python3 tools/scripts/test_verify_cobertura_xml.py`; `python3 tools/scripts/test_compat_aggregate.py`; `python3 tools/scripts/test_check_format_validation.py`; `python3 tools/scripts/test_docs_noise_lint.py`; `python3 tools/scripts/test_fetch_skia_for_release.py`; `python3 tools/scripts/test_version_bump_surfaces.py`; `python3 tools/packages/test_freshness_check_extra.py`; `python3 tools/scripts/test_check_docs_consistency.py`; `/tmp/pulp-pycoverage-venv/bin/python tools/scripts/run_python_coverage.py --pattern 'tools/scripts/test_auto_release_decision*.py' --pattern 'tools/scripts/test_bench_diff.py' --pattern 'tools/scripts/test_check_shipyard_pin.py' --pattern 'tools/scripts/test_validation_profile_select.py' --pattern 'tools/scripts/test_verify_cobertura_xml.py' --pattern 'tools/scripts/test_source_tree_pollution_check.py' --pattern 'tools/scripts/test_resolve_runs_on*.py'`; `/tmp/pulp-pycoverage-venv/bin/python tools/scripts/run_python_coverage.py --pattern 'tools/scripts/test_compat_aggregate.py' --pattern 'tools/scripts/test_docs_noise_lint.py' --pattern 'tools/scripts/test_fetch_skia_for_release*.py' --pattern 'tools/scripts/test_check_format_validation.py'`; `/tmp/pulp-pycoverage-venv/bin/python tools/scripts/run_python_coverage.py --pattern 'tools/scripts/test_version_bump_surfaces.py' --pattern 'tools/scripts/test_validate_registry_extra.py' --pattern 'tools/packages/test_freshness_check_extra.py'`; `/tmp/pulp-pycoverage-venv/bin/python tools/scripts/run_python_coverage.py --pattern 'tools/scripts/test_check_docs_consistency.py'`; import-validation strict/tests; skill-sync report; version-bump report; `git diff --check`. | Local only, not pushed. | Continue accumulating tools/CI coverage fixes toward the next 24-36 fix PR while monitoring open PR checks/comments through REST. |
| 2026-05-21 | `feature/phase3-codecov-deps-validate-batch-772` | `cbfa1d9cc` | `#643` harness adapter primitives: add deterministic coverage for adapter registry stamping and invalid-name guards, unstampable class registration, `CatalogEntry.from_compat_json`, short-name/default expected-status behavior, `Result.drifts`/`to_dict`, and the abstract adapter `run()` guard. Focused coverage now reports `tools/harness/adapters/base.py` at 100%. | `python3 -m unittest tools.harness.tests.test_adapters_base`; `python3 tools/scripts/test_run_python_coverage.py`; `python3 tools/scripts/test_run_python_coverage_extra.py`; `/tmp/pulp-pycoverage-venv/bin/python tools/scripts/run_python_coverage.py --pattern 'tools/harness/tests/test_adapters_base.py'`; `git diff --check`. | Local only, not pushed. | Continue accumulating harness adapter coverage fixes toward the next 24-36 fix PR while monitoring open PR checks/comments through REST. |
| 2026-05-21 | `feature/phase3-codecov-deps-validate-batch-772` | `3b90c0f48` | `#643` harness report rendering: add deterministic Markdown/JSON renderer tests for summary buckets, totals, drift lists, escaped/truncated detail text, default validation/visual labels, and alias fields. Focused coverage now reports `tools/harness/verify_report.py` at 99%. | `python3 -m unittest tools.harness.tests.test_verify_report`; `python3 tools/scripts/test_run_python_coverage.py`; `python3 tools/scripts/test_run_python_coverage_extra.py`; `/tmp/pulp-pycoverage-venv/bin/python tools/scripts/run_python_coverage.py --pattern 'tools/harness/tests/test_verify_report.py'`; `git diff --check`. | Local only, not pushed. | Continue accumulating harness coverage fixes toward the next 24-36 fix PR while monitoring open PR checks/comments through REST. |
| 2026-05-21 | `feature/phase3-codecov-deps-validate-batch-772` | `ac7bde2c8` | `#643` harness evidence checks: update stale test fixtures to include real Catch2 tags under the stricter #1737 evidence contract and include the evidence-check unittest in the Python coverage surface. Focused coverage now reports `tools/harness/verify_evidence.py` at 90%. | `python3 -m unittest tools.harness.tests.test_evidence_check`; `python3 tools/scripts/test_run_python_coverage.py`; `python3 tools/scripts/test_run_python_coverage_extra.py`; `/tmp/pulp-pycoverage-venv/bin/python tools/scripts/run_python_coverage.py --pattern 'tools/harness/tests/test_canvas2d_adapter_shim_files.py' --pattern 'tools/harness/tests/test_evidence_check.py' --pattern 'tools/harness/tests/test_html_adapter_element_events_union.py' --pattern 'tools/harness/tests/test_status.py' --pattern 'tools/harness/tests/test_validate_test_ref.py'`; `git diff --check`. | Local only, not pushed. | Continue accumulating harness coverage fixes toward the next 24-36 fix PR while monitoring open PR checks/comments through REST. |
| 2026-05-21 | `feature/phase3-codecov-deps-validate-batch-772` | `c6e21450c` | `#643` visual harness reachable edges: cover matching boolean semantic diffs, orphan golden discovery without a surface filter, JSON/PNG capture type guards, and empty-surface CLI error handling. Focused coverage now reports `tools/harness/visual/spec.py` at 100%, `runner.py` at 93%, and `differ.py` at 98%. | `python3 -m unittest tools.harness.visual.tests.test_differ`; `python3 -m unittest tools.harness.visual.tests.test_runner`; `/tmp/pulp-pycoverage-venv/bin/python tools/scripts/run_python_coverage.py --pattern 'tools/harness/visual/tests/test_check_skia_pin.py' --pattern 'tools/harness/visual/tests/test_differ.py' --pattern 'tools/harness/visual/tests/test_runner.py'`; `git diff --check`. | Local only, not pushed. | Continue accumulating non-overlapping visual harness coverage fixes toward the next 24-36 fix PR while monitoring open PR checks/comments through REST. |
| 2026-05-21 | `feature/phase3-codecov-deps-validate-batch-772` | `d39a7b58b` | `#643` visual harness Skia pin lint: cover Dockerfile ARG parsing, missing input files, malformed manifest determinism blocks, main parse/drift/success exits, and direct pin-constant import. Focused coverage now reports `tools/harness/visual/check_skia_pin.py` at 98% and `tools/harness/visual/pins.py` at 100%. | `python3 -m unittest tools.harness.visual.tests.test_check_skia_pin`; `python3 tools/scripts/test_run_python_coverage.py`; `python3 tools/scripts/test_run_python_coverage_extra.py`; `/tmp/pulp-pycoverage-venv/bin/python tools/scripts/run_python_coverage.py --pattern 'tools/harness/visual/tests/test_check_skia_pin.py' --pattern 'tools/harness/visual/tests/test_differ.py' --pattern 'tools/harness/visual/tests/test_runner.py'`; `git diff --check`. | Local only, not pushed. | Continue accumulating visual harness coverage fixes toward the next 24-36 fix PR while monitoring open PR checks/comments through REST. |
| 2026-05-21 | `feature/phase3-codecov-deps-validate-batch-772` | `859f6448a` | `#643` visual harness tooling: include the deterministic visual harness unittest modules in the Python coverage lane and cover validation-reference classification across semantic, visual, DOM, behavior, unit, and cannot-validate refs. Focused coverage reports `tools/harness/visual/differ.py` at 98%, `runner.py` at 91%, `spec.py` at 97%, and `check_skia_pin.py` at 80%. | `python3 -m unittest tools.harness.visual.tests.test_differ`; `python3 -m unittest tools.harness.visual.tests.test_check_skia_pin`; `python3 -m unittest tools.harness.visual.tests.test_runner`; `python3 tools/scripts/test_run_python_coverage.py`; `python3 tools/scripts/test_run_python_coverage_extra.py`; `/tmp/pulp-pycoverage-venv/bin/python tools/scripts/run_python_coverage.py --pattern 'tools/deps/test_*.py' --pattern 'tools/harness/tests/test_canvas2d_adapter_shim_files.py' --pattern 'tools/harness/tests/test_html_adapter_element_events_union.py' --pattern 'tools/harness/tests/test_status.py' --pattern 'tools/harness/tests/test_validate_test_ref.py' --pattern 'tools/harness/visual/tests/test_check_skia_pin.py' --pattern 'tools/harness/visual/tests/test_differ.py' --pattern 'tools/harness/visual/tests/test_runner.py'`; `git diff --check`. | Local only, not pushed. | Continue accumulating non-overlapping visual harness coverage fixes toward the next 24-36 fix PR while monitoring open PR checks/comments through REST. |
| 2026-05-21 | `feature/phase3-codecov-deps-validate-batch-772` | `ed7745c4d` | `#643` harness tooling: add explicit coverage for `Status` predicate and `StatusCounts` aggregate math, and include the deterministic harness status/test-reference/shim unit tests in the Python coverage surface. Focused coverage now reports `tools/harness/status.py` at 100% alongside partial adapter/verifier coverage from existing harness tests. | `python3 -m unittest tools.harness.tests.test_status`; `python3 -m unittest tools.harness.tests.test_validate_test_ref`; `python3 -m unittest tools.harness.tests.test_canvas2d_adapter_shim_files`; `python3 -m unittest tools.harness.tests.test_html_adapter_element_events_union`; `/tmp/pulp-pycoverage-venv/bin/python tools/scripts/run_python_coverage.py --pattern 'tools/deps/test_*.py' --pattern 'tools/harness/tests/test_canvas2d_adapter_shim_files.py' --pattern 'tools/harness/tests/test_html_adapter_element_events_union.py' --pattern 'tools/harness/tests/test_status.py' --pattern 'tools/harness/tests/test_validate_test_ref.py'`; `python3 tools/scripts/test_run_python_coverage.py`; `python3 tools/scripts/test_run_python_coverage_extra.py`; `git diff --check`. | Local only, not pushed. | Continue accumulating non-overlapping tools/harness coverage fixes toward the next 24-36 fix PR while monitoring open PR checks/comments through REST. |
| 2026-05-21 | `feature/phase3-codecov-deps-validate-batch-772` | `128aa252a` | `#643` dependency validation tooling: add deterministic unit coverage for `tools/deps/validate_hosts.py` config loading, current-branch detection, local subprocess result handling, Unix/Windows remote command builders, main orchestration, and script entrypoint behavior. Focused coverage now reports `tools/deps/audit.py` and `tools/deps/validate_hosts.py` at 100%. | `python3 tools/deps/test_validate_hosts.py`; `/tmp/pulp-pycoverage-venv/bin/python tools/scripts/run_python_coverage.py --pattern 'tools/deps/test_*.py'`; `git diff --check`. | Local only, not pushed. | Continue accumulating non-overlapping tools/deps coverage fixes toward the next 24-36 fix PR while monitoring open PR checks/comments through REST. |
| 2026-05-20 | `feature/phase3-host-format-batch-769` | `512fb2a68` | `#493` format: `ValidationHarness` installed-validator coverage for fake `pluginval` success/failure command paths and `clap-validator` default-format detection; `plugin_state_io` envelope layout coverage for payload size fields, store/plugin payload ordering, CRC footer encoding, and invalid migration registration guardrails; `RemoteViewSession` coverage for input forwarding, host-side parameter updates, invalid remote `param_get` results, null-channel attach rejection, closed-channel handshake failure, and escaped metadata names so plugin/parameter names with quotes or newlines remain valid JSON; `HeadlessHost` combined low-level process seam coverage for explicit MIDI, parameter events, and process context in one call; format registry coverage for processor factory registration, replacement, and scoped restoration; `SettingsPanel` test-tone off path, disabled frequency-change guard coverage, and explicit-output-device selection fix when the configured output follows the default device; `StandaloneApp` idle `apply_config` coverage and headless-without-screenshot rejection before processor/audio startup; `for_each_subblock` zero-sample, nonpositive-event, end-boundary, and input/output slice alignment coverage; `ControlRateParamSmoother` invalid-rate, `skip()`, and `reset()` control-path coverage. | `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF`; `cmake --build build --target pulp-test-validation-harness --parallel`; `./build/test/pulp-test-validation-harness --reporter compact`; `cmake --build build --target pulp-test-plugin-state-io --parallel`; `./build/test/pulp-test-plugin-state-io --reporter compact`; `cmake --build build --target pulp-test-remote-view --parallel`; `./build/test/pulp-test-remote-view --reporter compact`; `cmake --build build --target pulp-test-headless --parallel`; `./build/test/pulp-test-headless --reporter compact`; `cmake --build build --target pulp-test-standalone-editor-chrome --parallel`; `./build/test/pulp-test-standalone-editor-chrome --reporter compact`; `cmake --build build --target pulp-test-processor-defaults --parallel`; `./build/test/pulp-test-processor-defaults --reporter compact`; source-contract strict/tests; skill-sync report; version-bump report; `git diff --check`. | Local commit pending batch PR. | Continue accumulating host/format coverage tranches before pushing. |
| 2026-05-20 | `feature/phase3-coverage-scripts-batch-764` | `2f37d3bd` | `#643` tools: 25 focused tests covering coverage/CI helper edges in `bench_diff.py`, `check_cli_mcp_parity.py`, `docs_noise_lint.py`, `fetch_skia_for_release.py`, `macos_reroute_watcher.py`, and the root pollution allowlist; also allowlists the new top-level `compat/` directory so current-main pushes are not blocked. Focused coverage now reports `bench_diff.py` and `check_cli_mcp_parity.py` at 100%, with `docs_noise_lint.py`, `fetch_skia_for_release.py`, and `macos_reroute_watcher.py` at 99%. | `python3 tools/scripts/test_bench_diff.py`; `python3 tools/scripts/test_check_cli_mcp_parity.py`; `python3 tools/scripts/test_docs_noise_lint.py`; `python3 tools/scripts/test_fetch_skia_for_release_extra.py`; `python3 tools/scripts/test_macos_reroute_watcher.py`; `python3 tools/scripts/test_source_tree_pollution_check.py`; `python3 tools/scripts/source_tree_pollution_check.py --mode=root-allowlist --rev HEAD`; `/tmp/pulp-pycoverage-venv/bin/python tools/scripts/run_python_coverage.py --pattern 'tools/scripts/test_bench_diff.py' --pattern 'tools/scripts/test_check_cli_mcp_parity.py' --pattern 'tools/scripts/test_docs_noise_lint.py' --pattern 'tools/scripts/test_fetch_skia_for_release.py' --pattern 'tools/scripts/test_fetch_skia_for_release_extra.py' --pattern 'tools/scripts/test_macos_reroute_watcher.py'`; `yamllint --no-warnings -d relaxed .github/workflows/`; source-contract strict/tests; skill-sync report; version-bump report; `git diff --check origin/main..HEAD`. | Local commit pending PR. | Ship after final rebase validation. |
| 2026-05-20 | `feature/phase3-ci-tools-batch-763` | `2194d074` | `#643` tools: 30 focused tests covering `verify_cobertura_xml.py`, `format_baseline_diff.py`, `host_pump_lint.py`, `gate_common.py`, and `source_tree_pollution_check.py` edge paths; also tightens the Cobertura gate to reject non-positive `lines-valid`. Focused coverage now reports `format_baseline_diff.py`, `gate_common.py`, and `host_pump_lint.py` at 100%, `source_tree_pollution_check.py` at 98%, and `verify_cobertura_xml.py` at 98%. | `python3 tools/scripts/test_verify_cobertura_xml.py`; `python3 tools/scripts/test_format_baseline_diff.py`; `python3 tools/scripts/test_host_pump_lint.py`; `python3 tools/scripts/test_gate_common.py`; `python3 tools/scripts/test_source_tree_pollution_check.py`; `/tmp/pulp-pycoverage-venv/bin/python tools/scripts/run_python_coverage.py --pattern 'tools/scripts/test_verify_cobertura_xml.py' --pattern 'tools/scripts/test_format_baseline_diff.py' --pattern 'tools/scripts/test_host_pump_lint.py' --pattern 'tools/scripts/test_gate_common.py' --pattern 'tools/scripts/test_source_tree_pollution_check.py'`; `yamllint --no-warnings -d relaxed .github/workflows/`; source-contract strict/tests; skill-sync report; version-bump report; `git diff --check origin/main..HEAD`. | PR #2534 merged. | None. |
| 2026-05-20 | `feature/phase3-ci-scripts-batch-762` | `bdf57f2e9` | `#643` CI helper tests for classify gate, runner resolution, release decision, validation profile selection, and Shipyard pin checks. | Targeted Python suites for `test_classify_changes.py`, `test_resolve_runs_on.py`, `test_auto_release_decision.py`, `test_validation_profile_select.py`, and `test_check_shipyard_pin.py`; focused coverage showed all five target scripts at 100%; cheap gates passed. | PR #2533 merged. | None. |
| 2026-05-20 | `feature/phase3-runtime-batch-758` | `5b1b208e9` | Runtime/audio edge coverage across deterministic Phase 3 helpers. | Targeted local runtime/audio test validation, cheap gates, and rebase validation completed before push. | PR #2519 open; checks queued/in progress with no failures or comments as of latest sweep. | Monitor CI and comments; merge when green. |
| 2026-05-20 | `feature/phase3-midi-tools-batch-757` | `834e6e10` | `#643` tools: cover `tools/scripts/format_baseline_diff.py` success, failure, bootstrap, diff, and truncation paths; cover `tools/scripts/gate_common.py` glob/trailer/helper edge cases; cover `tools/scripts/validation_profile_select.py` file-input, git-failure, JSON, and recursive-match edges; cover `tools/scripts/classify_changes.py` diff query, rename-detection guard, and JSON fail-closed paths; cover `tools/scripts/source_tree_pollution_check.py` staged/push diff helpers and revision blob reads; cover `tools/scripts/resolve_runs_on.py` PR fallback routing and align `build.yml` with the current github-hosted default when Namespace vars are drained; cover `tools/scripts/verify_cobertura_xml.py` missing-file hint output; cover `tools/scripts/macos_reroute_watcher.py` failed-reroute fallthrough, watch-loop exception handling, interrupt exit, and CLI argument parsing; cover `tools/scripts/docs_noise_lint.py` path normalization, line-map merge, diff parser, git error, file read error, root validation, runtime error, and long finding formatting edges; cover `tools/scripts/merge_cobertura.py` parser guardrails for missing filenames, missing lines, invalid line attributes, lower duplicate hits, and zero-hit lines; make macOS coverage fetch the pinned Skia asset before `scripts/run_coverage.sh` so GPU example configure does not fail on fresh self-hosted runners; align Shipyard workflow pins with `tools/shipyard.toml`; update compat-sync unknown-kind test to the current fail-closed contract. Android coverage: stabilize motion rect component tests under mockable `android.jar` and use direct rect field arithmetic so the always-on Android JaCoCo lane emits coverage XML deterministically. | `python3 tools/scripts/test_format_baseline_diff.py`; `/tmp/pulp-pycoverage-venv/bin/python tools/scripts/run_python_coverage.py --pattern 'tools/scripts/test_format_baseline_diff.py'` (`format_baseline_diff.py` 97%); `python3 tools/scripts/test_gate_common.py`; `python3 tools/scripts/test_validation_profile_select.py`; `python3 tools/scripts/test_classify_changes.py`; `/tmp/pulp-pycoverage-venv/bin/python tools/scripts/run_python_coverage.py --pattern 'tools/scripts/test_classify_changes.py'` (`classify_changes.py` 100%); `python3 tools/scripts/test_source_tree_pollution_check.py`; `/tmp/pulp-pycoverage-venv/bin/python tools/scripts/run_python_coverage.py --pattern 'tools/scripts/test_source_tree_pollution_check.py'` (`source_tree_pollution_check.py` 94%); `python3 tools/scripts/test_resolve_runs_on.py`; `/tmp/pulp-pycoverage-venv/bin/python tools/scripts/run_python_coverage.py --pattern 'tools/scripts/test_resolve_runs_on.py'` (`resolve_runs_on.py` 96%); `python3 tools/scripts/test_verify_cobertura_xml.py`; `/tmp/pulp-pycoverage-venv/bin/python tools/scripts/run_python_coverage.py --pattern 'tools/scripts/test_verify_cobertura_xml.py'` (`verify_cobertura_xml.py` 98%); `python3 tools/scripts/test_macos_reroute_watcher.py`; `/tmp/pulp-pycoverage-venv/bin/python tools/scripts/run_python_coverage.py --pattern 'tools/scripts/test_macos_reroute_watcher.py'` (`macos_reroute_watcher.py` 98%); `python3 tools/scripts/test_docs_noise_lint.py`; `/tmp/pulp-pycoverage-venv/bin/python tools/scripts/run_python_coverage.py --pattern 'tools/scripts/test_docs_noise_lint.py'` (`docs_noise_lint.py` 95%); `python3 tools/scripts/test_merge_cobertura.py`; `/tmp/pulp-pycoverage-venv/bin/python tools/scripts/run_python_coverage.py --pattern 'tools/scripts/test_merge_cobertura.py'` (`merge_cobertura.py` 99%); `python3 tools/scripts/test_workflow_build_dirs.py`; `python3 tools/scripts/test_workflow_lint.py`; `yamllint --no-warnings -d relaxed .github/workflows/`; `python3 tools/scripts/test_check_shipyard_pin.py`; `python3 tools/scripts/test_compat_sync_check_extra.py`; `JAVA_HOME=/opt/homebrew/opt/openjdk@17 ANDROID_HOME=$HOME/Library/Android/sdk ANDROID_NDK_HOME=$HOME/Library/Android/sdk/ndk/30.0.14904198 ./gradlew :app:testDebugUnitTest --tests com.pulp.motion.PulpMotionTest`; `JAVA_HOME=/opt/homebrew/opt/openjdk@17 ANDROID_HOME=$HOME/Library/Android/sdk ANDROID_NDK_HOME=$HOME/Library/Android/sdk/ndk/30.0.14904198 ./gradlew :app:testDebugUnitTest :app:jacocoDebugUnitTestReport`; JaCoCo XML line counter `259/777`; `git diff --check github/main...HEAD`. | Local only, not pushed. | Batch is at the 24-commit lower bound; run final cheap checks, rebase if `github/main` moved, then ship through `shipyard pr` unless another queued PR needs an urgent cherry-pick. |

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
