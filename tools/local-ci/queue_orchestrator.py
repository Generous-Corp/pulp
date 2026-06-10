"""Pure queue policy helpers for local CI.

This module owns job identity, enqueue duplicate/priority policy, enqueue
supersedence candidate selection, queue-command lookup and priority mutation,
priority ordering, supersedence, cancellation result payloads, summaries,
target-state status detail formatting, status active-target selection and
recent-completed selection, log-command line fragments,
queue-command result line fragments, queue and result status line fragments,
runner status line fragments, recent-completed result summaries,
stale-running job selection/replacement/requeue state, stale-running
reconciliation action selection, runner-info active-target mutation,
completed-job state mutation, queue status grouping, and completed-queue
retention. Higher-level queue mutation, locking, runner
liveness, result persistence, and drain orchestration remain in local_ci.py
until later extraction slices.
"""

from __future__ import annotations

from collections.abc import Callable
import hashlib
import json
from pathlib import Path
import re
import uuid

from git_helpers import now_iso, short_sha
from normalize import normalize_priority, normalize_validation_mode, priority_value
from provenance import normalize_provenance, provenance_summary


ROOT = Path(__file__).resolve().parents[2]
_SAFE_CI_BRANCH_RE = re.compile(r"^[A-Za-z0-9._/-]+$")


def validate_ci_branch_name(branch: str) -> str:
    normalized = (branch or "").strip()
    if not normalized:
        raise ValueError("CI branch name is required")
    if not _SAFE_CI_BRANCH_RE.fullmatch(normalized):
        raise ValueError(
            "Unsupported branch name for local-ci transport. "
            "Use letters, numbers, dot, underscore, slash, or hyphen only."
        )
    return normalized


def default_priority_for(command: str, config: dict) -> str:
    defaults = config.get("defaults", {})
    if command in {"ship", "check"}:
        return normalize_priority(defaults.get(f"{command}_priority", "high"))
    return normalize_priority(defaults.get("priority", "normal"))


def make_fingerprint(branch: str, sha: str, targets: list[str], validation: str) -> str:
    raw = json.dumps(
        {"branch": branch, "sha": sha, "targets": sorted(targets), "validation": validation},
        sort_keys=True,
    )
    return hashlib.sha256(raw.encode("utf-8")).hexdigest()


def make_job(
    branch: str,
    sha: str,
    priority: str,
    targets: list[str],
    mode: str,
    validation: str,
    submission: dict | None = None,
    *,
    now_iso_fn: Callable[[], str] = now_iso,
    uuid_hex_fn: Callable[[], str] = lambda: uuid.uuid4().hex,
    root: Path | str = ROOT,
    validate_branch_fn: Callable[[str], str] = validate_ci_branch_name,
) -> dict:
    normalized_validation = normalize_validation_mode(validation)
    branch = validate_branch_fn(branch)
    job = {
        "id": uuid_hex_fn()[:12],
        "branch": branch,
        "sha": sha,
        "priority": normalize_priority(priority),
        "targets": sorted(targets),
        "queued_at": now_iso_fn(),
        "status": "pending",
        "fingerprint": make_fingerprint(branch, sha, targets, normalized_validation),
        "mode": mode,
        "validation": normalized_validation,
        "submitted_root": str(root),
    }
    if submission:
        job["submission"] = submission
        if submission.get("submitted_root"):
            job["submitted_root"] = submission["submitted_root"]
        if submission.get("provenance"):
            job["provenance"] = normalize_provenance(submission.get("provenance"))
    if "provenance" not in job:
        job["provenance"] = normalize_provenance()
    return job


def find_active_job_by_fingerprint_unlocked(queue: list[dict], fingerprint: str) -> dict | None:
    for job in queue:
        if job.get("fingerprint") == fingerprint and job.get("status") in {"pending", "running"}:
            return job
    return None


def bump_pending_job_priority_unlocked(
    job: dict,
    requested_priority: str,
    *,
    now_iso_fn: Callable[[], str] = now_iso,
) -> bool:
    requested_priority = normalize_priority(requested_priority)
    if job.get("status") != "pending":
        return False
    if priority_value(requested_priority) <= priority_value(job.get("priority", "normal")):
        return False

    job["priority"] = requested_priority
    job["bumped_at"] = now_iso_fn()
    return True


def find_queue_command_job_unlocked(queue: list[dict], job_ref: str) -> dict | None:
    return find_job_unlocked(queue, job_ref, statuses={"pending", "running"})


def set_pending_job_priority_unlocked(
    job: dict,
    requested_priority: str,
    *,
    now_iso_fn: Callable[[], str] = now_iso,
) -> bool:
    if job.get("status") != "pending":
        return False

    job["priority"] = normalize_priority(requested_priority)
    job["bumped_at"] = now_iso_fn()
    return True


def supersedence_key(job: dict) -> tuple[str, tuple[str, ...], str]:
    return (
        job.get("branch", ""),
        tuple(sorted(job.get("targets") or [])),
        normalize_validation_mode(job.get("validation", "full")),
    )


def supersedence_identity_key(job: dict) -> tuple[str, str, str]:
    return (
        job.get("branch", ""),
        job.get("sha", ""),
        normalize_validation_mode(job.get("validation", "full")),
    )


def jobs_share_supersedence_scope(newer_job: dict, older_job: dict) -> bool:
    return (
        newer_job.get("id") != older_job.get("id")
        and newer_job.get("fingerprint") != older_job.get("fingerprint")
        and supersedence_key(newer_job) == supersedence_key(older_job)
    )


def job_has_narrower_same_identity_scope(newer_job: dict, older_job: dict) -> bool:
    if (
        newer_job.get("id") == older_job.get("id")
        or newer_job.get("fingerprint") == older_job.get("fingerprint")
        or supersedence_identity_key(newer_job) != supersedence_identity_key(older_job)
    ):
        return False

    newer_targets = set(newer_job.get("targets") or [])
    older_targets = set(older_job.get("targets") or [])
    return bool(newer_targets) and newer_targets < older_targets


def supersedence_reason(newer_job: dict, older_job: dict) -> str | None:
    if jobs_share_supersedence_scope(newer_job, older_job):
        return "newer_sha_queued"
    if job_has_narrower_same_identity_scope(newer_job, older_job):
        return "narrower_scope_queued"
    return None


def pending_supersedence_candidates_unlocked(queue: list[dict], newer_job: dict) -> list[tuple[dict, str]]:
    candidates: list[tuple[dict, str]] = []
    for job in queue:
        if job.get("status") != "pending":
            continue
        reason = supersedence_reason(newer_job, job)
        if reason:
            candidates.append((job, reason))
    return candidates


def supersedence_result(
    job: dict,
    superseded_by: str,
    reason: str,
    *,
    now_iso_fn: Callable[[], str] = now_iso,
) -> dict:
    return {
        "job_id": job["id"],
        "branch": job["branch"],
        "sha": job["sha"],
        "priority": job["priority"],
        "validation": job.get("validation", "full"),
        "targets": job.get("targets", []),
        "queued_at": job.get("queued_at", ""),
        "completed_at": now_iso_fn(),
        "provenance": normalize_provenance(job.get("provenance")),
        "results": [],
        "overall": "superseded",
        "superseded_by": superseded_by,
        "superseded_reason": reason,
    }


def cancellation_result(
    job: dict,
    reason: str,
    *,
    now_iso_fn: Callable[[], str] = now_iso,
) -> dict:
    return {
        "job_id": job["id"],
        "branch": job["branch"],
        "sha": job["sha"],
        "priority": job["priority"],
        "validation": job.get("validation", "full"),
        "targets": job.get("targets", []),
        "queued_at": job.get("queued_at", ""),
        "completed_at": now_iso_fn(),
        "provenance": normalize_provenance(job.get("provenance")),
        "results": [],
        "overall": "canceled",
        "canceled_reason": reason,
    }


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


def complete_job_with_result_unlocked(job: dict, result: dict, result_path: Path | str) -> None:
    job["status"] = "completed"
    job["completed_at"] = result["completed_at"]
    job["result_file"] = str(result_path)
    job["overall"] = result.get("overall")
    for key in ("superseded_by", "superseded_reason", "canceled_reason"):
        if key in result:
            job[key] = result[key]
    job.pop("runner", None)
    job.pop("active_targets", None)
    job.pop("last_progress_at", None)


def summarize_job(job: dict) -> str:
    targets = ",".join(job.get("targets") or []) or "none"
    validation = job.get("validation", "full")
    validation_suffix = f" validation={validation}" if validation != "full" else ""
    return (
        f"[{job['id']}] {job['branch']} @ {short_sha(job.get('sha', ''))} "
        f"priority={job.get('priority', 'normal')} targets={targets}{validation_suffix}"
    )


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


def update_runner_info_active_targets(
    info: dict,
    job_id: str,
    active_targets: dict | None,
    *,
    now_iso_fn: Callable[[], str] = now_iso,
) -> bool:
    if info.get("active_job_id") != job_id:
        return False

    if active_targets:
        info["active_targets"] = active_targets
    else:
        info.pop("active_targets", None)
    info["updated_at"] = now_iso_fn()
    return True


def find_stale_running_replacement_unlocked(queue: list[dict], job: dict) -> tuple[dict | None, str | None]:
    replacement = None
    replacement_reason = None
    for candidate in queue:
        if candidate.get("status") not in {"pending", "running"}:
            continue
        reason = supersedence_reason(candidate, job)
        if not reason:
            continue
        if replacement is None or candidate.get("queued_at", "") > replacement.get("queued_at", ""):
            replacement = candidate
            replacement_reason = reason
    return replacement, replacement_reason


def stale_running_reconciliation_actions_unlocked(queue: list[dict], stale_jobs: list[dict]) -> list[dict]:
    actions: list[dict] = []
    for job in stale_jobs:
        replacement, reason = find_stale_running_replacement_unlocked(queue, job)
        if replacement is not None:
            actions.append(
                {
                    "action": "supersede",
                    "job": job,
                    "replacement": replacement,
                    "reason": reason or "newer_sha_queued",
                }
            )
        else:
            actions.append({"action": "requeue", "job": job})
    return actions


def stale_running_jobs_for_runner_unlocked(queue: list[dict], runner_pid: int | None) -> list[dict]:
    stale: list[dict] = []
    for job in queue:
        if job.get("status") != "running":
            continue
        job_runner = job.get("runner") or {}
        if runner_pid and job_runner.get("pid") == runner_pid:
            continue
        stale.append(job)
    return stale


def requeue_stale_running_job_unlocked(
    job: dict,
    *,
    now_iso_fn: Callable[[], str] = now_iso,
) -> None:
    job["status"] = "pending"
    job["requeued_at"] = now_iso_fn()
    job.pop("started_at", None)
    job.pop("runner", None)


def upsert_job_active_targets_unlocked(
    queue: list[dict],
    job_id: str,
    active_targets: dict | None,
    *,
    now_iso_fn: Callable[[], str] = now_iso,
) -> bool:
    for job in queue:
        if job["id"] != job_id:
            continue
        if active_targets:
            job["active_targets"] = active_targets
            job["last_progress_at"] = now_iso_fn()
        else:
            job.pop("active_targets", None)
            job.pop("last_progress_at", None)
        return True
    return False


def update_job_target_state_unlocked(
    queue: list[dict],
    job_id: str,
    target_name: str,
    fields: dict,
    *,
    now_iso_fn: Callable[[], str] = now_iso,
) -> bool:
    job = find_job_unlocked(queue, job_id)
    if job is None:
        return False

    active_targets = dict(job.get("active_targets") or {})
    state = dict(active_targets.get(target_name) or {})
    for key, value in fields.items():
        if value is None:
            state.pop(key, None)
        else:
            state[key] = value

    if state:
        active_targets[target_name] = state
    else:
        active_targets.pop(target_name, None)

    upsert_job_active_targets_unlocked(
        queue,
        job["id"],
        active_targets if active_targets else None,
        now_iso_fn=now_iso_fn,
    )
    return True


def complete_job_unlocked(
    queue: list[dict],
    job_id: str,
    result: dict,
    result_path: Path | str,
    *,
    now_iso_fn: Callable[[], str] = now_iso,
) -> bool:
    for job in queue:
        if job["id"] != job_id:
            continue
        job["status"] = "completed"
        job["completed_at"] = now_iso_fn()
        job["result_file"] = str(result_path)
        job["overall"] = result.get("overall")
        job.pop("runner", None)
        job.pop("active_targets", None)
        job.pop("last_progress_at", None)
        return True
    return False


def trim_completed_jobs_with_removed_ids(
    queue: list[dict],
    *,
    keep_completed_jobs: int,
) -> tuple[list[dict], set[str]]:
    completed = [job for job in queue if job.get("status") == "completed"]
    if len(completed) <= keep_completed_jobs:
        return queue, set()

    completed_by_time = sorted(completed, key=lambda job: job.get("completed_at", job.get("queued_at", "")))
    remove_ids = {job["id"] for job in completed_by_time[:-keep_completed_jobs]}
    return [job for job in queue if job["id"] not in remove_ids], remove_ids


def trim_completed_jobs(queue: list[dict], *, keep_completed_jobs: int) -> list[dict]:
    trimmed, _removed_ids = trim_completed_jobs_with_removed_ids(
        queue,
        keep_completed_jobs=keep_completed_jobs,
    )
    return trimmed


def job_sort_key(job: dict) -> tuple[int, str, str]:
    return (-priority_value(job.get("priority", "normal")), job.get("queued_at", ""), job["id"])


def queue_status_groups(queue: list[dict]) -> tuple[list[dict], list[dict], list[dict]]:
    pending = sorted([job for job in queue if job.get("status") == "pending"], key=job_sort_key)
    running = [job for job in queue if job.get("status") == "running"]
    completed = [job for job in queue if job.get("status") == "completed"]
    return pending, running, completed


def recent_completed_jobs_for_status(completed_jobs: list[dict], *, limit: int = 5) -> list[dict]:
    if limit <= 0:
        return []
    return completed_jobs[-limit:]


def claim_next_job_unlocked(
    queue: list[dict],
    *,
    runner: dict,
    now_iso_fn: Callable[[], str] = now_iso,
) -> dict | None:
    pending = sorted(
        [job for job in queue if job.get("status") == "pending"],
        key=job_sort_key,
    )
    if not pending:
        return None

    selected_id = pending[0]["id"]
    for job in queue:
        if job["id"] != selected_id:
            continue
        job["status"] = "running"
        job["started_at"] = now_iso_fn()
        job["runner"] = runner
        job.pop("active_targets", None)
        job.pop("last_progress_at", None)
        return job

    return None


def find_job_unlocked(queue: list[dict], job_ref: str, statuses: set[str] | None = None) -> dict | None:
    candidates = queue
    if statuses is not None:
        candidates = [job for job in candidates if job.get("status") in statuses]

    for job in candidates:
        if job["id"] == job_ref:
            return job

    id_prefix = [job for job in candidates if job["id"].startswith(job_ref)]
    if len(id_prefix) == 1:
        return id_prefix[0]
    if len(id_prefix) > 1:
        raise ValueError(f"Job reference '{job_ref}' is ambiguous.")

    branch_matches = [job for job in candidates if job.get("branch") == job_ref]
    if len(branch_matches) == 1:
        return branch_matches[0]
    if len(branch_matches) > 1:
        raise ValueError(
            f"Multiple jobs match branch '{job_ref}'. Use a job id or unique prefix."
        )

    return None


def select_job_for_logs(queue: list[dict], runner_info: dict | None, job_ref: str | None) -> dict | None:
    if job_ref:
        return find_job_unlocked(queue, job_ref)

    if runner_info and runner_info.get("active_job_id"):
        return find_job_unlocked(queue, runner_info["active_job_id"])

    for job in reversed(queue):
        if job.get("status") == "completed":
            return job
    return None


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
