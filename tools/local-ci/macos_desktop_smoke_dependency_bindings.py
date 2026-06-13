"""Dependency bindings for the macOS desktop smoke/action facade."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


MACOS_DESKTOP_SMOKE_DEPENDENCY_EXPORTS = ("macos_desktop_smoke_dependencies",)


def macos_desktop_smoke_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    desktop_actions = _binding(bindings, "_desktop_actions")
    subprocess_mod = _binding(bindings, "subprocess")
    time_mod = _binding(bindings, "time")
    shlex_mod = _binding(bindings, "shlex")
    os_mod = _binding(bindings, "os")

    return {
        "create_desktop_run_bundle_fn": _binding(bindings, "create_desktop_run_bundle"),
        "desktop_action_artifact_paths_fn": desktop_actions.desktop_action_artifact_paths,
        "desktop_interaction_requested_fn": desktop_actions.desktop_interaction_requested,
        "macos_accessibility_trusted_fn": _binding(bindings, "macos_accessibility_trusted"),
        "now_iso_fn": _binding(bindings, "now_iso"),
        "prepare_macos_exact_sha_source_fn": _binding(bindings, "prepare_macos_exact_sha_source"),
        "quit_macos_bundle_id_fn": _binding(bindings, "quit_macos_bundle_id"),
        "sleep_fn": time_mod.sleep,
        "run_fn": subprocess_mod.run,
        "activate_macos_bundle_id_fn": _binding(bindings, "activate_macos_bundle_id"),
        "wait_for_macos_bundle_window_fn": _binding(bindings, "wait_for_macos_bundle_window"),
        "split_command_fn": shlex_mod.split,
        "detect_macos_app_bundle_fn": _binding(bindings, "detect_macos_app_bundle"),
        "macos_bundle_id_for_app_path_fn": _binding(bindings, "macos_bundle_id_for_app_path"),
        "environ_copy_fn": os_mod.environ.copy,
        "popen_fn": subprocess_mod.Popen,
        "wait_for_macos_window_fn": _binding(bindings, "wait_for_macos_window"),
        "content_size_from_window_fn": desktop_actions.content_size_from_window,
        "wait_for_path_fn": _binding(bindings, "wait_for_path"),
        "content_size_from_view_tree_fn": desktop_actions.content_size_from_view_tree,
        "view_tree_inspector_summary_fn": desktop_actions.view_tree_inspector_summary,
        "pulp_app_interaction_summary_fn": desktop_actions.pulp_app_interaction_summary,
        "capture_macos_window_fn": _binding(bindings, "capture_macos_window"),
        "parse_coordinate_pair_fn": _binding(bindings, "parse_coordinate_pair"),
        "resolve_view_tree_click_point_fn": _binding(bindings, "resolve_view_tree_click_point"),
        "screen_point_for_content_point_fn": _binding(bindings, "screen_point_for_content_point"),
        "activate_macos_pid_fn": _binding(bindings, "activate_macos_pid"),
        "dispatch_macos_click_fn": _binding(bindings, "dispatch_macos_click"),
        "desktop_click_selector_fn": desktop_actions.desktop_click_selector,
        "image_change_summary_fn": _binding(bindings, "image_change_summary"),
        "attach_desktop_source_to_manifest_fn": _binding(bindings, "attach_desktop_source_to_manifest"),
        "atomic_write_text_fn": _binding(bindings, "atomic_write_text"),
        "write_desktop_run_rollups_fn": _binding(bindings, "write_desktop_run_rollups"),
        "terminate_process_fn": _binding(bindings, "terminate_process"),
    }


def install_macos_desktop_smoke_dependency_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = MACOS_DESKTOP_SMOKE_DEPENDENCY_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
