"""Windows desktop session-agent remote artifact fetch helpers."""

from __future__ import annotations

from collections.abc import Callable
from pathlib import Path


def fetch_windows_session_agent_outputs(
    *,
    host: str,
    request: dict,
    capture_before: bool,
    capture_ui_snapshot: bool,
    screenshot_path: Path,
    before_screenshot_path: Path,
    ui_snapshot_path: Path,
    log_path: Path,
    err_path: Path,
    windows_ssh_fetch_file_fn: Callable[..., bool],
) -> None:
    fetch_stdout = windows_ssh_fetch_file_fn(
        host,
        request["outputs"]["stdout"],
        log_path,
        optional=True,
        timeout=30,
    )
    fetch_stderr = windows_ssh_fetch_file_fn(
        host,
        request["outputs"]["stderr"],
        err_path,
        optional=True,
        timeout=30,
    )
    if not fetch_stdout:
        log_path.write_text("")
    if not fetch_stderr:
        err_path.write_text("")
    windows_ssh_fetch_file_fn(host, request["outputs"]["screenshot"], screenshot_path, timeout=60)
    if capture_before:
        windows_ssh_fetch_file_fn(
            host,
            request["outputs"]["before_screenshot"],
            before_screenshot_path,
            optional=False,
            timeout=60,
        )
    if capture_ui_snapshot:
        windows_ssh_fetch_file_fn(
            host,
            request["outputs"]["ui_snapshot"],
            ui_snapshot_path,
            optional=False,
            timeout=30,
        )
