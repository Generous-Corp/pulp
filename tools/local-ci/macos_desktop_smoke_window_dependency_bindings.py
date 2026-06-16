"""Window/capture dependency bindings for macOS desktop smoke actions."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import install_local_helpers
from binding_utils import binding as _binding


MACOS_DESKTOP_SMOKE_WINDOW_DEPENDENCY_EXPORTS = ("macos_desktop_smoke_window_dependencies",)


def macos_desktop_smoke_window_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    desktop_actions = _binding(bindings, "_desktop_actions")

    return {
        "wait_for_macos_window_fn": _binding(bindings, "wait_for_macos_window"),
        "content_size_from_window_fn": desktop_actions.content_size_from_window,
        "wait_for_path_fn": _binding(bindings, "wait_for_path"),
        "content_size_from_view_tree_fn": desktop_actions.content_size_from_view_tree,
        "capture_macos_window_fn": _binding(bindings, "capture_macos_window"),
    }


def install_macos_desktop_smoke_window_dependency_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = MACOS_DESKTOP_SMOKE_WINDOW_DEPENDENCY_EXPORTS,
) -> None:
    known_names = set(MACOS_DESKTOP_SMOKE_WINDOW_DEPENDENCY_EXPORTS)
    window_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), window_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
