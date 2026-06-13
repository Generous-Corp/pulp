"""Shared mobile video proof composition helpers."""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
from typing import Callable

from video_artifacts import compose_desktop_video_proof, create_issue_video_variant, resolve_ffmpeg_path


def compose_mobile_video_proof(
    manifest_path: Path,
    *,
    template: str,
    title: str | None = None,
    notes: list[str] | None = None,
    video_attachment_budget_mb: float = 100.0,
    small_video: bool = False,
    small_video_budget_mb: float = 10.0,
    tool_dir: Path | None = None,
    compose_fn: Callable[..., dict] = compose_desktop_video_proof,
    issue_variant_fn: Callable[..., dict] = create_issue_video_variant,
    resolve_ffmpeg_fn: Callable[..., str] = resolve_ffmpeg_path,
    run_fn: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
    atomic_write_text_fn: Callable[[Path, str], None] | None = None,
) -> dict:
    tool_dir = tool_dir or Path(__file__).resolve().parent
    script_path = tool_dir / "scripts" / "compose-video-proof.mjs"
    manifest_path = manifest_path.expanduser().resolve()
    manifest = json.loads(manifest_path.read_text())
    run_dir = manifest_path.parent
    video_dir = run_dir / "video"
    output_path = video_dir / "proof-composed.mp4"
    metadata_path = video_dir / "composed-metadata.json"
    issue_output_path = video_dir / "proof.issue.mp4"
    issue_metadata_path = video_dir / "issue-metadata.json"
    small_output_path = video_dir / "proof.small.mp4"
    small_metadata_path = video_dir / "small-metadata.json"
    notes = [note for note in (notes or []) if note]
    write_text = atomic_write_text_fn or (lambda path, text: path.write_text(text))

    output_path.parent.mkdir(parents=True, exist_ok=True)
    metadata_path.parent.mkdir(parents=True, exist_ok=True)
    issue_output_path.parent.mkdir(parents=True, exist_ok=True)
    issue_metadata_path.parent.mkdir(parents=True, exist_ok=True)
    if small_video:
        small_output_path.parent.mkdir(parents=True, exist_ok=True)
        small_metadata_path.parent.mkdir(parents=True, exist_ok=True)

    composed_summary = compose_fn(
        manifest_path,
        output_path,
        script_path=script_path,
        template=template,
        title=title,
        notes=notes,
        run_fn=run_fn,
    )
    write_text(metadata_path, json.dumps(composed_summary, indent=2) + "\n")

    ffmpeg_path = resolve_ffmpeg_fn(tool_dir=tool_dir)
    attachment_budget_bytes = int(float(video_attachment_budget_mb) * 1_000_000)
    issue_summary = issue_variant_fn(
        output_path,
        issue_output_path,
        issue_metadata_path,
        attachment_budget_bytes=attachment_budget_bytes,
        ffmpeg_path=ffmpeg_path,
        run_fn=run_fn,
    )

    small_summary = None
    if small_video:
        small_budget_bytes = int(float(small_video_budget_mb) * 1_000_000)
        small_summary = issue_variant_fn(
            output_path,
            small_output_path,
            small_metadata_path,
            attachment_budget_bytes=small_budget_bytes,
            ffmpeg_path=ffmpeg_path,
            run_fn=run_fn,
        )

    artifacts = manifest.setdefault("artifacts", {})
    manifest["video_composed"] = composed_summary
    manifest["video_issue"] = issue_summary
    if small_summary is not None:
        manifest["video_small"] = small_summary
    if notes:
        manifest["video_proof_notes"] = notes

    composition = manifest.setdefault("video_proof_composition", {})
    if not isinstance(composition, dict):
        composition = {}
        manifest["video_proof_composition"] = composition
    composition.update(
        {
            "template": template,
            "title": title if title is not None else composition.get("title"),
            "notes": notes if notes else composition.get("notes", []),
        }
    )

    if output_path.exists():
        artifacts["video_composed"] = str(output_path)
    if metadata_path.exists():
        artifacts["video_composed_metadata"] = str(metadata_path)
    if issue_output_path.exists():
        artifacts["video_issue"] = str(issue_output_path)
    if issue_metadata_path.exists():
        artifacts["video_issue_metadata"] = str(issue_metadata_path)
    if small_summary is not None and small_output_path.exists():
        artifacts["video_small"] = str(small_output_path)
    if small_summary is not None and small_metadata_path.exists():
        artifacts["video_small_metadata"] = str(small_metadata_path)

    write_text(manifest_path, json.dumps(manifest, indent=2) + "\n")
    return {
        "manifest": str(manifest_path),
        "video_composed": composed_summary,
        "video_issue": issue_summary,
        "video_small": small_summary,
        "artifacts": {
            "video_composed": artifacts.get("video_composed"),
            "video_composed_metadata": artifacts.get("video_composed_metadata"),
            "video_issue": artifacts.get("video_issue"),
            "video_issue_metadata": artifacts.get("video_issue_metadata"),
            "video_small": artifacts.get("video_small"),
            "video_small_metadata": artifacts.get("video_small_metadata"),
        },
    }
