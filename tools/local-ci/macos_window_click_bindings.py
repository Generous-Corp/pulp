"""Bindings from the local_ci facade to macOS window click helpers."""

from __future__ import annotations

from binding_utils import binding as _binding
from binding_utils import binding_attr as _binding_attr
from binding_utils import install_local_helpers


MACOS_WINDOW_CLICK_EXPORTS = ("dispatch_macos_click",)


def dispatch_macos_click(bindings: dict, screen_x: float, screen_y: float) -> dict:
    return _binding(bindings, "_macos_desktop").dispatch_macos_click(
        screen_x,
        screen_y,
        probe_path_fn=_binding(bindings, "macos_window_probe_path"),
        run_fn=_binding_attr(bindings, "subprocess", "run"),
    )


def install_macos_window_click_helpers(
    bindings: dict,
    names: tuple[str, ...] = MACOS_WINDOW_CLICK_EXPORTS,
) -> None:
    known_names = set(MACOS_WINDOW_CLICK_EXPORTS)
    click_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), click_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
