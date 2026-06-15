"""Windows target probe readiness and display helpers."""

from __future__ import annotations


WINDOWS_REQUIRED_REMOTE_TOOLS = {
    "git": {"winget_id": "Git.Git", "required": True},
}
WINDOWS_OPTIONAL_REMOTE_TOOLS = {
    "gh": {"winget_id": "GitHub.cli", "required": False},
}


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
