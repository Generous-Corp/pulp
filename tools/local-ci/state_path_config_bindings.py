"""Bindings from the local_ci facade to state/config path helpers."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


STATE_PATH_CONFIG_EXPORTS = (
    "state_dir",
    "config_path",
    "worktree_config_path",
    "shared_config_path",
)


def state_dir(bindings: Mapping[str, Any]) -> Path:
    return _binding(bindings, "_state_paths").state_dir()


def config_path(bindings: Mapping[str, Any]) -> Path:
    return _binding(bindings, "_state_paths").config_path()


def worktree_config_path(bindings: Mapping[str, Any]) -> Path:
    return _binding(bindings, "_state_paths").worktree_config_path()


def shared_config_path(bindings: Mapping[str, Any]) -> Path:
    return _binding(bindings, "_state_paths").shared_config_path()


def install_state_path_config_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = STATE_PATH_CONFIG_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
