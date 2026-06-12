"""Bindings from the local_ci facade to core state path helpers."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_module_attrs


STATE_PATH_CORE_EXPORTS = (
    "state_dir",
    "config_path",
    "worktree_config_path",
    "shared_config_path",
    "queue_path",
    "results_dir",
    "cloud_runs_dir",
    "evidence_path",
    "logs_dir",
    "ensure_state_dirs",
)


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


def ensure_state_dirs(bindings: Mapping[str, Any]) -> None:
    return _binding(bindings, "_state_paths").ensure_state_dirs()


def install_state_path_core_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = STATE_PATH_CORE_EXPORTS,
) -> None:
    install_module_attrs(bindings, "_state_paths", names)
