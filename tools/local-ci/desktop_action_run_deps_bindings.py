"""Shared dependency assembly for desktop action runner command bindings."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import binding_attr as _binding_attr


def desktop_action_command_kwargs(bindings: Mapping[str, Any]) -> dict[str, Any]:
    return {
        "load_config_fn": _binding(bindings, "load_config"),
        "resolve_desktop_target_fn": _binding(bindings, "resolve_desktop_target"),
        "make_desktop_source_request_fn": _binding(bindings, "make_desktop_source_request"),
        "run_macos_local_smoke_fn": _binding(bindings, "run_macos_local_smoke"),
        "run_linux_xvfb_remote_action_fn": _binding(bindings, "run_linux_xvfb_remote_action"),
        "run_windows_session_agent_action_fn": _binding(bindings, "run_windows_session_agent_action"),
        "desktop_action_success_lines_fn": _binding_attr(bindings, "_desktop_cli", "desktop_action_success_lines"),
        "sys_platform": _binding_attr(bindings, "sys", "platform"),
    }
