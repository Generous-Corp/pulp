"""macOS window probe, wait, and capture helpers for local CI."""

from __future__ import annotations

from collections.abc import Callable
import json
from pathlib import Path
import subprocess
import time


def macos_window_info_for_pid(
    pid: int,
    *,
    probe_path_fn: Callable[[], Path],
    run_fn: Callable[..., subprocess.CompletedProcess[str]],
) -> dict:
    result = run_fn(
        ["swift", str(probe_path_fn()), "window-info", "--pid", str(pid)],
        capture_output=True,
        text=True,
        check=True,
    )
    return json.loads(result.stdout)


def macos_window_info_for_bundle_id(
    bundle_id: str,
    *,
    probe_path_fn: Callable[[], Path],
    run_fn: Callable[..., subprocess.CompletedProcess[str]],
) -> dict:
    result = run_fn(
        ["swift", str(probe_path_fn()), "window-info", "--bundle-id", bundle_id],
        capture_output=True,
        text=True,
        check=True,
    )
    return json.loads(result.stdout)


def macos_accessibility_trusted(
    *,
    probe_path_fn: Callable[[], Path],
    run_fn: Callable[..., subprocess.CompletedProcess[str]],
) -> bool:
    result = run_fn(
        ["swift", str(probe_path_fn()), "accessibility-trusted"],
        capture_output=True,
        text=True,
        check=True,
    )
    payload = json.loads(result.stdout)
    return bool(payload.get("trusted"))


def wait_for_macos_window(
    pid: int,
    timeout_secs: float,
    *,
    macos_window_info_for_pid_fn: Callable[[int], dict],
    time_fn: Callable[[], float] = time.time,
    sleep_fn: Callable[[float], None] = time.sleep,
) -> dict:
    deadline = time_fn() + timeout_secs
    last_error = ""
    while time_fn() < deadline:
        try:
            payload = macos_window_info_for_pid_fn(pid)
        except (subprocess.SubprocessError, json.JSONDecodeError) as exc:
            last_error = str(exc)
            sleep_fn(0.2)
            continue
        windows = payload.get("windows", [])
        if windows:
            return windows[0]
        sleep_fn(0.2)
    raise RuntimeError(last_error or f"timed out waiting for a visible window for pid {pid}")


def wait_for_macos_bundle_window(
    bundle_id: str,
    timeout_secs: float,
    *,
    macos_window_info_for_bundle_id_fn: Callable[[str], dict],
    activate_macos_bundle_id_fn: Callable[[str], dict],
    time_fn: Callable[[], float] = time.time,
    sleep_fn: Callable[[float], None] = time.sleep,
) -> tuple[int, dict]:
    deadline = time_fn() + timeout_secs
    last_error = ""
    while time_fn() < deadline:
        try:
            payload = macos_window_info_for_bundle_id_fn(bundle_id)
        except (subprocess.SubprocessError, json.JSONDecodeError) as exc:
            last_error = str(exc)
            sleep_fn(0.2)
            continue
        windows = payload.get("windows", [])
        pid = payload.get("pid")
        if windows and isinstance(pid, int):
            return pid, windows[0]
        activation_payload = activate_macos_bundle_id_fn(bundle_id)
        if activation_payload.get("stderr"):
            last_error = activation_payload["stderr"]
        sleep_fn(0.2)
    raise RuntimeError(last_error or f"timed out waiting for a visible window for bundle id {bundle_id}")


def capture_macos_window(
    window_id: int,
    output_path: Path,
    *,
    run_fn: Callable[..., subprocess.CompletedProcess[str]],
    sleep_fn: Callable[[float], None] = time.sleep,
) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    last_error = ""
    for attempt in range(5):
        result = run_fn(
            ["screencapture", "-x", "-l", str(window_id), str(output_path)],
            capture_output=True,
            text=True,
        )
        if result.returncode == 0 and output_path.exists():
            return
        last_error = result.stderr.strip() or result.stdout.strip() or f"screencapture exited {result.returncode}"
        if attempt < 4:
            sleep_fn(0.2)
    raise RuntimeError(f"Could not capture macOS window {window_id}: {last_error}")
