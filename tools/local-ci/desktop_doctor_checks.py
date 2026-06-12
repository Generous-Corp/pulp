"""Focused desktop doctor check builders."""
from __future__ import annotations

from collections.abc import Callable
import json
import subprocess

from normalize import normalize_desktop_optional_config


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


def optional_desktop_doctor_checks(
    target: dict,
    *,
    which_fn: Callable[[str], str | None],
    probe_webdriver_endpoint_fn: Callable[..., dict],
    desktop_check_fn: Callable[..., dict],
) -> list[dict]:
    checks: list[dict] = []
    optional = normalize_desktop_optional_config(target.get("optional"))
    if optional.get("webview_driver"):
        webdriver_url = optional.get("webdriver_url")
        if not webdriver_url:
            checks.append(desktop_check_fn("webview_driver", False, "enabled but webdriver_url is not set", required=False))
        else:
            try:
                probe = probe_webdriver_endpoint_fn(webdriver_url)
                ready = probe.get("ready")
                ready_text = "" if ready is None else f" (ready={str(ready).lower()})"
                message = probe.get("message")
                detail = f"reachable at {probe['status_url']}{ready_text}"
                if message:
                    detail = f"{detail}: {message}"
                checks.append(desktop_check_fn("webview_driver", ready is not False, detail, required=False))
            except (RuntimeError, ValueError) as exc:
                checks.append(desktop_check_fn("webview_driver", False, str(exc), required=False))
    if optional.get("debug_attach"):
        debugger_command = optional.get("debugger_command")
        if target["target_type"] == "local":
            debugger = debugger_command or "lldb"
            debugger_path = which_fn(debugger)
            checks.append(
                desktop_check_fn(
                    "debug_attach",
                    debugger_path is not None,
                    debugger_path or f"{debugger} not found on PATH",
                    required=False,
                )
            )
        else:
            detail = debugger_command or "enabled; remote debugger validation deferred to target tooling"
            checks.append(desktop_check_fn("debug_attach", True, detail, required=False))
    if optional.get("video_capture"):
        if target["target_type"] == "local":
            ffmpeg_path = which_fn("ffmpeg")
            checks.append(
                desktop_check_fn(
                    "video_capture",
                    ffmpeg_path is not None,
                    ffmpeg_path or "ffmpeg not found on PATH",
                    required=False,
                )
            )
        else:
            checks.append(
                desktop_check_fn(
                    "video_capture",
                    True,
                    "enabled; remote video tooling validation deferred to target tooling",
                    required=False,
                )
            )
    if optional.get("frame_stats"):
        checks.append(desktop_check_fn("frame_stats", True, "enabled", required=False))
    return checks
