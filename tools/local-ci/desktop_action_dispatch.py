"""Desktop automation action runner selection."""

from __future__ import annotations

import argparse
from collections.abc import Callable


def windows_requires_pulp_app_selectors(args: argparse.Namespace) -> bool:
    return any([args.click_view_id, args.click_view_type, args.click_view_text, args.click_view_label])


def desktop_click_has_target(args: argparse.Namespace) -> bool:
    return any([args.click, args.click_view_id, args.click_view_type, args.click_view_text, args.click_view_label])


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
        return lambda: run_macos_local_smoke_fn(
            config,
            args.launch_command,
            action_name="smoke",
            bundle_id=args.bundle_id,
            label=args.label,
            output_path=args.output,
            capture_ui_snapshot=args.capture_ui_snapshot,
            click_point=args.click,
            click_view_id=args.click_view_id,
            click_view_type=args.click_view_type,
            click_view_text=args.click_view_text,
            click_view_label=args.click_view_label,
            pulp_app_automation=getattr(args, "pulp_app_automation", False),
            capture_before=args.capture_before,
            settle_secs=args.settle_secs,
            timeout_secs=args.timeout,
            source_request=source_request,
        ), None
    if adapter == "linux-xvfb":
        if args.bundle_id:
            return None, "linux-xvfb desktop smoke currently supports --command only."
        if not args.launch_command:
            return None, "desktop smoke requires --command for linux-xvfb targets."
        return lambda: run_linux_xvfb_remote_action_fn(
            config,
            args.target,
            target,
            args.launch_command,
            action_name="smoke",
            label=args.label,
            output_path=args.output,
            pulp_app_automation=getattr(args, "pulp_app_automation", False),
            capture_ui_snapshot=args.capture_ui_snapshot,
            click_point=args.click,
            click_view_id=args.click_view_id,
            click_view_type=args.click_view_type,
            click_view_text=args.click_view_text,
            click_view_label=args.click_view_label,
            capture_before=args.capture_before,
            settle_secs=args.settle_secs,
            timeout_secs=args.timeout,
            source_request=source_request,
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
        return lambda: run_windows_session_agent_action_fn(
            config,
            args.target,
            target,
            args.launch_command,
            action_name="smoke",
            label=args.label,
            output_path=args.output,
            pulp_app_automation=pulp_app_automation,
            capture_ui_snapshot=args.capture_ui_snapshot,
            click_point=args.click,
            click_view_id=args.click_view_id,
            click_view_type=args.click_view_type,
            click_view_text=args.click_view_text,
            click_view_label=args.click_view_label,
            capture_before=args.capture_before,
            settle_secs=args.settle_secs,
            timeout_secs=args.timeout,
            source_request=source_request,
        ), None
    return None, f"desktop smoke is not implemented for `{args.target}` yet; adapter `{adapter}` is still pending."


def desktop_click_runner(
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
            return None, f"macOS local desktop click must run on macOS (current platform: {sys_platform})."
        if bool(args.launch_command) == bool(args.bundle_id):
            return None, "desktop click requires exactly one of --command or --bundle-id."
        return lambda: run_macos_local_smoke_fn(
            config,
            args.launch_command,
            action_name="click",
            bundle_id=args.bundle_id,
            label=args.label,
            output_path=args.output,
            capture_ui_snapshot=args.capture_ui_snapshot,
            click_point=args.click,
            click_view_id=args.click_view_id,
            click_view_type=args.click_view_type,
            click_view_text=args.click_view_text,
            click_view_label=args.click_view_label,
            pulp_app_automation=getattr(args, "pulp_app_automation", False),
            capture_before=True,
            settle_secs=args.settle_secs,
            timeout_secs=args.timeout,
            source_request=source_request,
        ), None
    if adapter == "linux-xvfb":
        if args.bundle_id:
            return None, "linux-xvfb desktop click currently supports --command only."
        if not args.launch_command:
            return None, "desktop click requires --command for linux-xvfb targets."
        return lambda: run_linux_xvfb_remote_action_fn(
            config,
            args.target,
            target,
            args.launch_command,
            action_name="click",
            label=args.label,
            output_path=args.output,
            pulp_app_automation=getattr(args, "pulp_app_automation", False),
            capture_ui_snapshot=args.capture_ui_snapshot,
            click_point=args.click,
            click_view_id=args.click_view_id,
            click_view_type=args.click_view_type,
            click_view_text=args.click_view_text,
            click_view_label=args.click_view_label,
            capture_before=True,
            settle_secs=args.settle_secs,
            timeout_secs=args.timeout,
            source_request=source_request,
        ), None
    if adapter == "windows-session-agent":
        if args.bundle_id:
            return None, "windows desktop click currently supports --command only."
        if not args.launch_command:
            return None, "desktop click requires --command for windows targets."
        pulp_app_automation = bool(getattr(args, "pulp_app_automation", False))
        if args.capture_ui_snapshot and not pulp_app_automation:
            return None, "windows desktop click currently supports --capture-ui-snapshot only with --pulp-app-automation."
        if windows_requires_pulp_app_selectors(args) and not pulp_app_automation:
            return None, "windows desktop click currently supports view-target selectors only with --pulp-app-automation."
        return lambda: run_windows_session_agent_action_fn(
            config,
            args.target,
            target,
            args.launch_command,
            action_name="click",
            label=args.label,
            output_path=args.output,
            pulp_app_automation=pulp_app_automation,
            capture_ui_snapshot=args.capture_ui_snapshot,
            click_point=args.click,
            click_view_id=args.click_view_id,
            click_view_type=args.click_view_type,
            click_view_text=args.click_view_text,
            click_view_label=args.click_view_label,
            capture_before=True,
            settle_secs=args.settle_secs,
            timeout_secs=args.timeout,
            source_request=source_request,
        ), None
    return None, f"desktop click is not implemented for `{args.target}` yet; adapter `{adapter}` is still pending."


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
        return lambda: run_macos_local_smoke_fn(
            config,
            args.launch_command,
            action_name="inspect",
            bundle_id=args.bundle_id,
            label=args.label,
            output_path=args.output,
            capture_ui_snapshot=capture_ui_snapshot,
            click_point=None,
            click_view_id=None,
            click_view_type=None,
            click_view_text=None,
            click_view_label=None,
            pulp_app_automation=False,
            capture_before=False,
            settle_secs=0.0,
            timeout_secs=args.timeout,
            source_request=source_request,
        ), None
    if adapter == "linux-xvfb":
        if args.bundle_id:
            return None, "linux-xvfb desktop inspect currently supports --command only."
        if not args.launch_command:
            return None, "desktop inspect requires --command for linux-xvfb targets."
        pulp_app_automation = bool(getattr(args, "pulp_app_automation", False))
        return lambda: run_linux_xvfb_remote_action_fn(
            config,
            args.target,
            target,
            args.launch_command,
            action_name="inspect",
            label=args.label,
            output_path=args.output,
            pulp_app_automation=pulp_app_automation,
            capture_ui_snapshot=pulp_app_automation,
            click_point=None,
            click_view_id=None,
            click_view_type=None,
            click_view_text=None,
            click_view_label=None,
            capture_before=False,
            settle_secs=0.0,
            timeout_secs=args.timeout,
            source_request=source_request,
        ), None
    if adapter == "windows-session-agent":
        if args.bundle_id:
            return None, "windows desktop inspect currently supports --command only."
        if not args.launch_command:
            return None, "desktop inspect requires --command for windows targets."
        pulp_app_automation = bool(getattr(args, "pulp_app_automation", False))
        return lambda: run_windows_session_agent_action_fn(
            config,
            args.target,
            target,
            args.launch_command,
            action_name="inspect",
            label=args.label,
            output_path=args.output,
            pulp_app_automation=pulp_app_automation,
            capture_ui_snapshot=pulp_app_automation,
            click_point=None,
            click_view_id=None,
            click_view_type=None,
            click_view_text=None,
            click_view_label=None,
            capture_before=False,
            settle_secs=0.0,
            timeout_secs=args.timeout,
            source_request=source_request,
        ), None
    return None, f"desktop inspect is not implemented for `{args.target}` yet; adapter `{adapter}` is still pending."
