"""Bindings from the local_ci facade to desktop publish artifact helpers."""

from __future__ import annotations

from pathlib import Path

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


DESKTOP_PUBLISH_ARTIFACT_EXPORTS = (
    "desktop_publish_root",
    "create_desktop_publish_bundle",
)


def desktop_publish_root(bindings: dict, config: dict) -> Path:
    return _binding(bindings, "_desktop_artifacts").desktop_publish_root(config)


def create_desktop_publish_bundle(bindings: dict, config: dict) -> Path:
    return _binding(bindings, "_desktop_artifacts").create_desktop_publish_bundle(config)


def install_desktop_publish_artifact_helpers(
    bindings: dict,
    names: tuple[str, ...] = DESKTOP_PUBLISH_ARTIFACT_EXPORTS,
) -> None:
    known_names = set(DESKTOP_PUBLISH_ARTIFACT_EXPORTS)
    artifact_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), artifact_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
