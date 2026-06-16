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
        **_macos_video_kwargs(args),
    )


def _macos_video_kwargs(args: argparse.Namespace) -> dict:
    """Map parsed desktop-video flags onto run_macos_local_smoke kwargs.

    The video flags only exist on the smoke/click parsers (added by
    cli_parser_video.add_desktop_video_args), so each is read via getattr with
    the run_macos_local_smoke default — non-video and non-macOS invocations are
    unaffected. Flag dests differ from the run params (MB budgets convert to
    bytes; --duration → video_duration_secs; --video-note → video_notes;
    --video-audio → video_audio_source; --source-image/-label → video_source_*).
    """
    notes = getattr(args, "video_note", None)
    return {
        "record_video": getattr(args, "record_video", False),
        "video_duration_secs": getattr(args, "video_duration", 8.0),
        "video_fps": getattr(args, "video_fps", 30.0),
        "video_audio_source": getattr(args, "video_audio", "none"),
        "video_audio_file": getattr(args, "video_audio_file", None),
        "video_audio_device": getattr(args, "video_audio_device", None),
        "video_recorder": getattr(args, "video_recorder", "auto"),
        "video_focus": getattr(args, "video_focus", "auto"),
        "video_capture_target": getattr(args, "video_capture_target", "app"),
        "capture_bundle_id": getattr(args, "capture_bundle_id", None),
        "video_attachment_budget_bytes": int(getattr(args, "video_attachment_budget_mb", 100.0) * 1_000_000),
        "small_video": getattr(args, "small_video", False),
        "small_video_budget_bytes": int(getattr(args, "small_video_budget_mb", 10.0) * 1_000_000),
        "compose_video_proof": getattr(args, "compose_video_proof", False),
        "video_template": getattr(args, "video_template", None),
        "video_source_image": getattr(args, "source_image", None),
        "video_source_label": getattr(args, "source_label", None),
        "video_title": getattr(args, "video_title", None),
        "video_notes": list(notes) if notes else None,
        "video_context": getattr(args, "video_context", None),
    }


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
