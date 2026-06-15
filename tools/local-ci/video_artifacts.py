"""Video proof artifact policy for desktop automation."""

from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import subprocess
from typing import Callable


GITHUB_FREE_VIDEO_ATTACHMENT_LIMIT_BYTES = 10_000_000
GITHUB_PRO_VIDEO_ATTACHMENT_LIMIT_BYTES = 100_000_000
DEFAULT_VIDEO_ATTACHMENT_BUDGET_BYTES = GITHUB_PRO_VIDEO_ATTACHMENT_LIMIT_BYTES
LOCAL_FFMPEG_STATIC_RELATIVE_PATH = Path("node_modules") / "ffmpeg-static" / "ffmpeg"
ISSUE_VIDEO_TRANSCODE_ATTEMPTS = [
    {"name": "balanced-720p", "max_width": 1280, "fps": 24, "crf": 32},
    {"name": "compact-720p", "max_width": 1280, "fps": 15, "crf": 36},
    {"name": "compact-540p", "max_width": 960, "fps": 15, "crf": 38},
]


def desktop_video_size_status(
    path: Path,
    *,
    attachment_budget_bytes: int = DEFAULT_VIDEO_ATTACHMENT_BUDGET_BYTES,
) -> dict:
    exists = path.exists()
    size_bytes = path.stat().st_size if exists else 0
    return {
        "exists": exists,
        "size_bytes": size_bytes,
        "attachment_budget_bytes": attachment_budget_bytes,
        "fits_attachment_budget": exists and size_bytes <= attachment_budget_bytes,
        "github_free_limit_bytes": GITHUB_FREE_VIDEO_ATTACHMENT_LIMIT_BYTES,
        "github_pro_limit_bytes": GITHUB_PRO_VIDEO_ATTACHMENT_LIMIT_BYTES,
    }


def desktop_video_metadata(
    path: Path,
    *,
    duration_secs: float,
    fps: float,
    attachment_budget_bytes: int = DEFAULT_VIDEO_ATTACHMENT_BUDGET_BYTES,
    codec: str = "h264",
    has_audio: bool = False,
    audio_source: str = "none",
    bounds: dict | None = None,
    command: list[str] | None = None,
    encoder: dict | None = None,
) -> dict:
    return {
        "kind": "desktop-video-proof",
        "path": str(path),
        "duration_secs": duration_secs,
        "fps": fps,
        "codec": codec,
        "has_audio": has_audio,
        "audio_source": audio_source,
        "bounds": bounds or {},
        "command": command or [],
        "encoder": encoder or {},
        "size": desktop_video_size_status(path, attachment_budget_bytes=attachment_budget_bytes),
    }


def write_desktop_video_metadata(path: Path, metadata: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(metadata, indent=2) + "\n")


def resolve_ffmpeg_path(
    *,
    env: dict[str, str] | None = None,
    which_fn: Callable[[str], str | None] = shutil.which,
    tool_dir: Path | None = None,
) -> str:
    env = env or os.environ
    explicit = env.get("PULP_FFMPEG") or env.get("PULP_FFMPEG_PATH") or env.get("FFMPEG_PATH")
    if explicit:
        explicit_path = Path(explicit).expanduser()
        if explicit_path.exists():
            return str(explicit_path)
        raise RuntimeError(f"Configured ffmpeg path does not exist: {explicit_path}")

    path_ffmpeg = which_fn("ffmpeg")
    if path_ffmpeg:
        return path_ffmpeg

    if tool_dir is not None:
        local_ffmpeg = tool_dir / LOCAL_FFMPEG_STATIC_RELATIVE_PATH
        if local_ffmpeg.exists():
            return str(local_ffmpeg)

    install_hint = "npm --prefix tools/local-ci install"
    raise RuntimeError(f"ffmpeg not found; set PULP_FFMPEG, install ffmpeg on PATH, or run `{install_hint}`.")


def compose_desktop_video_proof(
    manifest_path: Path,
    output_path: Path,
    *,
    script_path: Path,
    template: str | None = None,
    source_image: Path | None = None,
    source_label: str | None = None,
    diff_image: Path | None = None,
    diff_label: str | None = None,
    title: str | None = None,
    notes: list[str] | None = None,
    node_path: str = "node",
    run_fn: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
) -> dict:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    command = [
        node_path,
        str(script_path),
        "--manifest",
        str(manifest_path),
        "--output",
        str(output_path),
    ]
    if template:
        command.extend(["--template", template])
    if source_image:
        command.extend(["--source-image", str(source_image)])
    if source_label:
        command.extend(["--source-label", source_label])
    if diff_image:
        command.extend(["--diff-image", str(diff_image)])
    if diff_label:
        command.extend(["--diff-label", diff_label])
    if title:
        command.extend(["--title", title])
    for note in notes or []:
        if note:
            command.extend(["--note", note])
    result = run_fn(
        command,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0 or not output_path.exists():
        detail = (result.stderr or result.stdout or f"composer exited {result.returncode}").strip()
        raise RuntimeError(f"Remotion video proof composition failed: {detail[-1000:]}")
    try:
        payload = json.loads(result.stdout)
    except json.JSONDecodeError:
        payload = {"output": str(output_path), "composer": "remotion"}
    payload.setdefault("output", str(output_path))
    payload.setdefault("composer", "remotion")
    payload["size"] = desktop_video_size_status(output_path)
    return payload


def mux_desktop_video_audio(
    video_path: Path,
    audio_path: Path,
    *,
    ffmpeg_path: str,
    attachment_budget_bytes: int = DEFAULT_VIDEO_ATTACHMENT_BUDGET_BYTES,
    run_fn: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
) -> dict:
    if not video_path.exists():
        raise RuntimeError(f"Cannot mux plugin audio; video file does not exist: {video_path}")
    if not audio_path.exists():
        raise RuntimeError(f"Cannot mux plugin audio; audio file does not exist: {audio_path}")

    tmp_path = video_path.with_name(f"{video_path.stem}.audio-tmp{video_path.suffix}")
    tmp_path.unlink(missing_ok=True)
    command = [
        ffmpeg_path,
        "-hide_banner",
        "-y",
        "-i",
        str(video_path),
        "-i",
        str(audio_path),
        "-map",
        "0:v:0",
        "-map",
        "1:a:0",
        "-c:v",
        "copy",
        "-c:a",
        "aac",
        "-b:a",
        "128k",
        "-shortest",
        "-movflags",
        "+faststart",
        str(tmp_path),
    ]
    result = run_fn(command, capture_output=True, text=True)
    payload = {
        "kind": "desktop-video-proof-audio-mux",
        "audio_source": "plugin",
        "source_video": str(video_path),
        "audio_file": str(audio_path),
        "output": str(video_path),
        "command": command,
        "returncode": result.returncode,
    }
    if result.stdout:
        payload["stdout_tail"] = result.stdout[-4000:]
    if result.stderr:
        payload["stderr_tail"] = result.stderr[-4000:]
    if result.returncode != 0 or not tmp_path.exists():
        detail = (result.stderr or result.stdout or f"ffmpeg exited {result.returncode}").strip()
        raise RuntimeError(f"Plugin audio mux failed: {detail[-1000:]}")

    tmp_path.replace(video_path)
    payload["status"] = "muxed"
    payload["size"] = desktop_video_size_status(video_path, attachment_budget_bytes=attachment_budget_bytes)
    return payload


def macos_interaction_focus_crop(
    content_point: tuple[float, float],
    window_bounds: dict,
    *,
    scale: float = 1.0,
    focus_points: tuple[float, float] = (220.0, 130.0),
) -> dict:
    """Compute the crop rect (in raw-video pixels) centered on an interacted
    control so a small change at the click point can be magnified. The raw window
    recording is the window bounds captured at the screen backing scale, so a
    logical content point maps to pixel = point * scale."""
    if scale <= 0:
        scale = 1.0
    vid_w = max(2, int(round(float(window_bounds.get("width", 0) or 0) * scale)))
    vid_h = max(2, int(round(float(window_bounds.get("height", 0) or 0) * scale)))
    box_w = min(vid_w, max(2, int(round(focus_points[0] * scale))))
    box_h = min(vid_h, max(2, int(round(focus_points[1] * scale))))
    if box_w % 2:
        box_w -= 1
    if box_h % 2:
        box_h -= 1
    cx = float(content_point[0]) * scale
    cy = float(content_point[1]) * scale
    x = max(0, min(int(round(cx - box_w / 2)), vid_w - box_w))
    y = max(0, min(int(round(cy - box_h / 2)), vid_h - box_h))
    return {"x": x, "y": y, "width": box_w, "height": box_h, "video_width": vid_w, "video_height": vid_h}


def macos_interaction_focus_command(
    video_path: Path,
    output_path: Path,
    crop: dict,
    *,
    ffmpeg_path: str,
    output_width: int = 1080,
) -> list[str]:
    out_w = output_width - (output_width % 2)
    out_h = int(round(out_w * crop["height"] / max(1, crop["width"])))
    out_h -= out_h % 2
    vf = (
        f"crop={crop['width']}:{crop['height']}:{crop['x']}:{crop['y']},"
        f"scale={out_w}:{out_h}:flags=neighbor"
    )
    return [
        ffmpeg_path,
        "-hide_banner",
        "-y",
        "-i",
        str(video_path),
        "-vf",
        vf,
        "-c:v",
        "libx264",
        "-pix_fmt",
        "yuv420p",
        "-movflags",
        "+faststart",
        str(output_path),
    ]


def generate_interaction_focus(
    video_path: Path,
    output_path: Path,
    *,
    content_point: tuple[float, float],
    window_bounds: dict,
    scale: float = 1.0,
    ffmpeg_path: str,
    focus_points: tuple[float, float] = (220.0, 130.0),
    output_width: int = 1080,
    attachment_budget_bytes: int = DEFAULT_VIDEO_ATTACHMENT_BUDGET_BYTES,
    run_fn: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
) -> dict:
    """Produce a "zoom to the demoed control" clip: crop the raw window recording
    to the region around the interaction point and scale it up so the on-screen
    change is clearly visible. Returns metadata; status='skipped' when the raw
    video is absent so smoke (no-interaction) runs are unaffected."""
    if not video_path.exists():
        return {
            "kind": "desktop-video-proof-focus",
            "status": "skipped",
            "reason": f"raw video does not exist: {video_path}",
        }
    crop = macos_interaction_focus_crop(
        content_point, window_bounds, scale=scale, focus_points=focus_points
    )
    command = macos_interaction_focus_command(
        video_path, output_path, crop, ffmpeg_path=ffmpeg_path, output_width=output_width
    )
    result = run_fn(command, capture_output=True, text=True)
    payload = {
        "kind": "desktop-video-proof-focus",
        "output": str(output_path),
        "crop": crop,
        "scale": scale,
        "content_point": {"x": content_point[0], "y": content_point[1]},
        "command": command,
        "returncode": result.returncode,
    }
    if result.returncode != 0 or not output_path.exists():
        payload["status"] = "failed"
        payload["error"] = (result.stderr or result.stdout or f"ffmpeg exited {result.returncode}").strip()[-500:]
        return payload
    payload["status"] = "created"
    payload["size"] = desktop_video_size_status(output_path, attachment_budget_bytes=attachment_budget_bytes)
    return payload


def create_issue_video_variant(
    source_path: Path,
    output_path: Path,
    metadata_path: Path,
    *,
    attachment_budget_bytes: int = DEFAULT_VIDEO_ATTACHMENT_BUDGET_BYTES,
    ffmpeg_path: str,
    run_fn: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
) -> dict:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    metadata_path.parent.mkdir(parents=True, exist_ok=True)
    source_size = desktop_video_size_status(source_path, attachment_budget_bytes=attachment_budget_bytes)
    payload = {
        "kind": "desktop-video-proof-issue-variant",
        "source": str(source_path),
        "output": str(output_path),
        "attachment_budget_bytes": attachment_budget_bytes,
        "source_size": source_size,
        "strategy": "copy-if-fits-else-retry-ladder",
        "attempts": [],
    }
    if not source_path.exists():
        payload["status"] = "missing-source"
        payload["size"] = desktop_video_size_status(output_path, attachment_budget_bytes=attachment_budget_bytes)
        metadata_path.write_text(json.dumps(payload, indent=2) + "\n")
        return payload

    if source_size["fits_attachment_budget"]:
        shutil.copyfile(source_path, output_path)
        payload["status"] = "copied"
        payload["command"] = []
        payload["attempts"].append({"name": "copy", "status": "copied"})
        payload["size"] = desktop_video_size_status(output_path, attachment_budget_bytes=attachment_budget_bytes)
        metadata_path.write_text(json.dumps(payload, indent=2) + "\n")
        return payload

    last_result = None
    for attempt in ISSUE_VIDEO_TRANSCODE_ATTEMPTS:
        output_path.unlink(missing_ok=True)
        scale = f"scale='min({attempt['max_width']},iw)':-2"
        command = [
            ffmpeg_path,
            "-hide_banner",
            "-y",
            "-i",
            str(source_path),
            "-map",
            "0:v:0",
            "-map",
            "0:a?",
            "-vf",
            scale,
            "-r",
            str(attempt["fps"]),
            "-c:v",
            "libx264",
            "-preset",
            "veryfast",
            "-crf",
            str(attempt["crf"]),
            "-c:a",
            "aac",
            "-b:a",
            "96k",
            "-movflags",
            "+faststart",
            str(output_path),
        ]
        result = run_fn(command, capture_output=True, text=True)
        last_result = result
        size = desktop_video_size_status(output_path, attachment_budget_bytes=attachment_budget_bytes)
        attempt_payload = {
            "name": attempt["name"],
            "max_width": attempt["max_width"],
            "fps": attempt["fps"],
            "crf": attempt["crf"],
            "command": command,
            "returncode": result.returncode,
            "size": size,
        }
        if result.stdout:
            attempt_payload["stdout_tail"] = result.stdout[-4000:]
        if result.stderr:
            attempt_payload["stderr_tail"] = result.stderr[-4000:]
        if result.returncode != 0 or not output_path.exists():
            attempt_payload["status"] = "transcode-failed"
        elif size["fits_attachment_budget"]:
            attempt_payload["status"] = "transcoded"
        else:
            attempt_payload["status"] = "exceeds-budget"
        payload["attempts"].append(attempt_payload)
        payload["command"] = command
        payload["returncode"] = result.returncode
        if result.stdout:
            payload["stdout_tail"] = result.stdout[-4000:]
        if result.stderr:
            payload["stderr_tail"] = result.stderr[-4000:]
        payload["size"] = size
        if attempt_payload["status"] == "transcoded":
            payload["status"] = "transcoded"
            payload["selected_attempt"] = attempt["name"]
            metadata_path.write_text(json.dumps(payload, indent=2) + "\n")
            return payload

    if last_result is None:
        payload["status"] = "transcode-failed"
        payload["size"] = desktop_video_size_status(output_path, attachment_budget_bytes=attachment_budget_bytes)
    elif output_path.exists():
        payload["status"] = "exceeds-budget"
        payload["selected_attempt"] = payload["attempts"][-1]["name"]
    else:
        payload["status"] = "transcode-failed"
    metadata_path.write_text(json.dumps(payload, indent=2) + "\n")
    return payload
