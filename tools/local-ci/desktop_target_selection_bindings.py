"""Bindings from the local_ci facade to desktop target selection helpers."""

from __future__ import annotations

from binding_utils import install_local_helpers


DESKTOP_TARGET_SELECTION_EXPORTS = ("resolve_desktop_target",)


def resolve_desktop_target(bindings: dict, config: dict, target_name: str) -> dict:
    desktop_targets = config.get("desktop_automation", {}).get("targets", {})
    if target_name not in desktop_targets:
        raise ValueError(f"Unknown desktop target '{target_name}'.")
    target = desktop_targets[target_name]
    if not target.get("enabled", True):
        raise ValueError(f"Desktop target '{target_name}' is disabled.")
    return target


def install_desktop_target_selection_helpers(
    bindings: dict,
    names: tuple[str, ...] = DESKTOP_TARGET_SELECTION_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
