"""Compatibility facade for macOS window probe/capture bindings."""

from __future__ import annotations

from binding_utils import install_local_helpers
from macos_window_capture_bindings import MACOS_WINDOW_CAPTURE_EXPORTS, capture_macos_window
from macos_window_info_bindings import (
    MACOS_WINDOW_INFO_EXPORTS,
    macos_accessibility_trusted,
    macos_window_info_for_bundle_id,
    macos_window_info_for_pid,
)
from macos_window_wait_bindings import (
    MACOS_WINDOW_WAIT_EXPORTS,
    wait_for_macos_bundle_window,
    wait_for_macos_window,
)


MACOS_WINDOW_PROBE_EXPORTS = (
    *MACOS_WINDOW_INFO_EXPORTS,
    *MACOS_WINDOW_WAIT_EXPORTS,
    *MACOS_WINDOW_CAPTURE_EXPORTS,
)


def install_macos_window_probe_helpers(
    bindings: dict,
    names: tuple[str, ...] = MACOS_WINDOW_PROBE_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
