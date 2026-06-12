"""Desktop automation platform runner invocation builders."""

from __future__ import annotations

import argparse
from collections.abc import Callable


def macos_desktop_action_runner(
    *,
    args: argparse.Namespace,
    config: dict,
    source_request: dict,
    action_name: str,
    pulp_app_automation: bool,
    capture_ui_snapshot: bool,
    click_point: str | None,
    click_view_id: str | None,
    click_view_type: str | None,
    click_view_text: str | None,
    click_view_label: str | None,
    capture_before: bool,
    settle_secs: float,
    run_macos_local_smoke_fn: Callable[..., dict],
) -> Callable[[], dict]:
    return lambda: run_macos_local_smoke_fn(
        config,
        args.launch_command,
        action_name=action_name,
        bundle_id=args.bundle_id,
        label=args.label,
        output_path=args.output,
        capture_ui_snapshot=capture_ui_snapshot,
        click_point=click_point,
        click_view_id=click_view_id,
        click_view_type=click_view_type,
        click_view_text=click_view_text,
        click_view_label=click_view_label,
        pulp_app_automation=pulp_app_automation,
        capture_before=capture_before,
        settle_secs=settle_secs,
        timeout_secs=args.timeout,
        source_request=source_request,
    )


def linux_desktop_action_runner(
    *,
    args: argparse.Namespace,
    config: dict,
    target: dict,
    source_request: dict,
    action_name: str,
    pulp_app_automation: bool,
    capture_ui_snapshot: bool,
    click_point: str | None,
    click_view_id: str | None,
    click_view_type: str | None,
    click_view_text: str | None,
    click_view_label: str | None,
    capture_before: bool,
    settle_secs: float,
    run_linux_xvfb_remote_action_fn: Callable[..., dict],
) -> Callable[[], dict]:
    return lambda: run_linux_xvfb_remote_action_fn(
        config,
        args.target,
        target,
        args.launch_command,
        action_name=action_name,
        label=args.label,
        output_path=args.output,
        pulp_app_automation=pulp_app_automation,
        capture_ui_snapshot=capture_ui_snapshot,
        click_point=click_point,
        click_view_id=click_view_id,
        click_view_type=click_view_type,
        click_view_text=click_view_text,
        click_view_label=click_view_label,
        capture_before=capture_before,
        settle_secs=settle_secs,
        timeout_secs=args.timeout,
        source_request=source_request,
    )


def windows_desktop_action_runner(
    *,
    args: argparse.Namespace,
    config: dict,
    target: dict,
    source_request: dict,
    action_name: str,
    pulp_app_automation: bool,
    capture_ui_snapshot: bool,
    click_point: str | None,
    click_view_id: str | None,
    click_view_type: str | None,
    click_view_text: str | None,
    click_view_label: str | None,
    capture_before: bool,
    settle_secs: float,
    run_windows_session_agent_action_fn: Callable[..., dict],
) -> Callable[[], dict]:
    return lambda: run_windows_session_agent_action_fn(
        config,
        args.target,
        target,
        args.launch_command,
        action_name=action_name,
        label=args.label,
        output_path=args.output,
        pulp_app_automation=pulp_app_automation,
        capture_ui_snapshot=capture_ui_snapshot,
        click_point=click_point,
        click_view_id=click_view_id,
        click_view_type=click_view_type,
        click_view_text=click_view_text,
        click_view_label=click_view_label,
        capture_before=capture_before,
        settle_secs=settle_secs,
        timeout_secs=args.timeout,
        source_request=source_request,
    )
