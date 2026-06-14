"""Source/session-agent request dependency bindings for Windows desktop actions."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


WINDOWS_DESKTOP_ACTION_SOURCE_DEPENDENCY_EXPORTS = ("windows_desktop_action_source_dependencies",)


def windows_desktop_action_source_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    return {
        "prepare_windows_exact_sha_source_fn": _binding(bindings, "prepare_windows_exact_sha_source"),
        "build_windows_session_agent_request_fn": _binding(bindings, "build_windows_session_agent_request"),
        "windows_path_join_fn": _binding(bindings, "windows_path_join"),
        "windows_ssh_write_text_fn": _binding(bindings, "windows_ssh_write_text"),
        "start_windows_session_agent_task_fn": _binding(bindings, "start_windows_session_agent_task"),
        "windows_ssh_read_json_fn": _binding(bindings, "windows_ssh_read_json"),
        "attach_desktop_source_to_manifest_fn": _binding(bindings, "attach_desktop_source_to_manifest"),
    }


def install_windows_desktop_action_source_dependency_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = WINDOWS_DESKTOP_ACTION_SOURCE_DEPENDENCY_EXPORTS,
) -> None:
    known_names = set(WINDOWS_DESKTOP_ACTION_SOURCE_DEPENDENCY_EXPORTS)
    source_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), source_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
