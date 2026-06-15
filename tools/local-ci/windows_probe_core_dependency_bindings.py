"""Dependency assembly for Windows probe core bindings."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


def run_windows_ssh_powershell_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    return {
        "run_ssh_subprocess_fn": _binding(bindings, "run_ssh_subprocess"),
    }


def windows_contract_expand_expression_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    return {
        "ps_literal_fn": _binding(bindings, "ps_literal"),
    }


def windows_session_agent_template_path_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    return {
        "script_dir": _binding(bindings, "SCRIPT_DIR"),
    }
