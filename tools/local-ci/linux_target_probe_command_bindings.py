"""Facade dependency bindings for Linux target probe execution."""

from __future__ import annotations

from binding_utils import install_local_helpers
from binding_utils import binding as _binding
from linux_target_probe_command_dependency_bindings import linux_target_probe_command_dependencies


LINUX_TARGET_PROBE_COMMAND_EXPORTS = (
    "probe_linux_launch_backend",
    "probe_linux_remote_tooling",
)


def probe_linux_launch_backend(bindings: dict, host: str) -> dict:
    return _binding(bindings, "_linux_target").probe_linux_launch_backend(
        host,
        **linux_target_probe_command_dependencies(bindings),
    )


def probe_linux_remote_tooling(bindings: dict, host: str) -> dict:
    return _binding(bindings, "_linux_target").probe_linux_remote_tooling(
        host,
        **linux_target_probe_command_dependencies(bindings),
    )


def install_linux_target_probe_command_helpers(
    bindings: dict,
    names: tuple[str, ...] = LINUX_TARGET_PROBE_COMMAND_EXPORTS,
) -> None:
    known_names = set(LINUX_TARGET_PROBE_COMMAND_EXPORTS)
    probe_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), probe_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
