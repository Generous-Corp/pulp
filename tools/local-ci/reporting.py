"""Desktop automation report and rollup helpers for local CI."""

from __future__ import annotations

from datetime import datetime
import html
import json
from pathlib import Path
import shutil
import tempfile
import time
from typing import Callable


ARTIFACT_KEYS = (
    "screenshot",
    "before_screenshot",
    "diff_screenshot",
    "ui_snapshot",
    "video",
    "video_composed",
    "video_issue",
    "video_small",
    "video_metadata",
    "video_composed_metadata",
    "video_issue_metadata",
    "video_small_metadata",
    "video_poster",
    "stdout",
    "stderr",
)


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


def clear_directory_contents(path: Path) -> None:
    if not path.exists():
        return
    for child in path.iterdir():
        if child.name == ".git":
            continue
        if child.is_dir():
            shutil.rmtree(child, ignore_errors=True)
        else:
            child.unlink(missing_ok=True)


def copy_directory_contents(src: Path, dest: Path) -> None:
    dest.mkdir(parents=True, exist_ok=True)
    for child in src.iterdir():
        target = dest / child.name
        if child.is_dir():
            shutil.copytree(child, target, dirs_exist_ok=True)
        else:
            shutil.copy2(child, target)


def publish_report_to_branch(
    config: dict,
    report: dict,
    *,
    root: Path,
    run_git_fn: Callable[..., object],
    reset_local_worktree_fn: Callable[[Path], None],
    clear_directory_contents_fn: Callable[[Path], None],
    git_origin_http_url_fn: Callable[[Path], str | None],
) -> dict:
    branch = config["desktop_automation"]["publish_branch"]
    report_dir = Path(report["output_dir"]).expanduser()
    report_name = report_dir.name
    publish_root = Path(tempfile.mkdtemp(prefix="pulp-desktop-publish-"))
    worktree = publish_root / "worktree"
    branch_exists = bool(run_git_fn(["ls-remote", "--heads", "origin", branch], cwd=root, check=False).stdout.strip())
    try:
        if branch_exists:
            run_git_fn(["worktree", "add", "--detach", str(worktree), f"origin/{branch}"], cwd=root)
            run_git_fn(["checkout", "-B", branch, f"origin/{branch}"], cwd=worktree)
        else:
            run_git_fn(["worktree", "add", "--detach", str(worktree), "HEAD"], cwd=root)
            run_git_fn(["checkout", "--orphan", branch], cwd=worktree)
            run_git_fn(["rm", "-rf", "--ignore-unmatch", "."], cwd=worktree, check=False)
            clear_directory_contents_fn(worktree)
        dest_root = worktree / "desktop-automation"
        report_dest = dest_root / "reports" / report_name
        latest_dest = dest_root / "latest"
        shutil.rmtree(report_dest, ignore_errors=True)
        shutil.rmtree(latest_dest, ignore_errors=True)
        report_dest.parent.mkdir(parents=True, exist_ok=True)
        latest_dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(report_dir, report_dest)
        shutil.copytree(report_dir, latest_dest)
        run_git_fn(["add", "desktop-automation"], cwd=worktree)
        status = run_git_fn(["status", "--short"], cwd=worktree).stdout.strip()
        if status:
            run_git_fn(["commit", "-m", f"Publish desktop automation report {report_name}"], cwd=worktree)
            run_git_fn(["push", "origin", f"HEAD:{branch}"], cwd=worktree)
        remote_base = git_origin_http_url_fn(root)
        published = {
            "mode": "branch",
            "branch": branch,
            "report_path": f"desktop-automation/reports/{report_name}",
            "latest_path": "desktop-automation/latest",
        }
        if remote_base:
            published["branch_url"] = f"{remote_base}/tree/{branch}"
            published["report_url"] = f"{remote_base}/tree/{branch}/desktop-automation/reports/{report_name}"
            published["latest_url"] = f"{remote_base}/tree/{branch}/desktop-automation/latest"
            published["latest_index_json_url"] = f"{remote_base}/blob/{branch}/desktop-automation/latest/index.json"
            published_runs = []
            for run in report.get("runs", []):
                artifact_urls = {}
                for key, value in (run.get("artifacts") or {}).items():
                    if isinstance(value, str):
                        artifact_urls[key] = f"{remote_base}/blob/{branch}/desktop-automation/latest/{value}"
                published_runs.append(
                    {
                        "label": run.get("label"),
                        "target": run.get("target"),
                        "action": run.get("action"),
                        "artifact_urls": artifact_urls,
                    }
                )
            published["runs"] = published_runs
        return published
    finally:
        reset_local_worktree_fn(worktree)
        shutil.rmtree(publish_root, ignore_errors=True)


def slugify_token(value: str, *, max_len: int = 48) -> str:
    cleaned = "".join(ch.lower() if ch.isalnum() else "-" for ch in value.strip())
    while "--" in cleaned:
        cleaned = cleaned.replace("--", "-")
    cleaned = cleaned.strip("-")
    if not cleaned:
        return "run"
    return cleaned[:max_len]


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


def desktop_review_issue_body(index_payload: dict, *, publish_dir: Path) -> str:
    serve_command = f"python3 tools/local-ci/local_ci.py desktop serve {publish_dir} --host 0.0.0.0 --port 8765"
    lines = [
        f"# {index_payload['label']}",
        "",
        "Desktop validation proof report is ready for review.",
        "",
        "## Review",
        "",
        f"- Open local report: `{publish_dir / 'index.html'}`",
        f"- Serve over local/Tailscale HTTP: `{serve_command}`",
        "- Served URL: `desktop serve` prints candidate URLs, including localhost, configured public hosts, and Tailscale IPs when available.",
        "- Friendly Tailnet name: set `PULP_DESKTOP_SERVE_HOSTS=<name-or-ip>` before running `desktop serve` if reviewers should tap a stable host name.",
        "- Reviewer verdict: comment `looks good to me` when the proof is accepted, or describe the mismatch and run label when changes are needed.",
        "",
        "## Runs",
        "",
    ]
    for run in index_payload.get("runs", []):
        artifacts = run.get("artifacts") or {}
        proof_notes = run.get("video_proof_notes") if isinstance(run.get("video_proof_notes"), list) else []
        proof_composition = run.get("video_proof_composition") if isinstance(run.get("video_proof_composition"), dict) else {}
        focus_label = _proof_focus_label(proof_composition)
        action_marker = _proof_action_marker_summary(proof_composition)
        action_label = action_marker.get("label") or action_marker.get("kind")
        context_items = _proof_context_items(proof_composition)
        video = artifacts.get("video_issue") or artifacts.get("video_composed") or artifacts.get("video")
        metadata_path = artifacts.get("video_issue_metadata") or artifacts.get("video_composed_metadata") or artifacts.get("video_metadata")
        metadata = _artifact_metadata(publish_dir, metadata_path)
        small_metadata = _artifact_metadata(publish_dir, artifacts.get("video_small_metadata"))
        size = metadata.get("size") if isinstance(metadata.get("size"), dict) else {}
        small_size = small_metadata.get("size") if isinstance(small_metadata.get("size"), dict) else {}
        size_bytes = size.get("size_bytes") or metadata.get("size_bytes")
        small_size_bytes = small_size.get("size_bytes") or small_metadata.get("size_bytes")
        fits = size.get("fits_attachment_budget")
        small_fits = small_size.get("fits_attachment_budget")
        issue_status = metadata.get("status")
        selected_attempt = metadata.get("selected_attempt")
        small_status = small_metadata.get("status")
        small_selected_attempt = small_metadata.get("selected_attempt")
        attach_status = "unknown"
        attach_action = "Attachment decision unknown; use the served report link if upload fails."
        if fits is True:
            attach_status = "fits configured attachment budget"
            if artifacts.get("video_issue"):
                attach_action = f"Attach `{publish_dir / artifacts['video_issue']}` to the issue."
        elif fits is False:
            attach_status = "exceeds configured attachment budget; use served/local link"
            attach_action = "Do not attach the MP4; use the served report link."
            if small_fits is True and artifacts.get("video_small"):
                attach_action = f"Attach small fallback `{publish_dir / artifacts['video_small']}` or use the served report link for the full proof."
        verdict_manifest = Path(str(run.get("bundle_dir") or "")) / "manifest.json"
        lines.extend(
            [
                f"### {run.get('target') or '?'}/{run.get('action') or '?'} - {run.get('label') or '?'}",
                "",
                f"- Completed: {run.get('completed_at') or '?'}",
                f"- Interaction: {run.get('interaction_mode') or 'not recorded'}",
                f"- Proof template: `{proof_composition.get('template')}`" if proof_composition.get("template") else "- Proof template: not recorded",
                f"- Focus component: `{focus_label}`" if focus_label else "- Focus component: not recorded",
                f"- Action marker: `{action_label}`" if action_label else "- Action marker: not recorded",
                f"- Action point: `{json.dumps(action_marker['content_point'], sort_keys=True)}`" if action_marker.get("content_point") else "- Action point: not recorded",
                f"- Source reference: `{artifacts['video_source_image']}`" if artifacts.get("video_source_image") else "- Source reference: not attached",
                f"- Issue video: `{artifacts['video_issue']}`" if artifacts.get("video_issue") else "- Issue video: not generated",
                f"- Small video: `{artifacts['video_small']}`" if artifacts.get("video_small") else "- Small video: not generated",
                f"- Review video: `{artifacts.get('video_composed') or artifacts.get('video')}`" if artifacts.get("video_composed") or artifacts.get("video") else "- Review video: not recorded",
                f"- Video size: {_format_bytes(size_bytes)} ({attach_status})",
                f"- Small video size: {_format_bytes(small_size_bytes)}" + (" (fits 10 MB budget)" if small_fits is True else " (over 10 MB budget)" if small_fits is False else "") if artifacts.get("video_small") else "- Small video size: not recorded",
                f"- Issue variant: `{issue_status}`" + (f" via `{selected_attempt}`" if selected_attempt else "") if issue_status else "- Issue variant: not recorded",
                f"- Small variant: `{small_status}`" + (f" via `{small_selected_attempt}`" if small_selected_attempt else "") if small_status else "- Small variant: not recorded",
                f"- Attachment action: {attach_action}",
                f"- Approve command: `python3 tools/local-ci/local_ci.py desktop verdict {verdict_manifest} --approved --issue-url <issue-url>`",
                f"- Needs-work command: `python3 tools/local-ci/local_ci.py desktop verdict {verdict_manifest} --needs-work --notes \"<what to change>\" --issue-url <issue-url>`",
            ]
        )
        for key, value in context_items[:8]:
            lines.append(f"- Context {key}: `{value}`")
        for note in proof_notes[:5]:
            lines.append(f"- Proof note: {note}")
        if artifacts.get("screenshot"):
            lines.append(f"- Screenshot: `{artifacts['screenshot']}`")
        if artifacts.get("diff_screenshot"):
            lines.append(f"- Diff screenshot: `{artifacts['diff_screenshot']}`")
        lines.append("")
    lines.extend(
        [
            "## Closeout",
            "",
            "When the reviewer confirms the proof, close the review issue. Keep this branch/worktree open until the broader validation-video-proof feature is accepted.",
            "",
        ]
    )
    return "\n".join(lines)


def desktop_review_package(index_payload: dict, *, publish_dir: Path) -> dict:
    serve_command = f"python3 tools/local-ci/local_ci.py desktop serve {publish_dir} --host 0.0.0.0 --port 8765"
    runs: list[dict] = []
    for run in index_payload.get("runs", []):
        artifacts = run.get("artifacts") or {}
        metadata_path = artifacts.get("video_issue_metadata") or artifacts.get("video_composed_metadata") or artifacts.get("video_metadata")
        small_metadata_path = artifacts.get("video_small_metadata")
        metadata = _artifact_metadata(publish_dir, metadata_path)
        small_metadata = _artifact_metadata(publish_dir, small_metadata_path)
        size = metadata.get("size") if isinstance(metadata.get("size"), dict) else {}
        small_size = small_metadata.get("size") if isinstance(small_metadata.get("size"), dict) else {}
        primary_fits = size.get("fits_attachment_budget")
        small_fits = small_size.get("fits_attachment_budget")
        primary_path = artifacts.get("video_issue")
        small_path = artifacts.get("video_small")
        attachment: dict = {
            "status": "fallback-link",
            "path": None,
            "size_bytes": size.get("size_bytes") or metadata.get("size_bytes"),
            "fits_attachment_budget": primary_fits,
            "budget_bytes": size.get("attachment_budget_bytes") or metadata.get("attachment_budget_bytes"),
            "reason": "no issue-ready MP4 fits the configured attachment budget",
        }
        if primary_path and primary_fits is True:
            attachment.update(
                {
                    "status": "attach-primary",
                    "path": str(publish_dir / str(primary_path)),
                    "relative_path": primary_path,
                    "reason": "primary issue MP4 fits the configured attachment budget",
                }
            )
        elif small_path and small_fits is True:
            attachment.update(
                {
                    "status": "attach-small",
                    "path": str(publish_dir / str(small_path)),
                    "relative_path": small_path,
                    "size_bytes": small_size.get("size_bytes") or small_metadata.get("size_bytes"),
                    "fits_attachment_budget": small_fits,
                    "budget_bytes": small_size.get("attachment_budget_bytes") or small_metadata.get("attachment_budget_bytes"),
                    "reason": "primary issue MP4 is unavailable or over budget; small fallback fits",
                }
            )
        proof_composition = run.get("video_proof_composition") if isinstance(run.get("video_proof_composition"), dict) else {}
        runs.append(
            {
                "target": run.get("target"),
                "action": run.get("action"),
                "label": run.get("label"),
                "completed_at": run.get("completed_at"),
                "bundle_dir": run.get("bundle_dir"),
                "template": proof_composition.get("template"),
                "context": proof_composition.get("context") if isinstance(proof_composition.get("context"), dict) else {},
                "notes": run.get("video_proof_notes") if isinstance(run.get("video_proof_notes"), list) else [],
                "attachment": attachment,
                "fallback": {
                    "report_path": str(publish_dir / "index.html"),
                    "review_markdown": str(publish_dir / "review.md"),
                    "serve_command": serve_command,
                    "internal_ephemeral": True,
                },
            }
        )
    return {
        "kind": "desktop-video-proof-review-package",
        "generated_at": index_payload.get("generated_at"),
        "label": index_payload.get("label"),
        "publish_mode": index_payload.get("publish_mode"),
        "publish_branch": index_payload.get("publish_branch"),
        "output_dir": str(publish_dir),
        "index_html": str(publish_dir / "index.html"),
        "index_json": str(publish_dir / "index.json"),
        "review_markdown": str(publish_dir / "review.md"),
        "serve_command": serve_command,
        "runs": runs,
    }


def desktop_review_issue_draft(
    review_package: dict,
    *,
    package_path: Path,
    title: str | None = None,
    repo: str | None = None,
) -> dict:
    package_dir = package_path.parent
    issue_title = title or f"Review desktop validation video proof: {review_package.get('label') or package_dir.name}"
    attachments: list[dict] = []
    fallback_links: list[dict] = []
    body_lines = [
        f"# {issue_title}",
        "",
        "Desktop validation video proof is ready for human review.",
        "",
        "## What to review",
        "",
        "- Watch the attached MP4 when an attachment is listed below.",
        "- If the MP4 is too large or unavailable, use the served report link from the fallback section.",
        "- Comment `looks good to me` when the proof is accepted. The local verdict command can then mark the run approved and the review issue can be closed.",
        "",
        "## Report",
        "",
        f"- Local report: `{review_package.get('index_html') or package_dir / 'index.html'}`",
        f"- Review markdown: `{review_package.get('review_markdown') or package_dir / 'review.md'}`",
    ]
    serve_command = review_package.get("serve_command")
    if serve_command:
        body_lines.append(f"- Serve command: `{serve_command}`")
    body_lines.extend(["", "## Runs", ""])
    for index, run in enumerate(review_package.get("runs") or [], start=1):
        attachment = run.get("attachment") if isinstance(run.get("attachment"), dict) else {}
        fallback = run.get("fallback") if isinstance(run.get("fallback"), dict) else {}
        context = run.get("context") if isinstance(run.get("context"), dict) else {}
        status = attachment.get("status") or "fallback-link"
        attach_path = attachment.get("path") if isinstance(attachment.get("path"), str) else None
        body_lines.extend(
            [
                f"### {index}. {run.get('target') or '?'}/{run.get('action') or '?'} - {run.get('label') or '?'}",
                "",
                f"- Template: `{run.get('template') or 'not recorded'}`",
                f"- Attachment decision: `{status}`",
                f"- Attachment reason: {attachment.get('reason') or 'not recorded'}",
            ]
        )
        for key, value in list(context.items())[:8]:
            body_lines.append(f"- Context {key}: `{value}`")
        for note in (run.get("notes") or [])[:5]:
            body_lines.append(f"- Proof note: {note}")
        if attach_path and status in {"attach-primary", "attach-small"}:
            item = {
                "run_index": index,
                "status": status,
                "path": attach_path,
                "relative_path": attachment.get("relative_path"),
                "size_bytes": attachment.get("size_bytes"),
                "budget_bytes": attachment.get("budget_bytes"),
                "reason": attachment.get("reason"),
            }
            attachments.append(item)
            body_lines.append(f"- Attach MP4: `{attach_path}`")
        else:
            fallback_item = {
                "run_index": index,
                "status": status,
                "report_path": fallback.get("report_path") or review_package.get("index_html"),
                "review_markdown": fallback.get("review_markdown") or review_package.get("review_markdown"),
                "serve_command": fallback.get("serve_command") or serve_command,
                "internal_ephemeral": bool(fallback.get("internal_ephemeral", True)),
                "reason": attachment.get("reason"),
            }
            fallback_links.append(fallback_item)
            body_lines.append("- Attach MP4: not available within budget; use the served report link.")
        body_lines.append("")
    body_lines.extend(
        [
            "## Closeout",
            "",
            "After a reviewer comments `looks good to me`, run the matching `desktop verdict ... --approved --issue-url <issue-url>` command from `review.md`, then close this review issue.",
            "",
        ]
    )
    draft: dict = {
        "kind": "desktop-video-proof-github-issue-draft",
        "title": issue_title,
        "body": "\n".join(body_lines),
        "body_file": str(package_dir / "github-issue.md"),
        "json_file": str(package_dir / "github-issue.json"),
        "review_package": str(package_path),
        "attachments": attachments,
        "fallback_links": fallback_links,
        "close_trigger": "looks good to me",
    }
    if repo:
        draft["repo"] = repo
        draft["create_command"] = f"gh issue create --repo {repo} --title {json.dumps(issue_title)} --body-file {draft['body_file']}"
    else:
        draft["create_command"] = f"gh issue create --title {json.dumps(issue_title)} --body-file {draft['body_file']}"
    return draft


def stage_desktop_publish_report(
    config: dict,
    manifests: list[dict],
    *,
    output_dir: Path | None = None,
    label: str | None = None,
    create_desktop_publish_bundle_fn: Callable[[dict], Path],
    now_iso_fn: Callable[[], str],
    atomic_write_text_fn: Callable[[Path, str], None],
    write_desktop_publish_rollups_fn: Callable[[dict], None],
    publish_report_to_branch_fn: Callable[[dict, dict], dict],
) -> dict:
    if not manifests:
        raise ValueError("Desktop publish requires at least one run manifest.")

    publish_dir = output_dir.expanduser() if output_dir else create_desktop_publish_bundle_fn(config)
    publish_dir.mkdir(parents=True, exist_ok=True)
    assets_root = publish_dir / "assets"
    assets_root.mkdir(parents=True, exist_ok=True)

    published_runs: list[dict] = []
    for index, manifest in enumerate(manifests, start=1):
        run_slug = "-".join(
            [
                f"run-{index:02d}",
                slugify_token(str(manifest.get("target", "target"))),
                slugify_token(str(manifest.get("action", "run"))),
                slugify_token(str(manifest.get("label", "artifact"))),
            ]
        )
        run_dir = assets_root / run_slug
        run_dir.mkdir(parents=True, exist_ok=True)

        copied_artifacts: dict[str, str | dict | None] = {}
        for key in ARTIFACT_KEYS:
            path_str = manifest.get("artifacts", {}).get(key)
            if not path_str:
                continue
            source = Path(path_str).expanduser()
            if not source.exists():
                continue
            destination = run_dir / source.name
            shutil.copy2(source, destination)
            copied_artifacts[key] = str(destination.relative_to(publish_dir))

        bundle_dir = Path(manifest.get("artifacts", {}).get("bundle_dir", "")).expanduser()
        manifest_path = bundle_dir / "manifest.json"
        if manifest_path.exists():
            destination = run_dir / "manifest.json"
            shutil.copy2(manifest_path, destination)
            copied_artifacts["manifest"] = str(destination.relative_to(publish_dir))

        if manifest.get("artifacts", {}).get("image_change"):
            copied_artifacts["image_change"] = manifest["artifacts"]["image_change"]

        video_proof_composition = manifest.get("video_proof_composition") if isinstance(manifest.get("video_proof_composition"), dict) else {}
        source_image = video_proof_composition.get("source_image") if video_proof_composition else None
        if _copy_optional_file(source_image, run_dir / "source-reference" / Path(str(source_image)).name):
            copied_artifacts["video_source_image"] = str((run_dir / "source-reference" / Path(str(source_image)).name).relative_to(publish_dir))

        published_runs.append(
            {
                "target": manifest.get("target"),
                "action": manifest.get("action"),
                "label": manifest.get("label"),
                "completed_at": manifest.get("completed_at"),
                "bundle_dir": manifest.get("artifacts", {}).get("bundle_dir"),
                "interaction_mode": (manifest.get("interaction") or {}).get("mode"),
                "video_proof_notes": _proof_notes_from_manifest(manifest),
                "video_proof_composition": video_proof_composition,
                "artifacts": copied_artifacts,
            }
        )

    index_payload = {
        "generated_at": now_iso_fn(),
        "label": label or "desktop-publish",
        "publish_mode": config["desktop_automation"]["publish_mode"],
        "publish_branch": config["desktop_automation"]["publish_branch"],
        "run_count": len(published_runs),
        "runs": published_runs,
    }

    index_json = publish_dir / "index.json"
    atomic_write_text_fn(index_json, json.dumps(index_payload, indent=2) + "\n")
    review_markdown = publish_dir / "review.md"
    atomic_write_text_fn(review_markdown, desktop_review_issue_body(index_payload, publish_dir=publish_dir))
    review_package = publish_dir / "review-package.json"
    review_package_payload = desktop_review_package(index_payload, publish_dir=publish_dir)
    atomic_write_text_fn(review_package, json.dumps(review_package_payload, indent=2) + "\n")

    cards: list[str] = []
    for run in published_runs:
        artifacts = run["artifacts"]
        screenshot = artifacts.get("screenshot")
        before = artifacts.get("before_screenshot")
        diff = artifacts.get("diff_screenshot")
        video = artifacts.get("video_composed") or artifacts.get("video")
        video_metadata = artifacts.get("video_issue_metadata") or artifacts.get("video_composed_metadata") or artifacts.get("video_metadata")
        proof_notes = run.get("video_proof_notes") if isinstance(run.get("video_proof_notes"), list) else []
        proof_composition = run.get("video_proof_composition") if isinstance(run.get("video_proof_composition"), dict) else {}
        proof_focus = _proof_focus_summary(proof_composition)
        action_marker = _proof_action_marker_summary(proof_composition)
        context_items = _proof_context_items(proof_composition)
        meta_lines = [
            f"<div><strong>{html.escape(str(run.get('target') or '?'))}/{html.escape(str(run.get('action') or '?'))}</strong></div>",
            f"<div>{html.escape(str(run.get('label') or '?'))}</div>",
        ]
        if run.get("completed_at"):
            meta_lines.append(f"<div>{html.escape(str(run['completed_at']))}</div>")
        if run.get("interaction_mode"):
            meta_lines.append(f"<div>interaction: {html.escape(str(run['interaction_mode']))}</div>")
        if artifacts.get("image_change"):
            meta_lines.append(
                f"<div>image_change: {html.escape(json.dumps(artifacts['image_change'], sort_keys=True))}</div>"
            )
        if video_metadata:
            meta_lines.append(f"<div><a href=\"{html.escape(str(video_metadata))}\">video metadata</a></div>")
        if proof_composition.get("template"):
            meta_lines.append(f"<div>template: {html.escape(str(proof_composition.get('template')))}</div>")
        if proof_composition.get("source_label"):
            meta_lines.append(f"<div>source: {html.escape(str(proof_composition.get('source_label')))}</div>")
        if proof_focus.get("label"):
            meta_lines.append(f"<div>focus: {html.escape(str(proof_focus['label']))}</div>")
        if proof_focus.get("content_point"):
            meta_lines.append(f"<div>focus_point: {html.escape(json.dumps(proof_focus['content_point'], sort_keys=True))}</div>")
        if action_marker:
            action_label = action_marker.get("label") or action_marker.get("kind") or "action"
            meta_lines.append(f"<div>action: {html.escape(str(action_label))}</div>")
        if action_marker.get("content_point"):
            meta_lines.append(f"<div>action_point: {html.escape(json.dumps(action_marker['content_point'], sort_keys=True))}</div>")
        for key, value in context_items[:8]:
            meta_lines.append(f"<div>context.{html.escape(key)}: {html.escape(value)}</div>")
        if proof_notes:
            meta_lines.append(
                "<ul class=\"proof-notes\">"
                + "".join(f"<li>{html.escape(str(note))}</li>" for note in proof_notes[:5])
                + "</ul>"
            )
        video_block = ""
        if video:
            video_block = (
                "<figure class=\"video-proof\">"
                "<figcaption>video proof</figcaption>"
                f"<video controls preload=\"metadata\" src=\"{html.escape(str(video))}\"></video>"
                "</figure>"
            )
        source_block = ""
        if artifacts.get("video_source_image"):
            source_block = (
                "<figure>"
                "<figcaption>source reference</figcaption>"
                f"<img src=\"{html.escape(str(artifacts['video_source_image']))}\" alt=\"source reference\" />"
                "</figure>"
            )
        image_blocks: list[str] = []
        for title, rel_path in (("before", before), ("after", screenshot), ("diff", diff)):
            if not rel_path:
                continue
            image_blocks.append(
                "<figure>"
                f"<figcaption>{html.escape(title)}</figcaption>"
                f"<img src=\"{html.escape(str(rel_path))}\" alt=\"{html.escape(title)}\" />"
                "</figure>"
            )
        cards.append(
            "<section class=\"run-card\">"
            + "".join(meta_lines)
            + video_block
            + "<div class=\"images\">"
            + source_block
            + "".join(image_blocks)
            + "</div></section>"
        )

    index_html = publish_dir / "index.html"
    atomic_write_text_fn(
        index_html,
        "\n".join(
            [
                "<!doctype html>",
                "<html><head><meta charset=\"utf-8\"><title>Pulp Desktop Automation Report</title>",
                "<style>body{font-family:-apple-system,BlinkMacSystemFont,Segoe UI,sans-serif;margin:24px;background:#111827;color:#e5e7eb}"
                " .run-card{border:1px solid #374151;border-radius:12px;padding:16px;margin:0 0 16px;background:#1f2937}"
                " .images{display:flex;gap:16px;flex-wrap:wrap;margin-top:12px}"
                " .proof-notes{margin:12px 0 0;padding-left:20px;color:#d1d5db}"
                " figure{margin:12px 0 0} figcaption{margin-bottom:8px;color:#9ca3af}"
                " img{max-width:320px;border-radius:8px;border:1px solid #374151;background:#000}"
                " video{max-width:min(960px,100%);border-radius:8px;border:1px solid #374151;background:#000}</style>",
                "</head><body>",
                f"<h1>{html.escape(index_payload['label'])}</h1>",
                f"<p>Generated at {html.escape(index_payload['generated_at'])} \u00b7 runs: {len(published_runs)}</p>",
                *cards,
                "</body></html>",
            ]
        )
        + "\n",
    )

    report = {
        "generated_at": index_payload["generated_at"],
        "label": index_payload["label"],
        "publish_mode": index_payload["publish_mode"],
        "publish_branch": index_payload["publish_branch"],
        "output_dir": str(publish_dir),
        "index_html": str(index_html),
        "index_json": str(index_json),
        "review_markdown": str(review_markdown),
        "review_package": str(review_package),
        "run_count": len(published_runs),
        "runs": published_runs,
    }
    write_desktop_publish_rollups_fn(config)
    if config["desktop_automation"]["publish_mode"] == "branch":
        report["published"] = publish_report_to_branch_fn(config, report)
    return report


def desktop_publish_reports(
    config: dict,
    *,
    limit: int | None = None,
    desktop_publish_root_fn: Callable[[dict], Path],
) -> list[dict]:
    root = desktop_publish_root_fn(config)
    reports: list[dict] = []
    for publish_dir in sorted((p for p in root.iterdir() if p.is_dir()), reverse=True):
        index_json = publish_dir / "index.json"
        index_html = publish_dir / "index.html"
        if not index_json.exists():
            continue
        try:
            payload = json.loads(index_json.read_text())
        except json.JSONDecodeError:
            continue
        payload["output_dir"] = str(publish_dir)
        payload.setdefault("index_json", str(index_json))
        payload.setdefault("index_html", str(index_html))
        review_markdown = publish_dir / "review.md"
        if review_markdown.exists():
            payload.setdefault("review_markdown", str(review_markdown))
        review_package = publish_dir / "review-package.json"
        if review_package.exists():
            payload.setdefault("review_package", str(review_package))
        reports.append(payload)
    reports.sort(key=lambda item: item.get("generated_at") or "", reverse=True)
    if limit is not None:
        reports = reports[:limit]
    return reports


def write_desktop_publish_rollups(
    config: dict,
    *,
    desktop_publish_root_fn: Callable[[dict], Path],
    desktop_publish_reports_fn: Callable[..., list[dict]],
    atomic_write_text_fn: Callable[[Path, str], None],
) -> None:
    root = desktop_publish_root_fn(config)
    reports = desktop_publish_reports_fn(config)
    latest_report = reports[0] if reports else None
    atomic_write_text_fn(root / "latest-report.json", json.dumps(latest_report, indent=2) + "\n")
    reports_jsonl = "".join(json.dumps(report, sort_keys=True) + "\n" for report in reports)
    atomic_write_text_fn(root / "reports.jsonl", reports_jsonl)


def desktop_run_manifests(
    config: dict,
    *,
    target_name: str | None = None,
    action: str | None = None,
    desktop_artifact_root_fn: Callable[[dict], Path],
) -> list[dict]:
    root = desktop_artifact_root_fn(config)
    manifests: list[dict] = []
    target_names = [target_name] if target_name else sorted(p.name for p in root.iterdir() if p.is_dir())
    for target in target_names:
        target_dir = root / target
        if not target_dir.is_dir():
            continue
        action_names = [action] if action else sorted(p.name for p in target_dir.iterdir() if p.is_dir())
        for action_name in action_names:
            action_dir = target_dir / action_name
            if not action_dir.is_dir():
                continue
            for bundle_dir in sorted((p for p in action_dir.iterdir() if p.is_dir()), reverse=True):
                manifest_path = bundle_dir / "manifest.json"
                if not manifest_path.exists():
                    continue
                try:
                    manifest = json.loads(manifest_path.read_text())
                except json.JSONDecodeError:
                    continue
                manifest.setdefault("artifacts", {})
                manifest["artifacts"].setdefault("bundle_dir", str(bundle_dir))
                manifests.append(manifest)
    manifests.sort(key=lambda item: item.get("completed_at") or item.get("started_at") or "", reverse=True)
    return manifests


def normalize_desktop_proof_source_mode(mode: str | None) -> str:
    value = (mode or "legacy").strip().lower().replace("_", "-")
    if value not in {"live", "exact-sha", "legacy"}:
        raise ValueError(f"Invalid desktop proof source mode '{mode}'. Use one of: live, exact-sha, legacy.")
    return value


def desktop_manifest_adapter(config: dict, manifest: dict) -> str:
    adapter = str(manifest.get("adapter") or "").strip()
    if adapter:
        return adapter
    target_name = manifest.get("target")
    targets = config.get("desktop_automation", {}).get("targets", {})
    target_cfg = targets.get(target_name) if isinstance(targets, dict) else None
    if isinstance(target_cfg, dict):
        return str(target_cfg.get("adapter") or "unknown")
    return "unknown"


def desktop_manifest_run_status(manifest: dict) -> str:
    for key in ("agent_status", "status"):
        value = str(manifest.get(key) or "").strip()
        if value:
            return value.lower()
    return "pass"


def desktop_manifest_source(manifest: dict) -> dict:
    raw = manifest.get("source")
    if not isinstance(raw, dict):
        return {
            "mode": "legacy",
            "branch": None,
            "sha": None,
            "prepare_command": None,
            "prepare_timeout_secs": None,
            "prepared_root": None,
            "launch_cwd": None,
        }
    mode = raw.get("mode")
    try:
        normalized_mode = normalize_desktop_proof_source_mode(mode)
    except ValueError:
        normalized_mode = "legacy"
    return {
        "mode": normalized_mode,
        "branch": raw.get("branch"),
        "sha": raw.get("sha"),
        "prepare_command": raw.get("prepare_command"),
        "prepare_timeout_secs": raw.get("prepare_timeout_secs"),
        "prepared_root": raw.get("prepared_root"),
        "launch_cwd": raw.get("launch_cwd"),
    }


def desktop_proof_scope_for_adapter(adapter: str) -> str:
    if adapter in {"linux-xvfb", "windows-session-agent"}:
        return "live-host"
    if adapter == "macos-local":
        return "local-session"
    return "unknown"


def desktop_run_summary(config: dict, manifest: dict) -> dict:
    artifacts = manifest.get("artifacts", {})
    source = desktop_manifest_source(manifest)
    adapter = desktop_manifest_adapter(config, manifest)
    return {
        "target": manifest.get("target"),
        "action": manifest.get("action", "run"),
        "label": manifest.get("label", manifest.get("action", "run")),
        "adapter": adapter,
        "proof_scope": desktop_proof_scope_for_adapter(adapter),
        "run_status": desktop_manifest_run_status(manifest),
        "completed_at": manifest.get("completed_at") or manifest.get("started_at") or "?",
        "interaction_mode": (manifest.get("interaction") or {}).get("mode"),
        "host": manifest.get("host"),
        "source": source,
        "artifacts": {
            "bundle_dir": artifacts.get("bundle_dir"),
            "screenshot": artifacts.get("screenshot"),
            "before_screenshot": artifacts.get("before_screenshot"),
            "diff_screenshot": artifacts.get("diff_screenshot"),
            "ui_snapshot": artifacts.get("ui_snapshot"),
            "video": artifacts.get("video"),
            "video_composed": artifacts.get("video_composed"),
            "video_issue": artifacts.get("video_issue"),
            "video_small": artifacts.get("video_small"),
            "video_metadata": artifacts.get("video_metadata"),
            "video_composed_metadata": artifacts.get("video_composed_metadata"),
            "video_issue_metadata": artifacts.get("video_issue_metadata"),
            "video_small_metadata": artifacts.get("video_small_metadata"),
            "video_poster": artifacts.get("video_poster"),
            "stdout": artifacts.get("stdout"),
            "stderr": artifacts.get("stderr"),
            "agent_manifest": artifacts.get("agent_manifest"),
            "image_change": artifacts.get("image_change"),
        },
    }


def desktop_proof_summaries(
    config: dict,
    *,
    target_name: str | None = None,
    action: str | None = None,
    source_mode: str | None = None,
    sha: str | None = None,
    branch: str | None = None,
    limit: int | None = None,
    desktop_run_manifests_fn: Callable[..., list[dict]],
    desktop_run_summary_fn: Callable[[dict, dict], dict],
) -> list[dict]:
    manifests = desktop_run_manifests_fn(config, target_name=target_name, action=action)
    summaries: dict[tuple[str | None, str, str, str | None], dict] = {}
    requested_mode = normalize_desktop_proof_source_mode(source_mode) if source_mode else None
    for manifest in manifests:
        run_summary = desktop_run_summary_fn(config, manifest)
        if run_summary["run_status"] != "pass":
            continue
        source = run_summary["source"]
        if requested_mode and source["mode"] != requested_mode:
            continue
        if sha and source.get("sha") != sha:
            continue
        if branch and source.get("branch") != branch:
            continue
        key = (
            run_summary.get("target"),
            run_summary.get("action"),
            source.get("mode", "legacy"),
            source.get("sha"),
        )
        existing = summaries.get(key)
        if existing is None:
            summaries[key] = {
                "key": {
                    "target": run_summary.get("target"),
                    "action": run_summary.get("action"),
                    "source_mode": source.get("mode", "legacy"),
                    "sha": source.get("sha"),
                },
                "target": run_summary.get("target"),
                "action": run_summary.get("action"),
                "adapter": run_summary.get("adapter"),
                "proof_scope": run_summary.get("proof_scope"),
                "host": run_summary.get("host"),
                "source": source,
                "interaction_mode": run_summary.get("interaction_mode"),
                "run_count": 1,
                "latest_run": run_summary,
            }
            continue
        existing["run_count"] += 1
    ordered = sorted(
        summaries.values(),
        key=lambda item: item.get("latest_run", {}).get("completed_at") or "",
        reverse=True,
    )
    if limit is not None:
        ordered = ordered[:limit]
    return ordered


def desktop_rollup_dir(
    config: dict,
    target_name: str | None = None,
    *,
    desktop_artifact_root_fn: Callable[[dict], Path],
) -> Path:
    root = desktop_artifact_root_fn(config)
    if target_name:
        path = root / target_name
        path.mkdir(parents=True, exist_ok=True)
        return path
    return root


def write_desktop_run_rollups(
    config: dict,
    *,
    target_name: str | None = None,
    desktop_rollup_dir_fn: Callable[..., Path],
    desktop_run_manifests_fn: Callable[..., list[dict]],
    desktop_run_summary_fn: Callable[[dict, dict], dict],
    desktop_proof_summaries_fn: Callable[..., list[dict]],
    atomic_write_text_fn: Callable[[Path, str], None],
) -> None:
    rollup_dir = desktop_rollup_dir_fn(config, target_name)
    manifests = desktop_run_manifests_fn(config, target_name=target_name)
    summaries = [desktop_run_summary_fn(config, manifest) for manifest in manifests]
    latest_run = summaries[0] if summaries else None
    latest_proof_matches = desktop_proof_summaries_fn(config, target_name=target_name, limit=1)
    latest_proof = latest_proof_matches[0] if latest_proof_matches else None
    atomic_write_text_fn(rollup_dir / "latest-run.json", json.dumps(latest_run, indent=2) + "\n")
    atomic_write_text_fn(rollup_dir / "latest-proof.json", json.dumps(latest_proof, indent=2) + "\n")
    jsonl_payload = "".join(json.dumps(summary, sort_keys=True) + "\n" for summary in summaries)
    atomic_write_text_fn(rollup_dir / "runs.jsonl", jsonl_payload)


def prune_desktop_run_manifests(
    config: dict,
    *,
    target_name: str | None = None,
    older_than_days: int | None = None,
    keep_last: int | None = None,
    desktop_run_manifests_fn: Callable[..., list[dict]],
) -> list[Path]:
    manifests = desktop_run_manifests_fn(config, target_name=target_name)
    if keep_last is not None:
        manifests = manifests[keep_last:]
    if older_than_days is not None:
        cutoff = time.time() - (older_than_days * 86400)
        filtered: list[dict] = []
        for manifest in manifests:
            completed_at = manifest.get("completed_at") or manifest.get("started_at")
            if not completed_at:
                continue
            try:
                timestamp = datetime.fromisoformat(completed_at.replace("Z", "+00:00")).timestamp()
            except ValueError:
                continue
            if timestamp <= cutoff:
                filtered.append(manifest)
        manifests = filtered
    to_remove: list[Path] = []
    for manifest in manifests:
        bundle_dir = Path(manifest.get("artifacts", {}).get("bundle_dir", "")).expanduser()
        if bundle_dir.is_dir():
            to_remove.append(bundle_dir)
    seen: set[Path] = set()
    ordered: list[Path] = []
    for path in to_remove:
        if path in seen:
            continue
        seen.add(path)
        ordered.append(path)
    return ordered
