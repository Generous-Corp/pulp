"""Cloud provider comparison and recommendation helpers."""
from __future__ import annotations

import statistics

from cloud_billing import (
    estimate_billing_period_totals,
    estimate_cloud_record_cost,
    resolve_billing_settings,
)
from cloud_records import normalize_cloud_record


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
