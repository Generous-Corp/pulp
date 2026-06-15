"""Compatibility installer for macOS window facade helpers."""

from __future__ import annotations

from binding_utils import install_local_helpers
from macos_window_action_bindings import (
    MACOS_WINDOW_ACTION_EXPORTS,
    activate_macos_bundle_id,
    activate_macos_pid,
    dispatch_macos_click,
    install_macos_window_action_helpers,
    quit_macos_bundle_id,
    terminate_process,
)
from macos_window_app_bindings import (
    MACOS_WINDOW_APP_EXPORTS,
    detect_macos_app_bundle,
    install_macos_window_app_helpers,
    macos_bundle_id_for_app_path,
    macos_window_probe_path,
)
from macos_window_probe_bindings import (
    MACOS_WINDOW_PROBE_EXPORTS,
    capture_macos_window,
    install_macos_window_probe_helpers,
    macos_accessibility_trusted,
    macos_window_info_for_bundle_id,
    macos_window_info_for_pid,
    wait_for_macos_bundle_window,
    wait_for_macos_window,
)


MACOS_WINDOW_EXPORTS = (
    *MACOS_WINDOW_APP_EXPORTS,
    *MACOS_WINDOW_PROBE_EXPORTS,
    *MACOS_WINDOW_ACTION_EXPORTS,
)


def install_macos_window_helpers(bindings: dict, names: tuple[str, ...] = MACOS_WINDOW_EXPORTS) -> None:
    app_names = tuple(name for name in names if name in MACOS_WINDOW_APP_EXPORTS)
    probe_names = tuple(name for name in names if name in MACOS_WINDOW_PROBE_EXPORTS)
    action_names = tuple(name for name in names if name in MACOS_WINDOW_ACTION_EXPORTS)
    known_names = set(MACOS_WINDOW_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_macos_window_app_helpers(bindings, app_names)
    install_macos_window_probe_helpers(bindings, probe_names)
    install_macos_window_action_helpers(bindings, action_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
