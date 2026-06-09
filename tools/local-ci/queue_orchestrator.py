"""Pure queue policy helpers for local CI.

This module owns job identity, priority ordering, supersedence, cancellation
result payloads, summaries, and completed-queue retention. Higher-level queue
mutation, locking, runner liveness, result persistence, and drain orchestration
remain in local_ci.py until later extraction slices.
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
from provenance import normalize_provenance


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
