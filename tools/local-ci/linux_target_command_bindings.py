"""Compatibility facade for Linux target command bindings."""

from __future__ import annotations

from binding_utils import install_local_helpers
from linux_target_bundle_bindings import (
    LINUX_TARGET_BUNDLE_EXPORTS,
    install_linux_target_bundle_helpers,
    remote_linux_bundle_relpath,
)
from linux_target_window_command_bindings import (
    LINUX_TARGET_WINDOW_COMMAND_EXPORTS,
    build_linux_window_driver_remote_command,
    install_linux_target_window_command_helpers,
)
from linux_target_xvfb_command_bindings import (
    LINUX_TARGET_XVFB_COMMAND_EXPORTS,
    build_linux_xvfb_remote_command,
    install_linux_target_xvfb_command_helpers,
)


LINUX_TARGET_COMMAND_EXPORTS = (
    *LINUX_TARGET_BUNDLE_EXPORTS,
    *LINUX_TARGET_XVFB_COMMAND_EXPORTS,
    *LINUX_TARGET_WINDOW_COMMAND_EXPORTS,
)


def install_linux_target_command_helpers(
    bindings: dict,
    names: tuple[str, ...] = LINUX_TARGET_COMMAND_EXPORTS,
) -> None:
    bundle_names = tuple(name for name in names if name in LINUX_TARGET_BUNDLE_EXPORTS)
    xvfb_names = tuple(name for name in names if name in LINUX_TARGET_XVFB_COMMAND_EXPORTS)
    window_names = tuple(name for name in names if name in LINUX_TARGET_WINDOW_COMMAND_EXPORTS)
    known_names = set(LINUX_TARGET_COMMAND_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_linux_target_bundle_helpers(bindings, bundle_names)
    install_linux_target_xvfb_command_helpers(bindings, xvfb_names)
    install_linux_target_window_command_helpers(bindings, window_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
