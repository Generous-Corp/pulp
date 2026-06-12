"""Line rendering helpers for `pulp ci-local cloud compare`."""
from __future__ import annotations

from cloud_billing import format_currency_amount
from cloud_records import format_duration_secs


def cloud_compare_summary_line(summary: dict) -> str:
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
    return line
