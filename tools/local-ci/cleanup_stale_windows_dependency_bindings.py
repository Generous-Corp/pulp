"""Dependency assembly for stale Windows validator cleanup bindings."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


def stale_windows_candidate_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    return {
        "stale_running_jobs_fn": _binding(bindings, "stale_running_jobs_unlocked"),
        "now_fn": _binding(bindings, "now_iso"),
    }


def cleanup_stale_windows_validator_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    return {
        "ps_literal_fn": _binding(bindings, "ps_literal"),
        "run_logged_command_fn": _binding(bindings, "run_logged_command"),
        "windows_ssh_powershell_command_fn": _binding(bindings, "windows_ssh_powershell_command"),
        "trim_line_fn": _binding(bindings, "trim_line"),
    }
