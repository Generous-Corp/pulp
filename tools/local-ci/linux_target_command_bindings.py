"""Compatibility facade for Linux target command bindings."""

from __future__ import annotations

from linux_target_bundle_bindings import LINUX_TARGET_BUNDLE_EXPORTS, remote_linux_bundle_relpath
from linux_target_window_command_bindings import (
    LINUX_TARGET_WINDOW_COMMAND_EXPORTS,
    build_linux_window_driver_remote_command,
)
from linux_target_xvfb_command_bindings import LINUX_TARGET_XVFB_COMMAND_EXPORTS, build_linux_xvfb_remote_command


LINUX_TARGET_COMMAND_EXPORTS = (
    *LINUX_TARGET_BUNDLE_EXPORTS,
    *LINUX_TARGET_XVFB_COMMAND_EXPORTS,
    *LINUX_TARGET_WINDOW_COMMAND_EXPORTS,
)
