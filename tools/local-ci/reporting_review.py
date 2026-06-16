"""GitHub review-issue draft for desktop video proofs.

Builds the human-facing review issue (body, package, and draft) that links the
served proof report and the approve / needs-work verdict commands a reviewer
runs back. Pure string/JSON assembly over the proof index payload.
"""

from __future__ import annotations

import html
import json
from pathlib import Path
import shlex

from reporting_video import (
    GITHUB_VIDEO_ATTACHMENT_EXTENSIONS,
    _artifact_metadata,
    _desktop_report_serve_commands,
    _format_bytes,
    _github_video_attachment_policy,
    _proof_action_marker_summary,
    _proof_context_items,
    _proof_focus_label,
    _proof_storyboard_from_metadata,
    _proof_storyboard_lines,
    _source_summary,
)


def _review_manifest_path_for_run(run: dict) -> Path | None:
    bundle_dir = run.get("bundle_dir")
    if bundle_dir:
        return Path(str(bundle_dir)) / "manifest.json"
    manifest_info = run.get("manifest") if isinstance(run.get("manifest"), dict) else {}
    manifest_path = manifest_info.get("path")
    if manifest_path:
        return Path(str(manifest_path))
    return None


def _review_status_command_for_manifest(manifest_path: Path | None, *, repo: str | None = None) -> str:
    command = "python3 tools/local-ci/local_ci.py desktop review-status <issue-url>"
    if repo:
        command += f" --repo {shlex.quote(repo)}"
    if manifest_path:
        command += f" --manifest {shlex.quote(str(manifest_path))} --close-issue"
    return command


def _review_approved_verdict_command_for_manifest(manifest_path: Path | None) -> str:
    manifest = shlex.quote(str(manifest_path)) if manifest_path else "<manifest.json>"
    return f"python3 tools/local-ci/local_ci.py desktop verdict {manifest} --approved --issue-url <issue-url>"


def _review_needs_work_verdict_command_for_manifest(manifest_path: Path | None) -> str:
    manifest = shlex.quote(str(manifest_path)) if manifest_path else "<manifest.json>"
    return (
        f"python3 tools/local-ci/local_ci.py desktop verdict {manifest} "
        '--needs-work --issue-url <issue-url> --comment-issue --notes "<what to change>"'
    )


def desktop_review_issue_body(index_payload: dict, *, publish_dir: Path) -> str:
    serve_commands = _desktop_report_serve_commands(index_payload, publish_dir)
    serve_command = serve_commands["serve_command"]
    serve_background_command = serve_commands["serve_background_command"]
    serve_status_command = serve_commands["serve_status_command"]
    serve_stop_command = serve_commands["serve_stop_command"]
    published_cleanup_command = serve_commands["published_cleanup_command"]
    serve_urls = [str(url) for url in index_payload.get("serve_urls", []) if url]
    lines = [
        f"# {index_payload['label']}",
        "",
        "Desktop validation proof report is ready for review.",
        "",
        "## Review",
        "",
        f"- Open local report: `{publish_dir / 'index.html'}`",
        f"- Serve over local/Tailscale HTTP: `{serve_command}`",
        f"- Start background server: `{serve_background_command}`",
        f"- Check server: `{serve_status_command}`",
        f"- Stop server: `{serve_stop_command}`",
        f"- Cleanup old published reports after review: `{published_cleanup_command}`",
        "- Served URL: `desktop serve` prints candidate URLs, including localhost, configured public hosts, and Tailscale IPs when available.",
        "- Friendly Tailnet name: set `PULP_DESKTOP_SERVE_HOSTS=<name-or-ip>` before running `desktop serve` if reviewers should tap a stable host name.",
        "- Reviewer verdict: comment `looks good to me` when the proof is accepted, or describe the mismatch and run label when changes are needed.",
    ]
    for url in serve_urls:
        lines.append(f"- Candidate watch URL: `{url}`")
    lines.extend(["", "## Runs", ""])
    for run in index_payload.get("runs", []):
        artifacts = run.get("artifacts") or {}
        proof_notes = run.get("video_proof_notes") if isinstance(run.get("video_proof_notes"), list) else []
        proof_composition = run.get("video_proof_composition") if isinstance(run.get("video_proof_composition"), dict) else {}
        focus_label = _proof_focus_label(proof_composition)
        action_marker = _proof_action_marker_summary(proof_composition)
        action_label = action_marker.get("label") or action_marker.get("kind")
        context_items = _proof_context_items(proof_composition)
        source_summary = _source_summary(run.get("source") if isinstance(run.get("source"), dict) else {})
        video = artifacts.get("video_issue") or artifacts.get("video_composed") or artifacts.get("video")
        metadata_path = artifacts.get("video_issue_metadata") or artifacts.get("video_composed_metadata") or artifacts.get("video_metadata")
        metadata = _artifact_metadata(publish_dir, metadata_path)
        composed_metadata = _artifact_metadata(publish_dir, artifacts.get("video_composed_metadata"))
        storyboard = _proof_storyboard_from_metadata(composed_metadata)
        storyboard_lines = _proof_storyboard_lines(storyboard)
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
        verdict_manifest = _review_manifest_path_for_run(run)
        lines.extend(
            [
                f"### {run.get('target') or '?'}/{run.get('action') or '?'} - {run.get('label') or '?'}",
                "",
                f"- Completed: {run.get('completed_at') or '?'}",
                f"- Command: `{run.get('command')}`" if run.get("command") else "- Command: not recorded",
                f"- Source: `{source_summary}`" if source_summary else "- Source: not recorded",
                f"- Host: `{run.get('host')}`" if run.get("host") else "- Host: local/default",
                f"- Adapter: `{run.get('adapter')}`" if run.get("adapter") else "- Adapter: not recorded",
                f"- Interaction: {run.get('interaction_mode') or 'not recorded'}",
                f"- Proof template: `{proof_composition.get('template')}`" if proof_composition.get("template") else "- Proof template: not recorded",
                f"- Focus component: `{focus_label}`" if focus_label else "- Focus component: not recorded",
                f"- Action marker: `{action_label}`" if action_label else "- Action marker: not recorded",
                f"- Action point: `{json.dumps(action_marker['content_point'], sort_keys=True)}`" if action_marker.get("content_point") else "- Action point: not recorded",
                f"- Source reference: `{artifacts['video_source_image']}`" if artifacts.get("video_source_image") else "- Source reference: not attached",
                f"- Visual diff reference: `{artifacts['video_diff_image']}`" if artifacts.get("video_diff_image") else "- Visual diff reference: not attached",
                f"- Issue video: `{artifacts['video_issue']}`" if artifacts.get("video_issue") else "- Issue video: not generated",
                f"- Small video: `{artifacts['video_small']}`" if artifacts.get("video_small") else "- Small video: not generated",
                f"- Review video: `{artifacts.get('video_composed') or artifacts.get('video')}`" if artifacts.get("video_composed") or artifacts.get("video") else "- Review video: not recorded",
                f"- Video size: {_format_bytes(size_bytes)} ({attach_status})",
                f"- Small video size: {_format_bytes(small_size_bytes)}" + (" (fits 10 MB budget)" if small_fits is True else " (over 10 MB budget)" if small_fits is False else "") if artifacts.get("video_small") else "- Small video size: not recorded",
                f"- Issue variant: `{issue_status}`" + (f" via `{selected_attempt}`" if selected_attempt else "") if issue_status else "- Issue variant: not recorded",
                f"- Small variant: `{small_status}`" + (f" via `{small_selected_attempt}`" if small_selected_attempt else "") if small_status else "- Small variant: not recorded",
                f"- Attachment action: {attach_action}",
                f"- Manifest: `{artifacts['manifest']}`" if artifacts.get("manifest") else "- Manifest: not copied",
                f"- Review status command: `{_review_status_command_for_manifest(verdict_manifest)}`",
                f"- Approve command: `{_review_approved_verdict_command_for_manifest(verdict_manifest)}`",
                f"- Needs-work command: `{_review_needs_work_verdict_command_for_manifest(verdict_manifest)}`",
            ]
        )
        if storyboard_lines:
            lines.append("- Storyboard:")
            for story_line in storyboard_lines:
                lines.append(f"  - {story_line}")
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
    serve_commands = _desktop_report_serve_commands(index_payload, publish_dir)
    serve_label = serve_commands["serve_label"]
    serve_command = serve_commands["serve_command"]
    serve_background_command = serve_commands["serve_background_command"]
    serve_status_command = serve_commands["serve_status_command"]
    serve_stop_command = serve_commands["serve_stop_command"]
    published_cleanup_command = serve_commands["published_cleanup_command"]
    serve_urls = [str(url) for url in index_payload.get("serve_urls", []) if url]
    serve_verification = index_payload.get("serve_verification") if isinstance(index_payload.get("serve_verification"), dict) else {}
    runs: list[dict] = []
    for run in index_payload.get("runs", []):
        artifacts = run.get("artifacts") or {}
        metadata_path = artifacts.get("video_issue_metadata") or artifacts.get("video_composed_metadata") or artifacts.get("video_metadata")
        small_metadata_path = artifacts.get("video_small_metadata")
        metadata = _artifact_metadata(publish_dir, metadata_path)
        composed_metadata = _artifact_metadata(publish_dir, artifacts.get("video_composed_metadata"))
        storyboard = _proof_storyboard_from_metadata(composed_metadata)
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
                "adapter": run.get("adapter"),
                "host": run.get("host"),
                "repo_path": run.get("repo_path"),
                "command": run.get("command"),
                "source": run.get("source") if isinstance(run.get("source"), dict) else {},
                "template": proof_composition.get("template"),
                "storyboard": storyboard,
                "context": proof_composition.get("context") if isinstance(proof_composition.get("context"), dict) else {},
                "notes": run.get("video_proof_notes") if isinstance(run.get("video_proof_notes"), list) else [],
                "manifest": {
                    "path": str(publish_dir / str(artifacts["manifest"])) if artifacts.get("manifest") else None,
                    "relative_path": artifacts.get("manifest"),
                },
                "attachment": attachment,
                "fallback": {
                    "report_path": str(publish_dir / "index.html"),
                    "review_markdown": str(publish_dir / "review.md"),
                    "serve_label": serve_label,
                    "serve_command": serve_command,
                    "serve_background_command": serve_background_command,
                    "serve_status_command": serve_status_command,
                    "serve_stop_command": serve_stop_command,
                    "published_cleanup_command": published_cleanup_command,
                    "serve_urls": serve_urls,
                    "serve_verification": serve_verification,
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
        "serve_label": serve_label,
        "serve_command": serve_command,
        "serve_background_command": serve_background_command,
        "serve_status_command": serve_status_command,
        "serve_stop_command": serve_stop_command,
        "published_cleanup_command": published_cleanup_command,
        "serve_urls": serve_urls,
        "serve_verification": serve_verification,
        "runs": runs,
    }


def desktop_review_issue_draft(
    review_package: dict,
    *,
    package_path: Path,
    title: str | None = None,
    repo: str | None = None,
    check_files: bool = False,
) -> dict:
    package_dir = package_path.parent
    issue_title = title or f"Review desktop validation video proof: {review_package.get('label') or package_dir.name}"
    attachment_policy = _github_video_attachment_policy()
    attachments: list[dict] = []
    fallback_links: list[dict] = []
    attachment_checks: list[dict] = []
    attachment_errors: list[str] = []
    body_lines = [
        f"# {issue_title}",
        "",
        "Desktop validation video proof is ready for human review.",
        "",
        "## What to review",
        "",
        "- Watch the attached video when an attachment is listed below.",
        "- If the video is too large, unsupported, or unavailable, use the served report link from the fallback section.",
        "- Comment `looks good to me` when the proof is accepted. The local verdict command can then mark the run approved and the review issue can be closed.",
        "- Comment `needs work`, `needs changes`, `needs another pass`, or `not approved` with specific notes when the proof needs another iteration. The local verdict command can then post the generated checklist back to this issue.",
        (
            "- GitHub issue/PR video uploads support "
            f"{', '.join(attachment_policy['supported_video_extensions'])}; "
            f"paid-plan video budget is {_format_bytes(attachment_policy['pro_video_limit_bytes'])}, "
            f"free-plan fallback budget is {_format_bytes(attachment_policy['free_video_limit_bytes'])}."
        ),
        "",
        "## Report",
        "",
        f"- Local report: `{review_package.get('index_html') or package_dir / 'index.html'}`",
        f"- Review markdown: `{review_package.get('review_markdown') or package_dir / 'review.md'}`",
    ]
    serve_command = review_package.get("serve_command")
    if serve_command:
        body_lines.append(f"- Serve command: `{serve_command}`")
    serve_background_command = review_package.get("serve_background_command")
    if serve_background_command:
        body_lines.append(f"- Background serve command: `{serve_background_command}`")
    serve_status_command = review_package.get("serve_status_command")
    if serve_status_command:
        body_lines.append(f"- Status command: `{serve_status_command}`")
    serve_stop_command = review_package.get("serve_stop_command")
    if serve_stop_command:
        body_lines.append(f"- Stop command: `{serve_stop_command}`")
    published_cleanup_command = review_package.get("published_cleanup_command")
    if published_cleanup_command:
        body_lines.append(f"- Published cleanup command: `{published_cleanup_command}`")
    serve_urls = [str(url) for url in review_package.get("serve_urls", []) if url]
    for url in serve_urls:
        body_lines.append(f"- Candidate watch URL: `{url}`")
    serve_verification = review_package.get("serve_verification") if isinstance(review_package.get("serve_verification"), dict) else {}
    if serve_verification:
        body_lines.append(
            f"- Watch URL verification: `{serve_verification.get('status') or 'unknown'}`"
            + (f" `{serve_verification.get('url')}`" if serve_verification.get("url") else "")
        )
    body_lines.extend(["", "## Runs", ""])
    for index, run in enumerate(review_package.get("runs") or [], start=1):
        attachment = run.get("attachment") if isinstance(run.get("attachment"), dict) else {}
        fallback = run.get("fallback") if isinstance(run.get("fallback"), dict) else {}
        context = run.get("context") if isinstance(run.get("context"), dict) else {}
        storyboard = run.get("storyboard") if isinstance(run.get("storyboard"), dict) else {}
        storyboard_lines = _proof_storyboard_lines(storyboard)
        source = run.get("source") if isinstance(run.get("source"), dict) else {}
        manifest_info = run.get("manifest") if isinstance(run.get("manifest"), dict) else {}
        review_manifest = _review_manifest_path_for_run(run)
        source_text = _source_summary(source)
        status = attachment.get("status") or "fallback-link"
        attach_path = attachment.get("path") if isinstance(attachment.get("path"), str) else None
        body_lines.extend(
            [
                f"### {index}. {run.get('target') or '?'}/{run.get('action') or '?'} - {run.get('label') or '?'}",
                "",
                f"- Template: `{run.get('template') or 'not recorded'}`",
                f"- Command: `{run.get('command')}`" if run.get("command") else "- Command: not recorded",
                f"- Source: `{source_text}`" if source_text else "- Source: not recorded",
                f"- Host: `{run.get('host')}`" if run.get("host") else "- Host: local/default",
                f"- Adapter: `{run.get('adapter')}`" if run.get("adapter") else "- Adapter: not recorded",
                f"- Manifest: `{manifest_info.get('path')}`" if manifest_info.get("path") else "- Manifest: not recorded",
                f"- Review status command: `{_review_status_command_for_manifest(review_manifest, repo=repo)}`",
                f"- Attachment decision: `{status}`",
                f"- Attachment reason: {attachment.get('reason') or 'not recorded'}",
            ]
        )
        if storyboard_lines:
            body_lines.append("- Storyboard:")
            for story_line in storyboard_lines:
                body_lines.append(f"  - {story_line}")
        for key, value in list(context.items())[:8]:
            body_lines.append(f"- Context {key}: `{value}`")
        for note in (run.get("notes") or [])[:5]:
            body_lines.append(f"- Proof note: {note}")
        if attach_path and status in {"attach-primary", "attach-small"}:
            extension = Path(attach_path).suffix.lower()
            supported_extension = extension in GITHUB_VIDEO_ATTACHMENT_EXTENSIONS
            if check_files:
                attachment_path = Path(attach_path).expanduser()
                exists = attachment_path.is_file()
                actual_size = attachment_path.stat().st_size if exists else None
                budget_bytes = attachment.get("budget_bytes")
                over_budget = isinstance(budget_bytes, int) and actual_size is not None and actual_size > budget_bytes
                check = {
                    "run_index": index,
                    "status": status,
                    "path": attach_path,
                    "exists": exists,
                    "size_bytes": actual_size,
                    "budget_bytes": budget_bytes,
                    "fits_attachment_budget": bool(exists and not over_budget),
                    "extension": extension,
                    "supported_video_extension": supported_extension,
                    "supported_video_extensions": list(GITHUB_VIDEO_ATTACHMENT_EXTENSIONS),
                }
                attachment_checks.append(check)
                if not supported_extension:
                    attachment_errors.append(
                        f"run {index} attachment has unsupported video extension: {attach_path} "
                        f"(supported: {', '.join(GITHUB_VIDEO_ATTACHMENT_EXTENSIONS)})"
                    )
                elif not exists:
                    attachment_errors.append(f"run {index} attachment missing: {attach_path}")
                elif over_budget:
                    attachment_errors.append(
                        f"run {index} attachment exceeds budget: {attach_path} "
                        f"({actual_size} bytes > {budget_bytes} bytes)"
                    )
            item = {
                "run_index": index,
                "status": status,
                "path": attach_path,
                "relative_path": attachment.get("relative_path"),
                "size_bytes": attachment.get("size_bytes"),
                "budget_bytes": attachment.get("budget_bytes"),
                "extension": extension,
                "supported_video_extension": supported_extension,
                "reason": attachment.get("reason"),
            }
            attachments.append(item)
            body_lines.append(f"- Attach video: `{attach_path}`")
        else:
            fallback_verification = fallback.get("serve_verification") if isinstance(fallback.get("serve_verification"), dict) else serve_verification
            fallback_verified = isinstance(fallback_verification, dict) and fallback_verification.get("status") == "ok"
            if check_files and not fallback_verified:
                attachment_errors.append(
                    f"run {index} fallback serve URL not verified; run the background serve command and retry review-issue --check-files"
                )
            fallback_item = {
                "run_index": index,
                "status": status,
                "report_path": fallback.get("report_path") or review_package.get("index_html"),
                "review_markdown": fallback.get("review_markdown") or review_package.get("review_markdown"),
                "serve_label": fallback.get("serve_label") or review_package.get("serve_label"),
                "serve_command": fallback.get("serve_command") or serve_command,
                "serve_background_command": fallback.get("serve_background_command") or serve_background_command,
                "serve_status_command": fallback.get("serve_status_command") or serve_status_command,
                "serve_stop_command": fallback.get("serve_stop_command") or serve_stop_command,
                "published_cleanup_command": fallback.get("published_cleanup_command") or published_cleanup_command,
                "serve_urls": fallback.get("serve_urls") or serve_urls,
                "serve_verification": fallback_verification,
                "serve_verified": fallback_verified,
                "internal_ephemeral": bool(fallback.get("internal_ephemeral", True)),
                "reason": attachment.get("reason"),
            }
            fallback_links.append(fallback_item)
            body_lines.append("- Attach video: not available within budget; use the served report link.")
            body_lines.append(f"- Served link verification: `{'ok' if fallback_verified else 'missing-or-failed'}`")
        body_lines.append("")
    if attachment_errors:
        raise ValueError("; ".join(attachment_errors))
    body_lines.extend(
        [
            "## Closeout",
            "",
            "After a reviewer comments `looks good to me` or requests changes with `needs work`, `needs changes`, `needs another pass`, or `not approved`, run `desktop review-status <issue-url>` to confirm the latest actionable trigger.",
            "",
            "For approval, run the suggested `desktop verdict ... --approved --issue-url <issue-url> --close-issue` command. For requested changes, run the suggested `desktop verdict ... --needs-work --issue-url <issue-url> --comment-issue --notes ...` command so the run manifest and this review issue stay aligned.",
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
        "serve_urls": serve_urls,
        "attachment_policy": attachment_policy,
        "close_trigger": "looks good to me",
        "needs_work_triggers": ["needs work", "needs changes", "needs another pass", "not approved"],
    }
    if check_files:
        draft["attachment_checks"] = attachment_checks
    if repo:
        draft["repo"] = repo
        draft["create_command"] = f"gh issue create --repo {repo} --title {json.dumps(issue_title)} --body-file {draft['body_file']}"
    else:
        draft["create_command"] = f"gh issue create --title {json.dumps(issue_title)} --body-file {draft['body_file']}"
    return draft


