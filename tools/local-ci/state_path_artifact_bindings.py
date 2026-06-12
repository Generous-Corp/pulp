"""Bindings from the local_ci facade to artifact state path helpers."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_module_attrs


STATE_PATH_ARTIFACT_EXPORTS = (
    "bundles_dir",
    "prepared_dir",
    "desktop_state_dir",
    "desktop_receipts_dir",
)


def bundles_dir(bindings: Mapping[str, Any]) -> Path:
    return _binding(bindings, "_state_paths").bundles_dir()


def prepared_dir(bindings: Mapping[str, Any]) -> Path:
    return _binding(bindings, "_state_paths").prepared_dir()


def desktop_state_dir(bindings: Mapping[str, Any]) -> Path:
    return _binding(bindings, "_state_paths").desktop_state_dir()


def desktop_receipts_dir(bindings: Mapping[str, Any]) -> Path:
    return _binding(bindings, "_state_paths").desktop_receipts_dir()


def install_state_path_artifact_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = STATE_PATH_ARTIFACT_EXPORTS,
) -> None:
    install_module_attrs(bindings, "_state_paths", names)
