"""Bindings from the local_ci facade to desktop run manifest lookup helpers."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


DESKTOP_RUN_MANIFEST_EXPORTS = (
    "desktop_run_manifests",
    "desktop_rollup_dir",
)


def desktop_run_manifests(
    bindings: Mapping[str, Any],
    config: dict,
    *,
    target_name: str | None = None,
    action: str | None = None,
) -> list[dict]:
    return _binding(bindings, "_reporting").desktop_run_manifests(
        config,
        target_name=target_name,
        action=action,
        desktop_artifact_root_fn=_binding(bindings, "desktop_artifact_root"),
    )


def desktop_rollup_dir(bindings: Mapping[str, Any], config: dict, target_name: str | None = None) -> Path:
    return _binding(bindings, "_reporting").desktop_rollup_dir(
        config,
        target_name,
        desktop_artifact_root_fn=_binding(bindings, "desktop_artifact_root"),
    )


def install_desktop_run_manifest_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_RUN_MANIFEST_EXPORTS,
) -> None:
    known_names = set(DESKTOP_RUN_MANIFEST_EXPORTS)
    manifest_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), manifest_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
