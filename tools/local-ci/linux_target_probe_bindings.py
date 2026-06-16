"""Compatibility facade for Linux target probe/tooling bindings."""

from __future__ import annotations

from binding_utils import install_local_helpers
from linux_target_constant_bindings import (
    LINUX_TARGET_CONSTANT_EXPORTS,
    install_linux_target_constant_helpers,
    linux_optional_remote_tools,
    linux_required_remote_tools,
)
from linux_target_probe_command_bindings import (
    LINUX_TARGET_PROBE_COMMAND_EXPORTS,
    install_linux_target_probe_command_helpers,
    probe_linux_launch_backend,
    probe_linux_remote_tooling,
)
from linux_target_tooling_status_bindings import (
    LINUX_TARGET_TOOLING_STATUS_EXPORTS,
    install_linux_target_tooling_status_helpers,
    linux_remote_tooling_ready,
    linux_tooling_detail,
)

LINUX_TARGET_PROBE_EXPORTS = (
    *LINUX_TARGET_PROBE_COMMAND_EXPORTS,
    *LINUX_TARGET_TOOLING_STATUS_EXPORTS,
)


def install_linux_target_probe_helpers(
    bindings: dict,
    names: tuple[str, ...] = LINUX_TARGET_PROBE_EXPORTS,
) -> None:
    known_names = set(LINUX_TARGET_PROBE_EXPORTS)
    probe_command_names = tuple(
        name for name in names if name in LINUX_TARGET_PROBE_COMMAND_EXPORTS
    )
    tooling_status_names = tuple(
        name for name in names if name in LINUX_TARGET_TOOLING_STATUS_EXPORTS
    )
    unknown_names = tuple(name for name in names if name not in known_names)

    install_linux_target_probe_command_helpers(bindings, probe_command_names)
    install_linux_target_tooling_status_helpers(bindings, tooling_status_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
