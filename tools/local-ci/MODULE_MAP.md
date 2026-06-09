# Local CI Module Map

This map is the Phase 1.3 contract for `tools/local-ci`: it records the
current ownership boundaries before more code moves out of `local_ci.py`.
Future extraction PRs should preserve the contracts below or update this file
and the matching contract tests in the same change.

## Extracted Modules

| Module | Owns | Must not own |
| --- | --- | --- |
| `state_paths.py` | State/config/log/bundle path resolution and state-directory creation. | Queue JSON contents, result schema, or target behavior. |
| `io_utils.py` | Atomic writes, file locks, log tailing, line trimming, and image-change summaries. | CI state semantics or target orchestration. |
| `normalize.py` | Priority, validation mode, desktop mode, boolean, adapter, and desktop config normalization. | Queue persistence or dispatch side effects. |
| `provenance.py` | Stable direct/hosted provenance dictionaries and summaries for jobs and results. | GitHub or Shipyard API calls. |
| `job_queue.py` | Queue entry normalization and unlocked queue load/save. | Lock acquisition, stale-running reconciliation, supersedence side effects. |
| `targets.py` | Enabled-target discovery, `--targets` parsing, and configured target resolution. | Transport-specific preflight, SSH probes, fallback routing. |
| `github_workflows.py` | Pure GitHub Actions workflow/default/provider resolution. | `gh` subprocess calls, workflow dispatch, or polling. |
| `cloud.py` | GitHub/Namespace cloud-run records, cost/history helpers, dispatch wrappers, and formatting. | Local/SSH validation execution. |
| `footprint.py` | Local-CI state-size accounting and cleanup entry descriptions. | Cleanup candidate selection or deletion. |
| `evidence_index.py` | Result-to-evidence normalization, latest passing target evidence, evidence index persistence, and evidence summaries. | Queue mutation, runner state, result creation, or target execution. |

## Remaining `local_ci.py` Clusters

`local_ci.py` remains the orchestration entry point. These clusters are
intentionally still there until a later mechanical extraction can move them
behind the contracts added in this slice.

| Cluster | Current responsibility | Extraction target |
| --- | --- | --- |
| Bundle transport | Build git bundles, upload/reuse them on SSH targets, and track bundle refs. | `ssh_bundle.py` |
| Desktop artifact layout | Create run/publish bundles, desktop artifact roots, receipts, screenshots, and reports. | `desktop_artifacts.py` |
| Desktop source preparation | Exact-SHA local/macOS, Linux, and Windows source preparation; command rewriting; prepare stamps. | `source_prep.py` |
| Desktop target probes | macOS/Linux/Windows desktop tool probes, launch backends, and automation adapters. | `macos_desktop.py`, `linux_target.py`, `windows_target.py` |
| Queue orchestration | Locking, enqueue/dedupe/supersede, stale-running requeue, runner info, target state updates. | `queue_orchestrator.py` |
| Cleanup planning | Retention policy for result/log/bundle/prepared artifacts and deletion execution. | `cleanup.py` |
| Target preflight | SSH primary/fallback/UTM/Namespace failover checks and submission metadata warnings. | `target_preflight.py` |
| Validation execution | Local, SSH, Windows, Linux, smoke/full validation commands and target status reporting. | `execution.py` |
| CLI dispatch | Argument parser, subcommand routing, and user-facing command output. | `cli.py` or retained thin entrypoint |

## Behavior Contracts

The contract tests in `test_local_ci_contracts.py` pin the seams that future
extractions must preserve:

- Queue/evidence: exact-SHA jobs dedupe by branch/SHA/targets/validation, a
  newer SHA supersedes older pending work in the same scope, and evidence keeps
  only latest passing per-target records.
- Target preflight: unreachable SSH targets either fail clearly, remain queued
  only with `--allow-unreachable-targets`, or fail over to Namespace when that
  provider is configured.
- Source preparation: source cache keys include SHA and prepare command, launch
  commands rewrite repo-relative executables into the prepared source root, and
  prepared state is target/mode scoped.
- Cleanup: live queue artifacts are retained, orphan retention counts are
  honored, and prepared-state cleanup is opt-in.
- Artifact publishing: desktop run/publish bundles keep their stable directory
  shape, and branch publishing reports deterministic paths and URLs without
  depending on live network operations in tests.
