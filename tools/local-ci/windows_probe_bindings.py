"""Facade dependency bindings for Windows SSH/PowerShell probe helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import binding_attr as _binding_attr


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


def windows_ssh_write_text(bindings: Mapping[str, Any], host: str, remote_path: str, content: str) -> None:
    return _binding(bindings, "_windows_probe").windows_ssh_write_text(
        host,
        remote_path,
        content,
        run_windows_ssh_powershell_fn=_binding(bindings, "run_windows_ssh_powershell"),
        parse_windows_ssh_json_fn=_binding(bindings, "parse_windows_ssh_json"),
        windows_contract_expand_expression_fn=_binding(bindings, "windows_contract_expand_expression"),
        ps_literal_fn=_binding(bindings, "ps_literal"),
    )


def windows_ssh_fetch_file(
    bindings: Mapping[str, Any],
    host: str,
    remote_path: str,
    local_path,
    *,
    optional: bool = False,
    timeout: int = 60,
) -> bool:
    return _binding(bindings, "_windows_probe").windows_ssh_fetch_file(
        host,
        remote_path,
        local_path,
        optional=optional,
        timeout=timeout,
        run_windows_ssh_powershell_fn=_binding(bindings, "run_windows_ssh_powershell"),
        windows_contract_expand_expression_fn=_binding(bindings, "windows_contract_expand_expression"),
    )


def windows_ssh_read_json(
    bindings: Mapping[str, Any],
    host: str,
    remote_path: str,
    *,
    timeout: int = 30,
    optional: bool = False,
) -> dict | None:
    return _binding(bindings, "_windows_probe").windows_ssh_read_json(
        host,
        remote_path,
        timeout=timeout,
        optional=optional,
        run_windows_ssh_powershell_fn=_binding(bindings, "run_windows_ssh_powershell"),
        windows_contract_expand_expression_fn=_binding(bindings, "windows_contract_expand_expression"),
    )


def windows_ssh_remove_path(bindings: Mapping[str, Any], host: str, remote_path: str) -> None:
    return _binding(bindings, "_windows_probe").windows_ssh_remove_path(
        host,
        remote_path,
        run_windows_ssh_powershell_fn=_binding(bindings, "run_windows_ssh_powershell"),
        windows_contract_expand_expression_fn=_binding(bindings, "windows_contract_expand_expression"),
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
