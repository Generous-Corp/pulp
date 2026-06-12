"""Pure cloud billing and cost-estimation helpers for local CI."""
from __future__ import annotations

from cloud_billing_config import (
    parse_optional_bool,
    parse_rate_value,
    resolve_billing_settings,
)
from cloud_billing_estimates import (
    billing_note_text,
    estimate_cloud_record_cost,
    estimate_github_hosted_cost,
    estimate_namespace_cost,
    infer_job_os,
    match_namespace_shape_rate,
)
from cloud_billing_period import (
    billing_period_window,
    estimate_billing_period_totals as _estimate_billing_period_totals,
    iter_year_months,
    parse_iso_date,
)


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


def provider_billing_note_text() -> str:
    return "actual when available"


def estimate_billing_period_totals(
    records: list[dict],
    config: dict | None,
    *,
    provider: str | None = None,
    period_window_func=None,
) -> dict:
    return _estimate_billing_period_totals(
        records,
        config,
        provider=provider,
        period_window_func=period_window_func or billing_period_window,
        estimate_cloud_record_cost_fn=estimate_cloud_record_cost,
    )


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
