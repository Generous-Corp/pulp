"""Pure cloud billing and cost-estimation helpers for local CI."""
from __future__ import annotations

from datetime import date, datetime, timezone

from cloud_records import duration_between, normalize_cloud_record, parse_iso_datetime


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
    period_window_func=None,
) -> dict:
    billing = resolve_billing_settings(config)
    window_func = period_window_func or billing_period_window
    period_start, period_end = window_func(billing["billing_period_start_day"])
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
