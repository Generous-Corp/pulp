"""Shared preflight policy for remote desktop action runners."""

from __future__ import annotations

from collections.abc import Callable


def resolve_remote_desktop_action_host(
    config: dict,
    target_name: str,
    target: dict,
    *,
    ensure_host_reachable_fn: Callable[[str, dict, dict], str | None],
) -> tuple[str, str]:
    host = ensure_host_reachable_fn(target_name, target, config.get("defaults", {}))
    if not host:
        raise RuntimeError(f"Desktop target `{target_name}` is not reachable over SSH.")
    repo_path = target.get("repo_path")
    if not repo_path:
        raise RuntimeError(f"Desktop target `{target_name}` is missing repo_path.")
    return host, repo_path


def require_pulp_app_automation_for_remote_view_options(
    *,
    target_name: str,
    pulp_app_automation: bool,
    capture_ui_snapshot: bool,
    click_view_id: str | None,
    click_view_type: str | None,
    click_view_text: str | None,
    click_view_label: str | None,
    snapshot_error: str,
    selector_error: str,
) -> None:
    if pulp_app_automation:
        return
    if capture_ui_snapshot:
        raise RuntimeError(snapshot_error.format(target_name=target_name))
    if any([click_view_id, click_view_type, click_view_text, click_view_label]):
        raise RuntimeError(selector_error.format(target_name=target_name))
