"""Desktop automation report and rollup helpers for local CI."""

from __future__ import annotations

from datetime import datetime
import json
from pathlib import Path
import time
from typing import Callable

from reporting_publish import (
    ARTIFACT_KEYS,
    clear_directory_contents,
    copy_directory_contents,
    publish_report_to_branch,
    slugify_token,
    stage_desktop_publish_report,
)


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
