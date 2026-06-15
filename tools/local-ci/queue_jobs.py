"""Queue job identity and priority helpers for local CI."""

from __future__ import annotations

from collections.abc import Callable
import hashlib
import json
from pathlib import Path
import re
import uuid

from git_helpers import now_iso
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
