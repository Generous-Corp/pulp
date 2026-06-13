"""Compatibility installer for macOS desktop action facade bindings."""

from __future__ import annotations

from typing import Any

from macos_desktop_smoke_bindings import (
    MACOS_DESKTOP_SMOKE_EXPORTS,
    install_macos_desktop_smoke_helpers,
    run_macos_local_smoke,
)


MACOS_DESKTOP_EXPORTS = MACOS_DESKTOP_SMOKE_EXPORTS


def install_macos_desktop_helpers(bindings: dict[str, Any], names: tuple[str, ...] = MACOS_DESKTOP_EXPORTS) -> None:
    install_macos_desktop_smoke_helpers(bindings, names)
