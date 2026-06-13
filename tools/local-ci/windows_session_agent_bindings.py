"""Bindings from the local_ci facade to Windows session-agent helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


WINDOWS_SESSION_AGENT_EXPORTS = (
    "bootstrap_windows_session_agent",
    "start_windows_session_agent_task",
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


def install_windows_session_agent_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = WINDOWS_SESSION_AGENT_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
