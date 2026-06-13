"""Bindings from the local_ci facade to Windows remote tooling helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


DESKTOP_WINDOWS_REMOTE_TOOLING_PROBE_EXPORTS = (
    "probe_windows_remote_tooling",
    "install_windows_remote_tool",
    "ensure_windows_remote_tooling",
)


def probe_windows_remote_tooling(bindings: Mapping[str, Any], host: str) -> dict:
    return _binding(bindings, "_windows_probe").probe_windows_remote_tooling(
        host,
        run_windows_ssh_powershell_fn=_binding(bindings, "run_windows_ssh_powershell"),
        parse_windows_ssh_json_fn=_binding(bindings, "parse_windows_ssh_json"),
    )


def install_windows_remote_tool(bindings: Mapping[str, Any], host: str, package_id: str, *, timeout: int = 900) -> None:
    return _binding(bindings, "_windows_probe").install_windows_remote_tool(
        host,
        package_id,
        timeout=timeout,
        run_windows_ssh_powershell_fn=_binding(bindings, "run_windows_ssh_powershell"),
        ps_literal_fn=_binding(bindings, "ps_literal"),
    )


def ensure_windows_remote_tooling(bindings: Mapping[str, Any], host: str, *, install_optional: bool = False) -> dict:
    return _binding(bindings, "_windows_probe").ensure_windows_remote_tooling(
        host,
        install_optional=install_optional,
        required_tools=_binding(bindings, "WINDOWS_REQUIRED_REMOTE_TOOLS"),
        optional_tools=_binding(bindings, "WINDOWS_OPTIONAL_REMOTE_TOOLS"),
        probe_windows_remote_tooling_fn=_binding(bindings, "probe_windows_remote_tooling"),
        install_windows_remote_tool_fn=_binding(bindings, "install_windows_remote_tool"),
    )


def install_desktop_windows_remote_tooling_probe_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_WINDOWS_REMOTE_TOOLING_PROBE_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
