"""Queue status and result line formatting helpers for local CI."""

from __future__ import annotations

from pathlib import Path

from git_helpers import short_sha
from provenance import provenance_summary


def summarize_job(job: dict) -> str:
    targets = ",".join(job.get("targets") or []) or "none"
    validation = job.get("validation", "full")
    validation_suffix = f" validation={validation}" if validation != "full" else ""
    return (
        f"[{job['id']}] {job['branch']} @ {short_sha(job.get('sha', ''))} "
        f"priority={job.get('priority', 'normal')} targets={targets}{validation_suffix}"
    )


def enqueue_command_result_line(job: dict, *, created: bool) -> str:
    prefix = "Enqueued" if created else "Already queued/running"
    return f"{prefix}: {summarize_job(job)}"


def bump_queue_command_result_line(result: dict, job_ref: str) -> tuple[int, str]:
    if result["status"] == "missing":
        return 1, f"No active job matches '{job_ref}'."
    if result["status"] == "not_pending":
        return 1, f"Job is already {result['job_status']}; only pending jobs can be reprioritized."
    return 0, f"Updated priority: {result['summary']}"


def cancel_queue_command_result_line(result: dict, job_ref: str) -> tuple[int, str]:
    if result["status"] == "missing":
        return 1, f"No active job matches '{job_ref}'."
    if result["status"] == "not_pending":
        return 1, f"Job is already {result['job_status']}; only pending jobs can be canceled safely."
    return 0, f"Canceled: {result['summary']}"


def drain_runner_active_line(runner_info: dict | None) -> str:
    if runner_info and runner_info.get("active_job_id"):
        return (
            f"Another local CI runner is active [{runner_info['active_job_id']}] "
            f"{runner_info.get('active_branch', '?')}."
        )
    return "Another local CI runner is active."


def summarize_active_targets(active_targets: dict | None, preferred_order: list[str] | None = None) -> str:
    if not active_targets:
        return ""

    parts: list[str] = []
    seen: set[str] = set()
    for name in preferred_order or []:
        state = active_targets.get(name)
        if not state:
            continue
        parts.append(f"{name}={state.get('status', '?')}")
        seen.add(name)

    for name in sorted(active_targets):
        if name in seen:
            continue
        state = active_targets.get(name) or {}
        parts.append(f"{name}={state.get('status', '?')}")

    return ", ".join(parts)


def status_active_targets(job: dict, runner_info: dict | None = None) -> dict | None:
    active_targets = job.get("active_targets")
    if active_targets:
        return active_targets

    if runner_info and runner_info.get("active_job_id") == job["id"]:
        return runner_info.get("active_targets")

    return None


def status_target_states(job: dict, active_targets: dict | None) -> list[tuple[str, dict]]:
    if not active_targets:
        return []

    states: list[tuple[str, dict]] = []
    for name in job.get("targets") or []:
        state = active_targets.get(name)
        if state:
            states.append((name, state))
    return states


def status_submission_lines(job: dict) -> list[str]:
    submission = job.get("submission") or {}
    lines: list[str] = []
    if submission.get("config_path"):
        lines.append(
            "submission: "
            f"root={submission.get('submitted_root', '?')} "
            f"config={submission.get('config_path')} "
            f"({submission.get('config_source', '?')})"
        )
    if submission.get("provenance"):
        lines.append(f"provenance: {provenance_summary(submission.get('provenance'))}")
    return lines


def target_state_detail_parts(state: dict) -> list[str]:
    details = []
    field_labels = [
        ("phase", "phase"),
        ("validation_mode", "mode"),
        ("transport_mode", "transport"),
        ("test_policy", "tests"),
        ("prepared_state", "prepared"),
        ("wait_reason", "wait"),
        ("cleanup_status", "cleanup"),
        ("last_output_at", "output"),
        ("last_heartbeat_at", "heartbeat"),
    ]
    for field, label in field_labels:
        if state.get(field):
            details.append(f"{label}={state[field]}")
    if state.get("quiet_for_secs") is not None:
        details.append(f"idle={state['quiet_for_secs']}s")
    if state.get("liveness"):
        details.append(f"liveness={state['liveness']}")
    if state.get("log_path"):
        details.append(f"log={Path(state['log_path']).name}")
    return details


def status_target_detail_lines(job: dict, active_targets: dict | None) -> list[str]:
    lines: list[str] = []
    for name, state in status_target_states(job, active_targets):
        details = target_state_detail_parts(state)
        if details:
            lines.append(f"{name}: " + ", ".join(details))
        if state.get("last_line"):
            lines.append(f"  {state['last_line']}")
        if state.get("cleanup_result"):
            lines.append(f"  cleanup: {state['cleanup_result']}")
    return lines


def status_runner_line(runner_info: dict | None) -> str:
    if not runner_info:
        return "Runner: idle"
    active_job = runner_info.get("active_job_id") or "?"
    active_branch = runner_info.get("active_branch") or "?"
    return f"Runner: pid={runner_info.get('pid', '?')} active=[{active_job}] {active_branch}"


def recent_completed_status_line(job: dict, result: dict) -> str:
    targets = ", ".join(f"{item['target']}={item['status']}" for item in result.get("results", []))
    return (
        f"[{job['id']}] {job['branch']} @ {short_sha(job.get('sha', ''))} "
        f"{result.get('overall', '?').upper()} [{targets}] "
        f"via {provenance_summary(result.get('provenance'))}"
    )


def recent_completed_missing_result_line(job: dict) -> str:
    return f"{summarize_job(job)} (result file missing)"


def result_validation_line(result: dict) -> str | None:
    validation = result.get("validation", "full")
    if validation == "full":
        return None
    return f"  {'validation':10s}  {validation}"


def result_execution_line(result: dict) -> str:
    return f"  {'execution':10s}  {provenance_summary(result.get('provenance'))}"


def target_result_line(item: dict) -> str:
    icon = "PASS" if item["status"] == "pass" else item["status"].upper()
    return f"  {item['target']:10s}  {icon:12s}  {item.get('duration_secs', 0)}s"


def result_target_lines(result: dict) -> list[str]:
    return [target_result_line(item) for item in result.get("results", [])]


def result_overall_line(result: dict) -> str:
    return f"  {'overall':10s}  {result['overall'].upper()}"


def missing_job_logs_line() -> str:
    return "No matching job logs found."


def missing_log_files_line(job: dict) -> str:
    return f"No logs found for job [{job['id']}] {job['branch']}."


def job_logs_header_line(job: dict) -> str:
    return f"Logs for [{job['id']}] {job['branch']} @ {short_sha(job.get('sha', ''))}"


def log_section_header_line(target: str) -> str:
    return f"== {target} =="


def empty_log_line() -> str:
    return "(empty)"
