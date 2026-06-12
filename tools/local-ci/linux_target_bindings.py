"""Compatibility installer for Linux target facade helpers."""

from __future__ import annotations

from binding_utils import install_local_helpers
from linux_target_command_bindings import (
    build_linux_window_driver_remote_command,
    build_linux_xvfb_remote_command,
    remote_linux_bundle_relpath,
)
from linux_target_probe_bindings import (
    linux_optional_remote_tools,
    linux_remote_tooling_ready,
    linux_required_remote_tools,
    linux_tooling_detail,
    probe_linux_launch_backend,
    probe_linux_remote_tooling,
)


LINUX_TARGET_EXPORTS = (
    "probe_linux_launch_backend",
    "probe_linux_remote_tooling",
    "linux_tooling_detail",
    "linux_remote_tooling_ready",
    "remote_linux_bundle_relpath",
    "build_linux_xvfb_remote_command",
    "build_linux_window_driver_remote_command",
)


def install_linux_target_helpers(bindings: dict, names: tuple[str, ...] = LINUX_TARGET_EXPORTS) -> None:
    install_local_helpers(bindings, globals(), names)
