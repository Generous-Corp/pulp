"""Artifact and rollup dependency bindings for Windows desktop actions."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


WINDOWS_DESKTOP_ACTION_ARTIFACT_DEPENDENCY_EXPORTS = ("windows_desktop_action_artifact_dependencies",)


def windows_desktop_action_artifact_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    desktop_actions = _binding(bindings, "_desktop_actions")

    return {
        "create_desktop_run_bundle_fn": _binding(bindings, "create_desktop_run_bundle"),
        "desktop_action_artifact_paths_fn": desktop_actions.desktop_action_artifact_paths,
        "atomic_write_text_fn": _binding(bindings, "atomic_write_text"),
        "windows_ssh_fetch_file_fn": _binding(bindings, "windows_ssh_fetch_file"),
        "windows_ssh_remove_path_fn": _binding(bindings, "windows_ssh_remove_path"),
        "write_desktop_run_rollups_fn": _binding(bindings, "write_desktop_run_rollups"),
        "now_iso_fn": _binding(bindings, "now_iso"),
    }


def install_windows_desktop_action_artifact_dependency_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = WINDOWS_DESKTOP_ACTION_ARTIFACT_DEPENDENCY_EXPORTS,
) -> None:
    known_names = set(WINDOWS_DESKTOP_ACTION_ARTIFACT_DEPENDENCY_EXPORTS)
    artifact_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), artifact_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
