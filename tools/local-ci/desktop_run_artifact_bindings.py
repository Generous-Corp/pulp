"""Bindings from the local_ci facade to desktop run artifact helpers."""

from __future__ import annotations

from pathlib import Path

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


DESKTOP_RUN_ARTIFACT_EXPORTS = (
    "desktop_artifact_root",
    "create_desktop_run_bundle",
)


def desktop_artifact_root(bindings: dict, config: dict) -> Path:
    return _binding(bindings, "_desktop_artifacts").desktop_artifact_root(config)


def create_desktop_run_bundle(bindings: dict, config: dict, target_name: str, action: str) -> Path:
    return _binding(bindings, "_desktop_artifacts").create_desktop_run_bundle(config, target_name, action)


def install_desktop_run_artifact_helpers(
    bindings: dict,
    names: tuple[str, ...] = DESKTOP_RUN_ARTIFACT_EXPORTS,
) -> None:
    known_names = set(DESKTOP_RUN_ARTIFACT_EXPORTS)
    artifact_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), artifact_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
