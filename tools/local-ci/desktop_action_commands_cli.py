"""Desktop automation action command orchestration for pulp ci-local."""

from __future__ import annotations

import argparse
from collections.abc import Callable
import json


VIDEO_PROOF_RECIPES = {
    "standalone-interaction",
    "reaper-plugin-editor",
    "inspector-workflow",
    "component-zoom",
    "design-parity",
}


def _print_lines(lines, *, print_fn: Callable[[str], None]) -> None:
    for line in lines:
        print_fn(line)


def _set_default(args: argparse.Namespace, name: str, value) -> None:
    if getattr(args, name, None) in {None, ""}:
        setattr(args, name, value)


def _apply_desktop_video_recipe(args: argparse.Namespace) -> None:
    recipe = getattr(args, "recipe", None)
    if not recipe:
        return
    if recipe not in VIDEO_PROOF_RECIPES:
        raise ValueError(f"unknown desktop video recipe `{recipe}`.")

    if recipe == "standalone-interaction":
        _set_default(args, "label", "standalone-interaction-proof")
        _set_default(args, "video_title", "Standalone UI interaction")
        if any([args.click, args.click_view_id, args.click_view_type, args.click_view_text, args.click_view_label]):
            args.capture_before = True
        return

    if recipe == "reaper-plugin-editor":
        plugin = getattr(args, "plugin", None)
        plugin_format = getattr(args, "plugin_format", None)
        if not plugin or not plugin_format:
            raise ValueError("recipe `reaper-plugin-editor` requires --plugin and --plugin-format.")
        _set_default(args, "host_app", "REAPER")
        _set_default(args, "bundle_id", "com.cockos.reaper")
        _set_default(args, "label", f"reaper-{plugin_format}-{plugin}-proof")
        _set_default(args, "video_title", f"{plugin} {plugin_format.upper()} editor in {args.host_app}")
        args.capture_before = True
        return

    if recipe == "inspector-workflow":
        args.action = "inspect"
        args.capture_ui_snapshot = True
        _set_default(args, "label", "inspector-workflow-proof")
        _set_default(args, "video_title", "Inspector workflow proof")
        return

    if recipe == "component-zoom":
        component_id = getattr(args, "component_id", None)
        if component_id and not getattr(args, "click_view_id", None):
            args.click_view_id = component_id
        if not any([args.click, args.click_view_id, args.click_view_type, args.click_view_text, args.click_view_label]):
            raise ValueError("recipe `component-zoom` requires --component-id or a click selector.")
        args.capture_ui_snapshot = True
        args.capture_before = True
        _set_default(args, "label", f"component-{args.click_view_id or component_id or 'zoom'}-proof")
        _set_default(args, "video_title", "Component validation")
        return

    if recipe == "design-parity":
        if not getattr(args, "source_image", None):
            raise ValueError("recipe `design-parity` requires --source-image.")
        args.action = "inspect"
        args.capture_ui_snapshot = True
        _set_default(args, "video_template", "design-parity")
        _set_default(args, "source_label", "Source reference")
        _set_default(args, "label", "design-parity-proof")
        _set_default(args, "video_title", "Design import parity")


def windows_requires_pulp_app_selectors(args: argparse.Namespace) -> bool:
    return any([args.click_view_id, args.click_view_type, args.click_view_text, args.click_view_label])


def _video_kwargs(args: argparse.Namespace) -> dict:
    audio_source = getattr(args, "video_audio", "none")
    if audio_source != "none":
        raise ValueError(
            f"video audio source `{audio_source}` is not implemented yet; use --video-audio none."
        )
    return {
        "record_video": bool(getattr(args, "record_video", False)),
        "video_duration_secs": float(getattr(args, "video_duration", 8.0)),
        "video_fps": float(getattr(args, "video_fps", 30.0)),
        "video_capture_target": getattr(args, "video_capture_target", "app"),
        "video_attachment_budget_bytes": int(float(getattr(args, "video_attachment_budget_mb", 100.0)) * 1_000_000),
        "compose_video_proof": bool(getattr(args, "compose_video_proof", False)),
        "video_template": getattr(args, "video_template", None),
        "video_source_image": getattr(args, "source_image", None),
        "video_source_label": getattr(args, "source_label", None),
        "video_title": getattr(args, "video_title", None),
    }


def cmd_desktop_video(
    args: argparse.Namespace,
    *,
    cmd_desktop_smoke_fn: Callable[[argparse.Namespace], int],
    cmd_desktop_click_fn: Callable[[argparse.Namespace], int],
    cmd_desktop_inspect_fn: Callable[[argparse.Namespace], int],
    print_fn: Callable[[str], None] = print,
) -> int:
    try:
        _apply_desktop_video_recipe(args)
    except ValueError as exc:
        print_fn(f"Error: {exc}")
        return 1

    audio_source = getattr(args, "video_audio", "none")
    if audio_source != "none":
        print_fn(f"Error: video audio source `{audio_source}` is not implemented yet; use --video-audio none.")
        return 1

    args.record_video = True
    if not getattr(args, "compose_video_proof", False):
        args.compose_video_proof = True

    action = getattr(args, "action", "click")
    if action == "smoke":
        return cmd_desktop_smoke_fn(args)
    if action == "click":
        return cmd_desktop_click_fn(args)
    if action == "inspect":
        return cmd_desktop_inspect_fn(args)
    print_fn(f"Error: unsupported desktop video action `{action}`.")
    return 1


def cmd_desktop_smoke(
    args: argparse.Namespace,
    *,
    load_config_fn: Callable[[], dict],
    resolve_desktop_target_fn: Callable[[dict, str], dict],
    make_desktop_source_request_fn: Callable[[argparse.Namespace], dict],
    run_macos_local_smoke_fn: Callable[..., dict],
    run_linux_xvfb_remote_action_fn: Callable[..., dict],
    run_windows_session_agent_action_fn: Callable[..., dict],
    desktop_action_success_lines_fn: Callable[[str, str, dict], list[str]],
    sys_platform: str,
    print_fn: Callable[[str], None] = print,
) -> int:
    try:
        config = load_config_fn()
        target = resolve_desktop_target_fn(config, args.target)
        source_request = make_desktop_source_request_fn(args)
    except (FileNotFoundError, ValueError) as exc:
        print_fn(f"Error: {exc}")
        return 1

    adapter = target["adapter"]
    if adapter == "macos-local":
        if sys_platform != "darwin":
            print_fn(f"Error: macOS local desktop smoke must run on macOS (current platform: {sys_platform}).")
            return 1
        if not args.launch_command and not args.bundle_id:
            print_fn("Error: desktop smoke requires either --command or --bundle-id.")
            return 1
        runner = lambda: run_macos_local_smoke_fn(
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
            **_video_kwargs(args),
        )
    elif adapter == "linux-xvfb":
        if getattr(args, "record_video", False):
            print_fn("Error: desktop video recording is not implemented for linux-xvfb targets yet.")
            return 1
        if args.bundle_id:
            print_fn("Error: linux-xvfb desktop smoke currently supports --command only.")
            return 1
        if not args.launch_command:
            print_fn("Error: desktop smoke requires --command for linux-xvfb targets.")
            return 1
        runner = lambda: run_linux_xvfb_remote_action_fn(
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
        )
    elif adapter == "windows-session-agent":
        if getattr(args, "record_video", False):
            print_fn("Error: desktop video recording is not implemented for windows-session-agent targets yet.")
            return 1
        if args.bundle_id:
            print_fn("Error: windows desktop smoke currently supports --command only.")
            return 1
        if not args.launch_command:
            print_fn("Error: desktop smoke requires --command for windows targets.")
            return 1
        pulp_app_automation = bool(getattr(args, "pulp_app_automation", False))
        if args.capture_ui_snapshot and not pulp_app_automation:
            print_fn("Error: windows desktop smoke currently supports --capture-ui-snapshot only with --pulp-app-automation.")
            return 1
        if windows_requires_pulp_app_selectors(args) and not pulp_app_automation:
            print_fn("Error: windows desktop smoke currently supports view-target selectors only with --pulp-app-automation.")
            return 1
        runner = lambda: run_windows_session_agent_action_fn(
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
        )
    else:
        print_fn(f"Error: desktop smoke is not implemented for `{args.target}` yet; adapter `{adapter}` is still pending.")
        return 1

    try:
        manifest = runner()
    except Exception as exc:
        print_fn(f"Error: {exc}")
        return 1

    if getattr(args, "json", False):
        print_fn(json.dumps(manifest, indent=2))
        return 0

    _print_lines(desktop_action_success_lines_fn("smoke", args.target, manifest), print_fn=print_fn)
    return 0


def cmd_desktop_click(
    args: argparse.Namespace,
    *,
    load_config_fn: Callable[[], dict],
    resolve_desktop_target_fn: Callable[[dict, str], dict],
    make_desktop_source_request_fn: Callable[[argparse.Namespace], dict],
    run_macos_local_smoke_fn: Callable[..., dict],
    run_linux_xvfb_remote_action_fn: Callable[..., dict],
    run_windows_session_agent_action_fn: Callable[..., dict],
    desktop_action_success_lines_fn: Callable[[str, str, dict], list[str]],
    sys_platform: str,
    print_fn: Callable[[str], None] = print,
) -> int:
    try:
        config = load_config_fn()
        target = resolve_desktop_target_fn(config, args.target)
        source_request = make_desktop_source_request_fn(args)
    except (FileNotFoundError, ValueError) as exc:
        print_fn(f"Error: {exc}")
        return 1

    adapter = target["adapter"]
    if adapter == "macos-local":
        if sys_platform != "darwin":
            print_fn(f"Error: macOS local desktop click must run on macOS (current platform: {sys_platform}).")
            return 1
        if bool(args.launch_command) == bool(args.bundle_id):
            print_fn("Error: desktop click requires exactly one of --command or --bundle-id.")
            return 1
        runner = lambda: run_macos_local_smoke_fn(
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
            **_video_kwargs(args),
        )
    elif adapter == "linux-xvfb":
        if getattr(args, "record_video", False):
            print_fn("Error: desktop video recording is not implemented for linux-xvfb targets yet.")
            return 1
        if args.bundle_id:
            print_fn("Error: linux-xvfb desktop click currently supports --command only.")
            return 1
        if not args.launch_command:
            print_fn("Error: desktop click requires --command for linux-xvfb targets.")
            return 1
        runner = lambda: run_linux_xvfb_remote_action_fn(
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
        )
    elif adapter == "windows-session-agent":
        if getattr(args, "record_video", False):
            print_fn("Error: desktop video recording is not implemented for windows-session-agent targets yet.")
            return 1
        if args.bundle_id:
            print_fn("Error: windows desktop click currently supports --command only.")
            return 1
        if not args.launch_command:
            print_fn("Error: desktop click requires --command for windows targets.")
            return 1
        pulp_app_automation = bool(getattr(args, "pulp_app_automation", False))
        if args.capture_ui_snapshot and not pulp_app_automation:
            print_fn("Error: windows desktop click currently supports --capture-ui-snapshot only with --pulp-app-automation.")
            return 1
        if windows_requires_pulp_app_selectors(args) and not pulp_app_automation:
            print_fn("Error: windows desktop click currently supports view-target selectors only with --pulp-app-automation.")
            return 1
        runner = lambda: run_windows_session_agent_action_fn(
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
        )
    else:
        print_fn(f"Error: desktop click is not implemented for `{args.target}` yet; adapter `{adapter}` is still pending.")
        return 1
    if not any([args.click, args.click_view_id, args.click_view_type, args.click_view_text, args.click_view_label]):
        print_fn("Error: desktop click requires --click or one view-target selector.")
        return 1

    try:
        manifest = runner()
    except Exception as exc:
        print_fn(f"Error: {exc}")
        return 1

    if getattr(args, "json", False):
        print_fn(json.dumps(manifest, indent=2))
        return 0

    _print_lines(desktop_action_success_lines_fn("click", args.target, manifest), print_fn=print_fn)
    return 0


def cmd_desktop_inspect(
    args: argparse.Namespace,
    *,
    load_config_fn: Callable[[], dict],
    resolve_desktop_target_fn: Callable[[dict, str], dict],
    make_desktop_source_request_fn: Callable[[argparse.Namespace], dict],
    run_macos_local_smoke_fn: Callable[..., dict],
    run_linux_xvfb_remote_action_fn: Callable[..., dict],
    run_windows_session_agent_action_fn: Callable[..., dict],
    desktop_action_success_lines_fn: Callable[[str, str, dict], list[str]],
    sys_platform: str,
    print_fn: Callable[[str], None] = print,
) -> int:
    try:
        config = load_config_fn()
        target = resolve_desktop_target_fn(config, args.target)
        source_request = make_desktop_source_request_fn(args)
    except (FileNotFoundError, ValueError) as exc:
        print_fn(f"Error: {exc}")
        return 1

    adapter = target["adapter"]
    if adapter == "macos-local":
        if sys_platform != "darwin":
            print_fn(f"Error: macOS local desktop inspect must run on macOS (current platform: {sys_platform}).")
            return 1
        if bool(args.launch_command) == bool(args.bundle_id):
            print_fn("Error: desktop inspect requires exactly one of --command or --bundle-id.")
            return 1
        capture_ui_snapshot = args.bundle_id is None
        runner = lambda: run_macos_local_smoke_fn(
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
            **_video_kwargs(args),
        )
    elif adapter == "linux-xvfb":
        if getattr(args, "record_video", False):
            print_fn("Error: desktop video recording is not implemented for linux-xvfb targets yet.")
            return 1
        if args.bundle_id:
            print_fn("Error: linux-xvfb desktop inspect currently supports --command only.")
            return 1
        if not args.launch_command:
            print_fn("Error: desktop inspect requires --command for linux-xvfb targets.")
            return 1
        runner = lambda: run_linux_xvfb_remote_action_fn(
            config,
            args.target,
            target,
            args.launch_command,
            action_name="inspect",
            label=args.label,
            output_path=args.output,
            pulp_app_automation=getattr(args, "pulp_app_automation", False),
            capture_ui_snapshot=bool(getattr(args, "pulp_app_automation", False)),
            click_point=None,
            click_view_id=None,
            click_view_type=None,
            click_view_text=None,
            click_view_label=None,
            capture_before=False,
            settle_secs=0.0,
            timeout_secs=args.timeout,
            source_request=source_request,
        )
    elif adapter == "windows-session-agent":
        if getattr(args, "record_video", False):
            print_fn("Error: desktop video recording is not implemented for windows-session-agent targets yet.")
            return 1
        if args.bundle_id:
            print_fn("Error: windows desktop inspect currently supports --command only.")
            return 1
        if not args.launch_command:
            print_fn("Error: desktop inspect requires --command for windows targets.")
            return 1
        pulp_app_automation = bool(getattr(args, "pulp_app_automation", False))
        runner = lambda: run_windows_session_agent_action_fn(
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
        )
    else:
        print_fn(f"Error: desktop inspect is not implemented for `{args.target}` yet; adapter `{adapter}` is still pending.")
        return 1

    try:
        manifest = runner()
    except Exception as exc:
        print_fn(f"Error: {exc}")
        return 1

    if getattr(args, "json", False):
        print_fn(json.dumps(manifest, indent=2))
        return 0

    _print_lines(desktop_action_success_lines_fn("inspect", args.target, manifest), print_fn=print_fn)
    return 0
