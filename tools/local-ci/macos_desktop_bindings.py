"""Compatibility installer for macOS desktop action facade bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from macos_desktop_smoke_bindings import run_macos_local_smoke


MACOS_DESKTOP_EXPORTS = (
    "run_macos_local_smoke",
)


def install_macos_desktop_helpers(bindings: dict[str, Any], names: tuple[str, ...] = MACOS_DESKTOP_EXPORTS) -> None:
    install_local_helpers(bindings, globals(), names)
