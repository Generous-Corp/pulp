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
