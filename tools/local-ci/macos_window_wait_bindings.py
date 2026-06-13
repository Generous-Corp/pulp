"""Facade dependency bindings for macOS window wait helpers."""

from __future__ import annotations

from binding_utils import binding as _binding
from binding_utils import binding_attr as _binding_attr


MACOS_WINDOW_WAIT_EXPORTS = (
    "wait_for_macos_window",
    "wait_for_macos_bundle_window",
)


def wait_for_macos_window(bindings: dict, pid: int, timeout_secs: float) -> dict:
    return _binding(bindings, "_macos_desktop").wait_for_macos_window(
        pid,
        timeout_secs,
        macos_window_info_for_pid_fn=_binding(bindings, "macos_window_info_for_pid"),
        time_fn=_binding_attr(bindings, "time", "time"),
        sleep_fn=_binding_attr(bindings, "time", "sleep"),
    )


def wait_for_macos_bundle_window(bindings: dict, bundle_id: str, timeout_secs: float) -> tuple[int, dict]:
    return _binding(bindings, "_macos_desktop").wait_for_macos_bundle_window(
        bundle_id,
        timeout_secs,
        macos_window_info_for_bundle_id_fn=_binding(bindings, "macos_window_info_for_bundle_id"),
        activate_macos_bundle_id_fn=_binding(bindings, "activate_macos_bundle_id"),
        time_fn=_binding_attr(bindings, "time", "time"),
        sleep_fn=_binding_attr(bindings, "time", "sleep"),
    )
