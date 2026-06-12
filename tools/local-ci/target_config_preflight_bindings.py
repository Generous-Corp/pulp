"""Bindings from the local_ci facade to target config preflight helpers."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any

from binding_utils import binding as _binding


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
