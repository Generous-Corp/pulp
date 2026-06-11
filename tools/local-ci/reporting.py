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
    "stdout",
    "stderr",
)


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

        published_runs.append(
            {
                "target": manifest.get("target"),
                "action": manifest.get("action"),
                "label": manifest.get("label"),
                "completed_at": manifest.get("completed_at"),
                "bundle_dir": manifest.get("artifacts", {}).get("bundle_dir"),
                "interaction_mode": (manifest.get("interaction") or {}).get("mode"),
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

    cards: list[str] = []
    for run in published_runs:
        artifacts = run["artifacts"]
        screenshot = artifacts.get("screenshot")
        before = artifacts.get("before_screenshot")
        diff = artifacts.get("diff_screenshot")
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
            + "<div class=\"images\">"
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
                " figure{margin:0} figcaption{margin-bottom:8px;color:#9ca3af} img{max-width:320px;border-radius:8px;border:1px solid #374151;background:#000}</style>",
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
