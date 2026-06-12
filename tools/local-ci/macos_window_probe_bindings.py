"""Facade dependency bindings for macOS window probe/capture helpers."""

from __future__ import annotations

from pathlib import Path

from binding_utils import binding as _binding
from binding_utils import binding_attr as _binding_attr


def macos_window_info_for_pid(bindings: dict, pid: int) -> dict:
    return _binding(bindings, "_macos_desktop").macos_window_info_for_pid(
        pid,
        probe_path_fn=_binding(bindings, "macos_window_probe_path"),
        run_fn=_binding_attr(bindings, "subprocess", "run"),
    )


def macos_window_info_for_bundle_id(bindings: dict, bundle_id: str) -> dict:
    return _binding(bindings, "_macos_desktop").macos_window_info_for_bundle_id(
        bundle_id,
        probe_path_fn=_binding(bindings, "macos_window_probe_path"),
        run_fn=_binding_attr(bindings, "subprocess", "run"),
    )


def macos_accessibility_trusted(bindings: dict) -> bool:
    return _binding(bindings, "_macos_desktop").macos_accessibility_trusted(
        probe_path_fn=_binding(bindings, "macos_window_probe_path"),
        run_fn=_binding_attr(bindings, "subprocess", "run"),
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


def capture_macos_window(bindings: dict, window_id: int, output_path: Path) -> None:
    return _binding(bindings, "_macos_desktop").capture_macos_window(
        window_id,
        output_path,
        run_fn=_binding_attr(bindings, "subprocess", "run"),
        sleep_fn=_binding_attr(bindings, "time", "sleep"),
    )
