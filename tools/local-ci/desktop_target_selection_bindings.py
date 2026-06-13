"""Bindings from the local_ci facade to desktop target selection helpers."""

from __future__ import annotations


def resolve_desktop_target(bindings: dict, config: dict, target_name: str) -> dict:
    desktop_targets = config.get("desktop_automation", {}).get("targets", {})
    if target_name not in desktop_targets:
        raise ValueError(f"Unknown desktop target '{target_name}'.")
    target = desktop_targets[target_name]
    if not target.get("enabled", True):
        raise ValueError(f"Desktop target '{target_name}' is disabled.")
    return target
