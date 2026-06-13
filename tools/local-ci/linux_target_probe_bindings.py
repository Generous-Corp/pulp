"""Facade dependency bindings for Linux target probe/tooling helpers."""

from __future__ import annotations

from binding_utils import install_local_helpers
from binding_utils import binding as _binding


LINUX_TARGET_CONSTANT_EXPORTS = (
    "linux_required_remote_tools",
    "linux_optional_remote_tools",
)

LINUX_TARGET_PROBE_EXPORTS = (
    "probe_linux_launch_backend",
    "probe_linux_remote_tooling",
    "linux_tooling_detail",
    "linux_remote_tooling_ready",
)


def linux_required_remote_tools(bindings: dict) -> dict:
    return _binding(bindings, "_linux_target").LINUX_REQUIRED_REMOTE_TOOLS


def linux_optional_remote_tools(bindings: dict) -> dict:
    return _binding(bindings, "_linux_target").LINUX_OPTIONAL_REMOTE_TOOLS


def probe_linux_launch_backend(bindings: dict, host: str) -> dict:
    return _binding(bindings, "_linux_target").probe_linux_launch_backend(
        host,
        ssh_command_result_fn=_binding(bindings, "ssh_command_result"),
    )


def probe_linux_remote_tooling(bindings: dict, host: str) -> dict:
    return _binding(bindings, "_linux_target").probe_linux_remote_tooling(
        host,
        ssh_command_result_fn=_binding(bindings, "ssh_command_result"),
    )


def linux_tooling_detail(
    bindings: dict,
    probe: dict,
    tool_name: str,
    *,
    missing_hint: str | None = None,
) -> str:
    return _binding(bindings, "_linux_target").linux_tooling_detail(
        probe,
        tool_name,
        missing_hint=missing_hint,
    )


def linux_remote_tooling_ready(bindings: dict, probe: dict) -> bool:
    return _binding(bindings, "_linux_target").linux_remote_tooling_ready(
        probe,
        required_tools=_binding(bindings, "LINUX_REQUIRED_REMOTE_TOOLS"),
    )


def install_linux_target_constant_helpers(
    bindings: dict,
    names: tuple[str, ...] = LINUX_TARGET_CONSTANT_EXPORTS,
) -> None:
    known_names = set(LINUX_TARGET_CONSTANT_EXPORTS)
    constant_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), constant_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)


def install_linux_target_probe_helpers(
    bindings: dict,
    names: tuple[str, ...] = LINUX_TARGET_PROBE_EXPORTS,
) -> None:
    known_names = set(LINUX_TARGET_PROBE_EXPORTS)
    probe_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), probe_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
