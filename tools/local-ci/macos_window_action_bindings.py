"""Compatibility facade for macOS window action helper bindings."""

from __future__ import annotations

from binding_utils import install_local_helpers
from macos_window_activation_bindings import (
    MACOS_WINDOW_ACTIVATION_EXPORTS,
    activate_macos_bundle_id,
    activate_macos_pid,
    install_macos_window_activation_helpers,
)
from macos_window_click_bindings import (
    MACOS_WINDOW_CLICK_EXPORTS,
    dispatch_macos_click,
    install_macos_window_click_helpers,
)
from macos_window_process_bindings import (
    MACOS_WINDOW_PROCESS_EXPORTS,
    install_macos_window_process_helpers,
    quit_macos_bundle_id,
    terminate_process,
)


MACOS_WINDOW_ACTION_EXPORTS = (
    *MACOS_WINDOW_ACTIVATION_EXPORTS,
    *MACOS_WINDOW_CLICK_EXPORTS,
    *MACOS_WINDOW_PROCESS_EXPORTS,
)


def install_macos_window_action_helpers(
    bindings: dict,
    names: tuple[str, ...] = MACOS_WINDOW_ACTION_EXPORTS,
) -> None:
    activation_names = tuple(name for name in names if name in MACOS_WINDOW_ACTIVATION_EXPORTS)
    click_names = tuple(name for name in names if name in MACOS_WINDOW_CLICK_EXPORTS)
    process_names = tuple(name for name in names if name in MACOS_WINDOW_PROCESS_EXPORTS)
    known_names = set(MACOS_WINDOW_ACTION_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_macos_window_activation_helpers(bindings, activation_names)
    install_macos_window_click_helpers(bindings, click_names)
    install_macos_window_process_helpers(bindings, process_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
