"""Facade dependency bindings for macOS window capture helpers."""

from __future__ import annotations

from pathlib import Path

from binding_utils import binding as _binding
from binding_utils import binding_attr as _binding_attr
from binding_utils import install_local_helpers


MACOS_WINDOW_CAPTURE_EXPORTS = ("capture_macos_window",)


def capture_macos_window(bindings: dict, window_id: int, output_path: Path) -> None:
    return _binding(bindings, "_macos_desktop").capture_macos_window(
        window_id,
        output_path,
        run_fn=_binding_attr(bindings, "subprocess", "run"),
        sleep_fn=_binding_attr(bindings, "time", "sleep"),
    )


def install_macos_window_capture_helpers(
    bindings: dict,
    names: tuple[str, ...] = MACOS_WINDOW_CAPTURE_EXPORTS,
) -> None:
    known_names = set(MACOS_WINDOW_CAPTURE_EXPORTS)
    capture_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), capture_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
