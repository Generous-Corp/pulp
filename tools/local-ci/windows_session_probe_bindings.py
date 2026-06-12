"""Bindings from the local_ci facade to Windows session-agent and CMake probe helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import binding_attr as _binding_attr


WINDOWS_SESSION_PROBE_EXPORTS = (
    "bootstrap_windows_session_agent",
    "start_windows_session_agent_task",
    "probe_windows_ssh_cmake_settings",
)


def bootstrap_windows_session_agent(bindings: Mapping[str, Any], host: str, contract: dict) -> dict:
    return _binding(bindings, "_windows_probe").bootstrap_windows_session_agent(
        host,
        contract,
        windows_session_agent_template_path_fn=_binding(bindings, "windows_session_agent_template_path"),
        windows_ssh_write_text_fn=_binding(bindings, "windows_ssh_write_text"),
        run_windows_ssh_powershell_fn=_binding(bindings, "run_windows_ssh_powershell"),
        parse_windows_ssh_json_fn=_binding(bindings, "parse_windows_ssh_json"),
        windows_contract_expand_expression_fn=_binding(bindings, "windows_contract_expand_expression"),
        ps_literal_fn=_binding(bindings, "ps_literal"),
    )


def start_windows_session_agent_task(bindings: Mapping[str, Any], host: str, contract: dict) -> None:
    return _binding(bindings, "_windows_probe").start_windows_session_agent_task(
        host,
        contract,
        run_windows_ssh_powershell_fn=_binding(bindings, "run_windows_ssh_powershell"),
        parse_windows_ssh_json_fn=_binding(bindings, "parse_windows_ssh_json"),
        ps_literal_fn=_binding(bindings, "ps_literal"),
    )


def probe_windows_ssh_cmake_settings(
    bindings: Mapping[str, Any],
    host: str,
    cmake_generator: str,
    cmake_platform: str,
    cmake_generator_instance: str,
) -> tuple[str, str]:
    return _binding(bindings, "_windows_probe").probe_windows_ssh_cmake_settings(
        host,
        cmake_generator,
        cmake_platform,
        cmake_generator_instance,
        windows_ssh_powershell_command_fn=_binding(bindings, "windows_ssh_powershell_command"),
        run_fn=_binding_attr(bindings, "subprocess", "run"),
        ps_literal_fn=_binding(bindings, "ps_literal"),
    )
