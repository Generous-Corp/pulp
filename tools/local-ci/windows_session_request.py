"""Windows session-agent contract and request helpers."""

from __future__ import annotations

from collections.abc import Callable
import uuid

from windows_target_paths import windows_path_join


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
