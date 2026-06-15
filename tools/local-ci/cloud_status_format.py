"""Line rendering helpers for `pulp ci-local cloud status`."""
from __future__ import annotations

from cloud_records import duration_between, format_duration_secs, summarize_runner_selector
from git_helpers import short_sha


def cloud_status_detail_lines(record: dict) -> list[str]:
    lines = [
        f"  workflow: {record.get('workflow_name')} ({record.get('workflow_file')})",
        f"  repo: {record.get('repository')}",
        f"  requested ref: {record.get('requested_ref')}",
    ]
    selector = summarize_runner_selector(record.get("runner_selector_json", ""))
    if selector:
        lines.append(f"  runner selector: {selector}")
    dispatch_fields = record.get("dispatch_fields") or {}
    if isinstance(dispatch_fields, dict):
        for key in sorted(dispatch_fields):
            value = dispatch_fields.get(key)
            if not value:
                continue
            rendered = summarize_runner_selector(value) if key.endswith("_selector_json") else str(value)
            lines.append(f"  {key}: {rendered}")
    if record.get("head_sha"):
        lines.append(f"  sha: {short_sha(record['head_sha'])}")
    if record.get("url"):
        lines.append(f"  url: {record['url']}")
    if record.get("matched_at"):
        lines.append(f"  matched: {record['matched_at']}")
    if record.get("started_at"):
        lines.append(f"  started: {record['started_at']}")
    if record.get("queue_delay_secs") is not None:
        lines.append(f"  queue delay: {format_duration_secs(record.get('queue_delay_secs'))}")
    if record.get("duration_secs") is not None:
        lines.append(f"  elapsed: {format_duration_secs(record.get('duration_secs'))}")
    if record.get("updated_at"):
        lines.append(f"  updated: {record['updated_at']}")
    if record.get("completed_at"):
        lines.append(f"  completed: {record['completed_at']}")
    return lines


def cloud_status_job_lines(record: dict) -> list[str]:
    if not record.get("jobs"):
        return []

    lines = ["  jobs:"]
    for job in record["jobs"]:
        status = job.get("status", "?")
        conclusion = job.get("conclusion", "")
        suffix = f"/{conclusion}" if conclusion else ""
        job_duration = format_duration_secs(
            duration_between(job.get("started_at"), job.get("completed_at"))
        )
        detail = f" duration={job_duration}" if job_duration else ""
        lines.append(f"    {job.get('name', '?')}: {status}{suffix}{detail}")
    return lines
