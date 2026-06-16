"""Desktop video-proof review commands (human verdict on a run manifest)."""

from __future__ import annotations

import argparse
from collections.abc import Callable, Mapping
import json
import re
import shlex
from pathlib import Path
import subprocess
import time


def cmd_desktop_verdict(
    args: argparse.Namespace,
    *,
    now_iso_fn: Callable[[], str],
    atomic_write_text_fn: Callable[[Path, str], None],
    run_fn: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
    print_fn: Callable[[str], None] = print,
) -> int:
    manifest_path = Path(args.manifest).expanduser()
    if not manifest_path.is_file():
        print_fn(f"Error: desktop run manifest not found: {manifest_path}")
        return 1
    try:
        manifest = json.loads(manifest_path.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        print_fn(f"Error: could not read desktop run manifest: {exc}")
        return 1
    status = "approved" if getattr(args, "approved", False) else "needs-work"
    reviewed_at = now_iso_fn()
    run_label = str(manifest.get("label") or manifest_path.parent.name)
    review = {
        "status": status,
        "reviewed_at": reviewed_at,
    }
    if args.notes:
        review["notes"] = args.notes
    if args.reviewer:
        review["reviewer"] = args.reviewer
    if args.issue_url:
        review["issue_url"] = args.issue_url
    if status == "approved":
        review["close_review_issue"] = True
    else:
        review["close_review_issue"] = False
        review["follow_up_required"] = True
    markdown_path = manifest_path.parent / "review-verdict.md"
    json_path = manifest_path.parent / "review-verdict.json"
    if status == "approved":
        summary_comment = f"Approved desktop video proof `{run_label}`."
        if args.issue_url:
            summary_comment += f" Review issue: {args.issue_url}."
        if args.notes:
            summary_comment += f" Notes: {args.notes}"
        follow_up = None
    else:
        summary_comment = f"Desktop video proof `{run_label}` needs another iteration."
        if args.notes:
            summary_comment += f" Requested change: {args.notes}"
        follow_up = {
            "kind": "same-issue-checklist",
            "text": f"- [ ] Re-record `{run_label}` after addressing: {args.notes or 'reviewer feedback'}",
        }
    comment_lines = [summary_comment]
    if follow_up:
        comment_lines.extend(["", "Follow-up:", follow_up["text"]])
    issue_comment_body = "\n".join(comment_lines)
    comment_result = None
    if getattr(args, "comment_issue", False) and not getattr(args, "close_issue", False):
        if not args.issue_url:
            print_fn("Error: --comment-issue requires --issue-url")
            return 1
        comment_argv = [
            "gh",
            "issue",
            "comment",
            args.issue_url,
            "--body",
            issue_comment_body,
        ]
        comment_process = run_fn(comment_argv, capture_output=True, text=True, check=False)
        comment_result = {
            "command": comment_argv,
            "returncode": int(comment_process.returncode),
            "stdout": comment_process.stdout,
            "stderr": comment_process.stderr,
        }
        if comment_process.returncode != 0:
            detail = (comment_process.stderr or comment_process.stdout or "gh issue comment failed").strip()
            print_fn(f"Error: {detail}")
            return 1
    close_result = None
    if getattr(args, "close_issue", False):
        if status != "approved":
            print_fn("Error: --close-issue requires --approved")
            return 1
        if not args.issue_url:
            print_fn("Error: --close-issue requires --issue-url")
            return 1
        close_argv = [
            "gh",
            "issue",
            "close",
            args.issue_url,
            "--comment",
            issue_comment_body,
            "--reason",
            getattr(args, "close_reason", "completed") or "completed",
        ]
        close_process = run_fn(close_argv, capture_output=True, text=True, check=False)
        close_result = {
            "command": close_argv,
            "returncode": int(close_process.returncode),
            "stdout": close_process.stdout,
            "stderr": close_process.stderr,
        }
        if close_process.returncode != 0:
            detail = (close_process.stderr or close_process.stdout or "gh issue close failed").strip()
            print_fn(f"Error: {detail}")
            return 1
    verdict_payload = {
        "kind": "desktop-video-proof-verdict",
        "manifest": str(manifest_path),
        "status": status,
        "reviewed_at": reviewed_at,
        "reviewer": args.reviewer or None,
        "issue_url": args.issue_url or None,
        "notes": args.notes or None,
        "close_review_issue": bool(review["close_review_issue"]),
        "follow_up_required": bool(review.get("follow_up_required", False)),
        "summary_comment": summary_comment,
        "follow_up": follow_up,
        "issue_comment": comment_result,
        "issue_close": close_result,
    }
    markdown_lines = [
        f"# Desktop Video Proof Verdict: {status}",
        "",
        f"- Manifest: `{manifest_path}`",
        f"- Run: `{run_label}`",
        f"- Reviewed at: `{reviewed_at}`",
    ]
    if args.reviewer:
        markdown_lines.append(f"- Reviewer: `{args.reviewer}`")
    if args.issue_url:
        markdown_lines.append(f"- Review issue: {args.issue_url}")
    if args.notes:
        markdown_lines.append(f"- Notes: {args.notes}")
    markdown_lines.extend(
        [
            "",
            "## Issue Comment",
            "",
            summary_comment,
        ]
    )
    if follow_up:
        markdown_lines.extend(["", "## Follow-up", "", follow_up["text"]])
    if status == "approved":
        markdown_lines.extend(["", "## Closeout", "", "Close the review issue after posting the summary comment."])
    else:
        markdown_lines.extend(["", "## Closeout", "", "Keep the review issue open until a replacement proof is recorded."])
    atomic_write_text_fn(markdown_path, "\n".join(markdown_lines) + "\n")
    atomic_write_text_fn(json_path, json.dumps(verdict_payload, indent=2) + "\n")
    review["verdict_markdown"] = str(markdown_path)
    review["verdict_json"] = str(json_path)
    review["summary_comment"] = summary_comment
    review["issue_comment_body"] = issue_comment_body
    if follow_up:
        review["follow_up"] = follow_up
    if comment_result:
        review["issue_comment"] = comment_result
    if close_result:
        review["issue_close"] = close_result
    manifest["review"] = review
    atomic_write_text_fn(manifest_path, json.dumps(manifest, indent=2) + "\n")
    payload = {
        "manifest": str(manifest_path),
        "review": review,
        "verdict_markdown": str(markdown_path),
        "verdict_json": str(json_path),
    }
    if getattr(args, "json", False):
        print_fn(json.dumps(payload, indent=2))
    else:
        print_fn(f"Desktop proof verdict recorded: {status}")
        print_fn(f"  manifest: {manifest_path}")
        print_fn(f"  verdict_markdown: {markdown_path}")
        print_fn(f"  verdict_json: {json_path}")
        if args.issue_url:
            print_fn(f"  issue_url: {args.issue_url}")
        if comment_result:
            print_fn("  issue_comment: posted")
        if close_result:
            print_fn("  issue_close: closed")
    return 0


def _review_package_path(path_value: str) -> Path:
    path = Path(path_value).expanduser().resolve()
    if path.is_dir():
        path = path / "review-package.json"
    return path


def _manifest_paths_from_review_package(review_package: dict) -> list[str]:
    paths: list[str] = []
    for run in review_package.get("runs") or []:
        if not isinstance(run, dict):
            continue
        bundle_dir = run.get("bundle_dir")
        if bundle_dir:
            paths.append(str(Path(str(bundle_dir)) / "manifest.json"))
            continue
        manifest = run.get("manifest") if isinstance(run.get("manifest"), dict) else {}
        manifest_path = manifest.get("path")
        if manifest_path:
            paths.append(str(manifest_path))
    return sorted(set(paths))


def _github_issue_number_from_url(issue_url: str) -> str | None:
    match = re.search(r"/issues/(\d+)(?:$|[?#])", issue_url)
    return match.group(1) if match else None


def _review_issue_manifest_map(review_package: dict, issue_url: str) -> tuple[dict[str, str], str | None]:
    manifests = _manifest_paths_from_review_package(review_package)
    if not issue_url:
        return {}, "missing issue URL"
    if len(manifests) != 1:
        return {}, f"expected exactly one run manifest, found {len(manifests)}"
    manifest = manifests[0]
    mapping = {issue_url: manifest}
    issue_number = _github_issue_number_from_url(issue_url)
    if issue_number:
        mapping[issue_number] = manifest
        mapping[f"#{issue_number}"] = manifest
    return mapping, None


def _review_watch_command_for_manifest_map(
    manifest_map_path: Path,
    *,
    repo: str | None = None,
    label: str | None = None,
) -> str:
    command = "python3 tools/local-ci/local_ci.py desktop review-watch"
    if repo:
        command += f" --repo {shlex.quote(repo)}"
    if label:
        command += f" --label {shlex.quote(label)}"
    command += f" --manifest-map {shlex.quote(str(manifest_map_path))}"
    command += " --state-file /tmp/pulp-video-review-watch.json --close-issue"
    return command


def _review_create_command_for_manifest_map(args: argparse.Namespace, manifest_map_path: Path) -> str:
    command = (
        "python3 tools/local-ci/local_ci.py desktop review-issue "
        f"{shlex.quote(str(Path(args.path).expanduser()))}"
    )
    if args.repo:
        command += f" --repo {shlex.quote(args.repo)}"
    if getattr(args, "check_files", False):
        command += " --check-files"
    command += " --create"
    for label in getattr(args, "label", []) or []:
        command += f" --label {shlex.quote(label)}"
    for assignee in getattr(args, "assignee", []) or []:
        command += f" --assignee {shlex.quote(assignee)}"
    command += f" --manifest-map-output {shlex.quote(str(manifest_map_path))}"
    if args.title:
        command += f" --title {shlex.quote(args.title)}"
    if args.body_output:
        command += f" --body-output {shlex.quote(str(Path(args.body_output).expanduser()))}"
    if args.json_output:
        command += f" --json-output {shlex.quote(str(Path(args.json_output).expanduser()))}"
    return command


def cmd_desktop_review_issue(
    args: argparse.Namespace,
    *,
    desktop_review_issue_draft_fn: Callable[..., dict],
    atomic_write_text_fn: Callable[[Path, str], None],
    run_fn: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
    print_fn: Callable[[str], None] = print,
) -> int:
    package_path = _review_package_path(args.path)
    if not package_path.exists():
        print_fn(f"Error: review package not found: {package_path}")
        return 1
    try:
        review_package = json.loads(package_path.read_text())
    except json.JSONDecodeError as exc:
        print_fn(f"Error: invalid review package JSON: {exc}")
        return 1
    try:
        draft = desktop_review_issue_draft_fn(
            review_package,
            package_path=package_path,
            title=args.title,
            repo=args.repo,
            check_files=getattr(args, "check_files", False),
        )
    except ValueError as exc:
        print_fn(f"Error: {exc}")
        return 1
    body_path = Path(args.body_output).expanduser().resolve() if args.body_output else Path(draft["body_file"])
    json_path = Path(args.json_output).expanduser().resolve() if args.json_output else Path(draft["json_file"])
    manifest_map_path = Path(args.manifest_map_output).expanduser().resolve() if getattr(args, "manifest_map_output", None) else None
    draft["body_file"] = str(body_path)
    draft["json_file"] = str(json_path)
    if manifest_map_path:
        review_label = next(iter(getattr(args, "label", []) or []), "video-review")
        review_watch_command = _review_watch_command_for_manifest_map(
            manifest_map_path,
            repo=args.repo,
            label=review_label,
        )
        review_create_command = _review_create_command_for_manifest_map(args, manifest_map_path)
        draft["review_create_command"] = review_create_command
        draft["review_watch_command"] = review_watch_command
        draft["manifest_map"] = {
            "path": str(manifest_map_path),
            "entries": {},
            "error": None,
            "status": "pending-create" if getattr(args, "create", False) else "requires-create",
        }
        draft["body"] = (
            draft["body"].rstrip()
            + "\n\n## Batch Review Create\n\n"
            + "To create this issue and write the manifest map for review monitoring, use:\n\n"
            + f"`{review_create_command}`\n"
            + "\n## Batch Review Watch\n\n"
            + "After the issue exists and the manifest map is written, use:\n\n"
            + f"`{review_watch_command}`\n"
        )
    atomic_write_text_fn(body_path, draft["body"])
    create_result = None
    if getattr(args, "create", False):
        create_argv = ["gh", "issue", "create"]
        if args.repo:
            create_argv.extend(["--repo", args.repo])
        create_argv.extend(["--title", draft["title"], "--body-file", str(body_path)])
        for label in getattr(args, "label", []) or []:
            create_argv.extend(["--label", label])
        for assignee in getattr(args, "assignee", []) or []:
            create_argv.extend(["--assignee", assignee])
        process = run_fn(create_argv, capture_output=True, text=True, check=False)
        issue_url = ""
        if process.returncode == 0:
            issue_url = next((line.strip() for line in reversed((process.stdout or "").splitlines()) if line.strip()), "")
        create_result = {
            "command": create_argv,
            "returncode": int(process.returncode),
            "stdout": process.stdout,
            "stderr": process.stderr,
            "issue_url": issue_url or None,
        }
        draft["create_result"] = create_result
        if issue_url:
            draft["issue_url"] = issue_url
            if manifest_map_path:
                manifest_map, manifest_map_error = _review_issue_manifest_map(review_package, issue_url)
                draft["manifest_map"] = {
                    "path": str(manifest_map_path),
                    "entries": manifest_map,
                    "error": manifest_map_error,
                    "status": "written" if manifest_map else "skipped",
                }
                if manifest_map:
                    atomic_write_text_fn(manifest_map_path, json.dumps(manifest_map, indent=2) + "\n")
        if process.returncode != 0:
            atomic_write_text_fn(json_path, json.dumps(draft, indent=2) + "\n")
            detail = (process.stderr or process.stdout or "gh issue create failed").strip()
            print_fn(f"Error: {detail}")
            return 1
    atomic_write_text_fn(json_path, json.dumps(draft, indent=2) + "\n")
    if args.json:
        print_fn(json.dumps(draft, indent=2))
    else:
        print_fn("Desktop video review issue draft ready:")
        print_fn(f"  title: {draft['title']}")
        print_fn(f"  body_file: {draft['body_file']}")
        print_fn(f"  json_file: {draft['json_file']}")
        print_fn(f"  attachments: {len(draft.get('attachments') or [])}")
        print_fn(f"  fallback_links: {len(draft.get('fallback_links') or [])}")
        print_fn(f"  create_command: {draft['create_command']}")
        if create_result and create_result.get("issue_url"):
            print_fn(f"  issue_url: {create_result['issue_url']}")
        if draft.get("manifest_map"):
            manifest_map = draft["manifest_map"]
            if manifest_map.get("entries"):
                print_fn(f"  manifest_map: {manifest_map['path']}")
            elif manifest_map.get("error"):
                print_fn(f"  manifest_map: skipped ({manifest_map['error']})")
    return 0


def _review_status_action_comment(
    comments: list[dict],
    *,
    approval_trigger: str = "looks good to me",
    needs_work_triggers: tuple[str, ...] = ("needs work", "needs another pass", "needs changes", "not approved"),
) -> tuple[str | None, dict | None]:
    approval_trigger_lower = approval_trigger.lower()
    needs_work_triggers_lower = tuple(trigger.lower() for trigger in needs_work_triggers)
    for comment in reversed(comments):
        body = str(comment.get("body") or "")
        body_lower = body.lower()
        if approval_trigger_lower in body_lower:
            return "approved", comment
        if any(trigger in body_lower for trigger in needs_work_triggers_lower):
            return "needs-work", comment
    return None, None


def _review_status_approval_comment(comments: list[dict], trigger: str = "looks good to me") -> dict | None:
    action, comment = _review_status_action_comment(comments, approval_trigger=trigger)
    return comment if action == "approved" else None


def _review_comment_notes(comment: dict | None) -> str | None:
    if not comment:
        return None
    body = " ".join(str(comment.get("body") or "").split())
    if not body:
        return None
    if len(body) > 500:
        body = body[:497].rstrip() + "..."
    return body


def cmd_desktop_review_status(
    args: argparse.Namespace,
    *,
    run_fn: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
    print_fn: Callable[[str], None] = print,
) -> int:
    argv = ["gh", "issue", "view", args.issue_url, "--json", "state,url,comments"]
    if getattr(args, "repo", None):
        argv.extend(["--repo", args.repo])
    process = run_fn(argv, capture_output=True, text=True, check=False)
    if process.returncode != 0:
        detail = (process.stderr or process.stdout or "gh issue view failed").strip()
        print_fn(f"Error: {detail}")
        return 1
    try:
        issue = json.loads(process.stdout or "{}")
    except json.JSONDecodeError as exc:
        print_fn(f"Error: invalid gh issue view JSON: {exc}")
        return 1
    comments = issue.get("comments") if isinstance(issue.get("comments"), list) else []
    action, action_comment = _review_status_action_comment(comments)
    approval_comment = action_comment if action == "approved" else None
    needs_work_comment = action_comment if action == "needs-work" else None
    issue_url = str(issue.get("url") or args.issue_url)
    verdict_command = None
    if action == "approved" and getattr(args, "manifest", None):
        manifest = shlex.quote(str(Path(args.manifest).expanduser()))
        quoted_issue_url = shlex.quote(issue_url)
        verdict_command = (
            "python3 tools/local-ci/local_ci.py desktop verdict "
            f"{manifest} --approved --issue-url {quoted_issue_url}"
        )
        if getattr(args, "close_issue", False):
            verdict_command += " --close-issue"
    elif action == "needs-work" and getattr(args, "manifest", None):
        manifest = shlex.quote(str(Path(args.manifest).expanduser()))
        quoted_issue_url = shlex.quote(issue_url)
        notes = _review_comment_notes(needs_work_comment)
        verdict_command = (
            "python3 tools/local-ci/local_ci.py desktop verdict "
            f"{manifest} --needs-work --issue-url {quoted_issue_url} --comment-issue"
        )
        if notes:
            verdict_command += f" --notes {shlex.quote(notes)}"
    payload = {
        "kind": "desktop-video-proof-review-status",
        "issue_url": issue_url,
        "state": issue.get("state"),
        "close_trigger": "looks good to me",
        "approved": approval_comment is not None,
        "approval_comment": approval_comment,
        "needs_work": needs_work_comment is not None,
        "needs_work_comment": needs_work_comment,
        "verdict_command": verdict_command,
    }
    if getattr(args, "json", False):
        print_fn(json.dumps(payload, indent=2))
        return 0
    print_fn(f"Desktop video review issue: {issue_url}")
    print_fn(f"  state: {payload['state'] or 'unknown'}")
    print_fn(f"  approved: {str(payload['approved']).lower()}")
    print_fn(f"  needs_work: {str(payload['needs_work']).lower()}")
    if approval_comment:
        author = approval_comment.get("author")
        if isinstance(author, dict) and author.get("login"):
            print_fn(f"  approved_by: {author['login']}")
        if approval_comment.get("url"):
            print_fn(f"  approval_comment: {approval_comment['url']}")
    if needs_work_comment:
        author = needs_work_comment.get("author")
        if isinstance(author, dict) and author.get("login"):
            print_fn(f"  needs_work_by: {author['login']}")
        if needs_work_comment.get("url"):
            print_fn(f"  needs_work_comment: {needs_work_comment['url']}")
    if verdict_command:
        print_fn(f"  verdict_command: {verdict_command}")
    elif not approval_comment and not needs_work_comment:
        print_fn("  waiting_for: looks good to me")
    return 0


def _load_review_watch_state(path: Path | None) -> dict:
    if not path or not path.is_file():
        return {"kind": "desktop-video-proof-review-watch-state", "issues": {}}
    try:
        payload = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError):
        return {"kind": "desktop-video-proof-review-watch-state", "issues": {}}
    if not isinstance(payload, dict):
        return {"kind": "desktop-video-proof-review-watch-state", "issues": {}}
    issues = payload.get("issues")
    if not isinstance(issues, dict):
        payload["issues"] = {}
    payload.setdefault("kind", "desktop-video-proof-review-watch-state")
    return payload


def _load_review_manifest_map(path: Path | None) -> dict[str, str]:
    if not path:
        return {}
    payload = json.loads(path.read_text())
    if not isinstance(payload, dict):
        raise ValueError("manifest map must be a JSON object")
    out: dict[str, str] = {}
    for key, value in payload.items():
        if isinstance(value, str):
            out[str(key)] = value
    return out


def _review_manifest_for_issue(issue: dict, manifest_map: Mapping[str, str]) -> str | None:
    keys = [
        str(issue.get("url") or ""),
        str(issue.get("number") or ""),
        f"#{issue.get('number')}" if issue.get("number") is not None else "",
    ]
    for key in keys:
        if key and key in manifest_map:
            return manifest_map[key]
    return None


def _review_verdict_command(
    issue_url: str,
    manifest: str | None,
    *,
    status: str,
    close_issue: bool,
    notes: str | None = None,
) -> str | None:
    if not manifest:
        return None
    status_flag = "--approved" if status == "approved" else "--needs-work"
    command = (
        "python3 tools/local-ci/local_ci.py desktop verdict "
        f"{shlex.quote(manifest)} {status_flag} --issue-url {shlex.quote(issue_url)}"
    )
    if status == "needs-work":
        command += " --comment-issue"
    if notes:
        command += f" --notes {shlex.quote(notes)}"
    if status == "approved" and close_issue:
        command += " --close-issue"
    return command


def _review_watch_once(
    args: argparse.Namespace,
    *,
    state_payload: dict,
    manifest_map: Mapping[str, str],
    run_fn: Callable[..., subprocess.CompletedProcess[str]],
) -> tuple[dict, dict]:
    list_argv = [
        "gh",
        "issue",
        "list",
        "--state",
        args.state,
        "--label",
        args.label,
        "--json",
        "number,title,url,updatedAt",
    ]
    if getattr(args, "repo", None):
        list_argv.extend(["--repo", args.repo])
    list_process = run_fn(list_argv, capture_output=True, text=True, check=False)
    if list_process.returncode != 0:
        detail = (list_process.stderr or list_process.stdout or "gh issue list failed").strip()
        raise RuntimeError(detail)
    try:
        listed_issues = json.loads(list_process.stdout or "[]")
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"invalid gh issue list JSON: {exc}") from exc
    if not isinstance(listed_issues, list):
        raise RuntimeError("gh issue list did not return a JSON array")

    cached_issues = state_payload.setdefault("issues", {})
    if not isinstance(cached_issues, dict):
        cached_issues = {}
        state_payload["issues"] = cached_issues

    checked_count = 0
    skipped_count = 0
    issues: list[dict] = []
    for listed in listed_issues:
        if not isinstance(listed, dict):
            continue
        issue_url = str(listed.get("url") or listed.get("number") or "")
        issue_number = listed.get("number")
        updated_at = str(listed.get("updatedAt") or "")
        cached = cached_issues.get(issue_url) if issue_url else None
        unchanged = bool(
            cached
            and not getattr(args, "refresh", False)
            and updated_at
            and cached.get("updated_at") == updated_at
        )
        if unchanged:
            manifest = _review_manifest_for_issue(listed, manifest_map)
            action = "approved" if cached.get("approved") else "needs-work" if cached.get("needs_work") else None
            cached_needs_work_comment = cached.get("needs_work_comment")
            verdict_command = (
                _review_verdict_command(
                    issue_url,
                    manifest,
                    status=action,
                    close_issue=bool(getattr(args, "close_issue", False)),
                    notes=_review_comment_notes(cached_needs_work_comment) if isinstance(cached_needs_work_comment, dict) else None,
                )
                if action in {"approved", "needs-work"} and manifest
                else cached.get("verdict_command")
            )
            skipped_count += 1
            issues.append(
                {
                    "number": issue_number,
                    "title": listed.get("title"),
                    "url": issue_url,
                    "updated_at": updated_at,
                    "checked": False,
                    "approved": bool(cached.get("approved")),
                    "needs_work": bool(cached.get("needs_work")),
                    "approval_comment": cached.get("approval_comment"),
                    "needs_work_comment": cached.get("needs_work_comment"),
                    "manifest": manifest,
                    "verdict_command": verdict_command,
                }
            )
            continue

        view_ref = issue_url or str(issue_number)
        view_argv = ["gh", "issue", "view", view_ref, "--json", "state,url,number,title,updatedAt,comments"]
        if getattr(args, "repo", None):
            view_argv.extend(["--repo", args.repo])
        view_process = run_fn(view_argv, capture_output=True, text=True, check=False)
        if view_process.returncode != 0:
            detail = (view_process.stderr or view_process.stdout or "gh issue view failed").strip()
            issue_payload = {
                "number": issue_number,
                "title": listed.get("title"),
                "url": issue_url,
                "updated_at": updated_at,
                "checked": True,
                "error": detail,
                "approved": False,
                "needs_work": False,
                "verdict_command": None,
            }
            issues.append(issue_payload)
            cached_issues[issue_url] = issue_payload
            checked_count += 1
            continue
        try:
            issue = json.loads(view_process.stdout or "{}")
        except json.JSONDecodeError as exc:
            raise RuntimeError(f"invalid gh issue view JSON for {view_ref}: {exc}") from exc
        comments = issue.get("comments") if isinstance(issue.get("comments"), list) else []
        action, action_comment = _review_status_action_comment(comments)
        approval_comment = action_comment if action == "approved" else None
        needs_work_comment = action_comment if action == "needs-work" else None
        resolved_url = str(issue.get("url") or issue_url)
        manifest = _review_manifest_for_issue(issue, manifest_map) or _review_manifest_for_issue(listed, manifest_map)
        verdict_command = _review_verdict_command(
            resolved_url,
            manifest,
            status=action or "",
            close_issue=bool(getattr(args, "close_issue", False)),
            notes=_review_comment_notes(needs_work_comment) if needs_work_comment else None,
        ) if action in {"approved", "needs-work"} else None
        issue_payload = {
            "number": issue.get("number") or issue_number,
            "title": issue.get("title") or listed.get("title"),
            "url": resolved_url,
            "state": issue.get("state"),
            "updated_at": issue.get("updatedAt") or updated_at,
            "checked": True,
            "approved": approval_comment is not None,
            "needs_work": needs_work_comment is not None,
            "approval_comment": approval_comment,
            "needs_work_comment": needs_work_comment,
            "manifest": manifest,
            "verdict_command": verdict_command,
        }
        issues.append(issue_payload)
        if resolved_url:
            cached_issues[resolved_url] = {
                "updated_at": issue_payload["updated_at"],
                "approved": issue_payload["approved"],
                "needs_work": issue_payload["needs_work"],
                "approval_comment": approval_comment,
                "needs_work_comment": needs_work_comment,
                "verdict_command": verdict_command,
            }
        checked_count += 1

    payload = {
        "kind": "desktop-video-proof-review-watch",
        "repo": getattr(args, "repo", None),
        "label": args.label,
        "state": args.state,
        "issue_count": len(issues),
        "checked_count": checked_count,
        "skipped_unchanged_count": skipped_count,
        "approved_count": sum(1 for issue in issues if issue.get("approved")),
        "needs_work_count": sum(1 for issue in issues if issue.get("needs_work")),
        "issues": issues,
    }
    return payload, state_payload


def cmd_desktop_review_watch(
    args: argparse.Namespace,
    *,
    run_fn: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
    print_fn: Callable[[str], None] = print,
    sleep_fn: Callable[[float], None] = time.sleep,
) -> int:
    state_path = Path(args.state_file).expanduser().resolve() if getattr(args, "state_file", None) else None
    try:
        manifest_map = _load_review_manifest_map(Path(args.manifest_map).expanduser().resolve() if getattr(args, "manifest_map", None) else None)
    except (OSError, json.JSONDecodeError, ValueError) as exc:
        print_fn(f"Error: could not read manifest map: {exc}")
        return 1
    state_payload = _load_review_watch_state(state_path)
    max_iterations = max(1, int(getattr(args, "max_iterations", 1) or 1))
    interval = max(0.0, float(getattr(args, "interval", 0.0) or 0.0))
    payloads: list[dict] = []
    for index in range(max_iterations):
        try:
            payload, state_payload = _review_watch_once(
                args,
                state_payload=state_payload,
                manifest_map=manifest_map,
                run_fn=run_fn,
            )
        except RuntimeError as exc:
            print_fn(f"Error: {exc}")
            return 1
        payload["iteration"] = index + 1
        payloads.append(payload)
        if state_path:
            state_path.parent.mkdir(parents=True, exist_ok=True)
            state_path.write_text(json.dumps(state_payload, indent=2) + "\n")
        if index + 1 < max_iterations and interval > 0:
            sleep_fn(interval)
    final_payload = payloads[-1] if len(payloads) == 1 else {
        "kind": "desktop-video-proof-review-watch-series",
        "iterations": payloads,
    }
    if getattr(args, "json", False):
        print_fn(json.dumps(final_payload, indent=2))
        return 0
    if final_payload["kind"] == "desktop-video-proof-review-watch-series":
        summary = final_payload["iterations"][-1]
    else:
        summary = final_payload
    print_fn(f"Desktop video review watch: label={summary['label']} state={summary['state']}")
    print_fn(f"  issues: {summary['issue_count']}")
    print_fn(f"  checked: {summary['checked_count']}")
    print_fn(f"  skipped_unchanged: {summary['skipped_unchanged_count']}")
    print_fn(f"  approved: {summary['approved_count']}")
    print_fn(f"  needs_work: {summary['needs_work_count']}")
    for issue in summary["issues"]:
        if issue.get("approved"):
            print_fn(f"  approved_issue: {issue.get('url')}")
            if issue.get("verdict_command"):
                print_fn(f"    verdict_command: {issue['verdict_command']}")
        if issue.get("needs_work"):
            print_fn(f"  needs_work_issue: {issue.get('url')}")
            if issue.get("verdict_command"):
                print_fn(f"    verdict_command: {issue['verdict_command']}")
    return 0


