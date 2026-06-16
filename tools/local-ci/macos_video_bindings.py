"""Facade dependency bindings for macOS desktop video-proof helpers.

Installs the video recording / focus / compose / terminal-proof / window-select
helpers into the local_ci facade by name, threading each underlying function's
injected dependencies (subprocess, time, ffmpeg path, sibling helpers) from the
bindings dict. Re-homed from the branch local_ci.py video wrappers.
"""

from __future__ import annotations

from pathlib import Path
from typing import Any

from binding_utils import binding as _binding
from binding_utils import binding_attr as _binding_attr
from binding_utils import install_local_helpers


MACOS_VIDEO_EXPORTS = (
    "wait_for_macos_bundle_window_title",
    "wait_for_macos_bundle_secondary_window",
    "launch_macos_terminal_proof_command",
    "close_macos_terminal_windows_with_title",
    "probe_macos_screencapture",
    "probe_macos_avfoundation_screen",
    "probe_macos_avfoundation_audio",
    "resolve_ffmpeg_path",
    "desktop_video_metadata",
    "write_desktop_video_metadata",
    "compose_desktop_video_proof",
    "video_proof_smoke",
    "mux_desktop_video_audio",
    "generate_interaction_focus",
    "create_issue_video_variant",
    "start_macos_window_video_recording",
    "stop_macos_window_video_recording",
)


def wait_for_macos_bundle_window_title(
    bindings: dict, bundle_id: str, title_contains: str, timeout_secs: float
) -> tuple[int, dict]:
    return _binding(bindings, "_macos_window_video_select").wait_for_macos_bundle_window_title(
        bundle_id,
        title_contains,
        timeout_secs,
        macos_window_info_for_bundle_id_fn=_binding(bindings, "macos_window_info_for_bundle_id"),
        activate_macos_bundle_id_fn=_binding(bindings, "activate_macos_bundle_id"),
        time_fn=_binding_attr(bindings, "time", "time"),
        sleep_fn=_binding_attr(bindings, "time", "sleep"),
    )


def wait_for_macos_bundle_secondary_window(bindings: dict, bundle_id: str, timeout_secs: float) -> tuple[int, dict]:
    return _binding(bindings, "_macos_window_video_select").wait_for_macos_bundle_secondary_window(
        bundle_id,
        timeout_secs,
        macos_window_info_for_bundle_id_fn=_binding(bindings, "macos_window_info_for_bundle_id"),
        activate_macos_bundle_id_fn=_binding(bindings, "activate_macos_bundle_id"),
        time_fn=_binding_attr(bindings, "time", "time"),
        sleep_fn=_binding_attr(bindings, "time", "sleep"),
    )


def launch_macos_terminal_proof_command(
    bindings: dict,
    command_args: list[str],
    *,
    cwd: Path,
    title: str,
    stdout_path: Path,
    stderr_path: Path,
    returncode_path: Path,
    keepalive_secs: float,
) -> dict:
    return _binding(bindings, "_macos_terminal_proof").launch_macos_terminal_proof_command(
        command_args,
        cwd=cwd,
        title=title,
        stdout_path=stdout_path,
        stderr_path=stderr_path,
        returncode_path=returncode_path,
        keepalive_secs=keepalive_secs,
        run_fn=_binding_attr(bindings, "subprocess", "run"),
    )


def close_macos_terminal_windows_with_title(bindings: dict, title_contains: str) -> dict:
    return _binding(bindings, "_macos_terminal_runner").close_terminal_windows_with_title(
        title_contains,
        run_fn=_binding_attr(bindings, "subprocess", "run"),
    )


def probe_macos_screencapture(bindings: dict) -> tuple[bool, str]:
    return _binding(bindings, "_macos_desktop_video").probe_macos_screencapture(
        run_fn=_binding_attr(bindings, "subprocess", "run"),
    )


def probe_macos_avfoundation_screen(bindings: dict, ffmpeg_path: str | None = None) -> tuple[bool, str]:
    module = _binding(bindings, "_macos_desktop_video")
    resolve = _binding(bindings, "resolve_ffmpeg_path")
    try:
        device = module.macos_avfoundation_screen_input_device(
            ffmpeg_path=ffmpeg_path or resolve(),
            run_fn=_binding_attr(bindings, "subprocess", "run"),
        )
        return True, f"Capture screen 0 ({device})"
    except RuntimeError as exc:
        return False, str(exc)


def probe_macos_avfoundation_audio(
    bindings: dict, audio_device: str | None = None, ffmpeg_path: str | None = None
) -> tuple[bool, str]:
    module = _binding(bindings, "_macos_desktop_video")
    resolve = _binding(bindings, "resolve_ffmpeg_path")
    try:
        return module.macos_avfoundation_audio_device_detail(
            audio_device,
            ffmpeg_path=ffmpeg_path or resolve(),
            run_fn=_binding_attr(bindings, "subprocess", "run"),
        )
    except RuntimeError as exc:
        return False, str(exc)


def resolve_ffmpeg_path(bindings: dict) -> str:
    return _binding(bindings, "_video_artifacts").resolve_ffmpeg_path(
        tool_dir=_binding(bindings, "SCRIPT_DIR"),
    )


def desktop_video_metadata(bindings: dict, path: Path, **kwargs) -> dict:
    return _binding(bindings, "_video_artifacts").desktop_video_metadata(path, **kwargs)


def write_desktop_video_metadata(bindings: dict, path: Path, metadata: dict) -> None:
    _binding(bindings, "_video_artifacts").write_desktop_video_metadata(path, metadata)


def compose_desktop_video_proof(
    bindings: dict,
    manifest_path: Path,
    output_path: Path,
    *,
    template: str | None = None,
    source_image: Path | None = None,
    source_label: str | None = None,
    diff_image: Path | None = None,
    diff_label: str | None = None,
    title: str | None = None,
    notes: list[str] | None = None,
    video: Path | None = None,
) -> dict:
    script_dir = _binding(bindings, "SCRIPT_DIR")
    return _binding(bindings, "_video_artifacts").compose_desktop_video_proof(
        manifest_path,
        output_path,
        script_path=Path(script_dir) / "scripts" / "compose-video-proof.mjs",
        template=template,
        source_image=source_image,
        source_label=source_label,
        diff_image=diff_image,
        diff_label=diff_label,
        title=title,
        notes=notes,
        video=video,
        run_fn=_binding_attr(bindings, "subprocess", "run"),
    )


def video_proof_smoke(bindings: dict) -> dict:
    script_dir = _binding(bindings, "SCRIPT_DIR")
    run_fn = _binding_attr(bindings, "subprocess", "run")
    json_mod = _binding(bindings, "json")
    command = ["npm", "--prefix", str(script_dir), "run", "smoke-video-proof"]
    try:
        result = run_fn(command, capture_output=True, text=True)
    except OSError as exc:
        return {"ok": False, "command": command, "detail": str(exc)}
    payload: dict = {}
    if result.stdout:
        json_start = result.stdout.find("{")
        try:
            payload = json_mod.loads(result.stdout[json_start:]) if json_start >= 0 else {}
        except (ValueError, json_mod.JSONDecodeError):
            payload = {}
    payload.update(
        {
            "ok": result.returncode == 0,
            "command": command,
            "returncode": result.returncode,
        }
    )
    if result.stdout:
        payload["stdout_tail"] = result.stdout[-4000:]
    if result.stderr:
        payload["stderr_tail"] = result.stderr[-4000:]
    if result.returncode == 0:
        payload["detail"] = payload.get("output") or "Remotion smoke render passed"
    else:
        payload["detail"] = (result.stderr or result.stdout or f"smoke exited {result.returncode}")[-1000:].strip()
    return payload


def mux_desktop_video_audio(
    bindings: dict,
    video_path: Path,
    audio_path: Path,
    *,
    attachment_budget_bytes: int | None = None,
) -> dict:
    video_artifacts = _binding(bindings, "_video_artifacts")
    budget = (
        attachment_budget_bytes
        if attachment_budget_bytes is not None
        else video_artifacts.DEFAULT_VIDEO_ATTACHMENT_BUDGET_BYTES
    )
    return video_artifacts.mux_desktop_video_audio(
        video_path,
        audio_path,
        attachment_budget_bytes=budget,
        ffmpeg_path=_binding(bindings, "resolve_ffmpeg_path")(),
        run_fn=_binding_attr(bindings, "subprocess", "run"),
    )


def generate_interaction_focus(
    bindings: dict,
    video_path: Path,
    output_path: Path,
    *,
    content_point: tuple[float, float],
    window_bounds: dict,
    scale: float = 1.0,
    attachment_budget_bytes: int | None = None,
) -> dict:
    video_artifacts = _binding(bindings, "_video_artifacts")
    budget = (
        attachment_budget_bytes
        if attachment_budget_bytes is not None
        else video_artifacts.DEFAULT_VIDEO_ATTACHMENT_BUDGET_BYTES
    )
    return video_artifacts.generate_interaction_focus(
        video_path,
        output_path,
        content_point=content_point,
        window_bounds=window_bounds,
        scale=scale,
        ffmpeg_path=_binding(bindings, "resolve_ffmpeg_path")(),
        attachment_budget_bytes=budget,
        run_fn=_binding_attr(bindings, "subprocess", "run"),
    )


def create_issue_video_variant(
    bindings: dict,
    source_path: Path,
    output_path: Path,
    metadata_path: Path,
    *,
    attachment_budget_bytes: int,
) -> dict:
    return _binding(bindings, "_video_artifacts").create_issue_video_variant(
        source_path,
        output_path,
        metadata_path,
        attachment_budget_bytes=attachment_budget_bytes,
        ffmpeg_path=_binding(bindings, "resolve_ffmpeg_path")(),
        run_fn=_binding_attr(bindings, "subprocess", "run"),
    )


def start_macos_window_video_recording(
    bindings: dict,
    window: dict,
    output_path: Path,
    *,
    duration_secs: float,
    fps: float,
    audio_source: str = "none",
    audio_device: str | None = None,
    prefer_frame_sequence: bool = False,
    activate_fn=None,
) -> dict:
    return _binding(bindings, "_macos_desktop_video").start_macos_window_video_recording(
        window,
        output_path,
        duration_secs=duration_secs,
        fps=fps,
        audio_source=audio_source,
        audio_device=audio_device,
        popen_fn=_binding_attr(bindings, "subprocess", "Popen"),
        run_fn=_binding_attr(bindings, "subprocess", "run"),
        ffmpeg_path=_binding(bindings, "resolve_ffmpeg_path")(),
        prefer_frame_sequence=prefer_frame_sequence,
        activate_fn=activate_fn,
    )


def stop_macos_window_video_recording(
    bindings: dict,
    recording: dict,
    *,
    output_path: Path,
    metadata_path: Path,
    poster_path: Path | None = None,
    duration_secs: float,
    fps: float,
    attachment_budget_bytes: int,
) -> dict:
    return _binding(bindings, "_macos_desktop_video").stop_macos_window_video_recording(
        recording,
        output_path=output_path,
        metadata_path=metadata_path,
        poster_path=poster_path,
        duration_secs=duration_secs,
        fps=fps,
        attachment_budget_bytes=attachment_budget_bytes,
        desktop_video_metadata_fn=_binding(bindings, "desktop_video_metadata"),
        write_desktop_video_metadata_fn=_binding(bindings, "write_desktop_video_metadata"),
    )


def install_macos_video_helpers(bindings: dict[str, Any], names: tuple[str, ...] = MACOS_VIDEO_EXPORTS) -> None:
    known_names = set(MACOS_VIDEO_EXPORTS)
    video_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), video_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
