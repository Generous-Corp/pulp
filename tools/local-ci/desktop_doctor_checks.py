"""Focused desktop doctor check builders."""
from __future__ import annotations

from collections.abc import Callable
import json
import subprocess

from desktop_doctor_optional import optional_desktop_doctor_checks
from desktop_doctor_remote import (
    linux_remote_doctor_checks,
    ssh_desktop_doctor_checks,
    windows_session_doctor_checks,
)


def macos_local_doctor_checks(
    *,
    platform: str,
    which_fn: Callable[[str], str | None],
    macos_accessibility_trusted_fn: Callable[[], bool],
    desktop_check_fn: Callable[..., dict],
) -> list[dict]:
    checks = [
        desktop_check_fn("platform", platform == "darwin", f"running on {platform}"),
        desktop_check_fn(
            "screencapture",
            which_fn("screencapture") is not None,
            which_fn("screencapture") or "missing",
        ),
        desktop_check_fn(
            "osascript",
            which_fn("osascript") is not None,
            which_fn("osascript") or "missing",
        ),
    ]
    try:
        trusted = macos_accessibility_trusted_fn()
        checks.append(
            desktop_check_fn(
                "accessibility",
                trusted,
                "trusted" if trusted else "not trusted; desktop-event click is unavailable but Pulp app automation still works",
                required=False,
            )
        )
    except (subprocess.SubprocessError, json.JSONDecodeError) as exc:
        checks.append(desktop_check_fn("accessibility", False, str(exc), required=False))
    return checks
