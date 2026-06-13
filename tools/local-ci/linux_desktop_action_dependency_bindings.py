"""Compatibility facade for Linux desktop action dependency bindings."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import install_local_helpers
from linux_desktop_action_artifact_dependency_bindings import linux_desktop_action_artifact_dependencies
from linux_desktop_action_host_dependency_bindings import linux_desktop_action_host_dependencies
from linux_desktop_action_interaction_dependency_bindings import linux_desktop_action_interaction_dependencies
from linux_desktop_action_source_dependency_bindings import linux_desktop_action_source_dependencies


LINUX_DESKTOP_ACTION_DEPENDENCY_EXPORTS = ("linux_desktop_action_dependencies",)


def linux_desktop_action_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    return {
        **linux_desktop_action_host_dependencies(bindings),
        **linux_desktop_action_source_dependencies(bindings),
        **linux_desktop_action_artifact_dependencies(bindings),
        **linux_desktop_action_interaction_dependencies(bindings),
    }


def install_linux_desktop_action_dependency_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = LINUX_DESKTOP_ACTION_DEPENDENCY_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
