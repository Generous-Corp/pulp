"""Bindings from the local_ci facade to state lock path helpers."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_module_attrs


STATE_PATH_LOCK_EXPORTS = (
    "queue_lock_path",
    "evidence_lock_path",
    "drain_lock_path",
    "runner_info_path",
)


def queue_lock_path(bindings: Mapping[str, Any]) -> Path:
    return _binding(bindings, "_state_paths").queue_lock_path()


def evidence_lock_path(bindings: Mapping[str, Any]) -> Path:
    return _binding(bindings, "_state_paths").evidence_lock_path()


def drain_lock_path(bindings: Mapping[str, Any]) -> Path:
    return _binding(bindings, "_state_paths").drain_lock_path()


def runner_info_path(bindings: Mapping[str, Any]) -> Path:
    return _binding(bindings, "_state_paths").runner_info_path()


def install_state_path_lock_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = STATE_PATH_LOCK_EXPORTS,
) -> None:
    install_module_attrs(bindings, "_state_paths", names)
