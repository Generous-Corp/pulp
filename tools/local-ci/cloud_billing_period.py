"""Cloud billing period window and period-total helpers."""
from __future__ import annotations

from datetime import date, datetime, timezone
from typing import Callable

from cloud_billing_config import resolve_billing_settings
from cloud_billing_estimates import billing_note_text, estimate_cloud_record_cost
from cloud_records import normalize_cloud_record, parse_iso_datetime


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


def estimate_billing_period_totals(
    records: list[dict],
    config: dict | None,
    *,
    provider: str | None = None,
    period_window_func: Callable[..., tuple[datetime, datetime]] | None = None,
    estimate_cloud_record_cost_fn: Callable[[dict, dict | None], dict] = estimate_cloud_record_cost,
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
        summary = estimate_cloud_record_cost_fn(record, config)
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
