"""Bindings from the local_ci facade to Windows PowerShell probe core helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import install_local_helpers
from binding_utils import binding as _binding


WINDOWS_PROBE_CORE_EXPORTS = (
    "ps_literal",
    "windows_ssh_powershell_command",
    "run_windows_ssh_powershell",
    "parse_windows_ssh_json",
    "windows_contract_expand_expression",
    "windows_session_agent_template_path",
)


def ps_literal(bindings: Mapping[str, Any], value: str) -> str:
    return _binding(bindings, "_windows_probe").ps_literal(value)


def windows_ssh_powershell_command(bindings: Mapping[str, Any], host: str) -> list[str]:
    return _binding(bindings, "_windows_probe").windows_ssh_powershell_command(host)


def run_windows_ssh_powershell(bindings: Mapping[str, Any], host: str, ps_script: str, *, timeout: int = 60):
    return _binding(bindings, "_windows_probe").run_windows_ssh_powershell(
        host,
        ps_script,
        timeout=timeout,
        run_ssh_subprocess_fn=_binding(bindings, "run_ssh_subprocess"),
    )


def parse_windows_ssh_json(bindings: Mapping[str, Any], stdout: str) -> dict:
    return _binding(bindings, "_windows_probe").parse_windows_ssh_json(stdout)


def windows_contract_expand_expression(bindings: Mapping[str, Any], raw_value: str) -> str:
    return _binding(bindings, "_windows_probe").windows_contract_expand_expression(
        raw_value,
        ps_literal_fn=_binding(bindings, "ps_literal"),
    )


def windows_session_agent_template_path(bindings: Mapping[str, Any]):
    return _binding(bindings, "_windows_probe").windows_session_agent_template_path(_binding(bindings, "SCRIPT_DIR"))


def install_windows_probe_core_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = WINDOWS_PROBE_CORE_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
