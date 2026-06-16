"""macOS window-selection helpers for desktop video proofs.

Picks the right window to record when an app opens several (main window vs a
floating plugin editor), and waits for a titled or secondary window to appear.
Window-info and activation are injected so the smoke flow can supply the
real probe helpers (and tests can supply fakes).
"""

from __future__ import annotations

from collections.abc import Callable
import json
import subprocess
import time


def wait_for_macos_bundle_window_title(
    bundle_id: str,
    title_contains: str,
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
        for window in windows:
            title = str(window.get("title") or "")
            if title_contains in title and isinstance(pid, int):
                return pid, window
        activation_payload = activate_macos_bundle_id_fn(bundle_id)
        if activation_payload.get("stderr"):
            last_error = activation_payload["stderr"]
        sleep_fn(0.2)
    raise RuntimeError(last_error or f"timed out waiting for Terminal window titled `{title_contains}`")


def _window_area(window: dict) -> float:
    bounds = window.get("bounds") if isinstance(window.get("bounds"), dict) else {}
    try:
        return float(bounds.get("width") or 0.0) * float(bounds.get("height") or 0.0)
    except (TypeError, ValueError):
        return 0.0


def _window_bounds(window: dict) -> tuple[float, float]:
    bounds = window.get("bounds") if isinstance(window.get("bounds"), dict) else {}
    try:
        return float(bounds.get("width") or 0.0), float(bounds.get("height") or 0.0)
    except (TypeError, ValueError):
        return 0.0, 0.0


def _likely_floating_editor_window(window: dict) -> bool:
    width, height = _window_bounds(window)
    return 320.0 <= width <= 560.0 and 240.0 <= height <= 560.0


def wait_for_macos_bundle_secondary_window(
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
        if len(windows) > 1 and isinstance(pid, int):
            primary_id = windows[0].get("windowId")
            candidates = [
                window
                for window in windows[1:]
                if window.get("windowId") != primary_id and _window_area(window) > 0
            ]
            if candidates:
                preferred = [window for window in candidates if _likely_floating_editor_window(window)]
                return pid, max(preferred or candidates, key=_window_area)
        activation_payload = activate_macos_bundle_id_fn(bundle_id)
        if activation_payload.get("stderr"):
            last_error = activation_payload["stderr"]
        sleep_fn(0.2)
    raise RuntimeError(last_error or f"timed out waiting for a secondary window for bundle id {bundle_id}")


