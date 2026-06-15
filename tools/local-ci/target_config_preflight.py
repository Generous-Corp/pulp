"""Target config source and drift helpers for local CI submission preflight."""

from __future__ import annotations

from collections.abc import Callable, Mapping
import json
from pathlib import Path


def config_source_name(
    path: Path,
    *,
    environ: Mapping[str, str],
    shared_config_path_fn: Callable[[], Path],
) -> str:
    override = environ.get("PULP_LOCAL_CI_CONFIG")
    if override:
        return "env-override"
    if path == shared_config_path_fn():
        return "shared-state"
    return "worktree-local"


def config_material_for_targets(config: dict, targets: list[str]) -> dict:
    material: dict[str, dict] = {}
    for name in targets:
        target_cfg = config.get("targets", {}).get(name)
        if not target_cfg:
            continue
        entry = {
            "type": target_cfg.get("type", "local"),
            "enabled": bool(target_cfg.get("enabled", True)),
        }
        for key in (
            "host",
            "fallback_host",
            "repo_path",
            "utm_fallback",
            "cmake_generator",
            "cmake_platform",
            "cmake_generator_instance",
        ):
            value = target_cfg.get(key)
            if value not in (None, "", {}):
                entry[key] = value
        material[name] = entry
    return material


def find_material_config_drift(
    targets: list[str],
    *,
    shared_config_path_fn: Callable[[], Path],
    worktree_config_path_fn: Callable[[], Path],
    config_material_for_targets_fn: Callable[[dict, list[str]], dict],
) -> list[str]:
    shared_path = shared_config_path_fn()
    worktree_path = worktree_config_path_fn()
    if not shared_path.exists() or not worktree_path.exists():
        return []
    try:
        shared_cfg = json.loads(shared_path.read_text())
        worktree_cfg = json.loads(worktree_path.read_text())
    except json.JSONDecodeError:
        return []

    drift: list[str] = []
    shared_material = config_material_for_targets_fn(shared_cfg, targets)
    worktree_material = config_material_for_targets_fn(worktree_cfg, targets)
    for name in targets:
        shared_entry = shared_material.get(name)
        worktree_entry = worktree_material.get(name)
        if shared_entry == worktree_entry:
            continue
        drift.append(
            f"{name}: shared-state {shared_entry or '(missing)'} vs worktree-local {worktree_entry or '(missing)'}"
        )
    return drift
