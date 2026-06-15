"""Compatibility facade for macOS desktop/window helpers."""

from __future__ import annotations

from macos_desktop_app import (
    detect_macos_app_bundle,
    macos_bundle_id_for_app_path,
    macos_window_probe_path,
)
from macos_desktop_window_action import (
    activate_macos_bundle_id,
    activate_macos_pid,
    dispatch_macos_click,
    quit_macos_bundle_id,
    terminate_process,
)
from macos_desktop_window_probe import (
    capture_macos_window,
    macos_accessibility_trusted,
    macos_window_info_for_bundle_id,
    macos_window_info_for_pid,
    wait_for_macos_bundle_window,
    wait_for_macos_window,
)


__all__ = (
    "activate_macos_bundle_id",
    "activate_macos_pid",
    "capture_macos_window",
    "detect_macos_app_bundle",
    "dispatch_macos_click",
    "macos_accessibility_trusted",
    "macos_bundle_id_for_app_path",
    "macos_window_info_for_bundle_id",
    "macos_window_info_for_pid",
    "macos_window_probe_path",
    "quit_macos_bundle_id",
    "terminate_process",
    "wait_for_macos_bundle_window",
    "wait_for_macos_window",
)
