"""Compatibility installer for macOS window facade helpers."""

from __future__ import annotations

from binding_utils import install_local_helpers
from macos_window_action_bindings import (
    activate_macos_bundle_id,
    activate_macos_pid,
    dispatch_macos_click,
    quit_macos_bundle_id,
    terminate_process,
)
from macos_window_app_bindings import (
    detect_macos_app_bundle,
    macos_bundle_id_for_app_path,
    macos_window_probe_path,
)
from macos_window_probe_bindings import (
    capture_macos_window,
    macos_accessibility_trusted,
    macos_window_info_for_bundle_id,
    macos_window_info_for_pid,
    wait_for_macos_bundle_window,
    wait_for_macos_window,
)


MACOS_WINDOW_EXPORTS = (
    "detect_macos_app_bundle",
    "macos_bundle_id_for_app_path",
    "macos_window_probe_path",
    "macos_window_info_for_pid",
    "macos_window_info_for_bundle_id",
    "macos_accessibility_trusted",
    "wait_for_macos_window",
    "wait_for_macos_bundle_window",
    "capture_macos_window",
    "activate_macos_pid",
    "activate_macos_bundle_id",
    "dispatch_macos_click",
    "terminate_process",
    "quit_macos_bundle_id",
)


def install_macos_window_helpers(bindings: dict, names: tuple[str, ...] = MACOS_WINDOW_EXPORTS) -> None:
    install_local_helpers(bindings, globals(), names)
