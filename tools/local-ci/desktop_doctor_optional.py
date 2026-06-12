"""Optional desktop-doctor capability and WebDriver probe helpers."""

from __future__ import annotations

from collections.abc import Callable
import json
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
