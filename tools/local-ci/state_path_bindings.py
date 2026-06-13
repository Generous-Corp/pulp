"""Compatibility installer for local_ci state path helpers."""

from __future__ import annotations

from typing import Any

from state_path_artifact_bindings import (
    STATE_PATH_ARTIFACT_EXPORTS,
    bundles_dir,
    desktop_receipts_dir,
    desktop_state_dir,
    install_state_path_artifact_helpers,
    prepared_dir,
)
from state_path_config_bindings import config_path, shared_config_path, state_dir, worktree_config_path
from state_path_core_bindings import (
    STATE_PATH_CORE_EXPORTS,
    cloud_runs_dir,
    ensure_state_dirs,
    evidence_path,
    install_state_path_core_helpers,
    logs_dir,
    queue_path,
    results_dir,
)
from state_path_lock_bindings import (
    STATE_PATH_LOCK_EXPORTS,
    drain_lock_path,
    evidence_lock_path,
    install_state_path_lock_helpers,
    queue_lock_path,
    runner_info_path,
)
from state_path_log_bindings import (
    STATE_PATH_LOG_EXPORTS,
    install_state_path_log_helpers,
    job_logs_dir,
    prepare_target_log,
    target_log_path,
)


STATE_PATH_EXPORTS = (
    STATE_PATH_CORE_EXPORTS
    + STATE_PATH_ARTIFACT_EXPORTS
    + STATE_PATH_LOCK_EXPORTS
    + STATE_PATH_LOG_EXPORTS
)


def install_state_path_helpers(bindings: dict[str, Any], names: tuple[str, ...] = STATE_PATH_EXPORTS) -> None:
    core_names = tuple(name for name in names if name in STATE_PATH_CORE_EXPORTS)
    artifact_names = tuple(name for name in names if name in STATE_PATH_ARTIFACT_EXPORTS)
    lock_names = tuple(name for name in names if name in STATE_PATH_LOCK_EXPORTS)
    log_names = tuple(name for name in names if name in STATE_PATH_LOG_EXPORTS)
    install_state_path_core_helpers(bindings, core_names)
    install_state_path_artifact_helpers(bindings, artifact_names)
    install_state_path_lock_helpers(bindings, lock_names)
    install_state_path_log_helpers(bindings, log_names)
