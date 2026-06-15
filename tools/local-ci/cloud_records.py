"""Pure cloud-run record normalization, lookup, and formatting helpers."""

from __future__ import annotations

import json
from datetime import datetime


def parse_iso_datetime(value: str | None) -> datetime | None:
    if not value:
        return None
    normalized = value.replace("Z", "+00:00")
    try:
        return datetime.fromisoformat(normalized)
    except ValueError:
        return None


def normalize_cloud_record(record: dict | None) -> dict:
    normalized = dict(record or {})
    normalized.setdefault("kind", "github-actions-run")
    normalized.setdefault("dispatch_id", "")
    normalized.setdefault("run_id", None)
    normalized.setdefault("repository", "")
    normalized.setdefault("workflow_key", "")
    normalized.setdefault("workflow_file", "")
    normalized.setdefault("workflow_name", "")
    normalized.setdefault("requested_ref", "")
    normalized.setdefault("head_branch", "")
    normalized.setdefault("head_sha", "")
    normalized.setdefault("requested_by", "")
    normalized.setdefault("orchestrator", "github-actions")
    normalized.setdefault("provider_requested", "github-hosted")
    normalized.setdefault("provider_resolved", "")
    normalized.setdefault("runner_selector_json", "")
    normalized.setdefault("dispatch_fields", {})
    normalized.setdefault("status", "unresolved")
    normalized.setdefault("conclusion", "")
    normalized.setdefault("url", "")
    normalized.setdefault("dispatched_at", "")
    normalized.setdefault("matched_at", "")
    normalized.setdefault("started_at", "")
    normalized.setdefault("updated_at", "")
    normalized.setdefault("completed_at", "")
    normalized.setdefault("queue_delay_secs", None)
    normalized.setdefault("duration_secs", None)
    normalized.setdefault("match_strategy", "")
    normalized.setdefault("match_ambiguous", False)
    normalized.setdefault("jobs", [])
    normalized.setdefault("provider_metadata", {})
    normalized.setdefault("usage_summary", {})
    normalized.setdefault("cost_summary", {})
    if not isinstance(normalized.get("dispatch_fields"), dict):
        normalized["dispatch_fields"] = {}
    if not isinstance(normalized.get("jobs"), list):
        normalized["jobs"] = []
    for field_name in ("provider_metadata", "usage_summary", "cost_summary"):
        if not isinstance(normalized.get(field_name), dict):
            normalized[field_name] = {}
    return normalized


def cloud_record_sort_key(record: dict) -> tuple[str, str]:
    timestamp = (
        record.get("completed_at")
        or record.get("updated_at")
        or record.get("matched_at")
        or record.get("dispatched_at")
        or ""
    )
    return (timestamp, record.get("dispatch_id", ""))


def find_cloud_record(records: list[dict], identifier: str | None) -> dict | None:
    if not records:
        return None
    if not identifier or identifier == "latest":
        return records[0]

    exact_dispatch = [record for record in records if record.get("dispatch_id") == identifier]
    if len(exact_dispatch) == 1:
        return exact_dispatch[0]

    prefix_dispatch = [record for record in records if record.get("dispatch_id", "").startswith(identifier)]
    if len(prefix_dispatch) == 1:
        return prefix_dispatch[0]
    if len(prefix_dispatch) > 1:
        raise ValueError(f"Cloud run reference '{identifier}' is ambiguous.")

    run_matches = [record for record in records if str(record.get("run_id") or "") == identifier]
    if len(run_matches) == 1:
        return run_matches[0]
    if len(run_matches) > 1:
        raise ValueError(f"Cloud run id '{identifier}' matched multiple records.")
    return None


def summarize_runner_selector(selector_json: str) -> str:
    raw = (selector_json or "").strip()
    if not raw:
        return ""
    try:
        decoded = json.loads(raw)
    except json.JSONDecodeError:
        return raw
    if isinstance(decoded, str):
        return decoded
    if isinstance(decoded, list):
        return ",".join(str(item) for item in decoded)
    return raw


def normalize_github_timestamp(value: str | None) -> str:
    raw = (value or "").strip()
    if not raw or raw.startswith("0001-01-01T00:00:00"):
        return ""
    return raw


def duration_between(started_at: str | None, completed_at: str | None) -> float | None:
    start_dt = parse_iso_datetime(normalize_github_timestamp(started_at))
    end_dt = parse_iso_datetime(normalize_github_timestamp(completed_at))
    if not start_dt or not end_dt:
        return None
    return round(max(0.0, (end_dt - start_dt).total_seconds()), 1)


def format_duration_secs(value: float | int | str | None) -> str:
    if value in (None, ""):
        return ""
    try:
        total = float(value)
    except (TypeError, ValueError):
        return ""
    if total < 0:
        return ""
    rounded = int(round(total))
    hours, remainder = divmod(rounded, 3600)
    minutes, seconds = divmod(remainder, 60)
    if hours:
        return f"{hours}h{minutes:02d}m{seconds:02d}s"
    if minutes:
        return f"{minutes}m{seconds:02d}s"
    if abs(total - rounded) >= 0.05:
        return f"{total:.1f}s"
    return f"{rounded}s"


def format_memory_megabytes(value: int | float | str | None) -> str:
    if value in (None, ""):
        return ""
    try:
        megabytes = float(value)
    except (TypeError, ValueError):
        return ""
    if megabytes <= 0:
        return ""
    gigabytes = megabytes / 1024.0
    return f"{gigabytes:g} GB"


def render_selector_value(value: str) -> str:
    return summarize_runner_selector(value) if value else ""
