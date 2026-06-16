"""Compatibility installer for macOS desktop action facade bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from macos_desktop_smoke_bindings import (
    MACOS_DESKTOP_SMOKE_EXPORTS,
    install_macos_desktop_smoke_helpers,
    run_macos_local_smoke,
)


MACOS_DESKTOP_EXPORTS = MACOS_DESKTOP_SMOKE_EXPORTS


def install_macos_desktop_helpers(bindings: dict[str, Any], names: tuple[str, ...] = MACOS_DESKTOP_EXPORTS) -> None:
    smoke_names = tuple(name for name in names if name in MACOS_DESKTOP_SMOKE_EXPORTS)
    known_names = set(MACOS_DESKTOP_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_macos_desktop_smoke_helpers(bindings, smoke_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
