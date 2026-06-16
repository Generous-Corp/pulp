"""Desktop automation inspect runner selection."""

from __future__ import annotations

import argparse
from collections.abc import Callable

from desktop_action_invocation import (
    linux_desktop_action_runner,
    macos_desktop_action_runner,
    windows_desktop_action_runner,
)


def desktop_inspect_runner(
    *,
    args: argparse.Namespace,
    config: dict,
    target: dict,
    source_request: dict,
    run_macos_local_smoke_fn: Callable[..., dict],
    run_linux_xvfb_remote_action_fn: Callable[..., dict],
    run_windows_session_agent_action_fn: Callable[..., dict],
    sys_platform: str,
) -> tuple[Callable[[], dict] | None, str | None]:
    adapter = target["adapter"]
    if adapter == "macos-local":
        if sys_platform != "darwin":
            return None, f"macOS local desktop inspect must run on macOS (current platform: {sys_platform})."
        if bool(args.launch_command) == bool(args.bundle_id):
            return None, "desktop inspect requires exactly one of --command or --bundle-id."
        capture_ui_snapshot = args.bundle_id is None
        return macos_desktop_action_runner(
            args=args,
            config=config,
            source_request=source_request,
            action_name="inspect",
            pulp_app_automation=False,
            capture_ui_snapshot=capture_ui_snapshot,
            click_point=None,
            click_view_id=None,
            click_view_type=None,
            click_view_text=None,
            click_view_label=None,
            capture_before=False,
            settle_secs=0.0,
            run_macos_local_smoke_fn=run_macos_local_smoke_fn,
        ), None
    if adapter == "linux-xvfb":
        if args.bundle_id:
            return None, "linux-xvfb desktop inspect currently supports --command only."
        if not args.launch_command:
            return None, "desktop inspect requires --command for linux-xvfb targets."
        pulp_app_automation = bool(getattr(args, "pulp_app_automation", False))
        return linux_desktop_action_runner(
            args=args,
            config=config,
            target=target,
            source_request=source_request,
            action_name="inspect",
            pulp_app_automation=pulp_app_automation,
            capture_ui_snapshot=pulp_app_automation,
            click_point=None,
            click_view_id=None,
            click_view_type=None,
            click_view_text=None,
            click_view_label=None,
            capture_before=False,
            settle_secs=0.0,
            run_linux_xvfb_remote_action_fn=run_linux_xvfb_remote_action_fn,
        ), None
    if adapter == "windows-session-agent":
        if args.bundle_id:
            return None, "windows desktop inspect currently supports --command only."
        if not args.launch_command:
            return None, "desktop inspect requires --command for windows targets."
        pulp_app_automation = bool(getattr(args, "pulp_app_automation", False))
        return windows_desktop_action_runner(
            args=args,
            config=config,
            target=target,
            source_request=source_request,
            action_name="inspect",
            pulp_app_automation=pulp_app_automation,
            capture_ui_snapshot=pulp_app_automation,
            click_point=None,
            click_view_id=None,
            click_view_type=None,
            click_view_text=None,
            click_view_label=None,
            capture_before=False,
            settle_secs=0.0,
            run_windows_session_agent_action_fn=run_windows_session_agent_action_fn,
        ), None
    return None, f"desktop inspect is not implemented for `{args.target}` yet; adapter `{adapter}` is still pending."
