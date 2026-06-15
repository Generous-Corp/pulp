"""Cloud-run record persistence and summary helpers."""
from __future__ import annotations

import json
from pathlib import Path
from typing import Callable

from cloud_billing import estimate_cloud_record_cost, format_currency_amount
from cloud_records import (
    cloud_record_sort_key,
    format_duration_secs,
    normalize_cloud_record,
    summarize_runner_selector,
)
from io_utils import atomic_write_text
from provenance import normalize_result
from state_paths import cloud_runs_dir, ensure_state_dirs


def load_result(path: Path) -> dict:
    return normalize_result(json.loads(path.read_text()))


def cloud_run_path(dispatch_id: str) -> Path:
    return cloud_runs_dir() / f"{dispatch_id}.json"


def save_cloud_record(
    record: dict,
    *,
    ensure_state_dirs_fn: Callable[[], None] = ensure_state_dirs,
    cloud_run_path_fn: Callable[[str], Path] = cloud_run_path,
    atomic_write_text_fn: Callable[[Path, str], None] = atomic_write_text,
) -> Path:
    ensure_state_dirs_fn()
    normalized = normalize_cloud_record(record)
    path = cloud_run_path_fn(normalized["dispatch_id"])
    atomic_write_text_fn(path, json.dumps(normalized, indent=2) + "\n")
    return path


def load_cloud_record(path: Path) -> dict:
    return normalize_cloud_record(json.loads(path.read_text()))


def list_cloud_records(
    limit: int | None = None,
    *,
    ensure_state_dirs_fn: Callable[[], None] = ensure_state_dirs,
    cloud_runs_dir_fn: Callable[[], Path] = cloud_runs_dir,
    load_cloud_record_fn: Callable[[Path], dict] = load_cloud_record,
) -> list[dict]:
    ensure_state_dirs_fn()
    records: list[dict] = []
    for path in cloud_runs_dir_fn().glob("*.json"):
        try:
            records.append(load_cloud_record_fn(path))
        except (OSError, json.JSONDecodeError):
            continue
    records.sort(key=cloud_record_sort_key, reverse=True)
    if limit is not None:
        return records[:limit]
    return records


def cloud_record_summary(
    record: dict,
    config: dict | None = None,
    *,
    estimate_cloud_record_cost_fn: Callable[[dict, dict | None], dict] = estimate_cloud_record_cost,
    format_currency_amount_fn: Callable[..., str] = format_currency_amount,
) -> str:
    record = normalize_cloud_record(record)
    status = record.get("status", "unknown").upper()
    conclusion = (record.get("conclusion") or "").upper()
    state = status if not conclusion else f"{status}/{conclusion}"
    provider = record.get("provider_resolved") or record.get("provider_requested") or "github-hosted"
    ref = record.get("head_branch") or record.get("requested_ref") or "?"
    summary = (
        f"[{record.get('dispatch_id', '?')}] {record.get('workflow_key', '?')} "
        f"ref={ref} provider={provider} {state}"
    )
    selector = summarize_runner_selector(record.get("runner_selector_json", ""))
    if selector:
        summary += f" selector={selector}"
    if record.get("run_id"):
        summary += f" gha#{record['run_id']}"
    duration = format_duration_secs(record.get("duration_secs"))
    if duration:
        summary += f" duration={duration}"
    provider_runtime = format_duration_secs((record.get("usage_summary") or {}).get("provider_runtime_secs"))
    if provider_runtime:
        summary += f" provider_time={provider_runtime}"
    if config is not None:
        cost = estimate_cloud_record_cost_fn(record, config)
        if cost.get("status") == "estimated":
            amount = format_currency_amount_fn(cost.get("estimated_total"), cost.get("currency", "USD"))
            if amount:
                summary += f" cost=est {amount}"
    return summary
