"""Dependency bindings for the Linux desktop action facade."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


LINUX_DESKTOP_ACTION_DEPENDENCY_EXPORTS = ("linux_desktop_action_dependencies",)


def linux_desktop_action_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    desktop_actions = _binding(bindings, "_desktop_actions")
    subprocess_mod = _binding(bindings, "subprocess")

    return {
        "ensure_host_reachable_fn": _binding(bindings, "ensure_host_reachable"),
        "probe_linux_launch_backend_fn": _binding(bindings, "probe_linux_launch_backend"),
        "create_desktop_run_bundle_fn": _binding(bindings, "create_desktop_run_bundle"),
        "desktop_action_artifact_paths_fn": desktop_actions.desktop_action_artifact_paths,
        "desktop_interaction_requested_fn": desktop_actions.desktop_interaction_requested,
        "prepare_linux_exact_sha_source_fn": _binding(bindings, "prepare_linux_exact_sha_source"),
        "remote_linux_bundle_relpath_fn": _binding(bindings, "remote_linux_bundle_relpath"),
        "build_linux_xvfb_remote_command_fn": _binding(bindings, "build_linux_xvfb_remote_command"),
        "build_linux_window_driver_remote_command_fn": _binding(bindings, "build_linux_window_driver_remote_command"),
        "run_fn": subprocess_mod.run,
        "fetch_ssh_artifact_fn": _binding(bindings, "fetch_ssh_artifact"),
        "cleanup_remote_ssh_dir_fn": _binding(bindings, "cleanup_remote_ssh_dir"),
        "default_desktop_label_fn": _binding(bindings, "default_desktop_label"),
        "image_change_summary_fn": _binding(bindings, "image_change_summary"),
        "parse_coordinate_pair_fn": _binding(bindings, "parse_coordinate_pair"),
        "attach_desktop_source_to_manifest_fn": _binding(bindings, "attach_desktop_source_to_manifest"),
        "atomic_write_text_fn": _binding(bindings, "atomic_write_text"),
        "write_desktop_run_rollups_fn": _binding(bindings, "write_desktop_run_rollups"),
        "now_iso_fn": _binding(bindings, "now_iso"),
        "view_tree_inspector_summary_fn": desktop_actions.view_tree_inspector_summary,
        "pulp_app_interaction_summary_fn": desktop_actions.pulp_app_interaction_summary,
    }


def install_linux_desktop_action_dependency_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = LINUX_DESKTOP_ACTION_DEPENDENCY_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
