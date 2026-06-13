"""Facade dependency bindings for macOS window action helpers."""

from __future__ import annotations

import subprocess

from binding_utils import binding as _binding
from binding_utils import binding_attr as _binding_attr
from binding_utils import install_local_helpers


MACOS_WINDOW_ACTION_EXPORTS = (
    "activate_macos_pid",
    "activate_macos_bundle_id",
    "dispatch_macos_click",
    "terminate_process",
    "quit_macos_bundle_id",
)


def activate_macos_pid(bindings: dict, pid: int) -> dict:
    return _binding(bindings, "_macos_desktop").activate_macos_pid(
        pid,
        probe_path_fn=_binding(bindings, "macos_window_probe_path"),
        run_fn=_binding_attr(bindings, "subprocess", "run"),
    )


def activate_macos_bundle_id(bindings: dict, bundle_id: str) -> dict:
    return _binding(bindings, "_macos_desktop").activate_macos_bundle_id(
        bundle_id,
        run_fn=_binding_attr(bindings, "subprocess", "run"),
    )


def dispatch_macos_click(bindings: dict, screen_x: float, screen_y: float) -> dict:
    return _binding(bindings, "_macos_desktop").dispatch_macos_click(
        screen_x,
        screen_y,
        probe_path_fn=_binding(bindings, "macos_window_probe_path"),
        run_fn=_binding_attr(bindings, "subprocess", "run"),
    )


def terminate_process(bindings: dict, proc: subprocess.Popen, timeout_secs: float = 5.0) -> None:
    return _binding(bindings, "_macos_desktop").terminate_process(proc, timeout_secs=timeout_secs)


def quit_macos_bundle_id(bindings: dict, bundle_id: str) -> None:
    return _binding(bindings, "_macos_desktop").quit_macos_bundle_id(
        bundle_id,
        run_fn=_binding_attr(bindings, "subprocess", "run"),
    )


def install_macos_window_action_helpers(
    bindings: dict,
    names: tuple[str, ...] = MACOS_WINDOW_ACTION_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
