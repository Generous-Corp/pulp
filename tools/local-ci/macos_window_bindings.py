"""Facade dependency bindings for macOS window helpers."""

from __future__ import annotations

from pathlib import Path
import subprocess

from binding_utils import binding as _binding


def detect_macos_app_bundle(bindings: dict, command: str | None) -> Path | None:
    return _binding(bindings, "_macos_desktop").detect_macos_app_bundle(command)


def macos_bundle_id_for_app_path(bindings: dict, app_path: Path) -> str | None:
    return _binding(bindings, "_macos_desktop").macos_bundle_id_for_app_path(app_path)


def macos_window_probe_path(bindings: dict) -> Path:
    return _binding(bindings, "_macos_desktop").macos_window_probe_path(_binding(bindings, "SCRIPT_DIR"))


def macos_window_info_for_pid(bindings: dict, pid: int) -> dict:
    return _binding(bindings, "_macos_desktop").macos_window_info_for_pid(
        pid,
        probe_path_fn=_binding(bindings, "macos_window_probe_path"),
        run_fn=_binding(bindings, "subprocess").run,
    )


def macos_window_info_for_bundle_id(bindings: dict, bundle_id: str) -> dict:
    return _binding(bindings, "_macos_desktop").macos_window_info_for_bundle_id(
        bundle_id,
        probe_path_fn=_binding(bindings, "macos_window_probe_path"),
        run_fn=_binding(bindings, "subprocess").run,
    )


def macos_accessibility_trusted(bindings: dict) -> bool:
    return _binding(bindings, "_macos_desktop").macos_accessibility_trusted(
        probe_path_fn=_binding(bindings, "macos_window_probe_path"),
        run_fn=_binding(bindings, "subprocess").run,
    )


def wait_for_macos_window(bindings: dict, pid: int, timeout_secs: float) -> dict:
    return _binding(bindings, "_macos_desktop").wait_for_macos_window(
        pid,
        timeout_secs,
        macos_window_info_for_pid_fn=_binding(bindings, "macos_window_info_for_pid"),
        time_fn=_binding(bindings, "time").time,
        sleep_fn=_binding(bindings, "time").sleep,
    )


def wait_for_macos_bundle_window(bindings: dict, bundle_id: str, timeout_secs: float) -> tuple[int, dict]:
    return _binding(bindings, "_macos_desktop").wait_for_macos_bundle_window(
        bundle_id,
        timeout_secs,
        macos_window_info_for_bundle_id_fn=_binding(bindings, "macos_window_info_for_bundle_id"),
        activate_macos_bundle_id_fn=_binding(bindings, "activate_macos_bundle_id"),
        time_fn=_binding(bindings, "time").time,
        sleep_fn=_binding(bindings, "time").sleep,
    )


def capture_macos_window(bindings: dict, window_id: int, output_path: Path) -> None:
    return _binding(bindings, "_macos_desktop").capture_macos_window(
        window_id,
        output_path,
        run_fn=_binding(bindings, "subprocess").run,
        sleep_fn=_binding(bindings, "time").sleep,
    )


def activate_macos_pid(bindings: dict, pid: int) -> dict:
    return _binding(bindings, "_macos_desktop").activate_macos_pid(
        pid,
        probe_path_fn=_binding(bindings, "macos_window_probe_path"),
        run_fn=_binding(bindings, "subprocess").run,
    )


def activate_macos_bundle_id(bindings: dict, bundle_id: str) -> dict:
    return _binding(bindings, "_macos_desktop").activate_macos_bundle_id(
        bundle_id,
        run_fn=_binding(bindings, "subprocess").run,
    )


def dispatch_macos_click(bindings: dict, screen_x: float, screen_y: float) -> dict:
    return _binding(bindings, "_macos_desktop").dispatch_macos_click(
        screen_x,
        screen_y,
        probe_path_fn=_binding(bindings, "macos_window_probe_path"),
        run_fn=_binding(bindings, "subprocess").run,
    )


def terminate_process(bindings: dict, proc: subprocess.Popen, timeout_secs: float = 5.0) -> None:
    return _binding(bindings, "_macos_desktop").terminate_process(proc, timeout_secs=timeout_secs)


def quit_macos_bundle_id(bindings: dict, bundle_id: str) -> None:
    return _binding(bindings, "_macos_desktop").quit_macos_bundle_id(
        bundle_id,
        run_fn=_binding(bindings, "subprocess").run,
    )
