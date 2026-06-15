"""macOS window action and process helpers for local CI."""

from __future__ import annotations

from collections.abc import Callable
import json
from pathlib import Path
import subprocess


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
