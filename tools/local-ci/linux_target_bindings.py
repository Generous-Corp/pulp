"""Compatibility installer for Linux target facade helpers."""

from __future__ import annotations

from binding_utils import install_local_helpers
from linux_target_command_bindings import (
    LINUX_TARGET_COMMAND_EXPORTS,
    build_linux_window_driver_remote_command,
    build_linux_xvfb_remote_command,
    install_linux_target_command_helpers,
    remote_linux_bundle_relpath,
)
from linux_target_probe_bindings import (
    LINUX_TARGET_CONSTANT_EXPORTS,
    LINUX_TARGET_PROBE_EXPORTS,
    install_linux_target_constant_helpers,
    install_linux_target_probe_helpers,
    linux_optional_remote_tools,
    linux_remote_tooling_ready,
    linux_required_remote_tools,
    linux_tooling_detail,
    probe_linux_launch_backend,
    probe_linux_remote_tooling,
)


LINUX_TARGET_EXPORTS = (
    *LINUX_TARGET_PROBE_EXPORTS,
    *LINUX_TARGET_COMMAND_EXPORTS,
)


def install_linux_target_helpers(bindings: dict, names: tuple[str, ...] = LINUX_TARGET_EXPORTS) -> None:
    known_names = set(LINUX_TARGET_EXPORTS) | set(LINUX_TARGET_CONSTANT_EXPORTS)
    constant_names = tuple(name for name in names if name in LINUX_TARGET_CONSTANT_EXPORTS)
    probe_names = tuple(name for name in names if name in LINUX_TARGET_PROBE_EXPORTS)
    command_names = tuple(name for name in names if name in LINUX_TARGET_COMMAND_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_linux_target_constant_helpers(bindings, constant_names)
    install_linux_target_probe_helpers(bindings, probe_names)
    install_linux_target_command_helpers(bindings, command_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
