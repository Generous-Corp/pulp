"""Bindings from the local_ci facade to state path helpers."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any


def _binding(bindings: Mapping[str, Any], name: str) -> Any:
    return bindings[name]


def state_dir(bindings: Mapping[str, Any]) -> Path:
    return _binding(bindings, "_state_paths").state_dir()


def config_path(bindings: Mapping[str, Any]) -> Path:
    return _binding(bindings, "_state_paths").config_path()


def worktree_config_path(bindings: Mapping[str, Any]) -> Path:
    return _binding(bindings, "_state_paths").worktree_config_path()


def shared_config_path(bindings: Mapping[str, Any]) -> Path:
    return _binding(bindings, "_state_paths").shared_config_path()


def queue_path(bindings: Mapping[str, Any]) -> Path:
    return _binding(bindings, "_state_paths").queue_path()


def results_dir(bindings: Mapping[str, Any]) -> Path:
    return _binding(bindings, "_state_paths").results_dir()


def cloud_runs_dir(bindings: Mapping[str, Any]) -> Path:
    return _binding(bindings, "_state_paths").cloud_runs_dir()


def evidence_path(bindings: Mapping[str, Any]) -> Path:
    return _binding(bindings, "_state_paths").evidence_path()


def logs_dir(bindings: Mapping[str, Any]) -> Path:
    return _binding(bindings, "_state_paths").logs_dir()


def bundles_dir(bindings: Mapping[str, Any]) -> Path:
    return _binding(bindings, "_state_paths").bundles_dir()


def prepared_dir(bindings: Mapping[str, Any]) -> Path:
    return _binding(bindings, "_state_paths").prepared_dir()


def desktop_state_dir(bindings: Mapping[str, Any]) -> Path:
    return _binding(bindings, "_state_paths").desktop_state_dir()


def desktop_receipts_dir(bindings: Mapping[str, Any]) -> Path:
    return _binding(bindings, "_state_paths").desktop_receipts_dir()


def queue_lock_path(bindings: Mapping[str, Any]) -> Path:
    return _binding(bindings, "_state_paths").queue_lock_path()


def evidence_lock_path(bindings: Mapping[str, Any]) -> Path:
    return _binding(bindings, "_state_paths").evidence_lock_path()


def drain_lock_path(bindings: Mapping[str, Any]) -> Path:
    return _binding(bindings, "_state_paths").drain_lock_path()


def runner_info_path(bindings: Mapping[str, Any]) -> Path:
    return _binding(bindings, "_state_paths").runner_info_path()


def ensure_state_dirs(bindings: Mapping[str, Any]) -> None:
    return _binding(bindings, "_state_paths").ensure_state_dirs()


def job_logs_dir(bindings: Mapping[str, Any], job_id: str) -> Path:
    return _binding(bindings, "_state_paths").job_logs_dir(job_id)


def target_log_path(bindings: Mapping[str, Any], job_id: str, target_name: str) -> Path:
    return _binding(bindings, "_state_paths").target_log_path(job_id, target_name)


def prepare_target_log(bindings: Mapping[str, Any], job_id: str, target_name: str) -> Path:
    return _binding(bindings, "_state_paths").prepare_target_log(job_id, target_name)
