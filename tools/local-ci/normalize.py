"""Config/value normalization helpers for local CI.

Extracted from local_ci.py to give the desktop-automation, queue, and
target subsystems a stable seam they can reuse without importing the
11k-line orchestrator. Pure functions only — nothing here touches disk
or subprocess.

Boolean parsing is intentionally permissive (true/yes/on/1, false/no/off/0,
empty string == False) so config files written by hand stay forgiving.
The desktop-automation block normalization is the one heavy entry point;
everything else is single-purpose.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

PRIORITY_VALUES = {"low": 10, "normal": 50, "high": 100}


def normalize_priority(priority: str | None) -> str:
    value = (priority or "normal").strip().lower()
    if value not in PRIORITY_VALUES:
        raise ValueError(f"Invalid priority '{priority}'. Use one of: low, normal, high.")
    return value


def priority_value(priority: str | None) -> int:
    return PRIORITY_VALUES[normalize_priority(priority)]


def normalize_validation_mode(mode: str | None) -> str:
    value = (mode or "full").strip().lower()
    if value not in {"full", "smoke"}:
        raise ValueError(f"Invalid validation mode '{mode}'. Use one of: full, smoke.")
    return value


def normalize_desktop_source_mode(mode: str | None) -> str:
    value = (mode or "live").strip().lower().replace("_", "-")
    if value not in {"live", "exact-sha"}:
        raise ValueError(f"Invalid desktop source mode '{mode}'. Use one of: live, exact-sha.")
    return value


def default_desktop_artifact_root() -> Path:
    override = os.environ.get("PULP_DESKTOP_ARTIFACT_ROOT")
    if override:
        return Path(override).expanduser()

    home = Path.home()
    if sys.platform == "darwin":
        return home / "Library" / "Application Support" / "Pulp" / "desktop-automation" / "runs"
    if sys.platform == "win32":
        local_appdata = os.environ.get("LOCALAPPDATA")
        if local_appdata:
            return Path(local_appdata) / "Pulp" / "desktop-automation" / "runs"
    xdg_state = os.environ.get("XDG_STATE_HOME")
    if xdg_state:
        return Path(xdg_state).expanduser() / "pulp" / "desktop-automation" / "runs"
    return home / ".local" / "state" / "pulp" / "desktop-automation" / "runs"


def normalize_publish_mode(mode: str | None) -> str:
    value = (mode or "none").strip().lower()
    if value not in {"none", "branch", "pr-comment", "issue-comment"}:
        raise ValueError(
            f"Invalid desktop publish mode '{mode}'. Use one of: none, branch, pr-comment, issue-comment."
        )
    return value


def parse_config_bool(value: object) -> bool:
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)):
        return bool(value)
    text = str(value or "").strip().lower()
    if text in {"1", "true", "yes", "on"}:
        return True
    if text in {"0", "false", "no", "off", ""}:
        return False
    raise ValueError(f"Invalid boolean value '{value}'. Use true/false, yes/no, or 1/0.")


def normalize_desktop_optional_config(optional_cfg: dict | None) -> dict:
    optional = dict(optional_cfg or {})
    return {
        "webview_driver": parse_config_bool(optional.get("webview_driver", False)),
        "webdriver_url": str(optional.get("webdriver_url") or "").strip(),
        "debug_attach": parse_config_bool(optional.get("debug_attach", False)),
        "debugger_command": str(optional.get("debugger_command") or "").strip(),
        "video_capture": parse_config_bool(optional.get("video_capture", False)),
        "frame_stats": parse_config_bool(optional.get("frame_stats", False)),
    }


def infer_desktop_adapter(name: str, target_cfg: dict) -> str:
    target_type = target_cfg.get("type")
    if name == "mac" and target_type == "local":
        return "macos-local"
    if name == "ubuntu":
        return "linux-xvfb"
    if name == "windows":
        return "windows-session-agent"
    if target_type == "local":
        return "local-window"
    if target_type == "ssh":
        return "remote-session-agent"
    return "unknown"


def default_desktop_bootstrap(adapter: str) -> str:
    return {
        "macos-local": "launchagent",
        "linux-xvfb": "xvfb-run",
        "windows-session-agent": "scheduled-task",
        "local-window": "local-process",
        "remote-session-agent": "ssh-bootstrap",
    }.get(adapter, "manual")


def default_desktop_capability_tier(adapter: str) -> str:
    return {
        "macos-local": "v2",
        "linux-xvfb": "v2",
        "windows-session-agent": "v2",
    }.get(adapter, "v1")


def normalize_desktop_config(config: dict) -> dict:
    normalized = dict(config)
    desktop = dict(normalized.get("desktop_automation", {}))
    desktop["artifact_root"] = str(
        Path(desktop.get("artifact_root") or default_desktop_artifact_root()).expanduser()
    )
    desktop["publish_mode"] = normalize_publish_mode(desktop.get("publish_mode", "none"))
    desktop["publish_branch"] = desktop.get("publish_branch", "dev-artifacts")
    desktop["retention_days"] = int(desktop.get("retention_days", 14))

    target_overrides = desktop.get("targets", {})
    normalized_targets = {}
    for name, target_cfg in normalized.get("targets", {}).items():
        override = dict(target_overrides.get(name, {}))
        adapter = override.get("adapter") or infer_desktop_adapter(name, target_cfg)
        normalized_targets[name] = {
            "enabled": bool(override.get("enabled", target_cfg.get("enabled", True))),
            "adapter": adapter,
            "bootstrap": override.get("bootstrap", default_desktop_bootstrap(adapter)),
            "capability_tier": override.get("capability_tier", default_desktop_capability_tier(adapter)),
            "host": override.get("host", target_cfg.get("host")),
            "repo_path": override.get("repo_path", target_cfg.get("repo_path")),
            "target_type": target_cfg.get("type", "unknown"),
            "task_name": override.get("task_name"),
            "remote_root": override.get("remote_root"),
            "optional": normalize_desktop_optional_config(override.get("optional")),
        }
    desktop["targets"] = normalized_targets
    normalized["desktop_automation"] = desktop
    return normalized
