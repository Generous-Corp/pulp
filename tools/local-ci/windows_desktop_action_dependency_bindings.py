"""Compatibility facade for Windows desktop action dependency bindings."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import install_local_helpers
from windows_desktop_action_artifact_dependency_bindings import windows_desktop_action_artifact_dependencies
from windows_desktop_action_host_dependency_bindings import windows_desktop_action_host_dependencies
from windows_desktop_action_interaction_dependency_bindings import windows_desktop_action_interaction_dependencies
from windows_desktop_action_source_dependency_bindings import windows_desktop_action_source_dependencies


WINDOWS_DESKTOP_ACTION_DEPENDENCY_EXPORTS = ("windows_desktop_action_dependencies",)


def windows_desktop_action_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    return {
        **windows_desktop_action_host_dependencies(bindings),
        **windows_desktop_action_source_dependencies(bindings),
        **windows_desktop_action_artifact_dependencies(bindings),
        **windows_desktop_action_interaction_dependencies(bindings),
    }


def install_windows_desktop_action_dependency_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = WINDOWS_DESKTOP_ACTION_DEPENDENCY_EXPORTS,
) -> None:
    known_names = set(WINDOWS_DESKTOP_ACTION_DEPENDENCY_EXPORTS)
    dependency_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), dependency_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
