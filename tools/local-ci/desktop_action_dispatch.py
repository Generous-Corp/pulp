"""Compatibility facade for desktop automation action runner selection."""

from __future__ import annotations

from desktop_action_selectors import desktop_click_has_target, windows_requires_pulp_app_selectors
from desktop_click_action_dispatch import desktop_click_runner
from desktop_inspect_action_dispatch import desktop_inspect_runner
from desktop_smoke_action_dispatch import desktop_smoke_runner


__all__ = (
    "desktop_click_has_target",
    "desktop_click_runner",
    "desktop_inspect_runner",
    "desktop_smoke_runner",
    "windows_requires_pulp_app_selectors",
)
