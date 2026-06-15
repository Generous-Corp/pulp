"""Cloud provider cost-estimation helpers."""
from __future__ import annotations

from cloud_billing_config import resolve_billing_settings
from cloud_records import duration_between, normalize_cloud_record


def billing_note_text() -> str:
    return "estimated; verify provider pricing"


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
