"""Facade dependency bindings for macOS app bundle helpers."""

from __future__ import annotations

from pathlib import Path

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


MACOS_WINDOW_APP_EXPORTS = (
    "detect_macos_app_bundle",
    "macos_bundle_id_for_app_path",
    "macos_window_probe_path",
)


def detect_macos_app_bundle(bindings: dict, command: str | None) -> Path | None:
    return _binding(bindings, "_macos_desktop").detect_macos_app_bundle(command)


def macos_bundle_id_for_app_path(bindings: dict, app_path: Path) -> str | None:
    return _binding(bindings, "_macos_desktop").macos_bundle_id_for_app_path(app_path)


def macos_window_probe_path(bindings: dict) -> Path:
    return _binding(bindings, "_macos_desktop").macos_window_probe_path(_binding(bindings, "SCRIPT_DIR"))


def install_macos_window_app_helpers(
    bindings: dict,
    names: tuple[str, ...] = MACOS_WINDOW_APP_EXPORTS,
) -> None:
    known_names = set(MACOS_WINDOW_APP_EXPORTS)
    app_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), app_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
