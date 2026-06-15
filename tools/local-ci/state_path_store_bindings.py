"""Bindings from the local_ci facade to queue/result/evidence state path helpers."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


STATE_PATH_STORE_EXPORTS = (
    "queue_path",
    "results_dir",
    "cloud_runs_dir",
    "evidence_path",
    "logs_dir",
    "ensure_state_dirs",
)


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


def install_state_path_store_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = STATE_PATH_STORE_EXPORTS,
) -> None:
    known_names = set(STATE_PATH_STORE_EXPORTS)
    store_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), store_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
