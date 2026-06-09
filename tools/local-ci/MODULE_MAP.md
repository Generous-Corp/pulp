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
| `ssh_bundle.py` | Git bundle naming, local bundle creation, and SSH upload/progress/probe mechanics. | Target validation execution or queue orchestration. |
| `cleanup.py` | Local-CI artifact cleanup planning/deletion and stale Windows validator cleanup mechanics. | Lock acquisition, runner ownership, or user-facing cleanup command output. |
| `desktop_artifacts.py` | Desktop automation artifact roots and run/publish bundle directory layout. | Report staging, rollup generation, pruning policy, or target execution. |
| `reporting.py` | Desktop automation report staging, run/publish rollups, manifest scanning, proof summaries, and pruning selection. | Artifact directory layout, target execution, source preparation, or CLI output. |
| `source_prep.py` | Exact-SHA desktop source requests, cache keys, launch-command rewriting, prepare manifests, and macOS/Linux/Windows prepared source materialization. | Desktop artifact layout, target execution, queue orchestration, or target probes. |
| `target_preflight.py` | SSH primary/fallback/UTM/Namespace failover checks, material config drift detection, and submission metadata formatting. | Queue mutation, target validation execution, or desktop automation adapters. |
| `queue_orchestrator.py` | Queue policy and in-memory queue helpers: branch validation, default priorities, job fingerprints/records, supersedence and cancellation result payloads, terminal job-state completion, job summaries, retention trimming, priority sort keys, job lookup, active-target upserts, target-state updates, job claiming, and runner-info active-target mutation. | Queue file locking, runner-info persistence, runner liveness, result persistence, drain orchestration, target execution, or user-facing CLI output. |
| `linux_target.py` | Linux desktop launch backend probes, remote tooling probes/readiness details, remote bundle paths, and remote command builders. | SSH execution orchestration, artifact fetch/cleanup, queue orchestration, or manifest writing. |
| `macos_desktop.py` | macOS app bundle detection, Swift window-probe wrappers, window wait/capture/activation/click helpers, app quit, and process termination. | Desktop action orchestration, artifact manifest writing, source preparation, or queue orchestration. |
| `windows_target.py` | Windows desktop target contracts, path safety, session-agent request payloads, and probe result formatting/readiness helpers. | SSH/PowerShell execution, desktop action execution, queue orchestration, or artifact layout. |
| `windows_probe.py` | Windows SSH/PowerShell command execution helpers, remote file transfer/read/remove helpers, repo/session/tooling probes, remote tool installation, session-agent bootstrap/start, and CMake generator probing. | Windows target contract formatting, desktop action orchestration, queue orchestration, or artifact layout. |

## Remaining `local_ci.py` Clusters

`local_ci.py` remains the orchestration entry point. These clusters are
intentionally still there until a later mechanical extraction can move them
behind the contracts added in this slice.

| Cluster | Current responsibility | Extraction target |
| --- | --- | --- |
| Desktop target probes | Desktop doctor orchestration, launch adapters, and automation adapters. | later `execution.py` |
| Queue orchestration | Locking, queue mutation, stale-running requeue, runner-info I/O/liveness, target state updates, drain/wait loops, and CLI-facing queue updates. | later `queue_orchestrator.py` |
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
