"""Facade dependency bindings for macOS app bundle helpers."""

from __future__ import annotations

from pathlib import Path

from binding_utils import binding as _binding


def detect_macos_app_bundle(bindings: dict, command: str | None) -> Path | None:
    return _binding(bindings, "_macos_desktop").detect_macos_app_bundle(command)


def macos_bundle_id_for_app_path(bindings: dict, app_path: Path) -> str | None:
    return _binding(bindings, "_macos_desktop").macos_bundle_id_for_app_path(app_path)


def macos_window_probe_path(bindings: dict) -> Path:
    return _binding(bindings, "_macos_desktop").macos_window_probe_path(_binding(bindings, "SCRIPT_DIR"))
