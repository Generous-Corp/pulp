"""Artifact/source dependency bindings for macOS desktop smoke actions."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import install_local_helpers
from binding_utils import binding as _binding


MACOS_DESKTOP_SMOKE_ARTIFACT_DEPENDENCY_EXPORTS = ("macos_desktop_smoke_artifact_dependencies",)


def macos_desktop_smoke_artifact_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    desktop_actions = _binding(bindings, "_desktop_actions")

    return {
        "create_desktop_run_bundle_fn": _binding(bindings, "create_desktop_run_bundle"),
        "desktop_action_artifact_paths_fn": desktop_actions.desktop_action_artifact_paths,
        "desktop_interaction_requested_fn": desktop_actions.desktop_interaction_requested,
        "now_iso_fn": _binding(bindings, "now_iso"),
        "prepare_macos_exact_sha_source_fn": _binding(bindings, "prepare_macos_exact_sha_source"),
        "image_change_summary_fn": _binding(bindings, "image_change_summary"),
        "attach_desktop_source_to_manifest_fn": _binding(bindings, "attach_desktop_source_to_manifest"),
        "atomic_write_text_fn": _binding(bindings, "atomic_write_text"),
        "write_desktop_run_rollups_fn": _binding(bindings, "write_desktop_run_rollups"),
    }


def install_macos_desktop_smoke_artifact_dependency_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = MACOS_DESKTOP_SMOKE_ARTIFACT_DEPENDENCY_EXPORTS,
) -> None:
    known_names = set(MACOS_DESKTOP_SMOKE_ARTIFACT_DEPENDENCY_EXPORTS)
    artifact_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), artifact_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
