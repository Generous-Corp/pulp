"""Cloud provider integration for local CI — GitHub Actions + Namespace.

Extracted from local_ci.py (R2-1, #2645): GitHub CLI plumbing (gh_*), Namespace
plumbing (nsc_*), cloud-record / billing / provider-metadata helpers, provider
comparison/recommendation, and the cmd_cloud_* subcommands. Public symbols are
re-exported into local_ci.py for the non-cloud commands + main() dispatch.

load_optional_config stays in local_ci.py (shared by core + desktop) and is
imported lazily here (_load_optional_config) to avoid an import cycle.
"""
from __future__ import annotations

import argparse
import base64
import json
import os
import re
import shlex
import statistics
import subprocess
import sys
import time
import uuid
import urllib.error
import urllib.parse
import urllib.request
from collections import defaultdict
from datetime import date, datetime, timezone, timedelta
from pathlib import Path

# Repo root — same derivation as local_ci.py (both live in tools/local-ci/).
ROOT = Path(__file__).resolve().parents[2]

from state_paths import (  # noqa: E402  -- re-exported for in-file consumers
    state_dir,
    config_path,
    worktree_config_path,
    shared_config_path,
    queue_path,
    results_dir,
    cloud_runs_dir,
    evidence_path,
    logs_dir,
    bundles_dir,
    prepared_dir,
    desktop_state_dir,
    desktop_receipts_dir,
    queue_lock_path,
    evidence_lock_path,
    drain_lock_path,
    runner_info_path,
    ensure_state_dirs,
    job_logs_dir,
    target_log_path,
    prepare_target_log,
)
from footprint import (  # noqa: E402  -- re-exported for in-file consumers
    format_size_bytes,
    path_size_bytes,
    local_ci_state_footprint,
    describe_path_for_cleanup,
)
from io_utils import (  # noqa: E402  -- re-exported for in-file consumers
    LockBusyError,
    tail_lines,
    trim_line,
    atomic_write_text,
    image_change_summary,
    file_lock,
)
from git_helpers import (  # noqa: E402  -- re-exported for in-file consumers
    now_iso,
    current_branch,
    current_sha,
    git_root_for,
    resolve_git_ref_sha,
    short_sha,
)
from normalize import (  # noqa: E402  -- re-exported for in-file consumers
    PRIORITY_VALUES,
    normalize_priority,
    priority_value,
    normalize_validation_mode,
    normalize_desktop_source_mode,
    default_desktop_artifact_root,
    normalize_publish_mode,
    parse_config_bool,
    normalize_desktop_optional_config,
    infer_desktop_adapter,
    default_desktop_bootstrap,
    default_desktop_capability_tier,
    normalize_desktop_config,
)
from github_workflows import (  # noqa: E402  -- re-exported for in-file consumers
    GITHUB_ACTIONS_DEFAULTS,
    BUILTIN_GITHUB_WORKFLOWS,
    REPO_VARIABLE_FALLBACKS,
    github_actions_settings_for_display,
    resolve_github_actions_settings,
    normalize_runs_on_json,
    resolve_workflow_runner_selector_json,
    resolve_workflow_dispatch_field_values,
    repo_variable_name_for_workflow_field,
    resolve_default_provider_for_workflow,
    resolve_workflow_field_value_and_source,
    resolve_workflow_dispatch_defaults,
    summarize_workflow_provider_defaults,
    resolve_cli_dispatch_field_values,
)
from provenance import (  # noqa: E402  -- re-exported for in-file consumers
    normalize_provenance,
    provenance_summary,
    normalize_result,
)
from job_queue import (  # noqa: E402  -- re-exported for in-file consumers
    normalize_job,
    load_queue_unlocked,
    save_queue_unlocked,
)
from targets import (  # noqa: E402  -- re-exported for in-file consumers
    enabled_targets,
    parse_targets_arg,
    resolve_targets,
)

def _load_optional_config():
    # Lazy import: local_ci imports this module at top level, so importing
    # local_ci at module scope here would cycle. load_optional_config is a
    # shared config helper that stays in local_ci.py.
    from local_ci import load_optional_config
    return load_optional_config()


def parse_iso_datetime(value: str | None) -> datetime | None:
    if not value:
        return None
    normalized = value.replace("Z", "+00:00")
    try:
        return datetime.fromisoformat(normalized)
    except ValueError:
        return None


def load_result(path: Path) -> dict:
    return normalize_result(json.loads(path.read_text()))


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


def cloud_run_path(dispatch_id: str) -> Path:
    return cloud_runs_dir() / f"{dispatch_id}.json"


def save_cloud_record(record: dict) -> Path:
    ensure_state_dirs()
    normalized = normalize_cloud_record(record)
    path = cloud_run_path(normalized["dispatch_id"])
    atomic_write_text(path, json.dumps(normalized, indent=2) + "\n")
    return path


def load_cloud_record(path: Path) -> dict:
    return normalize_cloud_record(json.loads(path.read_text()))


def cloud_record_sort_key(record: dict) -> tuple[str, str]:
    timestamp = (
        record.get("completed_at")
        or record.get("updated_at")
        or record.get("matched_at")
        or record.get("dispatched_at")
        or ""
    )
    return (timestamp, record.get("dispatch_id", ""))


def list_cloud_records(limit: int | None = None) -> list[dict]:
    ensure_state_dirs()
    records: list[dict] = []
    for path in cloud_runs_dir().glob("*.json"):
        try:
            records.append(load_cloud_record(path))
        except (OSError, json.JSONDecodeError):
            continue
    records.sort(key=cloud_record_sort_key, reverse=True)
    if limit is not None:
        return records[:limit]
    return records


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


def cloud_record_summary(record: dict, config: dict | None = None) -> str:
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
        cost = estimate_cloud_record_cost(record, config)
        if cost.get("status") == "estimated":
            amount = format_currency_amount(cost.get("estimated_total"), cost.get("currency", "USD"))
            if amount:
                summary += f" cost=est {amount}"
    return summary


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


def parse_rate_value(value) -> float | None:
    if value in (None, ""):
        return None
    try:
        parsed = float(value)
    except (TypeError, ValueError):
        return None
    if parsed < 0:
        return None
    return parsed


def parse_optional_bool(value, setting_name: str) -> bool | None:
    if value in (None, ""):
        return None
    if isinstance(value, bool):
        return value
    raise ValueError(f"{setting_name} must be true or false.")


def resolve_billing_settings(config: dict | None) -> dict:
    billing = (((config or {}).get("telemetry") or {}).get("billing") or {})
    settings = {
        "currency": "USD",
        "billing_period_start_day": 1,
        "enable_provider_reported_totals": False,
        "github_hosted_job_os_rates_per_minute": {},
        "namespace_profile_tag_rates_per_hour": {},
        "namespace_machine_shape_rates_per_hour": [],
    }
    if not isinstance(billing, dict):
        return settings

    currency = billing.get("currency")
    if isinstance(currency, str) and currency.strip():
        settings["currency"] = currency.strip().upper()

    start_day = billing.get("billing_period_start_day")
    if start_day not in (None, ""):
        try:
            parsed_start_day = int(start_day)
        except (TypeError, ValueError) as exc:
            raise ValueError("telemetry.billing.billing_period_start_day must be an integer.") from exc
        if parsed_start_day < 1 or parsed_start_day > 28:
            raise ValueError("telemetry.billing.billing_period_start_day must be between 1 and 28.")
        settings["billing_period_start_day"] = parsed_start_day

    provider_reported_totals = parse_optional_bool(
        billing.get("enable_provider_reported_totals"),
        "telemetry.billing.enable_provider_reported_totals",
    )
    if provider_reported_totals is not None:
        settings["enable_provider_reported_totals"] = provider_reported_totals

    github_rates = billing.get("github_hosted_job_os_rates_per_minute")
    if isinstance(github_rates, dict):
        for os_name, value in github_rates.items():
            if not isinstance(os_name, str) or not os_name.strip():
                continue
            parsed = parse_rate_value(value)
            if parsed is not None:
                settings["github_hosted_job_os_rates_per_minute"][os_name.strip().lower()] = parsed

    namespace_profile_rates = billing.get("namespace_profile_tag_rates_per_hour")
    if isinstance(namespace_profile_rates, dict):
        for tag, value in namespace_profile_rates.items():
            if not isinstance(tag, str) or not tag.strip():
                continue
            parsed = parse_rate_value(value)
            if parsed is not None:
                settings["namespace_profile_tag_rates_per_hour"][tag.strip()] = parsed

    shape_rates = billing.get("namespace_machine_shape_rates_per_hour")
    if isinstance(shape_rates, list):
        normalized_shape_rates = []
        for raw in shape_rates:
            if not isinstance(raw, dict):
                continue
            parsed_rate = parse_rate_value(raw.get("rate"))
            if parsed_rate is None:
                continue
            normalized_shape_rates.append(
                {
                    "os": str(raw.get("os", "")).strip().lower(),
                    "arch": str(raw.get("arch", "")).strip().lower(),
                    "virtual_cpu": int(raw.get("virtual_cpu") or 0),
                    "memory_megabytes": int(raw.get("memory_megabytes") or 0),
                    "rate": parsed_rate,
                }
            )
        settings["namespace_machine_shape_rates_per_hour"] = normalized_shape_rates

    return settings


def format_currency_amount(amount: float | int | None, currency: str = "USD") -> str:
    if amount is None:
        return ""
    try:
        value = float(amount)
    except (TypeError, ValueError):
        return ""
    if currency.upper() == "USD":
        return f"${value:.2f}"
    return f"{currency.upper()} {value:.2f}"


def billing_note_text() -> str:
    return "estimated; verify provider pricing"


def provider_billing_note_text() -> str:
    return "actual when available"


def billing_period_window(
    start_day: int,
    *,
    now_dt: datetime | None = None,
) -> tuple[datetime, datetime]:
    current = now_dt or datetime.now(timezone.utc)
    year = current.year
    month = current.month
    if current.day < start_day:
        month -= 1
        if month == 0:
            month = 12
            year -= 1
    period_start = datetime(year, month, start_day, tzinfo=timezone.utc)
    next_year = year
    next_month = month + 1
    if next_month == 13:
        next_month = 1
        next_year += 1
    period_end = datetime(next_year, next_month, start_day, tzinfo=timezone.utc)
    return period_start, period_end


def iter_year_months(start_dt: datetime, end_dt: datetime) -> list[tuple[int, int]]:
    current_year = start_dt.year
    current_month = start_dt.month
    months: list[tuple[int, int]] = []
    while True:
        months.append((current_year, current_month))
        if current_year == end_dt.year and current_month == end_dt.month:
            break
        current_month += 1
        if current_month == 13:
            current_month = 1
            current_year += 1
    return months


def parse_iso_date(value: str | None) -> date | None:
    raw = (value or "").strip()
    if not raw:
        return None
    try:
        return date.fromisoformat(raw)
    except ValueError:
        return None


def infer_job_os(workflow_key: str, job_name: str) -> str:
    name = (job_name or "").strip().lower()
    if "windows" in name:
        return "windows"
    if "macos" in name or "mac " in name or "mac (" in name:
        return "macos"
    if "linux" in name or "ubuntu" in name:
        return "linux"
    if workflow_key in {"docs-check", "sanitizers"}:
        return "linux"
    return ""


def match_namespace_shape_rate(shape: dict, billing: dict) -> float | None:
    profile_tag = (shape.get("profile_tag") or "").strip()
    if profile_tag:
        tagged_rate = (billing.get("namespace_profile_tag_rates_per_hour") or {}).get(profile_tag)
        if tagged_rate is not None:
            return float(tagged_rate)

    for candidate in billing.get("namespace_machine_shape_rates_per_hour") or []:
        if candidate.get("os") and candidate["os"] != str(shape.get("os", "")).strip().lower():
            continue
        if candidate.get("arch") and candidate["arch"] != str(shape.get("arch", "")).strip().lower():
            continue
        if candidate.get("virtual_cpu") and candidate["virtual_cpu"] != int(shape.get("virtual_cpu") or 0):
            continue
        if candidate.get("memory_megabytes") and candidate["memory_megabytes"] != int(shape.get("memory_megabytes") or 0):
            continue
        return float(candidate["rate"])
    return None


def estimate_namespace_cost(record: dict, billing: dict) -> dict:
    metadata = (record.get("provider_metadata") or {}).get("namespace_instances") or []
    shapes = (record.get("usage_summary") or {}).get("machine_shapes") or []
    currency = billing.get("currency", "USD")
    total = 0.0
    estimated_items = 0

    if metadata:
        for instance in metadata:
            rate = match_namespace_shape_rate(instance, billing)
            if rate is None:
                continue
            duration_secs = float(instance.get("duration_secs") or 0)
            total += (duration_secs / 3600.0) * rate
            estimated_items += 1
    elif shapes:
        for shape in shapes:
            rate = match_namespace_shape_rate(shape, billing)
            if rate is None:
                continue
            duration_secs = float(shape.get("duration_secs") or 0)
            total += (duration_secs / 3600.0) * rate
            estimated_items += 1

    if estimated_items:
        return {
            "status": "estimated",
            "currency": currency,
            "estimated_total": round(total, 4),
            "reason": billing_note_text(),
        }

    return {
        "status": "unavailable",
        "reason": "configure telemetry.billing Namespace rates",
    }


def estimate_github_hosted_cost(record: dict, billing: dict) -> dict:
    currency = billing.get("currency", "USD")
    rates = billing.get("github_hosted_job_os_rates_per_minute") or {}
    total = 0.0
    estimated_jobs = 0

    for job in record.get("jobs") or []:
        job_name = str(job.get("name", ""))
        if job_name == "resolve-provider":
            continue
        os_name = infer_job_os(record.get("workflow_key", ""), job_name)
        if not os_name:
            continue
        rate = rates.get(os_name)
        if rate is None:
            continue
        duration_secs = duration_between(job.get("started_at"), job.get("completed_at"))
        if duration_secs is None:
            continue
        total += (duration_secs / 60.0) * float(rate)
        estimated_jobs += 1

    if estimated_jobs:
        return {
            "status": "estimated",
            "currency": currency,
            "estimated_total": round(total, 4),
            "reason": billing_note_text(),
        }

    return {
        "status": "unavailable",
        "reason": "configure telemetry.billing GitHub-hosted rates",
    }


def estimate_cloud_record_cost(record: dict, config: dict | None) -> dict:
    record = normalize_cloud_record(record)
    provider = record.get("provider_resolved") or record.get("provider_requested") or "github-hosted"
    billing = resolve_billing_settings(config)
    if provider == "namespace":
        return estimate_namespace_cost(record, billing)
    if provider == "github-hosted":
        return estimate_github_hosted_cost(record, billing)
    return {"status": "unavailable", "reason": f"no estimator for provider '{provider}'"}


def estimate_billing_period_totals(
    records: list[dict],
    config: dict | None,
    *,
    provider: str | None = None,
) -> dict:
    billing = resolve_billing_settings(config)
    period_start, period_end = billing_period_window(billing["billing_period_start_day"])
    matched_runs = 0
    estimated_runs = 0
    estimated_total = 0.0

    for raw_record in records:
        record = normalize_cloud_record(raw_record)
        completed = parse_iso_datetime(record.get("completed_at") or record.get("updated_at"))
        if not completed:
            continue
        if provider and (record.get("provider_resolved") or record.get("provider_requested")) != provider:
            continue
        if completed < period_start or completed >= period_end:
            continue
        matched_runs += 1
        summary = estimate_cloud_record_cost(record, config)
        if summary.get("status") == "estimated":
            estimated_runs += 1
            estimated_total += float(summary.get("estimated_total") or 0.0)

    return {
        "currency": billing.get("currency", "USD"),
        "period_start": period_start.isoformat(),
        "period_end": period_end.isoformat(),
        "matched_runs": matched_runs,
        "estimated_runs": estimated_runs,
        "estimated_total": round(estimated_total, 4),
        "status": "estimated" if estimated_runs else "unavailable",
        "reason": billing_note_text() if estimated_runs else "configure telemetry.billing rates",
    }


def fetch_github_repo_actions_billing_summary(repository: str, config: dict | None) -> dict:
    billing = resolve_billing_settings(config)
    if not billing.get("enable_provider_reported_totals"):
        return {"status": "disabled", "reason": "disabled (opt-in)"}
    if not gh_available():
        return {"status": "unavailable", "reason": "gh CLI unavailable"}

    repo_payload, repo_error = gh_api_json(f"/repos/{repository}")
    if not isinstance(repo_payload, dict):
        return {
            "status": "unavailable",
            "reason": f"repo lookup failed ({repo_error or 'gh api failed'})",
        }

    owner = ((repo_payload.get("owner") or {}).get("login") or "").strip()
    owner_type = ((repo_payload.get("owner") or {}).get("type") or "").strip().lower()
    if not owner:
        return {"status": "unavailable", "reason": "repo owner unknown"}

    if owner_type == "organization":
        endpoint = f"/organizations/{owner}/settings/billing/usage"
    elif owner_type == "user":
        endpoint = f"/users/{owner}/settings/billing/usage"
    else:
        return {"status": "unavailable", "reason": f"unsupported owner type '{owner_type or 'unknown'}'"}

    period_start, period_end = billing_period_window(billing["billing_period_start_day"])
    month_pairs = iter_year_months(period_start, period_end)
    matched_items: list[dict] = []

    for year, month in month_pairs:
        payload, error = gh_api_json(endpoint, fields={"year": year, "month": month})
        if not isinstance(payload, dict):
            reason = "GitHub billing API unavailable; check auth/platform"
            if owner_type == "user" and "user" not in gh_token_scopes():
                reason = "GitHub billing API unavailable; check auth/platform"
            return {
                "status": "unavailable",
                "reason": reason,
                "detail": error,
            }
        for item in payload.get("usageItems") or []:
            if str(item.get("product", "")).strip().lower() != "actions":
                continue
            if str(item.get("repositoryName", "")).strip() != repository:
                continue
            item_date = parse_iso_date(item.get("date"))
            if not item_date:
                continue
            item_dt = datetime(item_date.year, item_date.month, item_date.day, tzinfo=timezone.utc)
            if item_dt < period_start or item_dt >= period_end:
                continue
            matched_items.append(item)

    total = 0.0
    for item in matched_items:
        amount = item.get("netAmount")
        if amount in (None, ""):
            amount = item.get("grossAmount")
        try:
            total += float(amount or 0.0)
        except (TypeError, ValueError):
            continue

    return {
        "status": "actual",
        "provider": "github-hosted",
        "scope": "repo current period",
        "currency": "USD",
        "period_start": period_start.isoformat(),
        "period_end": period_end.isoformat(),
        "matched_items": len(matched_items),
        "actual_total": round(total, 4),
        "reason": provider_billing_note_text(),
    }


def print_github_repo_billing_summary(summary: dict, *, indent: str = "  ") -> None:
    status = (summary.get("status") or "").strip()
    if status == "disabled":
        return
    if status == "actual":
        amount = format_currency_amount(summary.get("actual_total"), summary.get("currency", "USD"))
        if amount:
            print(
                f"{indent}github repo billing: actual {amount} current period (repo-wide)"
            )
        return
    reason = (summary.get("reason") or "").strip()
    if reason:
        print(f"{indent}github repo billing: unavailable ({reason})")


def print_cloud_field_detail(
    name: str,
    value: str,
    source: str = "",
    *,
    indent: str = "    ",
    unset_note: str = "",
) -> None:
    rendered = render_selector_value(value) if name.endswith("_selector_json") else str(value)
    if rendered:
        suffix = f" ({source})" if source else ""
        print(f"{indent}{name}: {rendered}{suffix}")
        return
    if unset_note:
        print(f"{indent}{name}: unset ({unset_note})")
    else:
        print(f"{indent}{name}: unset")


def print_namespace_usage_summary(record: dict) -> None:
    usage = (record.get("usage_summary") or {})
    if not usage:
        return

    instances_count = usage.get("instances_count")
    provider_runtime = format_duration_secs(usage.get("provider_runtime_secs"))
    if instances_count:
        runtime_suffix = f" runtime={provider_runtime}" if provider_runtime else ""
        print(f"  provider usage: {instances_count} Namespace instance(s){runtime_suffix}")
    for shape in usage.get("machine_shapes") or []:
        os_arch = "/".join(part for part in [shape.get("os", ""), shape.get("arch", "")] if part) or "unknown"
        resources = []
        if shape.get("virtual_cpu"):
            resources.append(f"{shape['virtual_cpu']} vCPU")
        memory = format_memory_megabytes(shape.get("memory_megabytes"))
        if memory:
            resources.append(memory)
        resources_text = f" {' '.join(resources)}" if resources else ""
        count = int(shape.get("count") or 0)
        runtime = format_duration_secs(shape.get("duration_secs"))
        runtime_text = f" runtime={runtime}" if runtime else ""
        profile_tag = shape.get("profile_tag") or "unlabeled"
        print(
            f"    {profile_tag}: {os_arch}{resources_text} x{count}{runtime_text}"
        )

    cost = record.get("cost_summary") or {}
    reason = (cost.get("reason") or "").strip()
    status = (cost.get("status") or "").strip()
    if status == "estimated":
        amount = format_currency_amount(cost.get("estimated_total"), cost.get("currency", "USD"))
        if amount:
            print(f"  cost: est {amount}; {reason or billing_note_text()}")
    elif status == "unavailable" and reason:
        print(f"  cost: unavailable ({reason})")


def print_billing_period_summary(summary: dict, *, indent: str = "  ") -> None:
    status = (summary.get("status") or "").strip()
    if status != "estimated":
        reason = (summary.get("reason") or "").strip()
        if reason:
            print(f"{indent}period cost: unavailable ({reason})")
        return
    amount = format_currency_amount(summary.get("estimated_total"), summary.get("currency", "USD"))
    if not amount:
        return
    runs_text = f"{int(summary.get('estimated_runs') or 0)} run(s)"
    print(f"{indent}period cost: est {amount} over {runs_text}; {summary.get('reason') or billing_note_text()}")


def summarize_cloud_timing(snapshot: dict) -> dict[str, str | float | None]:
    created_at = normalize_github_timestamp(snapshot.get("createdAt"))
    updated_at = normalize_github_timestamp(snapshot.get("updatedAt"))
    observed_updates = [updated_at] if updated_at else []
    job_starts = [
        normalize_github_timestamp(job.get("startedAt"))
        for job in snapshot.get("jobs", []) or []
        if normalize_github_timestamp(job.get("startedAt"))
    ]
    job_completions = [
        normalize_github_timestamp(job.get("completedAt"))
        for job in snapshot.get("jobs", []) or []
        if normalize_github_timestamp(job.get("completedAt"))
    ]
    for job in snapshot.get("jobs", []) or []:
        for step in job.get("steps", []) or []:
            step_started = normalize_github_timestamp(step.get("startedAt"))
            if step_started:
                observed_updates.append(step_started)
            step_completed = normalize_github_timestamp(step.get("completedAt"))
            if step_completed:
                observed_updates.append(step_completed)

    started_at = min(job_starts) if job_starts else ""
    status = snapshot.get("status", "")
    if status == "completed":
        if job_completions:
            completed_at = max(job_completions)
        else:
            completed_at = updated_at
    else:
        completed_at = ""

    duration_anchor = completed_at or (max(observed_updates) if observed_updates else "")
    return {
        "started_at": started_at,
        "completed_at": completed_at,
        "queue_delay_secs": duration_between(created_at, started_at),
        "duration_secs": duration_between(started_at, duration_anchor),
    }


def namespace_instance_duration_secs(instance: dict) -> float | None:
    created_at = instance.get("created")
    completed_at = instance.get("destroyed_at") or now_iso()
    return duration_between(created_at, completed_at)


def normalize_namespace_instance(instance: dict) -> dict:
    shape = instance.get("shape") or {}
    user_label = instance.get("user_label") or {}
    github_workflow = instance.get("github_workflow") or {}
    duration_secs = namespace_instance_duration_secs(instance)
    return {
        "cluster_id": instance.get("cluster_id", ""),
        "created_at": normalize_github_timestamp(instance.get("created", "")),
        "destroyed_at": normalize_github_timestamp(instance.get("destroyed_at", "")),
        "os": shape.get("os", ""),
        "arch": shape.get("machine_arch", ""),
        "virtual_cpu": shape.get("virtual_cpu", 0),
        "memory_megabytes": shape.get("memory_megabytes", 0),
        "profile_tag": user_label.get("nsc.runner-profile-tag", ""),
        "profile_id": user_label.get("nsc.runner-profile-id", ""),
        "repository": github_workflow.get("repository", ""),
        "run_id": github_workflow.get("run_id", ""),
        "workflow": github_workflow.get("workflow", ""),
        "duration_secs": duration_secs,
    }


def summarize_namespace_usage(instances: list[dict]) -> dict:
    machine_shapes: dict[tuple[str, str, int, int, str], dict] = {}
    total_runtime = 0.0
    for instance in instances:
        duration_secs = float(instance.get("duration_secs") or 0)
        total_runtime += duration_secs
        key = (
            instance.get("os", ""),
            instance.get("arch", ""),
            int(instance.get("virtual_cpu") or 0),
            int(instance.get("memory_megabytes") or 0),
            instance.get("profile_tag", ""),
        )
        shape = machine_shapes.setdefault(
            key,
            {
                "os": instance.get("os", ""),
                "arch": instance.get("arch", ""),
                "virtual_cpu": int(instance.get("virtual_cpu") or 0),
                "memory_megabytes": int(instance.get("memory_megabytes") or 0),
                "profile_tag": instance.get("profile_tag", ""),
                "count": 0,
                "duration_secs": 0.0,
            },
        )
        shape["count"] += 1
        shape["duration_secs"] += duration_secs

    summarized_shapes = sorted(
        machine_shapes.values(),
        key=lambda item: (item["os"], item["arch"], item["profile_tag"]),
    )
    return {
        "instances_count": len(instances),
        "provider_runtime_secs": round(total_runtime, 1),
        "machine_shapes": summarized_shapes,
    }


def enrich_cloud_record_provider_metadata(record: dict) -> dict:
    updated = normalize_cloud_record(record)
    provider = updated.get("provider_resolved") or updated.get("provider_requested") or "github-hosted"
    if provider != "namespace" or not updated.get("run_id") or not nsc_logged_in():
        if provider != "namespace":
            updated["provider_metadata"] = {}
            updated["usage_summary"] = {}
            updated["cost_summary"] = {}
        return updated

    instances = namespace_instances_for_run(updated.get("repository", ""), int(updated["run_id"]))
    if not instances:
        return updated

    updated["provider_metadata"] = {"namespace_instances": instances}
    updated["usage_summary"] = summarize_namespace_usage(instances)
    updated["cost_summary"] = {
        "status": "unavailable",
        "reason": "Namespace CLI does not expose billing totals; provider runtime is shown instead.",
    }
    return updated



def gh_available() -> bool:
    result = subprocess.run(["gh", "auth", "status"], capture_output=True, text=True)
    return result.returncode == 0


def gh_auth_status_text() -> str:
    result = subprocess.run(["gh", "auth", "status", "-t"], capture_output=True, text=True)
    if result.returncode != 0:
        return ""
    return result.stdout


def gh_token_scopes() -> set[str]:
    status_text = gh_auth_status_text()
    if not status_text:
        return set()
    marker = "Token scopes:"
    for raw_line in status_text.splitlines():
        line = raw_line.strip()
        if marker not in line:
            continue
        suffix = line.split(marker, 1)[1].strip()
        if suffix.startswith("'") and suffix.endswith("'"):
            suffix = suffix[1:-1]
        return {item.strip() for item in suffix.split(",") if item.strip()}
    return set()


def gh_api_json(path: str, *, fields: dict[str, str | int] | None = None) -> tuple[dict | list | None, str]:
    cmd = [
        "gh",
        "api",
        "-H",
        "Accept: application/vnd.github+json",
        "-H",
        "X-GitHub-Api-Version: 2026-03-10",
        path,
    ]
    for key, value in (fields or {}).items():
        cmd += ["-F", f"{key}={value}"]
    result = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if result.returncode != 0:
        detail = (result.stderr or result.stdout or "").strip()
        return None, detail or "gh api failed"
    try:
        payload = json.loads(result.stdout)
    except json.JSONDecodeError:
        return None, "gh api returned invalid JSON"
    return payload, ""


def nsc_run(args: list[str], *, capture_output: bool = True) -> subprocess.CompletedProcess | None:
    try:
        return subprocess.run(
            ["nsc", *args],
            cwd=ROOT,
            capture_output=capture_output,
            text=True,
            check=False,
        )
    except FileNotFoundError:
        return None


def nsc_available() -> bool:
    result = nsc_run(["version"])
    return bool(result and result.returncode == 0)


def nsc_version() -> str | None:
    result = nsc_run(["version"])
    if not result or result.returncode != 0:
        return None
    lines = [line.strip() for line in result.stdout.splitlines() if line.strip()]
    return lines[0] if lines else None


def nsc_logged_in() -> bool:
    result = nsc_run(["auth", "check-login"])
    return bool(result and result.returncode == 0)


def parse_colon_separated_fields(text: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if not line or ":" not in line:
            continue
        key, value = line.split(":", 1)
        fields[key.strip()] = value.strip()
    return fields


def nsc_workspace_info() -> dict[str, str] | None:
    result = nsc_run(["workspace", "describe"])
    if not result or result.returncode != 0:
        return None
    fields = parse_colon_separated_fields(result.stdout)
    return fields or None


def nsc_instance_history(max_entries: int = 100) -> list[dict]:
    result = nsc_run(["instance", "history", "--all", "-o", "json", "--max_entries", str(max_entries)])
    if not result or result.returncode != 0:
        return []
    try:
        payload = json.loads(result.stdout)
    except json.JSONDecodeError:
        return []
    return payload if isinstance(payload, list) else []


def namespace_instances_for_run(repository: str, run_id: int) -> list[dict]:
    matched: list[dict] = []
    for raw_instance in nsc_instance_history():
        github_workflow = raw_instance.get("github_workflow") or {}
        if github_workflow.get("repository") != repository:
            continue
        if str(github_workflow.get("run_id") or "") != str(run_id):
            continue
        matched.append(normalize_namespace_instance(raw_instance))
    matched.sort(key=lambda item: (item.get("created_at", ""), item.get("cluster_id", "")))
    return matched


def print_namespace_setup_help() -> None:
    print("Recommended Namespace setup:")
    print("  1. Install the `nsc` CLI")
    print("  2. Run `nsc login`")
    print("  3. Verify with `nsc workspace describe`")
    print("  4. Configure a Namespace runner selector/profile for the workflow you want to route")


def cmd_cloud_namespace_doctor(_args: argparse.Namespace) -> int:
    version = nsc_version()
    if not version:
        print("Namespace CLI: missing")
        print_namespace_setup_help()
        return 1

    print(f"Namespace CLI: ok ({version})")
    if not nsc_logged_in():
        print("Namespace login: missing")
        print("Run: nsc login")
        return 1

    print("Namespace login: ok")
    workspace = nsc_workspace_info()
    if workspace:
        name = workspace.get("Name")
        if name:
            print(f"Workspace: {name}")
        tenant = workspace.get("Tenant ID")
        if tenant:
            print(f"Tenant ID: {tenant}")
        registry = workspace.get("Registry URL")
        if registry:
            print(f"Registry URL: {registry}")
    else:
        print("Workspace: unavailable")
    return 0


def cmd_cloud_namespace_setup(_args: argparse.Namespace) -> int:
    if not nsc_available():
        print("Namespace CLI: missing")
        print_namespace_setup_help()
        return 1

    if not nsc_logged_in():
        print("Namespace login: starting `nsc login`...")
        login_result = nsc_run(["login"], capture_output=False)
        if not login_result or login_result.returncode != 0:
            print("Namespace login: failed")
            return 1

    return cmd_cloud_namespace_doctor(argparse.Namespace())


def gh_repo_variables(repository: str) -> dict[str, str]:
    result = subprocess.run(
        ["gh", "variable", "list", "--repo", repository, "--json", "name,value"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        return {}
    try:
        payload = json.loads(result.stdout)
    except json.JSONDecodeError:
        return {}
    variables: dict[str, str] = {}
    for item in payload:
        name = item.get("name")
        value = item.get("value")
        if not name or value in (None, ""):
            continue
        variables[str(name)] = str(value)
    return variables


def gh_repo_name() -> str | None:
    result = subprocess.run(
        ["gh", "repo", "view", "--json", "nameWithOwner"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        return None
    try:
        return json.loads(result.stdout).get("nameWithOwner")
    except json.JSONDecodeError:
        return None


def gh_current_login() -> str | None:
    result = subprocess.run(
        ["gh", "api", "user", "--jq", ".login"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        return None
    login = result.stdout.strip()
    return login or None


def resolve_github_repository(settings: dict) -> str:
    repository = settings.get("repository", "").strip()
    if repository:
        return repository
    discovered = gh_repo_name()
    if discovered:
        return discovered
    raise ValueError(
        "Could not determine GitHub repository. Set github_actions.repository in tools/local-ci/config.json "
        "or make sure `gh repo view` works in this checkout."
    )


def gh_workflow_dispatch(repository: str, workflow_file: str, ref: str, fields: dict[str, str]) -> None:
    cmd = ["gh", "workflow", "run", workflow_file, "--repo", repository, "--ref", ref]
    for key, value in fields.items():
        cmd += ["-f", f"{key}={value}"]
    result = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if result.returncode != 0:
        detail = (result.stderr or result.stdout or "").strip()
        raise RuntimeError(f"Failed to dispatch {workflow_file}: {detail or 'gh workflow run failed'}")


def gh_find_dispatched_run(
    repository: str,
    workflow_file: str,
    ref: str,
    dispatched_at: str,
    *,
    timeout_secs: int,
) -> dict | None:
    deadline = time.time() + timeout_secs
    dispatched_dt = parse_iso_datetime(dispatched_at)
    tolerance_secs = 10
    fields = (
        "databaseId,headBranch,headSha,status,conclusion,url,createdAt,updatedAt,workflowName,event"
    )

    while time.time() < deadline:
        result = subprocess.run(
            [
                "gh",
                "run",
                "list",
                "--repo",
                repository,
                "--workflow",
                workflow_file,
                "--branch",
                ref,
                "--event",
                "workflow_dispatch",
                "--json",
                fields,
                "--limit",
                "10",
            ],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        if result.returncode == 0:
            try:
                runs = json.loads(result.stdout)
            except json.JSONDecodeError:
                runs = []
            candidates = []
            for run in runs:
                if run.get("headBranch") != ref or run.get("event") != "workflow_dispatch":
                    continue
                created_dt = parse_iso_datetime(run.get("createdAt"))
                if dispatched_dt and created_dt and created_dt.timestamp() + tolerance_secs < dispatched_dt.timestamp():
                    continue
                candidates.append(run)
            if candidates:
                candidates.sort(key=lambda run: run.get("createdAt", ""), reverse=True)
                matched = dict(candidates[0])
                matched["match_ambiguous"] = len(candidates) > 1
                return matched
        time.sleep(2)

    return None


def gh_run_view(repository: str, run_id: int) -> dict | None:
    result = subprocess.run(
        [
            "gh",
            "run",
            "view",
            str(run_id),
            "--repo",
            repository,
            "--json",
            "databaseId,status,conclusion,url,headSha,headBranch,workflowName,createdAt,updatedAt,jobs",
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        return None
    try:
        return json.loads(result.stdout)
    except json.JSONDecodeError:
        return None


def gh_pr_create(branch: str, base: str = "main") -> int | None:
    result = subprocess.run(
        ["gh", "pr", "create", "--head", branch, "--base", base, "--fill", "--no-maintainer-edit"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        existing = subprocess.run(
            ["gh", "pr", "view", branch, "--json", "number"],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        if existing.returncode == 0:
            return json.loads(existing.stdout)["number"]
        print(f"  Failed to create PR: {result.stderr.strip()}")
        return None

    url = result.stdout.strip()
    try:
        return int(url.rstrip("/").split("/")[-1])
    except (ValueError, IndexError):
        return None


def gh_pr_comment(pr_number: int, body: str) -> bool:
    result = subprocess.run(
        ["gh", "pr", "comment", str(pr_number), "--body", body],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    return result.returncode == 0


def gh_pr_merge(pr_number: int, method: str = "squash") -> bool:
    result = subprocess.run(
        ["gh", "pr", "merge", str(pr_number), f"--{method}", "--delete-branch"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    return result.returncode == 0


def gh_pr_list_open() -> list[dict]:
    result = subprocess.run(
        ["gh", "pr", "list", "--json", "number,title,headRefName,author,createdAt,labels"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        return []
    return json.loads(result.stdout)


def gh_pr_head(pr_ref: str) -> tuple[int, str, str] | None:
    if pr_ref == "latest":
        prs = gh_pr_list_open()
        if not prs:
            print("No open PRs found.")
            return None
        pr_ref = str(prs[0]["number"])

    result = subprocess.run(
        ["gh", "pr", "view", pr_ref, "--json", "number,headRefName,headRefOid"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        print(f"  Could not find PR {pr_ref}: {result.stderr.strip()}")
        return None

    data = json.loads(result.stdout)
    return data["number"], data["headRefName"], data["headRefOid"]


def format_ci_comment(result: dict) -> str:
    result = normalize_result(result)
    validation = result.get("validation", "full")
    title = "Local CI Smoke Results" if validation == "smoke" else "Local CI Results"
    lines = [f"## {title}\n"]
    overall = result["overall"].upper()
    icon = "white_check_mark" if overall == "PASS" else "x"
    lines.append(f":{icon}: **Overall: {overall}**\n")
    lines.append(f"Job: `{result.get('job_id', '?')}`  Commit: `{short_sha(result.get('sha', ''))}`\n")
    lines.append(f"Execution: `{provenance_summary(result.get('provenance'))}`\n")
    if result.get("provenance", {}).get("run_url"):
        lines.append(f"Run URL: {result['provenance']['run_url']}\n")
    if validation != "full":
        lines.append(f"Validation: `{validation}`\n")
        lines.append("_Smoke mode is a fast clean install/export preflight and does not run the full test suite._\n")
    lines.append("| Target | Status | Duration |")
    lines.append("|--------|--------|----------|")
    for item in result["results"]:
        status = item["status"].upper()
        s_icon = "white_check_mark" if status == "PASS" else "x"
        lines.append(f"| {item['target']} | :{s_icon}: {status} | {item.get('duration_secs', 0)}s |")

    if any(item["status"] != "pass" for item in result["results"]):
        lines.append("\n<details><summary>Failure details</summary>\n")
        for item in result["results"]:
            if item["status"] == "pass":
                continue
            lines.append(f"### {item['target']} (exit {item.get('exit_code', '?')})")
            stderr = item.get("stderr_tail", "")
            if stderr:
                lines.append(f"```\n{stderr[-500:]}\n```")
        lines.append("</details>")

    lines.append(f"\n*Run at {result.get('completed_at', 'unknown')}*")
    return "\n".join(lines)


# ── CLI Commands ─────────────────────────────────────────────────────────────


def update_cloud_record_from_run(record: dict, snapshot: dict, *, provider_resolved: str | None = None) -> dict:
    updated = normalize_cloud_record(record)
    snapshot_updated_at = snapshot.get("updatedAt") or now_iso()
    current_updated_at = updated.get("updated_at") or ""
    if current_updated_at and snapshot_updated_at and current_updated_at > snapshot_updated_at:
        return updated

    updated["run_id"] = snapshot.get("databaseId", updated.get("run_id"))
    updated["workflow_name"] = snapshot.get("workflowName", updated.get("workflow_name"))
    updated["head_branch"] = snapshot.get("headBranch", updated.get("head_branch"))
    updated["head_sha"] = snapshot.get("headSha", updated.get("head_sha"))
    updated["status"] = snapshot.get("status", updated.get("status"))
    updated["conclusion"] = snapshot.get("conclusion") or updated.get("conclusion", "")
    updated["url"] = snapshot.get("url", updated.get("url"))
    updated["updated_at"] = snapshot_updated_at
    if provider_resolved:
        updated["provider_resolved"] = provider_resolved
    if snapshot.get("createdAt") and not updated.get("matched_at"):
        updated["matched_at"] = snapshot["createdAt"]

    timing = summarize_cloud_timing(snapshot)
    if timing.get("started_at"):
        updated["started_at"] = timing["started_at"]
    if timing.get("completed_at"):
        updated["completed_at"] = timing["completed_at"]
    elif updated.get("status") == "completed" and not updated.get("completed_at"):
        updated["completed_at"] = snapshot_updated_at
    elif updated.get("status") != "completed":
        updated["completed_at"] = ""
    updated["queue_delay_secs"] = timing.get("queue_delay_secs")
    updated["duration_secs"] = timing.get("duration_secs")

    jobs = []
    for job in snapshot.get("jobs", []) or []:
        jobs.append(
            {
                "name": job.get("name", ""),
                "status": job.get("status", ""),
                "conclusion": job.get("conclusion", ""),
                "started_at": normalize_github_timestamp(job.get("startedAt", "")),
                "completed_at": normalize_github_timestamp(job.get("completedAt", "")),
            }
        )
    if jobs:
        updated["jobs"] = jobs
    return updated


def refresh_cloud_record(record: dict, repository: str, *, require_snapshot: bool = False) -> dict:
    run_id = record.get("run_id")
    if not run_id:
        return normalize_cloud_record(record)
    snapshot = gh_run_view(repository, int(run_id))
    if not snapshot:
        if require_snapshot:
            raise RuntimeError(f"Failed to refresh GitHub run {run_id} from {repository}.")
        return normalize_cloud_record(record)
    refreshed = enrich_cloud_record_provider_metadata(
        update_cloud_record_from_run(record, snapshot)
    )
    save_cloud_record(refreshed)
    return refreshed


def filter_cloud_records(
    records: list[dict],
    *,
    workflow_key: str | None = None,
    provider: str | None = None,
) -> list[dict]:
    filtered = []
    for raw_record in records:
        record = normalize_cloud_record(raw_record)
        if workflow_key and record.get("workflow_key") != workflow_key:
            continue
        resolved_provider = record.get("provider_resolved") or record.get("provider_requested") or "github-hosted"
        if provider and resolved_provider != provider:
            continue
        filtered.append(record)
    return filtered


def median_or_none(values: list[float], *, digits: int = 1) -> float | None:
    cleaned = [float(value) for value in values if value not in (None, "")]
    if not cleaned:
        return None
    return round(float(statistics.median(cleaned)), digits)


def compare_cloud_providers(
    records: list[dict],
    config: dict | None,
    *,
    workflow_key: str,
) -> list[dict]:
    grouped: dict[str, dict] = {}
    for record in filter_cloud_records(records, workflow_key=workflow_key):
        if record.get("status") != "completed":
            continue
        provider = record.get("provider_resolved") or record.get("provider_requested") or "github-hosted"
        group = grouped.setdefault(
            provider,
            {
                "provider": provider,
                "workflow_key": workflow_key,
                "runs": [],
                "durations": [],
                "queue_delays": [],
                "provider_runtimes": [],
                "estimated_costs": [],
                "success_count": 0,
                "completed_count": 0,
                "latest_completed_at": "",
                "latest_success_at": "",
            },
        )
        group["runs"].append(record)
        group["completed_count"] += 1
        completed_at = record.get("completed_at") or record.get("updated_at") or ""
        if completed_at and completed_at > group["latest_completed_at"]:
            group["latest_completed_at"] = completed_at
        if record.get("conclusion") == "success":
            group["success_count"] += 1
            if completed_at and completed_at > group["latest_success_at"]:
                group["latest_success_at"] = completed_at
        if record.get("duration_secs") not in (None, ""):
            group["durations"].append(float(record["duration_secs"]))
        if record.get("queue_delay_secs") not in (None, ""):
            group["queue_delays"].append(float(record["queue_delay_secs"]))
        provider_runtime = (record.get("usage_summary") or {}).get("provider_runtime_secs")
        if provider_runtime not in (None, ""):
            group["provider_runtimes"].append(float(provider_runtime))
        cost = estimate_cloud_record_cost(record, config)
        if cost.get("status") == "estimated":
            group["estimated_costs"].append(float(cost.get("estimated_total") or 0.0))

    summaries = []
    for provider, group in grouped.items():
        period = estimate_billing_period_totals(records, config, provider=provider)
        summaries.append(
            {
                "provider": provider,
                "workflow_key": workflow_key,
                "runs_count": len(group["runs"]),
                "completed_count": group["completed_count"],
                "success_count": group["success_count"],
                "median_duration_secs": median_or_none(group["durations"]),
                "median_queue_delay_secs": median_or_none(group["queue_delays"]),
                "median_provider_runtime_secs": median_or_none(group["provider_runtimes"]),
                "median_estimated_cost": median_or_none(group["estimated_costs"], digits=4),
                "currency": resolve_billing_settings(config).get("currency", "USD"),
                "period": period,
                "latest_completed_at": group["latest_completed_at"],
                "latest_success_at": group["latest_success_at"],
            }
        )

    summaries.sort(key=lambda item: item["provider"])
    return summaries


def recommend_cloud_provider(
    records: list[dict],
    config: dict | None,
    *,
    workflow_key: str,
) -> tuple[str | None, str]:
    summaries = compare_cloud_providers(records, config, workflow_key=workflow_key)
    viable = [item for item in summaries if item.get("success_count")]
    if not viable:
        return None, "no successful runs recorded yet"
    if len(viable) == 1:
        return viable[0]["provider"], "only measured provider"

    viable_with_duration = [item for item in viable if item.get("median_duration_secs") is not None]
    if not viable_with_duration:
        return viable[0]["provider"], "no timing medians available"

    fastest = min(viable_with_duration, key=lambda item: float(item["median_duration_secs"]))
    cheapest_candidates = [item for item in viable if item.get("median_estimated_cost") is not None]
    if len(cheapest_candidates) >= 2:
        cheapest = min(cheapest_candidates, key=lambda item: float(item["median_estimated_cost"]))
        fastest_duration = float(fastest["median_duration_secs"] or 0.0)
        cheapest_duration = float(cheapest.get("median_duration_secs") or fastest_duration or 0.0)
        fastest_cost = float(fastest.get("median_estimated_cost") or 0.0)
        cheapest_cost = float(cheapest.get("median_estimated_cost") or 0.0)
        speedup = 0.0
        if cheapest_duration > 0:
            speedup = max(0.0, (cheapest_duration - fastest_duration) / cheapest_duration)
        if cheapest["provider"] != fastest["provider"] and fastest_cost > 0 and cheapest_cost > 0:
            if fastest_cost > cheapest_cost * 1.2 and speedup < 0.15:
                return cheapest["provider"], "lower estimated cost with similar timing"
        return fastest["provider"], "fastest observed median"

    return fastest["provider"], "fastest observed median"


def cmd_cloud_history(args: argparse.Namespace) -> int:
    config = _load_optional_config()
    records = filter_cloud_records(
        list_cloud_records(limit=None),
        workflow_key=getattr(args, "workflow", None),
        provider=getattr(args, "provider", None),
    )
    if not records:
        print("No tracked cloud runs found.")
        return 0

    limit = max(1, int(getattr(args, "limit", 10)))
    print("Cloud history:\n")
    for record in records[:limit]:
        print(f"  {cloud_record_summary(record, config)}")

    print()
    print_billing_period_summary(estimate_billing_period_totals(records, config))
    if getattr(args, "provider", None) in (None, "github-hosted"):
        try:
            repository = resolve_github_repository(resolve_github_actions_settings(config))
        except ValueError as exc:
            print_github_repo_billing_summary({"status": "unavailable", "reason": str(exc)})
        else:
            print_github_repo_billing_summary(fetch_github_repo_actions_billing_summary(repository, config))
    return 0


def cmd_cloud_compare(args: argparse.Namespace) -> int:
    config = _load_optional_config()
    workflow_key = args.workflow or resolve_github_actions_settings(config).get("workflow", "build")
    summaries = compare_cloud_providers(list_cloud_records(limit=None), config, workflow_key=workflow_key)
    if not summaries:
        print(f"No tracked cloud runs found for workflow '{workflow_key}'.")
        return 0

    print(f"Cloud compare: workflow={workflow_key}\n")
    for summary in summaries:
        line = (
            f"  {summary['provider']}: runs={summary['runs_count']} "
            f"success={summary['success_count']}/{summary['completed_count'] or summary['runs_count']}"
        )
        duration = format_duration_secs(summary.get("median_duration_secs"))
        if duration:
            line += f" median_elapsed={duration}"
        queue_delay = format_duration_secs(summary.get("median_queue_delay_secs"))
        if queue_delay:
            line += f" median_queue={queue_delay}"
        provider_runtime = format_duration_secs(summary.get("median_provider_runtime_secs"))
        if provider_runtime:
            line += f" median_provider_time={provider_runtime}"
        if summary.get("median_estimated_cost") is not None:
            amount = format_currency_amount(summary.get("median_estimated_cost"), summary.get("currency", "USD"))
            if amount:
                line += f" median_cost=est {amount}"
        latest_success = summary.get("latest_success_at") or ""
        latest_completed = summary.get("latest_completed_at") or ""
        if latest_success:
            line += f" latest_success={latest_success}"
        elif latest_completed:
            line += f" latest={latest_completed}"
        print(line)
        print_billing_period_summary(summary.get("period") or {}, indent="    ")
    print("\n  note: estimated; verify provider pricing")
    return 0


def cmd_cloud_recommend(args: argparse.Namespace) -> int:
    config = _load_optional_config()
    workflow_key = args.workflow or resolve_github_actions_settings(config).get("workflow", "build")
    provider, reason = recommend_cloud_provider(list_cloud_records(limit=None), config, workflow_key=workflow_key)
    if not provider:
        print(f"No recommendation for workflow '{workflow_key}': {reason}.")
        return 0
    print(f"Recommended provider for {workflow_key}: {provider} ({reason})")
    print(f"  note: {billing_note_text()}")
    return 0


def cmd_cloud_workflows(_args: argparse.Namespace) -> int:
    print("GitHub Actions workflows:\n")
    for key, info in BUILTIN_GITHUB_WORKFLOWS.items():
        providers = ", ".join(info.get("providers", [])) or "github-hosted"
        print(f"  {key:12s} {info['display_name']} ({info['file']})")
        print(f"               providers: {providers}")
    return 0


def cmd_cloud_defaults(_args: argparse.Namespace) -> int:
    config = _load_optional_config()
    settings = github_actions_settings_for_display(config)
    repository = ""
    repository_note = ""
    repository_variables: dict[str, str] = {}
    try:
        resolved_settings = resolve_github_actions_settings(config)
        settings = resolved_settings
        repository = resolve_github_repository(resolved_settings)
    except ValueError as exc:
        repository_note = str(exc)
        try:
            repository = resolve_github_repository(settings)
        except ValueError:
            repository = ""
    else:
        if gh_available():
            repository_variables = gh_repo_variables(repository)
        else:
            repository_note = "gh CLI unavailable; repo-variable fallbacks not inspected"

    print("Cloud defaults:\n")
    if repository:
        print(f"  repository: {repository}")
    else:
        print("  repository: unresolved")
    if repository_note:
        print(f"  note: {repository_note}")
    print(f"  configured default workflow: {settings.get('workflow', 'build')}")
    print(f"  configured default provider: {settings.get('provider', 'github-hosted')}")
    billing = resolve_billing_settings(config)
    print(
        f"  billing estimates: {billing.get('currency', 'USD')} period-day={billing.get('billing_period_start_day', 1)} "
        f"({billing_note_text()})"
    )
    provider_truth_state = "enabled (local opt-in)" if billing.get("enable_provider_reported_totals") else "disabled (opt-in; off by default)"
    print(f"  provider billing truth: {provider_truth_state}")

    for workflow_key, workflow in BUILTIN_GITHUB_WORKFLOWS.items():
        summary = summarize_workflow_provider_defaults(
            config,
            repository_variables,
            settings,
            workflow_key,
        )
        print(f"\n  {workflow_key}: {workflow['display_name']} ({workflow['file']})")
        print(f"    supported providers: {', '.join(workflow.get('providers', ['github-hosted']))}")
        print(
            f"    default provider: {summary['provider']} ({summary['provider_source']})"
        )
        selector_input = summary.get("selector_input") or ""
        if selector_input:
            print_cloud_field_detail(
                selector_input,
                summary.get("selector_value", ""),
                summary.get("selector_source", ""),
            )
        for field_name in workflow.get("dispatch_fields") or []:
            unset_note = ""
            if workflow_key == "build" and field_name == "macos_runner_selector_json":
                unset_note = "macOS stays local-first unless a config default or one-off override is set"
            print_cloud_field_detail(
                field_name,
                (summary.get("dispatch_fields") or {}).get(field_name, ""),
                (summary.get("dispatch_sources") or {}).get(field_name, ""),
                unset_note=unset_note,
            )
    return 0


def cmd_cloud_run(args: argparse.Namespace) -> int:
    if not gh_available():
        print("Error: gh CLI not available or not authenticated. Run: gh auth login")
        return 1

    config = _load_optional_config()
    try:
        settings = resolve_github_actions_settings(config)
        repository = resolve_github_repository(settings)
    except ValueError as exc:
        print(f"Error: {exc}")
        return 1

    workflow_key = args.workflow or settings.get("workflow", "build")
    workflow = BUILTIN_GITHUB_WORKFLOWS.get(workflow_key)
    if workflow is None:
        print(
            f"Error: Unknown workflow '{workflow_key}'. Use `pulp ci-local cloud workflows` to list supported workflows."
        )
        return 1

    branch = args.branch or current_branch()
    try:
        provider, _provider_source = resolve_default_provider_for_workflow(
            settings,
            workflow_key,
            explicit_provider=getattr(args, "provider", None),
        )
        repository_variables = gh_repo_variables(repository)
        config_dispatch_fields, _config_dispatch_sources = resolve_workflow_dispatch_defaults(
            config,
            repository_variables,
            workflow_key,
            provider,
            workflow.get("dispatch_fields"),
        )
        cli_dispatch_fields = resolve_cli_dispatch_field_values(
            args, workflow.get("dispatch_fields")
        )
        selector_input = workflow.get("selector_input")
        if getattr(args, "runner_selector_json", None):
            selector_json = normalize_runs_on_json(
                args.runner_selector_json,
                setting_name="--runner-selector-json",
            )
        elif selector_input:
            selector_json, _selector_source = resolve_workflow_field_value_and_source(
                config,
                repository_variables,
                workflow_key,
                provider,
                selector_input,
            )
        else:
            selector_json = ""
        config_dispatch_fields.update(cli_dispatch_fields)
    except ValueError as exc:
        print(f"Error: {exc}")
        return 1

    selector_input = workflow.get("selector_input")
    if selector_json and not selector_input:
        print(f"Error: workflow '{workflow_key}' does not accept an explicit runner selector.")
        return 1

    dispatch_id = uuid.uuid4().hex[:12]
    dispatch_time = now_iso()
    record = normalize_cloud_record(
        {
            "dispatch_id": dispatch_id,
            "repository": repository,
            "workflow_key": workflow_key,
            "workflow_file": workflow["file"],
            "workflow_name": workflow["display_name"],
            "requested_ref": branch,
            "requested_by": gh_current_login() or "",
            "provider_requested": provider,
            "runner_selector_json": selector_json,
            "dispatch_fields": config_dispatch_fields,
            "status": "unresolved",
            "dispatched_at": dispatch_time,
            "updated_at": dispatch_time,
            "match_strategy": "workflow+branch+created_at",
        }
    )
    save_cloud_record(record)

    fields: dict[str, str] = {}
    provider_input = workflow.get("provider_input")
    if provider_input:
        fields[provider_input] = provider
    fields.update(config_dispatch_fields)
    if selector_input and selector_json:
        fields[selector_input] = selector_json

    try:
        gh_workflow_dispatch(repository, workflow["file"], branch, fields)
    except RuntimeError as exc:
        print(f"Error: {exc}")
        return 1

    matched = gh_find_dispatched_run(
        repository,
        workflow["file"],
        branch,
        dispatch_time,
        timeout_secs=int(settings["match_timeout_secs"]),
    )

    if matched:
        record = enrich_cloud_record_provider_metadata(
            update_cloud_record_from_run(record, matched, provider_resolved=provider)
        )
        record["match_ambiguous"] = bool(matched.get("match_ambiguous"))
        save_cloud_record(record)

    print(f"Dispatched: {workflow_key} ref={branch} provider={provider}")
    print(f"  dispatch id: {dispatch_id}")
    if record.get("run_id"):
        print(f"  GitHub run: {record['run_id']}")
        if record.get("url"):
            print(f"  URL: {record['url']}")
    else:
        print("  warning: dispatched workflow could not be matched to a GitHub run yet")

    if not args.wait:
        return 0

    if not record.get("run_id"):
        print("Error: blocking wait requested, but the dispatched GitHub run could not be matched.")
        return 1

    while record.get("status") != "completed":
        time.sleep(int(settings["wait_poll_secs"]))
        try:
            record = refresh_cloud_record(record, repository, require_snapshot=True)
        except RuntimeError as exc:
            print(f"Error: {exc}")
            return 1

    print(f"  final: {record.get('status', '?')}/{(record.get('conclusion') or 'unknown').upper()}")
    return 0 if record.get("conclusion") == "success" else 1


def cmd_cloud_status(args: argparse.Namespace) -> int:
    config = _load_optional_config()
    if args.identifier is None:
        records = list_cloud_records(limit=args.limit)
        if not records:
            print("No tracked cloud runs yet.")
            return 0
        print("Recent cloud runs:\n")
        for item in records:
            print(f"  {cloud_record_summary(item, config)}")
        print()
        print_billing_period_summary(estimate_billing_period_totals(list_cloud_records(limit=None), config))
        return 0

    try:
        record = find_cloud_record(list_cloud_records(), args.identifier)
    except ValueError as exc:
        print(f"Error: {exc}")
        return 1

    if record is None:
        print("No matching cloud runs found.")
        return 1

    if args.refresh:
        if not gh_available():
            print("Error: gh CLI not available or not authenticated. Run: gh auth login")
            return 1
        try:
            repository = record.get("repository") or resolve_github_repository(
                resolve_github_actions_settings(_load_optional_config())
            )
        except ValueError as exc:
            print(f"Error: {exc}")
            return 1
        try:
            record = refresh_cloud_record(record, repository, require_snapshot=True)
        except RuntimeError as exc:
            print(f"Error: {exc}")
            return 1

    rendered_record = normalize_cloud_record(record)
    rendered_record["cost_summary"] = estimate_cloud_record_cost(rendered_record, config)
    print(cloud_record_summary(rendered_record, config))
    print(f"  workflow: {record.get('workflow_name')} ({record.get('workflow_file')})")
    print(f"  repo: {record.get('repository')}")
    print(f"  requested ref: {record.get('requested_ref')}")
    selector = summarize_runner_selector(record.get("runner_selector_json", ""))
    if selector:
        print(f"  runner selector: {selector}")
    dispatch_fields = record.get("dispatch_fields") or {}
    if isinstance(dispatch_fields, dict):
        for key in sorted(dispatch_fields):
            value = dispatch_fields.get(key)
            if not value:
                continue
            rendered = (
                summarize_runner_selector(value)
                if key.endswith("_selector_json")
                else str(value)
            )
            print(f"  {key}: {rendered}")
    if record.get("head_sha"):
        print(f"  sha: {short_sha(record['head_sha'])}")
    if record.get("url"):
        print(f"  url: {record['url']}")
    if record.get("matched_at"):
        print(f"  matched: {record['matched_at']}")
    if record.get("started_at"):
        print(f"  started: {record['started_at']}")
    if record.get("queue_delay_secs") is not None:
        print(f"  queue delay: {format_duration_secs(record.get('queue_delay_secs'))}")
    if record.get("duration_secs") is not None:
        print(f"  elapsed: {format_duration_secs(record.get('duration_secs'))}")
    if record.get("updated_at"):
        print(f"  updated: {record['updated_at']}")
    if record.get("completed_at"):
        print(f"  completed: {record['completed_at']}")
    print_namespace_usage_summary(rendered_record)
    print_billing_period_summary(estimate_billing_period_totals(list_cloud_records(limit=None), config))
    if record.get("jobs"):
        print("  jobs:")
        for job in record["jobs"]:
            status = job.get("status", "?")
            conclusion = job.get("conclusion", "")
            suffix = f"/{conclusion}" if conclusion else ""
            job_duration = format_duration_secs(
                duration_between(job.get("started_at"), job.get("completed_at"))
            )
            detail = f" duration={job_duration}" if job_duration else ""
            print(f"    {job.get('name', '?')}: {status}{suffix}{detail}")
    return 0


