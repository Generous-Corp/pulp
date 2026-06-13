"""Facade dependency bindings for macOS window capture helpers."""

from __future__ import annotations

from pathlib import Path

from binding_utils import binding as _binding
from binding_utils import binding_attr as _binding_attr


MACOS_WINDOW_CAPTURE_EXPORTS = ("capture_macos_window",)


def capture_macos_window(bindings: dict, window_id: int, output_path: Path) -> None:
    return _binding(bindings, "_macos_desktop").capture_macos_window(
        window_id,
        output_path,
        run_fn=_binding_attr(bindings, "subprocess", "run"),
        sleep_fn=_binding_attr(bindings, "time", "sleep"),
    )
