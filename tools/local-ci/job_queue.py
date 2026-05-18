"""Queue persistence + job normalization for local CI.

Extracted from local_ci.py as part of the R2-1 phased split. The three
helpers here own:

  - `normalize_job` — fill in id (sha1 of branch|sha|queued_at if missing),
    priority, target list ordering, status, validation mode, and the
    nested provenance dict. Pure function, no I/O.
  - `load_queue_unlocked` — read queue.json, tolerate both the legacy
    bare-array format and the modern {"jobs": [...]} envelope, run every
    entry through `normalize_job`. Reader uses no lock — caller must
    hold `queue_lock_path()` if the read needs to be coherent with a
    concurrent writer.
  - `save_queue_unlocked` — write the queue back atomically. Same lock
    discipline.

The higher-level `load_queue` (which acquires the queue lock and calls
the reconcile-running-jobs cleanup) stays in local_ci.py because it
pulls in the running-job state machine — not part of this seam.
"""

from __future__ import annotations

import hashlib
import json

from io_utils import atomic_write_text
from normalize import normalize_priority, normalize_validation_mode
from provenance import normalize_provenance
from state_paths import queue_path


def normalize_job(job: dict) -> dict:
    normalized = dict(job)
    if "id" not in normalized:
        legacy_raw = "|".join(
            [normalized.get("branch", ""), normalized.get("sha", ""), normalized.get("queued_at", "")]
        )
        normalized["id"] = hashlib.sha1(legacy_raw.encode("utf-8")).hexdigest()[:12]
    normalized["priority"] = normalize_priority(normalized.get("priority", "normal"))
    normalized["targets"] = sorted(dict.fromkeys(normalized.get("targets") or []))
    normalized["status"] = normalized.get("status", "pending")
    normalized["validation"] = normalize_validation_mode(normalized.get("validation", "full"))
    submission = dict(normalized.get("submission") or {})
    submission["provenance"] = normalize_provenance(submission.get("provenance"))
    normalized["submission"] = submission
    normalized["provenance"] = normalize_provenance(
        normalized.get("provenance") or submission.get("provenance")
    )
    return normalized


def load_queue_unlocked() -> list[dict]:
    path = queue_path()
    if not path.exists():
        return []

    raw = json.loads(path.read_text())
    jobs = raw.get("jobs", raw) if isinstance(raw, dict) else raw
    return [normalize_job(job) for job in jobs]


def save_queue_unlocked(queue: list[dict]) -> None:
    atomic_write_text(queue_path(), json.dumps(queue, indent=2) + "\n")
