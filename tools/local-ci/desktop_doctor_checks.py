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
    probe_macos_screencapture_fn: Callable[[], tuple[bool, str]] | None = None,
    resolve_ffmpeg_path_fn: Callable[[], str] | None = None,
    probe_macos_avfoundation_screen_fn: Callable[[str], tuple[bool, str]] | None = None,
) -> list[dict]:
    screencapture_path = which_fn("screencapture")
    if not screencapture_path:
        screencapture_check = desktop_check_fn("screencapture", False, "missing")
    elif platform == "darwin" and probe_macos_screencapture_fn is not None:
        # A path alone is not proof of Screen Recording permission; probe a real
        # capture so the doctor flags a missing TCC grant before a proof run.
        ok, detail = probe_macos_screencapture_fn()
        screencapture_check = desktop_check_fn("screencapture", ok, screencapture_path if ok else detail)
    else:
        screencapture_check = desktop_check_fn("screencapture", True, screencapture_path)
    checks = [
        desktop_check_fn("platform", platform == "darwin", f"running on {platform}"),
        screencapture_check,
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
                "trusted"
                if trusted
                else (
                    "not trusted; synthetic-cursor (desktop-event) clicks are unavailable. "
                    "Grant System Settings > Privacy & Security > Accessibility to Terminal.app, "
                    "then quit and reopen Terminal. Pulp app automation clicks still work without it."
                ),
                required=False,
            )
        )
    except (subprocess.SubprocessError, json.JSONDecodeError) as exc:
        checks.append(desktop_check_fn("accessibility", False, str(exc), required=False))

    # Video-proof readiness (only when the video probes are wired in): ffmpeg
    # availability + a live AVFoundation screen-capture probe.
    if platform == "darwin" and resolve_ffmpeg_path_fn is not None:
        ffmpeg_path = None
        try:
            ffmpeg_path = resolve_ffmpeg_path_fn()
            checks.append(
                desktop_check_fn("video_capture", ffmpeg_path is not None, ffmpeg_path or "ffmpeg not found", required=False)
            )
        except RuntimeError as exc:
            checks.append(desktop_check_fn("video_capture", False, str(exc), required=False))
        if ffmpeg_path and probe_macos_avfoundation_screen_fn is not None:
            ok, detail = probe_macos_avfoundation_screen_fn(ffmpeg_path)
            checks.append(desktop_check_fn("avfoundation_screen", ok, detail, required=False))
    return checks
