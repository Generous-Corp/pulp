"""Optional desktop-doctor capability and WebDriver probe helpers."""

from __future__ import annotations

from collections.abc import Callable
import json
import shutil
import urllib.error
import urllib.parse
import urllib.request

from normalize import normalize_desktop_optional_config


def desktop_optional_capabilities(optional_cfg: dict | None) -> list[str]:
    optional = normalize_desktop_optional_config(optional_cfg)
    caps: list[str] = []
    if optional.get("webview_driver"):
        caps.extend(["webview_dom", "semantic_click", "semantic_type", "script_eval", "element_screenshot"])
    if optional.get("debug_attach"):
        caps.extend(["debug_attach", "debug_command"])
    if optional.get("video_capture"):
        caps.append("video_capture")
    if optional.get("frame_stats"):
        caps.append("frame_stats")
    return caps


def desktop_capabilities_for(adapter: str, tier: str, optional_cfg: dict | None = None) -> list[str]:
    base = ["launch_app", "wait_ready", "window_screenshot", "collect_logs", "crash_artifacts"]
    if tier in {"v2", "v3"}:
        if adapter == "linux-xvfb":
            base.extend(["coordinate_click", "before_after_capture", "image_diff"])
        else:
            base.extend(["ui_snapshot", "coordinate_click", "view_target_click", "before_after_capture", "image_diff"])
            if adapter in {"macos-local", "windows-session-agent"}:
                base.append("pulp_app_automation")
    if tier == "v3":
        base.extend(["type_text", "wheel", "desktop_screenshot", "record_video", "debug_attach"])
    base.extend(desktop_optional_capabilities(optional_cfg))
    return list(dict.fromkeys(base))


def webdriver_status_url(base_url: str) -> str:
    parsed = urllib.parse.urlparse((base_url or "").strip())
    if not parsed.scheme or not parsed.netloc:
        raise ValueError("webdriver_url must include a scheme and host, for example http://127.0.0.1:4444")
    path = parsed.path or ""
    if not path or path == "/":
        path = "/status"
    elif not path.rstrip("/").endswith("/status"):
        path = f"{path.rstrip('/')}/status"
    return urllib.parse.urlunparse(parsed._replace(path=path, params="", query="", fragment=""))


def probe_webdriver_endpoint(
    base_url: str,
    *,
    timeout: float = 5.0,
    request_cls: Callable[..., urllib.request.Request] | None = None,
    urlopen_fn: Callable[..., object] | None = None,
) -> dict:
    request_cls = request_cls or urllib.request.Request
    urlopen_fn = urlopen_fn or urllib.request.urlopen
    status_url = webdriver_status_url(base_url)
    request = request_cls(status_url, headers={"Accept": "application/json"})
    try:
        with urlopen_fn(request, timeout=timeout) as response:
            payload = json.loads(response.read().decode("utf-8") or "{}")
    except urllib.error.HTTPError as exc:
        body = exc.read().decode("utf-8", errors="replace").strip()
        detail = f"HTTP {exc.code}"
        if body:
            detail = f"{detail}: {body[:200]}"
        raise RuntimeError(detail) from exc
    except urllib.error.URLError as exc:
        reason = getattr(exc, "reason", exc)
        raise RuntimeError(str(reason)) from exc
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"invalid JSON response: {exc}") from exc

    value = payload.get("value") if isinstance(payload, dict) else None
    if isinstance(value, dict):
        ready = value.get("ready")
        message = value.get("message")
    else:
        ready = payload.get("ready") if isinstance(payload, dict) else None
        message = payload.get("message") if isinstance(payload, dict) else None
    return {
        "status_url": status_url,
        "ready": ready,
        "message": str(message).strip() if message is not None else "",
        "payload": payload,
    }


def optional_desktop_doctor_checks(
    target: dict,
    *,
    which_fn: Callable[[str], str | None] | None = None,
    probe_webdriver_endpoint_fn: Callable[..., dict] = probe_webdriver_endpoint,
    desktop_check_fn: Callable[..., dict],
) -> list[dict]:
    checks: list[dict] = []
    which_fn = which_fn or shutil.which
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
