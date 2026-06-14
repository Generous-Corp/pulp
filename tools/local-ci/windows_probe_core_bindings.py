"""Bindings from the local_ci facade to Windows PowerShell probe core helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import install_local_helpers
from binding_utils import binding as _binding
from windows_probe_core_dependency_bindings import (
    run_windows_ssh_powershell_dependencies,
    windows_contract_expand_expression_dependencies,
    windows_session_agent_template_path_dependencies,
)


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
        **run_windows_ssh_powershell_dependencies(bindings),
    )


def parse_windows_ssh_json(bindings: Mapping[str, Any], stdout: str) -> dict:
    return _binding(bindings, "_windows_probe").parse_windows_ssh_json(stdout)


def windows_contract_expand_expression(bindings: Mapping[str, Any], raw_value: str) -> str:
    return _binding(bindings, "_windows_probe").windows_contract_expand_expression(
        raw_value,
        **windows_contract_expand_expression_dependencies(bindings),
    )


def windows_session_agent_template_path(bindings: Mapping[str, Any]):
    dependencies = windows_session_agent_template_path_dependencies(bindings)
    return _binding(bindings, "_windows_probe").windows_session_agent_template_path(dependencies["script_dir"])


def install_windows_probe_core_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = WINDOWS_PROBE_CORE_EXPORTS,
) -> None:
    known_names = set(WINDOWS_PROBE_CORE_EXPORTS)
    probe_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), probe_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
