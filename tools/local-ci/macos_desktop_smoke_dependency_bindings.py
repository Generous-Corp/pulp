"""Dependency bindings for the macOS desktop smoke/action facade."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import install_local_helpers
from macos_desktop_smoke_artifact_dependency_bindings import (
    MACOS_DESKTOP_SMOKE_ARTIFACT_DEPENDENCY_EXPORTS,
    macos_desktop_smoke_artifact_dependencies,
)
from macos_desktop_smoke_interaction_dependency_bindings import (
    MACOS_DESKTOP_SMOKE_INTERACTION_DEPENDENCY_EXPORTS,
    macos_desktop_smoke_interaction_dependencies,
)
from macos_desktop_smoke_process_dependency_bindings import (
    MACOS_DESKTOP_SMOKE_PROCESS_DEPENDENCY_EXPORTS,
    macos_desktop_smoke_process_dependencies,
)
from macos_desktop_smoke_window_dependency_bindings import (
    MACOS_DESKTOP_SMOKE_WINDOW_DEPENDENCY_EXPORTS,
    macos_desktop_smoke_window_dependencies,
)


MACOS_DESKTOP_SMOKE_DEPENDENCY_EXPORTS = ("macos_desktop_smoke_dependencies",)
MACOS_DESKTOP_SMOKE_FOCUSED_DEPENDENCY_EXPORTS = (
    *MACOS_DESKTOP_SMOKE_ARTIFACT_DEPENDENCY_EXPORTS,
    *MACOS_DESKTOP_SMOKE_PROCESS_DEPENDENCY_EXPORTS,
    *MACOS_DESKTOP_SMOKE_WINDOW_DEPENDENCY_EXPORTS,
    *MACOS_DESKTOP_SMOKE_INTERACTION_DEPENDENCY_EXPORTS,
)


def macos_desktop_smoke_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    return {
        **macos_desktop_smoke_artifact_dependencies(bindings),
        **macos_desktop_smoke_process_dependencies(bindings),
        **macos_desktop_smoke_window_dependencies(bindings),
        **macos_desktop_smoke_interaction_dependencies(bindings),
    }


def install_macos_desktop_smoke_dependency_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = MACOS_DESKTOP_SMOKE_DEPENDENCY_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
