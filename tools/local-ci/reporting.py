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
from reporting_proofs import (
    desktop_manifest_adapter,
    desktop_manifest_run_status,
    desktop_manifest_source,
    desktop_proof_scope_for_adapter,
    desktop_proof_summaries,
    desktop_run_summary,
    normalize_desktop_proof_source_mode,
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
