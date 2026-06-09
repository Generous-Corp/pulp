"""Windows desktop target helpers for local CI."""

from __future__ import annotations

from pathlib import PureWindowsPath
from typing import Callable
import uuid


WINDOWS_REQUIRED_REMOTE_TOOLS = {
    "git": {"winget_id": "Git.Git", "required": True},
}
WINDOWS_OPTIONAL_REMOTE_TOOLS = {
    "gh": {"winget_id": "GitHub.cli", "required": False},
}
WINDOWS_DEFAULT_REMOTE_REPO_DIRNAME = "pulp-validate"


def default_windows_session_task_name(target_name: str) -> str:
    cleaned = "".join(ch if ch.isalnum() else "-" for ch in target_name.strip())
    cleaned = cleaned.strip("-") or "windows"
    return f"PulpDesktopAutomationAgent-{cleaned}"


def desktop_target_contract(target_name: str, target: dict) -> dict:
    adapter = target.get("adapter")
    if adapter == "windows-session-agent":
        remote_root = target.get("remote_root") or r"%LOCALAPPDATA%\Pulp\desktop-automation-agent"
        task_name = target.get("task_name") or default_windows_session_task_name(target_name)
        return {
            "kind": "windows-session-agent",
            "task_name": task_name,
            "remote_root": remote_root,
            "jobs_dir": remote_root + r"\jobs",
            "results_dir": remote_root + r"\results",
            "logs_dir": remote_root + r"\logs",
            "script_path": remote_root + r"\agent.ps1",
        }
    return {}


def windows_path_join(*parts: str) -> str:
    cleaned: list[str] = []
    for index, part in enumerate(parts):
        if not part:
            continue
        piece = str(part)
        if index == 0:
            cleaned.append(piece.rstrip("\\"))
        else:
            cleaned.append(piece.strip("\\"))
    return "\\".join(cleaned)


def windows_default_repo_checkout_path(home_dir: str | None) -> str:
    home = (home_dir or "").strip()
    if not home:
        return WINDOWS_DEFAULT_REMOTE_REPO_DIRNAME
    return windows_path_join(home, WINDOWS_DEFAULT_REMOTE_REPO_DIRNAME)


def windows_repo_path_is_unsafe(repo_path: str | None, home_dir: str | None = None) -> bool:
    value = (repo_path or "").strip()
    if not value:
        return True
    repo = PureWindowsPath(value)
    repo_text = str(repo).rstrip("\\")
    anchor = repo.anchor.rstrip("\\")
    if not repo_text or (anchor and repo_text.lower() == anchor.lower()):
        return True

    home_value = (home_dir or "").strip()
    if home_value:
        home = PureWindowsPath(home_value)
        home_text = str(home).rstrip("\\")
        if home_text and repo_text.lower() == home_text.lower():
            return True
    return False


def update_target_repo_path(config: dict, target_name: str, repo_path: str) -> None:
    config.setdefault("targets", {}).setdefault(target_name, {})["repo_path"] = repo_path
    desktop = config.setdefault("desktop_automation", {})
    desktop_targets = desktop.setdefault("targets", {})
    desktop_targets.setdefault(target_name, {})["repo_path"] = repo_path


def windows_repo_checkout_ready(probe: dict | None) -> bool:
    if not probe:
        return False
    return (
        bool(probe.get("git_dir_exists"))
        and bool(probe.get("head_exists"))
        and bool(probe.get("setup_exists"))
        and not bool(probe.get("repo_path_unsafe"))
    )


def build_windows_session_agent_request(
    target_name: str,
    contract: dict,
    command: str,
    *,
    repo_path: str,
    action_name: str,
    label: str | None,
    pulp_app_automation: bool,
    capture_ui_snapshot: bool,
    click_point: str | None,
    click_view_id: str | None,
    click_view_type: str | None,
    click_view_text: str | None,
    click_view_label: str | None,
    capture_before: bool,
    settle_secs: float,
    timeout_secs: float,
    default_desktop_label_fn: Callable[[str | None], str],
    uuid_hex_fn: Callable[[], str] | None = None,
) -> dict:
    job_id = uuid_hex_fn() if uuid_hex_fn is not None else uuid.uuid4().hex
    result_root = windows_path_join(contract["results_dir"], job_id)
    screenshot_path = windows_path_join(result_root, "screenshots", "window.png")
    request = {
        "schema": 1,
        "job_id": job_id,
        "target": target_name,
        "action": action_name,
        "label": label or default_desktop_label_fn(command),
        "command": command,
        "cwd": repo_path,
        "timeout_secs": timeout_secs,
        "settle_secs": settle_secs,
        "outputs": {
            "result_root": result_root,
            "screenshot": screenshot_path,
            "stdout": windows_path_join(result_root, "stdout.log"),
            "stderr": windows_path_join(result_root, "stderr.log"),
            "manifest": windows_path_join(result_root, "manifest.json"),
        },
        "execution": {
            "capture_mode": "pulp-app" if pulp_app_automation else "window-capture",
            "capture_ui_snapshot": bool(capture_ui_snapshot),
            "capture_before": bool(capture_before),
        },
        "interaction": {
            "click_point": click_point,
            "view_id": click_view_id,
            "view_type": click_view_type,
            "view_text": click_view_text,
            "view_label": click_view_label,
        },
        "env": {
            "PULP_AUTOMATION_AFTER_OUT": screenshot_path,
            "PULP_AUTOMATION_DELAY_MS": "1000",
            "PULP_AUTOMATION_AFTER_DELAY_MS": str(max(0, int(settle_secs * 1000.0))),
            "PULP_AUTOMATION_EXIT_AFTER": "1",
        },
    }
    if capture_ui_snapshot:
        request["outputs"]["ui_snapshot"] = windows_path_join(result_root, "ui-tree.json")
        request["env"]["PULP_VIEW_TREE_OUT"] = request["outputs"]["ui_snapshot"]
    if capture_before:
        request["outputs"]["before_screenshot"] = windows_path_join(result_root, "screenshots", "before.png")
        request["env"]["PULP_AUTOMATION_BEFORE_OUT"] = request["outputs"]["before_screenshot"]
    if click_point:
        request["env"]["PULP_AUTOMATION_CLICK_POINT"] = click_point
    if click_view_id:
        request["env"]["PULP_AUTOMATION_CLICK_VIEW_ID"] = click_view_id
    if click_view_type:
        request["env"]["PULP_AUTOMATION_CLICK_VIEW_TYPE"] = click_view_type
    if click_view_text:
        request["env"]["PULP_AUTOMATION_CLICK_VIEW_TEXT"] = click_view_text
    if click_view_label:
        request["env"]["PULP_AUTOMATION_CLICK_VIEW_LABEL"] = click_view_label
    return request


def windows_tooling_detail(probe: dict, tool_name: str, *, missing_hint: str | None = None) -> str:
    if probe.get(f"{tool_name}_found"):
        version = (probe.get(f"{tool_name}_version") or "").strip()
        path = probe.get(f"{tool_name}_path") or tool_name
        return f"{version} ({path})" if version else path
    if missing_hint:
        return missing_hint
    return "missing"


def windows_remote_tooling_ready(probe: dict, *, required_tools: dict | None = None) -> bool:
    tools = required_tools if required_tools is not None else WINDOWS_REQUIRED_REMOTE_TOOLS
    return all(bool(probe.get(f"{tool_name}_found")) for tool_name in tools)


def windows_desktop_session_user(probe: dict | None) -> str:
    if not probe:
        return ""
    return str(probe.get("interactive_user") or probe.get("logged_on_user") or "").strip()


def windows_desktop_session_state(probe: dict | None) -> str:
    if not probe:
        return ""
    return str(probe.get("session_state") or "").strip()


def windows_repo_checkout_detail(probe: dict | None, *, fallback_path: str | None = None) -> str:
    if not probe:
        return fallback_path or "missing"
    repo_path = str(probe.get("repo_path") or fallback_path or "").strip() or "missing"
    origin_url = str(probe.get("origin_url") or "").strip()
    detail = f"{repo_path} ({origin_url})" if origin_url else repo_path
    notes: list[str] = []
    if probe.get("repo_exists") and not probe.get("git_dir_exists"):
        notes.append("not a git checkout")
    elif probe.get("git_dir_exists") and not probe.get("head_exists"):
        notes.append("empty git repo")
    elif probe.get("git_dir_exists") and not probe.get("setup_exists"):
        notes.append("checkout incomplete; setup.sh missing")
    if notes:
        detail = f"{detail}; {'; '.join(notes)}"
    return detail
