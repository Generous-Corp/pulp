"""Bindings from the local_ci facade to target config preflight helpers."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any

from binding_utils import install_local_helpers
from binding_utils import binding as _binding


TARGET_CONFIG_PREFLIGHT_EXPORTS = (
    "config_source_name",
    "config_material_for_targets",
    "find_material_config_drift",
)


def config_source_name(bindings: Mapping[str, Any], path: Path) -> str:
    return _binding(bindings, "_target_preflight").config_source_name(
        path,
        environ=_binding(bindings, "os").environ,
        shared_config_path_fn=_binding(bindings, "shared_config_path"),
    )


def config_material_for_targets(bindings: Mapping[str, Any], config: dict, targets: list[str]) -> dict:
    return _binding(bindings, "_target_preflight").config_material_for_targets(config, targets)


def find_material_config_drift(bindings: Mapping[str, Any], targets: list[str]) -> list[str]:
    return _binding(bindings, "_target_preflight").find_material_config_drift(
        targets,
        shared_config_path_fn=_binding(bindings, "shared_config_path"),
        worktree_config_path_fn=_binding(bindings, "worktree_config_path"),
        config_material_for_targets_fn=_binding(bindings, "config_material_for_targets"),
    )


def install_target_config_preflight_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = TARGET_CONFIG_PREFLIGHT_EXPORTS,
) -> None:
    known_names = set(TARGET_CONFIG_PREFLIGHT_EXPORTS)
    config_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), config_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
