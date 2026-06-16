"""Desktop automation smoke runner selection."""

from __future__ import annotations

import argparse
from collections.abc import Callable

from desktop_action_invocation import (
    linux_desktop_action_runner,
    macos_desktop_action_runner,
    windows_desktop_action_runner,
)
from desktop_action_selectors import windows_requires_pulp_app_selectors


def desktop_smoke_runner(
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
            return None, f"macOS local desktop smoke must run on macOS (current platform: {sys_platform})."
        if not args.launch_command and not args.bundle_id:
            return None, "desktop smoke requires either --command or --bundle-id."
        return macos_desktop_action_runner(
            args=args,
            config=config,
            source_request=source_request,
            action_name="smoke",
            pulp_app_automation=getattr(args, "pulp_app_automation", False),
            capture_ui_snapshot=args.capture_ui_snapshot,
            click_point=args.click,
            click_view_id=args.click_view_id,
            click_view_type=args.click_view_type,
            click_view_text=args.click_view_text,
            click_view_label=args.click_view_label,
            capture_before=args.capture_before,
            settle_secs=args.settle_secs,
            run_macos_local_smoke_fn=run_macos_local_smoke_fn,
        ), None
    if adapter == "linux-xvfb":
        if args.bundle_id:
            return None, "linux-xvfb desktop smoke currently supports --command only."
        if not args.launch_command:
            return None, "desktop smoke requires --command for linux-xvfb targets."
        return linux_desktop_action_runner(
            args=args,
            config=config,
            target=target,
            source_request=source_request,
            action_name="smoke",
            pulp_app_automation=getattr(args, "pulp_app_automation", False),
            capture_ui_snapshot=args.capture_ui_snapshot,
            click_point=args.click,
            click_view_id=args.click_view_id,
            click_view_type=args.click_view_type,
            click_view_text=args.click_view_text,
            click_view_label=args.click_view_label,
            capture_before=args.capture_before,
            settle_secs=args.settle_secs,
            run_linux_xvfb_remote_action_fn=run_linux_xvfb_remote_action_fn,
        ), None
    if adapter == "windows-session-agent":
        if args.bundle_id:
            return None, "windows desktop smoke currently supports --command only."
        if not args.launch_command:
            return None, "desktop smoke requires --command for windows targets."
        pulp_app_automation = bool(getattr(args, "pulp_app_automation", False))
        if args.capture_ui_snapshot and not pulp_app_automation:
            return None, "windows desktop smoke currently supports --capture-ui-snapshot only with --pulp-app-automation."
        if windows_requires_pulp_app_selectors(args) and not pulp_app_automation:
            return None, "windows desktop smoke currently supports view-target selectors only with --pulp-app-automation."
        return windows_desktop_action_runner(
            args=args,
            config=config,
            target=target,
            source_request=source_request,
            action_name="smoke",
            pulp_app_automation=pulp_app_automation,
            capture_ui_snapshot=args.capture_ui_snapshot,
            click_point=args.click,
            click_view_id=args.click_view_id,
            click_view_type=args.click_view_type,
            click_view_text=args.click_view_text,
            click_view_label=args.click_view_label,
            capture_before=args.capture_before,
            settle_secs=args.settle_secs,
            run_windows_session_agent_action_fn=run_windows_session_agent_action_fn,
        ), None
    return None, f"desktop smoke is not implemented for `{args.target}` yet; adapter `{adapter}` is still pending."
