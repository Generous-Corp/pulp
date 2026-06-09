"""macOS desktop/window helpers for local CI."""

from __future__ import annotations

from collections.abc import Callable
import json
import plistlib
from pathlib import Path
import shlex
import subprocess
import time


def detect_macos_app_bundle(command: str | None) -> Path | None:
    if not command:
        return None
    args = shlex.split(command)
    if not args:
        return None
    exec_path = Path(args[0]).expanduser()
    candidates = [exec_path, *exec_path.parents]
    for candidate in candidates:
        if candidate.suffix == ".app":
            return candidate
    return None


def macos_bundle_id_for_app_path(app_path: Path) -> str | None:
    info_plist = app_path / "Contents" / "Info.plist"
    if not info_plist.exists():
        return None
    try:
        payload = plistlib.loads(info_plist.read_bytes())
    except (plistlib.InvalidFileException, OSError):
        return None
    bundle_id = payload.get("CFBundleIdentifier")
    return bundle_id if isinstance(bundle_id, str) and bundle_id else None


def macos_window_probe_path(script_dir: Path) -> Path:
    return script_dir / "macos_window_probe.swift"


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


def activate_macos_pid(
    pid: int,
    *,
    probe_path_fn: Callable[[], Path],
    run_fn: Callable[..., subprocess.CompletedProcess[str]],
) -> dict:
    result = run_fn(
        ["swift", str(probe_path_fn()), "activate", "--pid", str(pid)],
        capture_output=True,
        text=True,
        check=True,
    )
    return json.loads(result.stdout)


def activate_macos_bundle_id(
    bundle_id: str,
    *,
    run_fn: Callable[..., subprocess.CompletedProcess[str]],
) -> dict:
    result = run_fn(
        ["osascript", "-e", f'tell application id "{bundle_id}" to activate'],
        capture_output=True,
        text=True,
    )
    return {
        "activated": result.returncode == 0,
        "bundle_id": bundle_id,
        "stdout": result.stdout.strip(),
        "stderr": result.stderr.strip(),
        "returncode": result.returncode,
    }


def dispatch_macos_click(
    screen_x: float,
    screen_y: float,
    *,
    probe_path_fn: Callable[[], Path],
    run_fn: Callable[..., subprocess.CompletedProcess[str]],
) -> dict:
    result = run_fn(
        [
            "swift",
            str(probe_path_fn()),
            "click",
            "--x",
            str(screen_x),
            "--y",
            str(screen_y),
        ],
        capture_output=True,
        text=True,
        check=True,
    )
    return json.loads(result.stdout)


def terminate_process(proc: subprocess.Popen, timeout_secs: float = 5.0) -> None:
    if proc.poll() is not None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=timeout_secs)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=timeout_secs)


def quit_macos_bundle_id(
    bundle_id: str,
    *,
    run_fn: Callable[..., subprocess.CompletedProcess[str]],
) -> None:
    run_fn(
        ["osascript", "-e", f'tell application id "{bundle_id}" to quit'],
        capture_output=True,
        text=True,
        check=False,
    )
