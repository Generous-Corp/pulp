"""Shared proof/video report helpers for desktop video proofs.

Small building blocks used by the HTML report, proof summaries, and the GitHub
review-issue draft: focus/marker/storyboard summaries, artifact metadata, byte
formatting, manifest source/command context, and the local serve commands.
"""

from __future__ import annotations

import json
from pathlib import Path
import shlex
import shutil

from reporting_publish import slugify_token


GITHUB_VIDEO_ATTACHMENT_EXTENSIONS = (".mp4", ".mov", ".webm")
GITHUB_PRO_VIDEO_ATTACHMENT_LIMIT_BYTES = 100_000_000
GITHUB_FREE_VIDEO_ATTACHMENT_LIMIT_BYTES = 10_000_000
GITHUB_ATTACHMENT_POLICY_SOURCE = (
    "https://docs.github.com/en/get-started/writing-on-github/"
    "working-with-advanced-formatting/attaching-files"
)


def _github_video_attachment_policy() -> dict:
    return {
        "source": GITHUB_ATTACHMENT_POLICY_SOURCE,
        "supported_video_extensions": list(GITHUB_VIDEO_ATTACHMENT_EXTENSIONS),
        "pro_video_limit_bytes": GITHUB_PRO_VIDEO_ATTACHMENT_LIMIT_BYTES,
        "free_video_limit_bytes": GITHUB_FREE_VIDEO_ATTACHMENT_LIMIT_BYTES,
        "fallback": "Use the served local/Tailscale report link when no supported issue-ready video fits the configured budget.",
    }


def _proof_notes_from_manifest(manifest: dict) -> list[str]:
    notes: list[str] = []
    for source in (
        manifest.get("video_proof_notes"),
        (manifest.get("video_proof_composition") or {}).get("notes"),
    ):
        if not isinstance(source, list):
            continue
        for note in source:
            if isinstance(note, str) and note.strip() and note.strip() not in notes:
                notes.append(note.strip())
    return notes


def _copy_optional_file(path_value: object, destination: Path) -> bool:
    if not isinstance(path_value, str) or not path_value:
        return False
    source = Path(path_value).expanduser()
    if not source.exists() or not source.is_file():
        return False
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)
    return True




def _artifact_metadata(publish_dir: Path, rel_path: str | None) -> dict:
    if not rel_path:
        return {}
    path = publish_dir / rel_path
    if not path.exists():
        return {}
    try:
        return json.loads(path.read_text())
    except (json.JSONDecodeError, OSError):
        return {}


def _format_bytes(value: object) -> str:
    if not isinstance(value, (int, float)):
        return "unknown"
    if value >= 1_000_000:
        return f"{value / 1_000_000:.1f} MB"
    return f"{max(1, round(value / 1000))} KB"


def _proof_focus_summary(proof_composition: dict) -> dict:
    focus = proof_composition.get("focus") if isinstance(proof_composition, dict) else None
    if not isinstance(focus, dict):
        return {}
    selector = focus.get("selector") if isinstance(focus.get("selector"), dict) else {}
    content_point = focus.get("content_point") if isinstance(focus.get("content_point"), dict) else {}
    normalized_center = focus.get("normalized_center") if isinstance(focus.get("normalized_center"), dict) else {}
    label = focus.get("label")
    if not label:
        for key in ("click_view_id", "id", "click_view_label", "label", "click_view_text", "text", "click_view_type", "type"):
            if selector.get(key):
                label = selector[key]
                break
    summary: dict = {}
    if label:
        summary["label"] = str(label)
    if selector:
        summary["selector"] = selector
    if content_point:
        summary["content_point"] = content_point
    if normalized_center:
        summary["normalized_center"] = normalized_center
    return summary


def _proof_focus_label(proof_composition: dict) -> str | None:
    focus = _proof_focus_summary(proof_composition)
    label = focus.get("label")
    return str(label) if label else None


def _proof_action_marker_summary(proof_composition: dict) -> dict:
    marker = proof_composition.get("action_marker") if isinstance(proof_composition, dict) else None
    if not isinstance(marker, dict):
        return {}
    summary: dict = {}
    for key in ("kind", "label"):
        if marker.get(key):
            summary[key] = str(marker[key])
    content_point = marker.get("content_point") if isinstance(marker.get("content_point"), dict) else {}
    normalized_point = marker.get("normalized_point") if isinstance(marker.get("normalized_point"), dict) else {}
    if content_point:
        summary["content_point"] = content_point
    if normalized_point:
        summary["normalized_point"] = normalized_point
    return summary


def _proof_context_items(proof_composition: dict) -> list[tuple[str, str]]:
    context = proof_composition.get("context") if isinstance(proof_composition, dict) else None
    if not isinstance(context, dict):
        return []
    items: list[tuple[str, str]] = []
    for key, value in context.items():
        if value is None:
            continue
        text = str(value)
        if text:
            items.append((str(key), text))
    return items


def _proof_storyboard_from_metadata(metadata: dict) -> dict:
    storyboard = metadata.get("review_storyboard") if isinstance(metadata, dict) else None
    if not isinstance(storyboard, dict):
        return {}
    steps: list[dict] = []
    for index, step in enumerate(storyboard.get("steps") or [], start=1):
        if not isinstance(step, dict):
            continue
        label = str(step.get("label") or f"Step {index}").strip()
        detail = str(step.get("detail") or "").strip()
        if not label and not detail:
            continue
        steps.append(
            {
                "index": int(step.get("index") or index),
                "label": label,
                "detail": detail,
            }
        )
    summary: dict = {
        "title": str(storyboard.get("title") or "").strip(),
        "subtitle": str(storyboard.get("subtitle") or "").strip(),
        "template": str(storyboard.get("template") or "").strip(),
        "steps": steps,
    }
    notes = [str(note).strip() for note in storyboard.get("notes") or [] if str(note).strip()]
    if notes:
        summary["notes"] = notes[:5]
    for key in ("source", "capture", "issue"):
        value = storyboard.get(key)
        if isinstance(value, dict):
            summary[key] = {str(k): v for k, v in value.items() if v is not None}
    return {key: value for key, value in summary.items() if value}


def _proof_storyboard_lines(storyboard: dict) -> list[str]:
    steps = storyboard.get("steps") if isinstance(storyboard, dict) else None
    if not isinstance(steps, list):
        return []
    lines = []
    for step in steps[:6]:
        if not isinstance(step, dict):
            continue
        label = str(step.get("label") or "").strip()
        detail = str(step.get("detail") or "").strip()
        if label and detail:
            lines.append(f"{label}: {detail}")
        elif label:
            lines.append(label)
        elif detail:
            lines.append(detail)
    return lines


def _manifest_command_text(manifest: dict) -> str | None:
    command = manifest.get("command")
    if isinstance(command, list):
        return shlex.join(str(part) for part in command)
    if isinstance(command, str) and command.strip():
        return command.strip()
    bundle_id = manifest.get("bundle_id")
    if isinstance(bundle_id, str) and bundle_id.strip():
        return f"open -b {shlex.quote(bundle_id.strip())}"
    app_path = manifest.get("app_path")
    if isinstance(app_path, str) and app_path.strip():
        return f"open {shlex.quote(app_path.strip())}"
    return None


def _manifest_source_context(manifest: dict) -> dict:
    source = manifest.get("source")
    if not isinstance(source, dict):
        return {}
    context = {}
    for key in ("mode", "branch", "sha", "prepare_command", "prepared_root", "launch_cwd", "launch_command"):
        value = source.get(key)
        if value is not None:
            context[key] = value
    return context


def _source_summary(source: dict) -> str | None:
    if not isinstance(source, dict) or not source:
        return None
    parts = []
    for key in ("mode", "branch", "sha"):
        value = source.get(key)
        if value:
            parts.append(f"{key}={value}")
    return ", ".join(parts) if parts else None


def _desktop_report_serve_label(index_payload: dict, publish_dir: Path) -> str:
    label = slugify_token(str(index_payload.get("label") or publish_dir.name))
    return label or "desktop-proof-report"


def _desktop_report_serve_commands(index_payload: dict, publish_dir: Path) -> dict:
    quoted_dir = shlex.quote(str(publish_dir))
    label = _desktop_report_serve_label(index_payload, publish_dir)
    return {
        "serve_label": label,
        "serve_command": f"python3 tools/local-ci/local_ci.py desktop serve {quoted_dir} --host 0.0.0.0 --port 8765",
        "serve_background_command": (
            f"python3 tools/local-ci/local_ci.py desktop serve {quoted_dir} "
            f"--host 0.0.0.0 --port 8765 --auto-port --background --label {shlex.quote(label)} --json"
        ),
        "serve_status_command": (
            f"python3 tools/local-ci/local_ci.py desktop serve --status "
            f"--label {shlex.quote(label)} --json"
        ),
        "serve_stop_command": (
            f"python3 tools/local-ci/local_ci.py desktop serve --stop "
            f"--label {shlex.quote(label)} --json"
        ),
        "published_cleanup_command": (
            "python3 tools/local-ci/local_ci.py desktop cleanup "
            "--published --older-than-days 14 --keep-last 3 --json"
        ),
    }


