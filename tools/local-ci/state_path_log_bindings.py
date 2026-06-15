"""Bindings from the local_ci facade to target log path helpers."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


STATE_PATH_LOG_EXPORTS = (
    "job_logs_dir",
    "target_log_path",
    "prepare_target_log",
)


def job_logs_dir(bindings: Mapping[str, Any], job_id: str) -> Path:
    return _binding(bindings, "_state_paths").job_logs_dir(job_id)


def target_log_path(bindings: Mapping[str, Any], job_id: str, target_name: str) -> Path:
    return _binding(bindings, "_state_paths").target_log_path(job_id, target_name)


def prepare_target_log(bindings: Mapping[str, Any], job_id: str, target_name: str) -> Path:
    return _binding(bindings, "_state_paths").prepare_target_log(job_id, target_name)


def install_state_path_log_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = STATE_PATH_LOG_EXPORTS,
) -> None:
    known_names = set(STATE_PATH_LOG_EXPORTS)
    log_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), log_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
