"""Namespace provider usage normalization and output helpers for local CI."""
from __future__ import annotations

from cloud_billing import billing_note_text, format_currency_amount
from cloud_records import (
    duration_between,
    format_duration_secs,
    format_memory_megabytes,
    normalize_github_timestamp,
)


def namespace_instance_duration_secs(instance: dict, *, now_iso_fn) -> float | None:
    created_at = instance.get("created")
    completed_at = instance.get("destroyed_at") or now_iso_fn()
    return duration_between(created_at, completed_at)


def normalize_namespace_instance(instance: dict, *, now_iso_fn) -> dict:
    shape = instance.get("shape") or {}
    user_label = instance.get("user_label") or {}
    github_workflow = instance.get("github_workflow") or {}
    duration_secs = namespace_instance_duration_secs(instance, now_iso_fn=now_iso_fn)
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
