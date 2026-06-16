"""Shared line rendering helpers for cloud command output."""
from __future__ import annotations

from collections.abc import Callable, Mapping, Sequence

from cloud_billing import billing_note_text


def cloud_history_lines(
    records: Sequence[dict],
    config: dict | None,
    *,
    limit: int,
    summary_fn: Callable[[dict, dict | None], str],
) -> list[str]:
    lines = ["Cloud history:", ""]
    for record in records[:limit]:
        lines.append(f"  {summary_fn(record, config)}")
    return lines


def cloud_recent_status_lines(
    records: Sequence[dict],
    config: dict | None,
    *,
    summary_fn: Callable[[dict, dict | None], str],
) -> list[str]:
    lines = ["Recent cloud runs:", ""]
    for record in records:
        lines.append(f"  {summary_fn(record, config)}")
    lines.append("")
    return lines


def cloud_recommend_lines(workflow_key: str, provider: str | None, reason: str) -> list[str]:
    if not provider:
        return [f"No recommendation for workflow '{workflow_key}': {reason}."]
    return [
        f"Recommended provider for {workflow_key}: {provider} ({reason})",
        f"  note: {billing_note_text()}",
    ]


def cloud_workflow_lines(workflows: Mapping[str, dict]) -> list[str]:
    lines = ["GitHub Actions workflows:", ""]
    for key, info in workflows.items():
        providers = ", ".join(info.get("providers", [])) or "github-hosted"
        lines.append(f"  {key:12s} {info['display_name']} ({info['file']})")
        lines.append(f"               providers: {providers}")
    return lines


def cloud_dispatch_lines(record: dict, *, workflow_key: str, branch: str, provider: str) -> list[str]:
    lines = [
        f"Dispatched: {workflow_key} ref={branch} provider={provider}",
        f"  dispatch id: {record.get('dispatch_id', '')}",
    ]
    if record.get("run_id"):
        lines.append(f"  GitHub run: {record['run_id']}")
        if record.get("url"):
            lines.append(f"  URL: {record['url']}")
    else:
        lines.append("  warning: dispatched workflow could not be matched to a GitHub run yet")
    return lines


def cloud_final_status_line(record: dict) -> str:
    return f"  final: {record.get('status', '?')}/{(record.get('conclusion') or 'unknown').upper()}"
