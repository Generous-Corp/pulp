"""Cloud provider metadata enrichment helpers."""
from __future__ import annotations

from collections.abc import Callable


def enrich_cloud_record_provider_metadata(
    record: dict,
    *,
    normalize_cloud_record_fn: Callable[[dict], dict],
    nsc_logged_in_fn: Callable[[], bool],
    namespace_instances_for_run_fn: Callable[[str, int], list[dict]],
    summarize_namespace_usage_fn: Callable[[list[dict]], dict],
) -> dict:
    updated = normalize_cloud_record_fn(record)
    provider = updated.get("provider_resolved") or updated.get("provider_requested") or "github-hosted"
    if provider != "namespace" or not updated.get("run_id") or not nsc_logged_in_fn():
        if provider != "namespace":
            updated["provider_metadata"] = {}
            updated["usage_summary"] = {}
            updated["cost_summary"] = {}
        return updated

    instances = namespace_instances_for_run_fn(updated.get("repository", ""), int(updated["run_id"]))
    if not instances:
        return updated

    updated["provider_metadata"] = {"namespace_instances": instances}
    updated["usage_summary"] = summarize_namespace_usage_fn(instances)
    updated["cost_summary"] = {
        "status": "unavailable",
        "reason": "Namespace CLI does not expose billing totals; provider runtime is shown instead.",
    }
    return updated
