"""Bindings from the local_ci facade to state lock path helpers."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


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
    known_names = set(STATE_PATH_LOCK_EXPORTS)
    lock_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), lock_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
