"""Dependency bindings for the Windows desktop action facade."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


WINDOWS_DESKTOP_ACTION_DEPENDENCY_EXPORTS = ("windows_desktop_action_dependencies",)


def windows_desktop_action_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    desktop_actions = _binding(bindings, "_desktop_actions")
    time_mod = _binding(bindings, "time")

    return {
        "ensure_host_reachable_fn": _binding(bindings, "ensure_host_reachable"),
        "desktop_receipt_for_fn": _binding(bindings, "desktop_receipt_for"),
        "desktop_target_contract_fn": _binding(bindings, "desktop_target_contract"),
        "probe_windows_session_agent_fn": _binding(bindings, "probe_windows_session_agent"),
        "windows_desktop_session_user_fn": _binding(bindings, "windows_desktop_session_user"),
        "create_desktop_run_bundle_fn": _binding(bindings, "create_desktop_run_bundle"),
        "desktop_action_artifact_paths_fn": desktop_actions.desktop_action_artifact_paths,
        "desktop_interaction_requested_fn": desktop_actions.desktop_interaction_requested,
        "prepare_windows_exact_sha_source_fn": _binding(bindings, "prepare_windows_exact_sha_source"),
        "build_windows_session_agent_request_fn": _binding(bindings, "build_windows_session_agent_request"),
        "windows_path_join_fn": _binding(bindings, "windows_path_join"),
        "windows_ssh_write_text_fn": _binding(bindings, "windows_ssh_write_text"),
        "start_windows_session_agent_task_fn": _binding(bindings, "start_windows_session_agent_task"),
        "time_fn": time_mod.time,
        "sleep_fn": time_mod.sleep,
        "windows_ssh_read_json_fn": _binding(bindings, "windows_ssh_read_json"),
        "atomic_write_text_fn": _binding(bindings, "atomic_write_text"),
        "windows_ssh_fetch_file_fn": _binding(bindings, "windows_ssh_fetch_file"),
        "windows_ssh_remove_path_fn": _binding(bindings, "windows_ssh_remove_path"),
        "default_desktop_label_fn": _binding(bindings, "default_desktop_label"),
        "image_change_summary_fn": _binding(bindings, "image_change_summary"),
        "view_tree_inspector_summary_fn": desktop_actions.view_tree_inspector_summary,
        "pulp_app_interaction_summary_fn": desktop_actions.pulp_app_interaction_summary,
        "attach_desktop_source_to_manifest_fn": _binding(bindings, "attach_desktop_source_to_manifest"),
        "write_desktop_run_rollups_fn": _binding(bindings, "write_desktop_run_rollups"),
        "now_iso_fn": _binding(bindings, "now_iso"),
    }


def install_windows_desktop_action_dependency_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = WINDOWS_DESKTOP_ACTION_DEPENDENCY_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
