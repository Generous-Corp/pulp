#!/usr/bin/env python3
"""Local CI runner for Pulp — validates queued jobs on Mac, Ubuntu, and Windows.

Usage:
    pulp ci-local run [branch]                # Queue and wait for completion
    pulp ci-local run [branch] --smoke        # Fast install/export preflight, no tests
    pulp ci-local ship [branch]               # PR -> queued CI -> merge on green
    pulp ci-local check <PR#|latest>          # Validate an existing PR
    pulp ci-local check <PR#|latest> --smoke  # Fast PR smoke preflight
    pulp ci-local list                        # Show open PRs
    pulp ci-local status                      # Show queue, runner, and VM status
    pulp ci-local enqueue [branch]            # Queue for later drain
    pulp ci-local drain                       # Drain pending jobs if no runner is active
    pulp ci-local bump <job> <priority>       # Reprioritize a pending job

Queueing model:
    - CI state is machine-global, not per worktree.
    - Only one drain owner runs jobs at a time.
    - Jobs are ordered by priority, then FIFO within each priority.
    - Each job validates an exact git SHA.
    - SSH targets receive the queued SHA via a git bundle before validation.
"""

from __future__ import annotations

import argparse
from collections import deque
from collections import defaultdict
import fcntl
import hashlib
import json
import os
import queue as queue_module
import shlex
import subprocess
import sys
import threading
import time
import uuid
from concurrent.futures import ThreadPoolExecutor, as_completed
from contextlib import contextmanager
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT_DIR = Path(__file__).resolve().parent
PRIORITY_VALUES = {"low": 10, "normal": 50, "high": 100}
WAIT_POLL_SECS = 3
KEEP_COMPLETED_JOBS = 25
HEARTBEAT_INTERVAL_SECS = 15.0
STUCK_IDLE_SECS = 90.0
_BUNDLE_BUILD_LOCK = threading.Lock()


class LockBusyError(RuntimeError):
    """Raised when a non-blocking lock cannot be acquired."""


def state_dir() -> Path:
    override = os.environ.get("PULP_LOCAL_CI_HOME")
    if override:
        return Path(override).expanduser()

    home = Path.home()
    if sys.platform == "darwin":
        return home / "Library" / "Application Support" / "Pulp" / "local-ci"

    xdg_state = os.environ.get("XDG_STATE_HOME")
    if xdg_state:
        return Path(xdg_state).expanduser() / "pulp" / "local-ci"
    return home / ".local" / "state" / "pulp" / "local-ci"


def config_path() -> Path:
    override = os.environ.get("PULP_LOCAL_CI_CONFIG")
    if override:
        return Path(override).expanduser()
    return SCRIPT_DIR / "config.json"


def queue_path() -> Path:
    return state_dir() / "queue.json"


def results_dir() -> Path:
    return state_dir() / "results"


def evidence_path() -> Path:
    return state_dir() / "evidence.json"


def logs_dir() -> Path:
    return state_dir() / "logs"


def bundles_dir() -> Path:
    return state_dir() / "bundles"


def queue_lock_path() -> Path:
    return state_dir() / "queue.lock"


def evidence_lock_path() -> Path:
    return state_dir() / "evidence.lock"


def drain_lock_path() -> Path:
    return state_dir() / "drain.lock"


def runner_info_path() -> Path:
    return state_dir() / "runner.json"


def ensure_state_dirs() -> None:
    state_dir().mkdir(parents=True, exist_ok=True)
    results_dir().mkdir(parents=True, exist_ok=True)
    logs_dir().mkdir(parents=True, exist_ok=True)
    bundles_dir().mkdir(parents=True, exist_ok=True)


def job_logs_dir(job_id: str) -> Path:
    return logs_dir() / job_id


def target_log_path(job_id: str, target_name: str) -> Path:
    return job_logs_dir(job_id) / f"{target_name}.log"


def prepare_target_log(job_id: str, target_name: str) -> Path:
    path = target_log_path(job_id, target_name)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("")
    return path


def bundle_ref_name(job_id: str) -> str:
    return f"refs/pulp-ci-bundles/{job_id}"


def remote_bundle_name(job_id: str) -> str:
    return f"pulp-ci-{job_id}.bundle"


def create_job_bundle(job: dict) -> Path:
    ensure_state_dirs()
    bundle_path = bundles_dir() / f"{job['id']}.bundle"
    bundle_lock_path = Path(f"{bundle_path}.lock")

    with _BUNDLE_BUILD_LOCK:
        if bundle_path.exists() and bundle_path.stat().st_size > 0:
            return bundle_path

        bundle_lock_path.unlink(missing_ok=True)
        bundle_path.unlink(missing_ok=True)

        temp_ref = bundle_ref_name(job["id"])
        subprocess.run(["git", "update-ref", temp_ref, job["sha"]], cwd=ROOT, check=True)
        try:
            subprocess.run(["git", "bundle", "create", str(bundle_path), temp_ref], cwd=ROOT, check=True)
        finally:
            subprocess.run(["git", "update-ref", "-d", temp_ref], cwd=ROOT, check=True)
    return bundle_path


def sync_job_bundle_to_ssh_host(host: str, job: dict, report_progress=None) -> tuple[str, str]:
    bundle_path = create_job_bundle(job)
    remote_name = remote_bundle_name(job["id"])
    try:
        if report_progress:
            report_progress(
                phase="bundle-upload",
                host=host,
                bundle=remote_name,
                last_output_at=now_iso(),
            )
        subprocess.run(
            ["scp", str(bundle_path), f"{host}:{remote_name}"],
            capture_output=True,
            text=True,
            timeout=300,
            check=True,
        )
    except subprocess.SubprocessError as exc:
        detail = ""
        if isinstance(exc, subprocess.CalledProcessError):
            detail = (exc.stderr or exc.stdout or "").strip()
        raise RuntimeError(
            f"failed to upload git bundle to {host}: {detail or exc}"
        ) from exc
    return remote_name, bundle_ref_name(job["id"])


def tail_lines(path: Path, limit: int = 80) -> list[str]:
    if not path.exists():
        return []
    with path.open("r", errors="replace") as handle:
        return list(deque(handle, maxlen=limit))


def trim_line(value: str, max_len: int = 160) -> str:
    value = value.strip()
    if len(value) <= max_len:
        return value
    return "…" + value[-(max_len - 1):]


def atomic_write_text(path: Path, text: str) -> None:
    ensure_state_dirs()
    tmp = path.with_name(f".{path.name}.{os.getpid()}.{uuid.uuid4().hex}.tmp")
    try:
        tmp.write_text(text)
        tmp.replace(path)
    finally:
        tmp.unlink(missing_ok=True)


@contextmanager
def file_lock(path: Path, *, blocking: bool):
    ensure_state_dirs()
    handle = path.open("a+")
    mode = fcntl.LOCK_EX
    if not blocking:
        mode |= fcntl.LOCK_NB

    try:
        fcntl.flock(handle.fileno(), mode)
    except BlockingIOError as exc:
        handle.close()
        raise LockBusyError(str(path)) from exc

    try:
        yield handle
    finally:
        fcntl.flock(handle.fileno(), fcntl.LOCK_UN)
        handle.close()


def now_iso() -> str:
    return datetime.now(timezone.utc).isoformat()


def current_branch() -> str:
    result = subprocess.run(
        ["git", "rev-parse", "--abbrev-ref", "HEAD"],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=True,
    )
    return result.stdout.strip()


def current_sha() -> str:
    result = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=True,
    )
    return result.stdout.strip()


def short_sha(sha: str) -> str:
    return sha[:12] if sha else "?"


def normalize_priority(priority: str | None) -> str:
    value = (priority or "normal").strip().lower()
    if value not in PRIORITY_VALUES:
        raise ValueError(f"Invalid priority '{priority}'. Use one of: low, normal, high.")
    return value


def priority_value(priority: str | None) -> int:
    return PRIORITY_VALUES[normalize_priority(priority)]


def normalize_validation_mode(mode: str | None) -> str:
    value = (mode or "full").strip().lower()
    if value not in {"full", "smoke"}:
        raise ValueError(f"Invalid validation mode '{mode}'. Use one of: full, smoke.")
    return value


def load_config() -> dict:
    path = config_path()
    if not path.exists():
        raise FileNotFoundError(
            f"Local CI config not found at {path}. Copy tools/local-ci/config.example.json first."
        )
    return json.loads(path.read_text())


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


def load_queue() -> list[dict]:
    with file_lock(queue_lock_path(), blocking=True):
        queue = load_queue_unlocked()
        queue, changed = reconcile_running_jobs_unlocked(queue)
        if changed:
            save_queue_unlocked(queue)
        return queue


def enabled_targets(config: dict) -> list[str]:
    return [
        name
        for name, target_cfg in config.get("targets", {}).items()
        if target_cfg.get("enabled", True)
    ]


def parse_targets_arg(value: str | None) -> list[str] | None:
    if value is None or value.strip() == "":
        return None
    parts = [part.strip() for part in value.split(",") if part.strip()]
    return sorted(dict.fromkeys(parts))


def resolve_targets(config: dict, requested: list[str] | None) -> list[str]:
    if requested is None:
        configured = config.get("defaults", {}).get("targets")
        if configured is not None:
            if isinstance(configured, str):
                requested = parse_targets_arg(configured)
            else:
                requested = sorted(dict.fromkeys(configured))
        else:
            requested = enabled_targets(config)

    if not requested:
        return []

    valid = set(config.get("targets", {}).keys())
    unknown = [target for target in requested if target not in valid]
    if unknown:
        raise ValueError(f"Unknown target(s): {', '.join(unknown)}")

    disabled = [
        target
        for target in requested
        if not config["targets"].get(target, {}).get("enabled", True)
    ]
    if disabled:
        raise ValueError(
            f"Requested target(s) disabled in config: {', '.join(disabled)}"
        )

    return sorted(dict.fromkeys(requested))


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


def make_job(branch: str, sha: str, priority: str, targets: list[str], mode: str, validation: str) -> dict:
    normalized_validation = normalize_validation_mode(validation)
    return {
        "id": uuid.uuid4().hex[:12],
        "branch": branch,
        "sha": sha,
        "priority": normalize_priority(priority),
        "targets": sorted(targets),
        "queued_at": now_iso(),
        "status": "pending",
        "fingerprint": make_fingerprint(branch, sha, targets, normalized_validation),
        "mode": mode,
        "validation": normalized_validation,
        "submitted_root": str(ROOT),
    }


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


def supersedence_result(job: dict, superseded_by: str, reason: str) -> dict:
    return {
        "job_id": job["id"],
        "branch": job["branch"],
        "sha": job["sha"],
        "priority": job["priority"],
        "validation": job.get("validation", "full"),
        "targets": job.get("targets", []),
        "queued_at": job.get("queued_at", ""),
        "completed_at": now_iso(),
        "results": [],
        "overall": "superseded",
        "superseded_by": superseded_by,
        "superseded_reason": reason,
    }


def supersede_job_unlocked(job: dict, superseded_by: str, reason: str) -> None:
    result = supersedence_result(job, superseded_by, reason)
    result_path = save_result(result)
    job["status"] = "completed"
    job["completed_at"] = result["completed_at"]
    job["result_file"] = str(result_path)
    job["overall"] = "superseded"
    job["superseded_by"] = superseded_by
    job["superseded_reason"] = reason
    job.pop("runner", None)
    job.pop("active_targets", None)
    job.pop("last_progress_at", None)


def cancellation_result(job: dict, reason: str) -> dict:
    return {
        "job_id": job["id"],
        "branch": job["branch"],
        "sha": job["sha"],
        "priority": job["priority"],
        "validation": job.get("validation", "full"),
        "targets": job.get("targets", []),
        "queued_at": job.get("queued_at", ""),
        "completed_at": now_iso(),
        "results": [],
        "overall": "canceled",
        "canceled_reason": reason,
    }


def cancel_job_unlocked(job: dict, reason: str = "operator_canceled") -> None:
    result = cancellation_result(job, reason)
    result_path = save_result(result)
    job["status"] = "completed"
    job["completed_at"] = result["completed_at"]
    job["result_file"] = str(result_path)
    job["overall"] = "canceled"
    job["canceled_reason"] = reason
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


def upsert_job_active_targets_unlocked(queue: list[dict], job_id: str, active_targets: dict | None) -> bool:
    for job in queue:
        if job["id"] != job_id:
            continue
        if active_targets:
            job["active_targets"] = active_targets
            job["last_progress_at"] = now_iso()
        else:
            job.pop("active_targets", None)
            job.pop("last_progress_at", None)
        return True
    return False


def update_job_active_targets(job_id: str, active_targets: dict | None) -> None:
    with file_lock(queue_lock_path(), blocking=True):
        queue = load_queue_unlocked()
        if upsert_job_active_targets_unlocked(queue, job_id, active_targets):
            save_queue_unlocked(queue)


def enqueue_job(
    branch: str,
    sha: str,
    priority: str,
    targets: list[str],
    mode: str,
    validation: str,
) -> tuple[dict, bool]:
    requested_priority = normalize_priority(priority)
    normalized_validation = normalize_validation_mode(validation)

    with file_lock(queue_lock_path(), blocking=True):
        queue = load_queue_unlocked()
        queue, changed = reconcile_running_jobs_unlocked(queue)
        if changed:
            save_queue_unlocked(queue)
        fingerprint = make_fingerprint(branch, sha, targets, normalized_validation)

        for job in queue:
            if job.get("fingerprint") != fingerprint or job.get("status") not in {"pending", "running"}:
                continue

            changed = False
            if (
                job["status"] == "pending"
                and priority_value(requested_priority) > priority_value(job.get("priority", "normal"))
            ):
                job["priority"] = requested_priority
                job["bumped_at"] = now_iso()
                changed = True

            if changed:
                save_queue_unlocked(queue)
            return normalize_job(job), False

        job = make_job(branch, sha, requested_priority, targets, mode, normalized_validation)
        queue.append(job)
        for existing in queue:
            if existing.get("status") != "pending":
                continue
            reason = supersedence_reason(job, existing)
            if reason:
                supersede_job_unlocked(existing, job["id"], reason)
        save_queue_unlocked(trim_completed_jobs(queue))
        return job, True


def trim_completed_jobs(queue: list[dict]) -> list[dict]:
    completed = [job for job in queue if job.get("status") == "completed"]
    if len(completed) <= KEEP_COMPLETED_JOBS:
        return queue

    completed_by_time = sorted(completed, key=lambda job: job.get("completed_at", job.get("queued_at", "")))
    remove_ids = {job["id"] for job in completed_by_time[:-KEEP_COMPLETED_JOBS]}
    return [job for job in queue if job["id"] not in remove_ids]


def job_sort_key(job: dict) -> tuple[int, str, str]:
    return (-priority_value(job.get("priority", "normal")), job.get("queued_at", ""), job["id"])


def reconcile_running_jobs_unlocked(queue: list[dict]) -> tuple[list[dict], bool]:
    changed = False
    for job in stale_running_jobs_unlocked(queue):
        replacement = None
        for candidate in queue:
            if candidate.get("status") not in {"pending", "running"}:
                continue
            reason = supersedence_reason(candidate, job)
            if not reason:
                continue
            if replacement is None or candidate.get("queued_at", "") > replacement.get("queued_at", ""):
                replacement = candidate

        if replacement is not None:
            supersede_job_unlocked(job, replacement["id"], supersedence_reason(replacement, job) or "newer_sha_queued")
            changed = True
            continue

        job["status"] = "pending"
        job["requeued_at"] = now_iso()
        job.pop("started_at", None)
        job.pop("runner", None)
        changed = True

    return queue, changed


def read_runner_info() -> dict | None:
    path = runner_info_path()
    if not path.exists():
        return None
    return json.loads(path.read_text())


def pid_alive(pid: int | None) -> bool:
    if not pid or pid <= 0:
        return False
    try:
        os.kill(pid, 0)
    except OSError:
        return False
    return True


def current_runner_info() -> dict | None:
    info = read_runner_info()
    if not info:
        return None

    if pid_alive(info.get("pid")):
        return info

    try:
        with file_lock(drain_lock_path(), blocking=False):
            runner_info_path().unlink(missing_ok=True)
            return None
    except LockBusyError:
        return info


def stale_running_jobs_unlocked(queue: list[dict]) -> list[dict]:
    runner = read_runner_info()
    runner_pid = runner.get("pid") if runner else None
    runner_alive = pid_alive(runner_pid)

    if runner and not runner_alive:
        runner_info_path().unlink(missing_ok=True)
        runner = None
        runner_pid = None

    stale: list[dict] = []
    for job in queue:
        if job.get("status") != "running":
            continue
        job_runner = job.get("runner") or {}
        if runner and runner_pid and job_runner.get("pid") == runner_pid:
            continue
        stale.append(job)
    return stale


def update_job_target_state(job_id: str, target_name: str, **fields) -> None:
    with file_lock(queue_lock_path(), blocking=True):
        queue = load_queue_unlocked()
        job = find_job_unlocked(queue, job_id)
        if job is None:
            return

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

        if active_targets:
            job["active_targets"] = active_targets
            job["last_progress_at"] = now_iso()
        else:
            job.pop("active_targets", None)
            job.pop("last_progress_at", None)

        save_queue_unlocked(queue)


def collect_stale_windows_cleanup_candidates_unlocked(queue: list[dict]) -> list[dict]:
    candidates: list[dict] = []
    for job in stale_running_jobs_unlocked(queue):
        active_targets = job.get("active_targets") or {}
        state = dict(active_targets.get("windows") or {})
        host = state.get("host")
        validator_pid = state.get("validator_pid")
        validator_started_at = state.get("validator_started_at")
        if not host or validator_pid is None or not validator_started_at:
            continue
        if state.get("cleanup_requested_at"):
            continue

        state["cleanup_requested_at"] = now_iso()
        state["cleanup_status"] = "requested"
        state["cleanup_reason"] = "stale_runner_recovery"
        active_targets["windows"] = state
        job["active_targets"] = active_targets
        job["last_progress_at"] = now_iso()
        candidates.append(
            {
                "job_id": job["id"],
                "target": "windows",
                "host": host,
                "validator_pid": int(validator_pid),
                "validator_started_at": validator_started_at,
            }
        )
    return candidates


def cleanup_stale_windows_validator(host: str, pid: int, started_at: str) -> dict:
    ps_script = f"""
$PidToKill = {pid}
$ExpectedStart = '{ps_literal(started_at)}'

function Get-DescendantProcessIds {{
    param([int]$RootPid)
    $result = New-Object System.Collections.Generic.List[int]
    $queue = New-Object System.Collections.Generic.Queue[int]
    $queue.Enqueue($RootPid)
    while ($queue.Count -gt 0) {{
        $current = $queue.Dequeue()
        $children = @(Get-CimInstance Win32_Process -Filter "ParentProcessId = $current" -ErrorAction SilentlyContinue)
        foreach ($child in $children) {{
            $childPid = [int]$child.ProcessId
            $result.Add($childPid)
            $queue.Enqueue($childPid)
        }}
    }}
    return $result
}}

$result = [ordered]@{{
    found = $false
    matched = $false
    killed = $false
    pid = $PidToKill
}}

try {{
    $proc = Get-Process -Id $PidToKill -ErrorAction SilentlyContinue
    if ($null -ne $proc) {{
        $result.found = $true
        $start = $proc.StartTime.ToUniversalTime().ToString('o')
        $result.start = $start
        if ($ExpectedStart -and $start -ne $ExpectedStart) {{
            $result.matched = $false
        }} else {{
            $result.matched = $true
            $children = @(Get-DescendantProcessIds -RootPid $PidToKill | Sort-Object -Descending -Unique)
            foreach ($childPid in $children) {{
                try {{
                    Stop-Process -Id $childPid -Force -ErrorAction Stop
                }} catch {{
                }}
            }}
            Stop-Process -Id $PidToKill -Force -ErrorAction Stop
            $result.killed = $true
            $result.children = @($children)
        }}
    }}
}} catch {{
    $result.error = $_.Exception.Message
}}

$result | ConvertTo-Json -Compress
""".strip()
    run = run_logged_command(
        windows_ssh_powershell_command(host),
        input_text=ps_script,
        timeout=120,
    )
    lines = [line.strip() for line in run["output"].splitlines() if line.strip()]
    payload = {}
    if lines:
        try:
            payload = json.loads(lines[-1])
        except json.JSONDecodeError:
            payload = {"error": trim_line(lines[-1])}
    if run["returncode"] != 0:
        payload.setdefault("error", f"cleanup command exited {run['returncode']}")
    return payload


def reclaim_stale_remote_validators(_config: dict) -> int:
    with file_lock(queue_lock_path(), blocking=True):
        queue = load_queue_unlocked()
        candidates = collect_stale_windows_cleanup_candidates_unlocked(queue)
        if candidates:
            save_queue_unlocked(queue)

    for candidate in candidates:
        result = cleanup_stale_windows_validator(
            candidate["host"],
            candidate["validator_pid"],
            candidate["validator_started_at"],
        )
        update_job_target_state(
            candidate["job_id"],
            candidate["target"],
            cleanup_completed_at=now_iso(),
            cleanup_status=(
                "killed"
                if result.get("killed")
                else "not-found"
                if not result.get("found", True)
                else "mismatch"
                if result.get("found") and not result.get("matched", True)
                else "error"
                if result.get("error")
                else "checked"
            ),
            cleanup_result=trim_line(json.dumps(result, sort_keys=True)),
            validator_pid=None if result.get("killed") or not result.get("found", True) else candidate["validator_pid"],
            validator_started_at=None
            if result.get("killed") or not result.get("found", True)
            else candidate["validator_started_at"],
        )
    return len(candidates)


def write_runner_info(info: dict) -> None:
    atomic_write_text(runner_info_path(), json.dumps(info, indent=2) + "\n")


def update_runner_active_targets(job_id: str, active_targets: dict | None) -> None:
    info = current_runner_info()
    if not info or info.get("active_job_id") != job_id:
        return

    if active_targets:
        info["active_targets"] = active_targets
    else:
        info.pop("active_targets", None)
    info["updated_at"] = now_iso()
    write_runner_info(info)


def clear_runner_info() -> None:
    runner_info_path().unlink(missing_ok=True)


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


def load_job(job_id: str) -> dict | None:
    with file_lock(queue_lock_path(), blocking=True):
        queue = load_queue_unlocked()
        queue, changed = reconcile_running_jobs_unlocked(queue)
        if changed:
            save_queue_unlocked(queue)
        job = find_job_unlocked(queue, job_id)
        return normalize_job(job) if job else None


def claim_next_job() -> dict | None:
    with file_lock(queue_lock_path(), blocking=True):
        queue = load_queue_unlocked()
        queue, changed = reconcile_running_jobs_unlocked(queue)
        if changed:
            save_queue_unlocked(queue)
        pending = sorted(
            [job for job in queue if job.get("status") == "pending"],
            key=job_sort_key,
        )
        if not pending:
            return None

        selected_id = pending[0]["id"]
        claimed = None
        for job in queue:
            if job["id"] != selected_id:
                continue
            job["status"] = "running"
            job["started_at"] = now_iso()
            job["runner"] = {"pid": os.getpid(), "root": str(ROOT)}
            job.pop("active_targets", None)
            job.pop("last_progress_at", None)
            claimed = normalize_job(job)
            break

        save_queue_unlocked(queue)
        return claimed


def finalize_job(job_id: str, result: dict, result_path: Path) -> None:
    with file_lock(queue_lock_path(), blocking=True):
        queue = load_queue_unlocked()
        for job in queue:
            if job["id"] != job_id:
                continue
            job["status"] = "completed"
            job["completed_at"] = now_iso()
            job["result_file"] = str(result_path)
            job["overall"] = result.get("overall")
            job.pop("runner", None)
            job.pop("active_targets", None)
            job.pop("last_progress_at", None)
            break

        save_queue_unlocked(trim_completed_jobs(queue))


def load_result(path: Path) -> dict:
    return json.loads(path.read_text())


def empty_evidence_index() -> dict:
    return {"version": 2, "entries": {}}


def evidence_entry_key(branch: str, sha: str, target: str, validation: str) -> str:
    return f"{branch}:{sha}:{validation}:{target}"


def normalize_evidence_index(index: dict | None) -> dict:
    if not isinstance(index, dict):
        return empty_evidence_index()
    entries = index.get("entries")
    if not isinstance(entries, dict):
        entries = {}
    return {"version": int(index.get("version", 1)), "entries": entries}


def evidence_record_from_result(result: dict, item: dict, result_path: Path) -> dict:
    return {
        "job_id": result.get("job_id", ""),
        "branch": result.get("branch", ""),
        "sha": result.get("sha", ""),
        "validation": result.get("validation", "full"),
        "target": item.get("target", ""),
        "status": item.get("status", ""),
        "completed_at": result.get("completed_at", ""),
        "duration_secs": item.get("duration_secs", 0),
        "result_file": str(result_path),
    }


def merge_result_into_evidence_index(index: dict, result: dict, result_path: Path) -> bool:
    changed = False
    for item in result.get("results", []):
        if item.get("status") != "pass":
            continue
        record = evidence_record_from_result(result, item, result_path)
        key = evidence_entry_key(
            record["branch"], record["sha"], record["target"], record["validation"]
        )
        existing = index["entries"].get(key)
        if existing and existing.get("completed_at", "") >= record["completed_at"]:
            continue
        index["entries"][key] = record
        changed = True
    return changed


def rebuild_evidence_index_unlocked() -> dict:
    index = empty_evidence_index()
    for path in sorted(results_dir().glob("*.json")):
        try:
            result = load_result(path)
        except (OSError, json.JSONDecodeError):
            continue
        merge_result_into_evidence_index(index, result, path)
    return index


def load_evidence_index_unlocked() -> tuple[dict, bool]:
    path = evidence_path()
    if not path.exists():
        return rebuild_evidence_index_unlocked(), True

    try:
        index = normalize_evidence_index(json.loads(path.read_text()))
    except (OSError, json.JSONDecodeError):
        return rebuild_evidence_index_unlocked(), True
    if index.get("version") != empty_evidence_index()["version"]:
        return rebuild_evidence_index_unlocked(), True
    return index, False


def save_evidence_index_unlocked(index: dict) -> None:
    atomic_write_text(evidence_path(), json.dumps(index, indent=2) + "\n")


def load_evidence_index() -> dict:
    with file_lock(evidence_lock_path(), blocking=True):
        index, rebuilt = load_evidence_index_unlocked()
        if rebuilt:
            save_evidence_index_unlocked(index)
        return index


def update_evidence_index(result: dict, result_path: Path) -> None:
    with file_lock(evidence_lock_path(), blocking=True):
        index, rebuilt = load_evidence_index_unlocked()
        changed = merge_result_into_evidence_index(index, result, result_path)
        if rebuilt or changed:
            save_evidence_index_unlocked(index)


def collect_evidence_groups(branch: str | None = None, sha: str | None = None) -> dict[str, list[dict]]:
    index = load_evidence_index()
    grouped: dict[str, dict[str, dict]] = defaultdict(dict)

    for record in index.get("entries", {}).values():
        if branch and record.get("branch") != branch:
            continue
        if sha and record.get("sha") != sha:
            continue

        validation = record.get("validation", "full")
        sha_value = record.get("sha", "")
        if not sha_value:
            continue

        bucket = grouped[validation].setdefault(
            sha_value,
            {
                "sha": sha_value,
                "branch": record.get("branch", ""),
                "validation": validation,
                "completed_at": record.get("completed_at", ""),
                "targets": {},
            },
        )
        bucket["targets"][record.get("target", "")] = record
        if record.get("completed_at", "") > bucket.get("completed_at", ""):
            bucket["completed_at"] = record.get("completed_at", "")

    return {
        validation: sorted(
            sha_groups.values(),
            key=lambda item: (item.get("completed_at", ""), item.get("sha", "")),
            reverse=True,
        )
        for validation, sha_groups in grouped.items()
    }


def print_evidence_summary(
    *,
    branch: str | None = None,
    sha: str | None = None,
    limit: int = 3,
    indent: str = "",
) -> bool:
    groups = collect_evidence_groups(branch=branch, sha=sha)
    if not groups:
        return False

    for validation in sorted(groups):
        print(f"{indent}{validation}:")
        for item in groups[validation][:limit]:
            targets = ", ".join(f"{target}=pass" for target in sorted(item.get("targets", {})))
            print(
                f"{indent}  {short_sha(item.get('sha', ''))} [{targets}] "
                f"last={item.get('completed_at', '?')}"
            )
    return True


def wait_for_job(job_id: str, config: dict) -> tuple[dict | None, int]:
    announced_wait = False

    while True:
        job = load_job(job_id)
        if job is None:
            print(f"Job not found: {job_id}")
            return None, 1

        if job.get("status") == "completed":
            result_file = job.get("result_file")
            if not result_file:
                print(f"Job completed without a result file: {job_id}")
                return None, 1
            result = load_result(Path(result_file))
            return result, 0 if result.get("overall") == "pass" else 1

        acquired, _ = drain_pending_jobs(config, blocking=False)
        if acquired:
            continue

        runner = current_runner_info()
        if runner and not announced_wait:
            active_job = runner.get("active_job_id")
            active_branch = runner.get("active_branch")
            if active_job and active_branch:
                print(
                    f"Another local CI runner is active [{active_job}] {active_branch}; waiting for {job_id}..."
                )
            else:
                print("Another local CI runner is active; waiting for queued job completion...")
            announced_wait = True

        time.sleep(WAIT_POLL_SECS)


def notify(message: str) -> None:
    print("\a", end="", flush=True)
    try:
        subprocess.run(
            ["osascript", "-e", f'display notification "{message}" with title "Pulp CI"'],
            capture_output=True,
            timeout=5,
        )
    except Exception:
        pass


# ── VM Management ────────────────────────────────────────────────────────────


def ssh_reachable(host: str, timeout: int = 5) -> bool:
    result = subprocess.run(
        ["ssh", "-o", f"ConnectTimeout={timeout}", "-o", "BatchMode=yes", host, "echo", "up"],
        capture_output=True,
        text=True,
    )
    return result.returncode == 0


def utmctl_vm_status(vm_name: str) -> str | None:
    result = subprocess.run(["utmctl", "list"], capture_output=True, text=True)
    if result.returncode != 0:
        return None
    for line in result.stdout.splitlines():
        if vm_name in line:
            parts = line.split()
            if len(parts) >= 2:
                return parts[1]
    return None


def utmctl_start(vm_name: str) -> bool:
    result = subprocess.run(["utmctl", "start", vm_name], capture_output=True, text=True)
    return result.returncode == 0


def ensure_host_reachable(target_name: str, target_cfg: dict, defaults: dict) -> str | None:
    host = target_cfg["host"]
    fallback_host = target_cfg.get("fallback_host")
    timeout = defaults.get("ssh_timeout_secs", 5)

    print(f"  [{target_name}] Checking ssh {host}...")
    if ssh_reachable(host, timeout):
        print(f"  [{target_name}] {host} is up")
        return host

    if fallback_host:
        print(f"  [{target_name}] {host} unreachable, trying fallback ssh {fallback_host}...")
        if ssh_reachable(fallback_host, timeout):
            print(f"  [{target_name}] {fallback_host} is up")
            return fallback_host

    fallback = target_cfg.get("utm_fallback")
    if not fallback:
        print(f"  [{target_name}] {host} unreachable, no UTM fallback configured")
        return None

    vm_name = fallback["vm_name"]
    boot_wait = fallback.get("boot_wait_secs", 30)
    ssh_retry = fallback.get("ssh_retry_secs", 60)

    print(f"  [{target_name}] {host} unreachable, checking UTM VM '{vm_name}'...")
    status = utmctl_vm_status(vm_name)
    if status is None:
        print(f"  [{target_name}] UTM VM '{vm_name}' not found")
        return None

    if status != "started":
        print(f"  [{target_name}] Starting UTM VM '{vm_name}'...")
        if not utmctl_start(vm_name):
            print(f"  [{target_name}] Failed to start UTM VM")
            return None
        print(f"  [{target_name}] Waiting {boot_wait}s for boot...")
        time.sleep(boot_wait)

    deadline = time.time() + ssh_retry
    attempt = 0
    while time.time() < deadline:
        attempt += 1
        if ssh_reachable(host, timeout):
            print(f"  [{target_name}] {host} up after UTM start (attempt {attempt})")
            return host
        time.sleep(5)

    print(f"  [{target_name}] {host} still unreachable after UTM start")
    return None


# ── Validation Runners ───────────────────────────────────────────────────────


def remote_commit_error(target_name: str, host: str, job: dict) -> str:
    return (
        f"{target_name} cannot validate {short_sha(job['sha'])} on {host}: "
        f"commit is not available on origin. Push the branch first or use --targets mac."
    )


def parse_progress_marker(line: str) -> dict:
    stripped = line.strip()
    if stripped.startswith("__PULP_PHASE__:"):
        return {"phase": stripped.split(":", 1)[1]}
    if stripped.startswith("__PULP_WAIT__:"):
        return {"wait_reason": stripped.split(":", 1)[1]}
    if stripped.startswith("__PULP_VALIDATION__:"):
        return {"validation_mode": stripped.split(":", 1)[1]}
    if stripped.startswith("__PULP_TEST_POLICY__:"):
        return {"test_policy": stripped.split(":", 1)[1]}
    if stripped.startswith("__PULP_PREPARED__:"):
        return {"prepared_state": stripped.split(":", 1)[1]}
    if stripped.startswith("__PULP_VALIDATOR_PID__:"):
        value = stripped.split(":", 1)[1]
        try:
            return {"validator_pid": int(value)}
        except ValueError:
            return {"validator_pid": value}
    if stripped.startswith("__PULP_VALIDATOR_STARTED__:"):
        return {"validator_started_at": stripped.split(":", 1)[1]}
    return {}


def prepared_state_root(target_name: str, validation: str) -> Path:
    return state_dir() / "prepared" / target_name / normalize_validation_mode(validation)


def should_reuse_prepared_state(job: dict) -> bool:
    return len(job.get("targets", [])) == 1


def run_logged_command(
    cmd: list[str],
    *,
    cwd: Path | None = None,
    input_text: str | None = None,
    timeout: int = 3600,
    log_path: Path | None = None,
    report_progress=None,
    heartbeat_interval_secs: float = HEARTBEAT_INTERVAL_SECS,
    stuck_idle_secs: float = STUCK_IDLE_SECS,
) -> dict:
    start = time.time()
    proc = subprocess.Popen(
        cmd,
        cwd=cwd,
        stdin=subprocess.PIPE if input_text is not None else None,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
        bufsize=1,
    )

    if input_text is not None and proc.stdin is not None:
        proc.stdin.write(input_text)
        proc.stdin.close()

    output_queue: queue_module.Queue[str | None] = queue_module.Queue()

    def reader() -> None:
        assert proc.stdout is not None
        for line in proc.stdout:
            output_queue.put(line)
        output_queue.put(None)

    threading.Thread(target=reader, daemon=True).start()

    combined: list[str] = []
    saw_eof = False
    last_output_ts = start
    last_heartbeat_ts = start
    log_handle = log_path.open("a", errors="replace") if log_path else None
    try:
        while True:
            remaining = timeout - (time.time() - start)
            if remaining <= 0:
                proc.kill()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    pass
                return {
                    "timed_out": True,
                    "returncode": -1,
                    "output": "".join(combined),
                    "duration_secs": round(time.time() - start, 1),
                }

            try:
                poll_timeout = 0.25
                if heartbeat_interval_secs > 0:
                    poll_timeout = min(poll_timeout, max(heartbeat_interval_secs / 2.0, 0.01))
                item = output_queue.get(timeout=min(poll_timeout, max(remaining, 0.01)))
            except queue_module.Empty:
                if proc.poll() is not None and saw_eof:
                    break
                now = time.time()
                quiet_for_secs_raw = now - last_output_ts
                quiet_for_secs = int(round(quiet_for_secs_raw))
                if (
                    report_progress
                    and proc.poll() is None
                    and (now - last_heartbeat_ts) >= heartbeat_interval_secs
                ):
                    report_progress(
                        last_heartbeat_at=now_iso(),
                        quiet_for_secs=quiet_for_secs,
                        liveness="stuck" if quiet_for_secs_raw >= stuck_idle_secs else "quiet",
                    )
                    last_heartbeat_ts = now
                continue

            if item is None:
                saw_eof = True
                if proc.poll() is not None:
                    break
                continue

            progress = parse_progress_marker(item)
            if progress:
                combined.append(item)
                if log_handle is not None:
                    log_handle.write(item)
                    log_handle.flush()
                last_output_ts = time.time()
                last_heartbeat_ts = last_output_ts
                progress["last_output_at"] = now_iso()
                progress["last_heartbeat_at"] = None
                progress["quiet_for_secs"] = None
                progress["liveness"] = None
                if report_progress:
                    report_progress(**progress)
                continue

            combined.append(item)
            if log_handle is not None:
                log_handle.write(item)
                log_handle.flush()

            stripped = item.strip()
            if report_progress:
                last_output_ts = time.time()
                last_heartbeat_ts = last_output_ts
                fields = {
                    "last_output_at": now_iso(),
                    "last_heartbeat_at": None,
                    "quiet_for_secs": None,
                    "liveness": None,
                }
                if stripped:
                    fields["last_line"] = trim_line(stripped)
                report_progress(**fields)

        return {
            "timed_out": False,
            "returncode": proc.wait(),
            "output": "".join(combined),
            "duration_secs": round(time.time() - start, 1),
        }
    finally:
        if proc.stdout is not None:
            proc.stdout.close()
        if log_handle is not None:
            log_handle.close()


def run_local_validation(job: dict, exclude_tests: str = "", report_progress=None) -> dict:
    print(f"  [mac] Running local validation on {job['branch']} @ {short_sha(job['sha'])}...")
    log_path = prepare_target_log(job["id"], "mac")
    if report_progress:
        report_progress(phase="validate", log_path=str(log_path), last_output_at=now_iso())

    validation = job.get("validation", "full")
    prepared_root = prepared_state_root("mac", validation)
    reuse_prepared = should_reuse_prepared_state(job)
    env_args = [
        f"PULP_VALIDATE_ROOT_OVERRIDE={prepared_root}",
        f"PULP_VALIDATE_REUSE_PREPARED={'1' if reuse_prepared else '0'}",
    ]
    cmd = ["env", *env_args, "./validate-build.sh", "--quiet", "--keep-worktree", "--ref", job["sha"]]
    if validation == "smoke":
        cmd = [
            "env",
            *env_args,
            "PULP_EXPECT_SMOKE=1",
            "./validate-build.sh",
            "--quiet",
            "--keep-worktree",
            "--ref",
            job["sha"],
            "--smoke",
            "--no-tests",
        ]
    if exclude_tests:
        cmd += ["--exclude-regex", exclude_tests]

    run = run_logged_command(cmd, cwd=ROOT, timeout=3600, log_path=log_path, report_progress=report_progress)
    if run["timed_out"]:
        return {
            "target": "mac",
            "status": "timeout",
            "exit_code": -1,
            "duration_secs": run["duration_secs"],
            "stdout_tail": "",
            "stderr_tail": "Validation timed out after 3600s",
            "log_file": str(log_path),
        }
    tail = run["output"][-2000:] if run["output"] else ""
    if validation == "smoke":
        if "__PULP_VALIDATION__:smoke" not in run["output"] or "__PULP_TEST_POLICY__:skip" not in run["output"]:
            failed = True
            tail = (
                "Smoke validation contract violated: expected validation=smoke and test_policy=skip markers.\n"
                + tail
            )[-2000:]
        else:
            failed = run["returncode"] != 0
    else:
        failed = run["returncode"] != 0
    return {
        "target": "mac",
        "status": "pass" if not failed else "fail",
        "exit_code": run["returncode"],
        "duration_secs": run["duration_secs"],
        "stdout_tail": "" if failed else tail,
        "stderr_tail": tail if failed else "",
        "log_file": str(log_path),
        "validation": validation,
    }


def run_posix_ssh_validation(
    target_name: str,
    host: str,
    repo_path: str,
    job: dict,
    exclude_tests: str = "",
    report_progress=None,
) -> dict:
    print(f"  [{target_name}] Running validation on {host}:{repo_path} @ {short_sha(job['sha'])}...")
    log_path = prepare_target_log(job["id"], target_name)
    if report_progress:
        report_progress(phase="connect", host=host, log_path=str(log_path), last_output_at=now_iso())

    try:
        bundle_name, bundle_ref = sync_job_bundle_to_ssh_host(host, job, report_progress=report_progress)
    except RuntimeError as exc:
        return {
            "target": target_name,
            "status": "error",
            "exit_code": -1,
            "duration_secs": 0.0,
            "stdout_tail": "",
            "stderr_tail": str(exc),
            "log_file": str(log_path),
        }

    branch_q = shlex.quote(job["branch"])
    sha_q = shlex.quote(job["sha"])
    repo_q = shlex.quote(repo_path)
    bundle_name_q = shlex.quote(bundle_name)
    bundle_ref_q = shlex.quote(bundle_ref)
    script_name_q = shlex.quote(f".pulp-ci-validate-{job['id']}.sh")
    validation = normalize_validation_mode(job.get("validation", "full"))
    reuse_prepared_q = shlex.quote("1" if should_reuse_prepared_state(job) else "0")
    remote_cmd = (
        "set -euo pipefail; "
        f"branch={branch_q}; "
        f"sha={sha_q}; "
        f"bundle_name={bundle_name_q}; "
        f"bundle_ref={bundle_ref_q}; "
        f"script_name={script_name_q}; "
        f"reuse_prepared={reuse_prepared_q}; "
        "bundle=\"$HOME/$bundle_name\"; "
        f"prepared_root=\"$HOME/.local/state/pulp/local-ci/prepared/{target_name}/{validation}\"; "
        "script=''; "
        "trap 'rm -f \"$bundle\" \"$script\"' EXIT; "
        "export GIT_LFS_SKIP_SMUDGE=1; "
        f"cd {repo_q}; "
        "script=\"$PWD/$script_name\"; "
        "if [ -f \"$bundle\" ]; then "
        "printf '__PULP_PHASE__:bundle-sync\n'; "
        "git fetch \"$bundle\" \"$bundle_ref:refs/remotes/origin/$branch\" >/dev/null 2>&1 || true; "
        "fi; "
        "printf '__PULP_PHASE__:fetch\n'; "
        "git fetch origin >/dev/null 2>&1 || true; "
        "if ! git cat-file -e \"$sha^{commit}\" 2>/dev/null; then "
        "git fetch origin \"refs/heads/$branch:refs/remotes/origin/$branch\" >/dev/null 2>&1 || true; "
        "fi; "
        "if ! git cat-file -e \"$sha^{commit}\" 2>/dev/null; then "
        f"echo {shlex.quote(remote_commit_error(target_name, host, job))} >&2; "
        "exit 2; "
        "fi; "
        "printf '__PULP_PHASE__:validate\n'; "
        "git show \"$sha:validate-build.sh\" > \"$script\"; "
        "chmod +x \"$script\"; "
        "PULP_VALIDATE_ROOT_OVERRIDE=\"$prepared_root\" "
        "PULP_VALIDATE_REUSE_PREPARED=\"$reuse_prepared\" "
        "PULP_EXPECT_SMOKE=0 "
        "bash \"$script\" --quiet --keep-worktree --ref \"$sha\""
    )
    if validation == "smoke":
        remote_cmd = remote_cmd.replace("PULP_EXPECT_SMOKE=0", "PULP_EXPECT_SMOKE=1", 1)
        remote_cmd += " --smoke --no-tests"
    if exclude_tests:
        remote_cmd += f" --exclude-regex {shlex.quote(exclude_tests)}"

    cmd = ["ssh", host, "bash", "-lc", shlex.quote(remote_cmd)]

    run = run_logged_command(cmd, timeout=3600, log_path=log_path, report_progress=report_progress)
    if run["timed_out"]:
        return {
            "target": target_name,
            "status": "timeout",
            "exit_code": -1,
            "duration_secs": run["duration_secs"],
            "stdout_tail": "",
            "stderr_tail": "Validation timed out after 3600s",
            "log_file": str(log_path),
        }
    tail = run["output"][-2000:] if run["output"] else ""
    if validation == "smoke":
        if "__PULP_VALIDATION__:smoke" not in run["output"] or "__PULP_TEST_POLICY__:skip" not in run["output"]:
            failed = True
            tail = (
                "Smoke validation contract violated: expected validation=smoke and test_policy=skip markers.\n"
                + tail
            )[-2000:]
        else:
            failed = run["returncode"] != 0
    else:
        failed = run["returncode"] != 0
    return {
        "target": target_name,
        "status": "pass" if not failed else "fail",
        "exit_code": run["returncode"],
        "duration_secs": run["duration_secs"],
        "stdout_tail": "" if failed else tail,
        "stderr_tail": tail if failed else "",
        "log_file": str(log_path),
        "validation": validation,
    }


def ps_literal(value: str) -> str:
    return value.replace("'", "''")


def windows_ssh_powershell_command(host: str) -> list[str]:
    # `powershell -Command -` silently no-ops some multi-line try/finally scripts on WinRM/OpenSSH.
    # Read stdin explicitly and invoke it so complex validation scripts execute reliably.
    return [
        "ssh",
        host,
        "powershell",
        "-NoProfile",
        "-NonInteractive",
        "-Command",
        "$script = [Console]::In.ReadToEnd(); Invoke-Expression $script",
    ]


def probe_windows_ssh_cmake_settings(
    host: str,
    cmake_generator: str,
    cmake_platform: str,
    cmake_generator_instance: str,
) -> tuple[str, str]:
    if cmake_platform and cmake_generator_instance:
        return cmake_platform, cmake_generator_instance

    ps_script = f"""
$RequestedPlatform = '{ps_literal(cmake_platform)}'
$RequestedGeneratorInstance = '{ps_literal(cmake_generator_instance)}'
$Generator = '{ps_literal(cmake_generator)}'

function Resolve-CMakePlatform {{
    param([string]$Requested)
    if ($Requested) {{
        return $Requested
    }}
    if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64') {{
        return 'ARM64'
    }}
    return 'x64'
}}

function Resolve-VisualStudioInstance {{
    param([string]$Requested, [string]$Generator)
    if ($Requested) {{
        return $Requested
    }}
    if (-not $Generator -or -not $Generator.StartsWith('Visual Studio')) {{
        return ''
    }}
    $vswhere = Join-Path ${{env:ProgramFiles(x86)}} 'Microsoft Visual Studio\\Installer\\vswhere.exe'
    if (-not (Test-Path $vswhere)) {{
        return ''
    }}
    try {{
        $raw = (& $vswhere -latest -products * -format json) -join "`n"
        if (-not $raw) {{
            return ''
        }}
        $instances = $raw | ConvertFrom-Json
        if ($instances -isnot [System.Array]) {{
            $instances = @($instances)
        }}
        $preferred = $instances | Where-Object {{
            $_.productId -and $_.productId -ne 'Microsoft.VisualStudio.Product.BuildTools'
        }} | Select-Object -First 1
        if (-not $preferred) {{
            $preferred = $instances | Select-Object -First 1
        }}
        if ($preferred -and $preferred.installationPath) {{
            return $preferred.installationPath.Replace('\\', '/')
        }}
    }} catch {{
    }}
    return ''
}}

$resolved = @{{
    platform = Resolve-CMakePlatform $RequestedPlatform
    generator_instance = Resolve-VisualStudioInstance $RequestedGeneratorInstance $Generator
}}
$resolved | ConvertTo-Json -Compress
"""

    try:
        run = subprocess.run(
            windows_ssh_powershell_command(host),
            input=ps_script,
            capture_output=True,
            text=True,
            timeout=60,
        )
    except (subprocess.SubprocessError, OSError):
        return cmake_platform, cmake_generator_instance

    if run.returncode != 0:
        return cmake_platform, cmake_generator_instance

    for line in reversed(run.stdout.splitlines()):
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            resolved = json.loads(line)
        except json.JSONDecodeError:
            continue
        return (
            resolved.get("platform") or cmake_platform,
            resolved.get("generator_instance") or cmake_generator_instance,
        )
    return cmake_platform, cmake_generator_instance


def run_windows_ssh_validation(
    target_name: str,
    host: str,
    repo_path: str,
    job: dict,
    exclude_tests: str = "",
    cmake_generator: str = "Visual Studio 17 2022",
    cmake_platform: str = "",
    cmake_generator_instance: str = "",
    report_progress=None,
) -> dict:
    print(f"  [{target_name}] Running validation on {host}:{repo_path} @ {short_sha(job['sha'])}...")
    log_path = prepare_target_log(job["id"], target_name)
    if report_progress:
        report_progress(phase="connect", host=host, log_path=str(log_path), last_output_at=now_iso())

    try:
        bundle_name, bundle_ref = sync_job_bundle_to_ssh_host(host, job, report_progress=report_progress)
    except RuntimeError as exc:
        return {
            "target": target_name,
            "status": "error",
            "exit_code": -1,
            "duration_secs": 0.0,
            "stdout_tail": "",
            "stderr_tail": str(exc),
            "log_file": str(log_path),
        }

    resolved_platform, resolved_generator_instance = probe_windows_ssh_cmake_settings(
        host,
        cmake_generator,
        cmake_platform,
        cmake_generator_instance,
    )

    ps_script = f"""
$ErrorActionPreference = 'Stop'

function Invoke-Native {{
    param([string]$File, [string[]]$Arguments)
    & $File @Arguments
    if ($LASTEXITCODE -ne 0) {{
        throw "$File exited with code $LASTEXITCODE"
    }}
}}

function Test-CommitRef {{
    param([string]$Ref)
    & git rev-parse --verify --quiet "$Ref`^{{commit}}" 1> $null 2> $null
    return $LASTEXITCODE -eq 0
}}

function Remove-WorktreeSafe {{
    param([string]$RepoRoot, [string]$Path)
    try {{
        Invoke-Native git @('-C', $RepoRoot, 'worktree', 'remove', '--force', '--force', $Path)
    }} catch {{
    }}
    if (Test-Path $Path) {{
        try {{
            Remove-Item -Recurse -Force -ErrorAction Stop $Path
        }} catch {{
        }}
    }}
    try {{
        Invoke-Native git @('-C', $RepoRoot, 'worktree', 'prune', '--expire', 'now')
    }} catch {{
    }}
}}

function Remove-PreparedRoot {{
    param([string]$RepoRoot, [string]$PreparedRoot)

    $PreparedSrc = Join-Path $PreparedRoot 'src'
    if (Test-Path $PreparedSrc) {{
        Remove-WorktreeSafe $RepoRoot $PreparedSrc
    }}
    if (Test-Path $PreparedRoot) {{
        try {{
            Remove-Item -Recurse -Force -ErrorAction Stop $PreparedRoot
        }} catch {{
        }}
    }}
}}

function Test-PreparedStateMatches {{
    param(
        [string]$StatePath,
        [string]$ExpectedSha,
        [string]$ExpectedValidation,
        [string]$ExpectedGenerator,
        [string]$ExpectedPlatform,
        [string]$ExpectedGeneratorInstance
    )

    if (-not (Test-Path $StatePath)) {{
        return $false
    }}

    try {{
        $state = Get-Content $StatePath -Raw | ConvertFrom-Json
    }} catch {{
        return $false
    }}

    if (
        $state.sha -ne $ExpectedSha -or
        $state.validation -ne $ExpectedValidation -or
        $state.generator -ne $ExpectedGenerator -or
        $state.platform -ne $ExpectedPlatform -or
        $state.generator_instance -ne $ExpectedGeneratorInstance
    ) {{
        return $false
    }}

    $PreparedRoot = Split-Path $StatePath -Parent
    $PreparedSrc = Join-Path $PreparedRoot 'src'
    $PreparedBuild = Join-Path $PreparedRoot 'build'
    $PreparedInstall = Join-Path $PreparedRoot 'install'
    if (-not (Test-Path $PreparedSrc) -or -not (Test-Path $PreparedBuild) -or -not (Test-Path $PreparedInstall)) {{
        return $false
    }}

    $preparedHead = ((& git -C $PreparedSrc rev-parse HEAD 2>$null) | Select-Object -Last 1).Trim()
    if ($LASTEXITCODE -ne 0) {{
        return $false
    }}
    return $preparedHead -eq $ExpectedSha
}}

function Write-PreparedState {{
    param(
        [string]$StatePath,
        [string]$Sha,
        [string]$Validation,
        [string]$Generator,
        [string]$Platform,
        [string]$GeneratorInstance
    )

    $payload = @{{
        sha = $Sha
        validation = $Validation
        generator = $Generator
        platform = $Platform
        generator_instance = $GeneratorInstance
        updated_at = (Get-Date).ToString('o')
    }}
    $payload | ConvertTo-Json | Set-Content -Path $StatePath
}}

function Wait-HostMutex {{
    param(
        [System.Threading.Mutex]$Mutex,
        [bool]$Immediate
    )

    try {{
        if ($Immediate) {{
            return $Mutex.WaitOne(0)
        }}
        $null = $Mutex.WaitOne()
        return $true
    }} catch [System.Threading.AbandonedMutexException] {{
        Write-Host "Recovered abandoned host validation lock: $MutexName"
        return $true
    }}
}}

$Repo = '{ps_literal(repo_path)}'
$RepoDrive = Split-Path -Path $Repo -Qualifier
if (-not $RepoDrive) {{
    $RepoDrive = 'C:'
}}
$env:GIT_LFS_SKIP_SMUDGE = '1'
$CiRoot = Join-Path $RepoDrive 'pulp-ci'
$Branch = '{ps_literal(job['branch'])}'
$Sha = '{ps_literal(job['sha'])}'
$BundleName = '{ps_literal(bundle_name)}'
$BundleRef = '{ps_literal(bundle_ref)}'
$Bundle = Join-Path $HOME $BundleName
$ExcludeRegex = '{ps_literal(exclude_tests)}'
$Generator = '{ps_literal(cmake_generator)}'
$Platform = '{ps_literal(resolved_platform)}'
$GeneratorInstance = '{ps_literal(resolved_generator_instance)}'
$ValidationMode = '{ps_literal(job.get("validation", "full"))}'
$PreparedRoot = Join-Path $CiRoot 'prepared\\{ps_literal(target_name)}'
$PreparedRoot = Join-Path $PreparedRoot $ValidationMode
$PreparedState = Join-Path $PreparedRoot 'state.json'
$Src = Join-Path $PreparedRoot 'src'
$Build = Join-Path $PreparedRoot 'build'
$Install = Join-Path $PreparedRoot 'install'
$Smoke = Join-Path $PreparedRoot 'smoke'
$ReusePrepared = {'$true' if should_reuse_prepared_state(job) else '$false'}
$UsePrepared = $false
$MutexName = 'Global\\PulpLocalCIValidate'
$Mutex = New-Object System.Threading.Mutex($false, $MutexName)
$LockAcquired = $false
$ValidatorStartedAt = (Get-Process -Id $PID).StartTime.ToUniversalTime().ToString('o')

try {{
    Write-Host "__PULP_VALIDATOR_PID__:$PID"
    Write-Host "__PULP_VALIDATOR_STARTED__:$ValidatorStartedAt"
    if (-not (Wait-HostMutex -Mutex $Mutex -Immediate $true)) {{
        Write-Host "__PULP_WAIT__:host-lock"
        Write-Host "__PULP_PHASE__:waiting-lock"
        Write-Host "Waiting for host validation lock: $MutexName"
        $LockAcquired = Wait-HostMutex -Mutex $Mutex -Immediate $false
    }} else {{
        $LockAcquired = $true
    }}

    Write-Host "__PULP_PHASE__:fetch"
    New-Item -ItemType Directory -Force -Path $PreparedRoot | Out-Null
    Set-Location $Repo
    if (Test-Path $Bundle) {{
        Write-Host "__PULP_PHASE__:bundle-sync"
        try {{
            Invoke-Native git @(
                'fetch',
                $Bundle,
                "$BundleRef`:refs/pulp-ci-bundles/{job['id']}"
            )
        }} finally {{
            Remove-Item -Force -ErrorAction SilentlyContinue $Bundle
        }}
    }}
    try {{
        Invoke-Native git @('fetch', 'origin')
    }} catch {{
    }}

    if (-not (Test-CommitRef $Sha)) {{
        try {{
            Invoke-Native git @(
                'fetch',
                'origin',
                "refs/heads/$Branch`:refs/remotes/origin/$Branch"
            )
        }} catch {{
        }}
    }}

    if (-not (Test-CommitRef $Sha)) {{
        throw '{ps_literal(remote_commit_error(target_name, host, job))}'
    }}

    if ($ReusePrepared -and (Test-PreparedStateMatches `
        -StatePath $PreparedState `
        -ExpectedSha $Sha `
        -ExpectedValidation $ValidationMode `
        -ExpectedGenerator $Generator `
        -ExpectedPlatform $Platform `
        -ExpectedGeneratorInstance $GeneratorInstance)) {{
        $UsePrepared = $true
        Write-Host "__PULP_PREPARED__:reused"
    }} else {{
        Write-Host "__PULP_PREPARED__:clean"
        Remove-PreparedRoot $Repo $PreparedRoot
        New-Item -ItemType Directory -Force -Path $PreparedRoot | Out-Null
        Write-Host "__PULP_PHASE__:worktree"
        Invoke-Native git @('worktree', 'add', '--force', '--detach', $Src, $Sha)
    }}

    try {{
        Write-Host "__PULP_PHASE__:configure"
        Write-Host "CMake platform: $Platform"
        if ($GeneratorInstance) {{
            Write-Host "CMake generator instance: $GeneratorInstance"
        }}
        $configureArgs = @('-S', $Src, '-B', $Build)
        if ($Generator) {{
            $configureArgs += @('-G', $Generator)
        }}
        if ($Platform) {{
            $configureArgs += @('-A', $Platform)
        }}
        if ($GeneratorInstance) {{
            $configureArgs += @("-DCMAKE_GENERATOR_INSTANCE=$GeneratorInstance")
        }}
        $configureArgs += @('-DCMAKE_BUILD_TYPE=Release')
        if ($ValidationMode -eq 'smoke') {{
            $configureArgs += @(
                '-DPULP_BUILD_TESTS=OFF',
                '-DPULP_BUILD_EXAMPLES=OFF',
                '-DPULP_ENABLE_GPU=OFF'
            )
        }}
        Invoke-Native cmake $configureArgs
        Write-Host "__PULP_PHASE__:build"
        Invoke-Native cmake @('--build', $Build, '--config', 'Release')
        if ($ValidationMode -eq 'smoke') {{
            Write-Host "__PULP_PHASE__:install"
            Invoke-Native cmake @('--install', $Build, '--prefix', $Install, '--config', 'Release')
            New-Item -ItemType Directory -Force -Path $Smoke | Out-Null
            @"
cmake_minimum_required(VERSION 3.24)
project(PulpSDKSmoke LANGUAGES CXX)

find_package(Pulp REQUIRED CONFIG)

add_library(smoke INTERFACE)
target_link_libraries(smoke INTERFACE Pulp::format Pulp::standalone)
"@ | Set-Content -Path (Join-Path $Smoke 'CMakeLists.txt')
            Write-Host "__PULP_PHASE__:smoke"
            $smokeConfigureArgs = @('-S', $Smoke, '-B', (Join-Path $Smoke 'build'))
            if ($Generator) {{
                $smokeConfigureArgs += @('-G', $Generator)
            }}
            if ($Platform) {{
                $smokeConfigureArgs += @('-A', $Platform)
            }}
            if ($GeneratorInstance) {{
                $smokeConfigureArgs += @("-DCMAKE_GENERATOR_INSTANCE=$GeneratorInstance")
            }}
            $smokeConfigureArgs += @("-DCMAKE_PREFIX_PATH=$Install")
            Invoke-Native cmake $smokeConfigureArgs
            Write-PreparedState `
                -StatePath $PreparedState `
                -Sha $Sha `
                -Validation $ValidationMode `
                -Generator $Generator `
                -Platform $Platform `
                -GeneratorInstance $GeneratorInstance
        }} else {{
            Write-PreparedState `
                -StatePath $PreparedState `
                -Sha $Sha `
                -Validation $ValidationMode `
                -Generator $Generator `
                -Platform $Platform `
                -GeneratorInstance $GeneratorInstance
            Write-Host "__PULP_PHASE__:test"
            $ctestArgs = @('--test-dir', $Build, '--output-on-failure', '-C', 'Release')
            if ($ExcludeRegex) {{
                $ctestArgs += @('--exclude-regex', $ExcludeRegex)
            }}
            Invoke-Native ctest $ctestArgs
        }}
    }} finally {{
        Write-Host "__PULP_PHASE__:cleanup"
        if (-not (Test-Path $PreparedState)) {{
            Remove-PreparedRoot $Repo $PreparedRoot
        }}
    }}
}} finally {{
    if ($LockAcquired) {{
        try {{
            $Mutex.ReleaseMutex() | Out-Null
        }} catch [System.ApplicationException] {{
        }}
    }}
    $Mutex.Dispose()
}}
""".strip()

    cmd = windows_ssh_powershell_command(host)

    run = run_logged_command(
        cmd,
        input_text=ps_script,
        timeout=3600,
        log_path=log_path,
        report_progress=report_progress,
    )
    if run["timed_out"]:
        return {
            "target": target_name,
            "status": "timeout",
            "exit_code": -1,
            "duration_secs": run["duration_secs"],
            "stdout_tail": "",
            "stderr_tail": "Validation timed out after 3600s",
            "log_file": str(log_path),
        }
    tail = run["output"][-2000:] if run["output"] else ""
    failed = run["returncode"] != 0
    return {
        "target": target_name,
        "status": "pass" if not failed else "fail",
        "exit_code": run["returncode"],
        "duration_secs": run["duration_secs"],
        "stdout_tail": "" if failed else tail,
        "stderr_tail": tail if failed else "",
        "log_file": str(log_path),
        "validation": job.get("validation", "full"),
    }


# ── Job Processing ───────────────────────────────────────────────────────────


def _build_target_tasks(job: dict, config: dict, progress_factory=None) -> list[tuple[str, object]]:
    targets = config["targets"]
    defaults = config.get("defaults", {})
    requested = set(job.get("targets") or enabled_targets(config))
    tasks: list[tuple[str, object]] = []

    mac_cfg = targets.get("mac", {})
    if "mac" in requested and mac_cfg.get("enabled", True):
        reporter = progress_factory("mac") if progress_factory else None
        tasks.append(("mac", lambda r=reporter: run_local_validation(job, mac_cfg.get("exclude_tests", ""), r)))

    ubuntu_cfg = targets.get("ubuntu")
    if "ubuntu" in requested and ubuntu_cfg and ubuntu_cfg.get("enabled", True):
        host = ensure_host_reachable("ubuntu", ubuntu_cfg, defaults)
        if host:
            exc = ubuntu_cfg.get("exclude_tests", "")
            reporter = progress_factory("ubuntu") if progress_factory else None
            tasks.append(
                (
                    "ubuntu",
                    lambda h=host, e=exc, r=reporter: run_posix_ssh_validation(
                        "ubuntu", h, ubuntu_cfg["repo_path"], job, exclude_tests=e, report_progress=r
                    ),
                )
            )
        else:
            tasks.append(
                (
                    "ubuntu",
                    lambda: {
                        "target": "ubuntu",
                        "status": "unreachable",
                        "exit_code": -1,
                        "duration_secs": 0,
                        "stdout_tail": "",
                        "stderr_tail": "Host unreachable",
                    },
                )
            )

    win_cfg = targets.get("windows")
    if "windows" in requested and win_cfg and win_cfg.get("enabled", True):
        host = ensure_host_reachable("windows", win_cfg, defaults)
        if host:
            exc = win_cfg.get("exclude_tests", "")
            reporter = progress_factory("windows") if progress_factory else None
            generator = win_cfg.get("cmake_generator", "Visual Studio 17 2022")
            platform = win_cfg.get("cmake_platform", "")
            generator_instance = win_cfg.get("cmake_generator_instance", "")
            tasks.append(
                (
                    "windows",
                    lambda h=host, e=exc, r=reporter, g=generator, p=platform, i=generator_instance: run_windows_ssh_validation(
                        "windows",
                        h,
                        win_cfg["repo_path"],
                        job,
                        exclude_tests=e,
                        cmake_generator=g,
                        cmake_platform=p,
                        cmake_generator_instance=i,
                        report_progress=r,
                    ),
                )
            )
        else:
            tasks.append(
                (
                    "windows",
                    lambda: {
                        "target": "windows",
                        "status": "unreachable",
                        "exit_code": -1,
                        "duration_secs": 0,
                        "stdout_tail": "",
                        "stderr_tail": "Host unreachable",
                    },
                )
            )

    return tasks


def process_job(job: dict, config: dict) -> dict:
    print(
        f"\n=== Validating [{job['id']}] {job['branch']} @ {short_sha(job['sha'])} "
        f"priority={job['priority']} ===\n"
    )

    target_states: dict[str, dict] = {}
    state_lock = threading.Lock()

    def flush_target_states() -> None:
        with state_lock:
            snapshot = {name: dict(state) for name, state in target_states.items()}
        update_runner_active_targets(job["id"], snapshot or None)
        update_job_active_targets(job["id"], snapshot or None)

    def progress_factory(name: str):
        def report(**fields) -> None:
            with state_lock:
                state = dict(target_states.get(name, {}))
                for key, value in fields.items():
                    if value is None:
                        state.pop(key, None)
                    else:
                        state[key] = value
                target_states[name] = state
            flush_target_states()

        return report

    tasks = _build_target_tasks(job, config, progress_factory=progress_factory)
    if not tasks:
        return {
            "job_id": job["id"],
            "branch": job["branch"],
            "sha": job["sha"],
            "priority": job["priority"],
            "targets": job.get("targets", []),
            "queued_at": job.get("queued_at", ""),
            "completed_at": now_iso(),
            "results": [],
            "overall": "pass",
        }

    for name, _fn in tasks:
        target_states[name] = {
            "status": "running",
            "started_at": now_iso(),
            "phase": "starting",
            "log_path": str(target_log_path(job["id"], name)),
        }
    flush_target_states()

    results = []
    with ThreadPoolExecutor(max_workers=len(tasks)) as pool:
        futures = {pool.submit(fn): name for name, fn in tasks}
        for future in as_completed(futures):
            name = futures[future]
            try:
                result = future.result()
            except Exception as exc:
                result = {
                    "target": name,
                    "status": "error",
                    "exit_code": -1,
                    "duration_secs": 0,
                    "stdout_tail": "",
                    "stderr_tail": str(exc),
                }

            results.append(result)
            target_states[name] = {
                "status": result.get("status", "?"),
                "exit_code": result.get("exit_code"),
                "duration_secs": result.get("duration_secs"),
                "completed_at": now_iso(),
                "phase": "done" if result.get("status") == "pass" else target_states.get(name, {}).get("phase", "done"),
                "log_path": result.get("log_file", str(target_log_path(job["id"], name))),
                "last_output_at": target_states.get(name, {}).get("last_output_at"),
                "last_line": target_states.get(name, {}).get("last_line"),
                "host": target_states.get(name, {}).get("host"),
                "wait_reason": target_states.get(name, {}).get("wait_reason"),
            }
            flush_target_states()

    results.sort(key=lambda item: item["target"])
    return {
        "job_id": job["id"],
        "branch": job["branch"],
        "sha": job["sha"],
        "priority": job["priority"],
        "validation": job.get("validation", "full"),
        "targets": job.get("targets", []),
        "queued_at": job.get("queued_at", ""),
        "completed_at": now_iso(),
        "results": results,
        "overall": "pass" if all(result["status"] == "pass" for result in results) else "fail",
    }


def save_result(result: dict) -> Path:
    ensure_state_dirs()
    ts = datetime.now().strftime("%Y%m%d-%H%M%S")
    branch_slug = result["branch"].replace("/", "-")
    path = results_dir() / f"{ts}-{result['job_id']}-{branch_slug}.json"
    path.write_text(json.dumps(result, indent=2) + "\n")
    update_evidence_index(result, path)
    return path


def print_result(result: dict, result_path: Path | None = None) -> None:
    print(f"\n--- Result: [{result['job_id']}] {result['branch']} ---")
    if result.get("validation", "full") != "full":
        print(f"  {'validation':10s}  {result['validation']}")
    for item in result["results"]:
        icon = "PASS" if item["status"] == "pass" else item["status"].upper()
        print(f"  {item['target']:10s}  {icon:12s}  {item.get('duration_secs', 0)}s")
    print(f"  {'overall':10s}  {result['overall'].upper()}")
    if result_path:
        print(f"  Saved: {result_path}")
    print()


def drain_pending_jobs(config: dict, *, blocking: bool) -> tuple[bool, bool]:
    acquired = False
    try:
        with file_lock(drain_lock_path(), blocking=blocking):
            acquired = True
            runner_info = {
                "pid": os.getpid(),
                "root": str(ROOT),
                "started_at": now_iso(),
                "active_job_id": None,
                "active_branch": None,
            }
            write_runner_info(runner_info)
            any_failure = False

            while True:
                reclaim_stale_remote_validators(config)
                job = claim_next_job()
                if job is None:
                    break

                runner_info.update(
                    {
                        "active_job_id": job["id"],
                        "active_branch": job["branch"],
                        "updated_at": now_iso(),
                    }
                )
                write_runner_info(runner_info)

                try:
                    result = process_job(job, config)
                except Exception as exc:
                    result = {
                        "job_id": job["id"],
                        "branch": job["branch"],
                        "sha": job["sha"],
                        "priority": job["priority"],
                        "validation": job.get("validation", "full"),
                        "targets": job.get("targets", []),
                        "queued_at": job.get("queued_at", ""),
                        "completed_at": now_iso(),
                        "results": [
                            {
                                "target": "scheduler",
                                "status": "error",
                                "exit_code": -1,
                                "duration_secs": 0,
                                "stdout_tail": "",
                                "stderr_tail": str(exc),
                            }
                        ],
                        "overall": "fail",
                    }

                result_path = save_result(result)
                finalize_job(job["id"], result, result_path)
                print_result(result, result_path)
                if result["overall"] != "pass":
                    any_failure = True

            return True, any_failure
    except LockBusyError:
        return False, False
    finally:
        if acquired:
            clear_runner_info()


# ── GitHub Helpers ───────────────────────────────────────────────────────────


def gh_available() -> bool:
    result = subprocess.run(["gh", "auth", "status"], capture_output=True, text=True)
    return result.returncode == 0


def gh_pr_create(branch: str, base: str = "main") -> int | None:
    result = subprocess.run(
        ["gh", "pr", "create", "--head", branch, "--base", base, "--fill", "--no-maintainer-edit"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        existing = subprocess.run(
            ["gh", "pr", "view", branch, "--json", "number"],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        if existing.returncode == 0:
            return json.loads(existing.stdout)["number"]
        print(f"  Failed to create PR: {result.stderr.strip()}")
        return None

    url = result.stdout.strip()
    try:
        return int(url.rstrip("/").split("/")[-1])
    except (ValueError, IndexError):
        return None


def gh_pr_comment(pr_number: int, body: str) -> bool:
    result = subprocess.run(
        ["gh", "pr", "comment", str(pr_number), "--body", body],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    return result.returncode == 0


def gh_pr_merge(pr_number: int, method: str = "squash") -> bool:
    result = subprocess.run(
        ["gh", "pr", "merge", str(pr_number), f"--{method}", "--delete-branch"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    return result.returncode == 0


def gh_pr_list_open() -> list[dict]:
    result = subprocess.run(
        ["gh", "pr", "list", "--json", "number,title,headRefName,author,createdAt,labels"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        return []
    return json.loads(result.stdout)


def gh_pr_head(pr_ref: str) -> tuple[int, str, str] | None:
    if pr_ref == "latest":
        prs = gh_pr_list_open()
        if not prs:
            print("No open PRs found.")
            return None
        pr_ref = str(prs[0]["number"])

    result = subprocess.run(
        ["gh", "pr", "view", pr_ref, "--json", "number,headRefName,headRefOid"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        print(f"  Could not find PR {pr_ref}: {result.stderr.strip()}")
        return None

    data = json.loads(result.stdout)
    return data["number"], data["headRefName"], data["headRefOid"]


def format_ci_comment(result: dict) -> str:
    validation = result.get("validation", "full")
    title = "Local CI Smoke Results" if validation == "smoke" else "Local CI Results"
    lines = [f"## {title}\n"]
    overall = result["overall"].upper()
    icon = "white_check_mark" if overall == "PASS" else "x"
    lines.append(f":{icon}: **Overall: {overall}**\n")
    lines.append(f"Job: `{result.get('job_id', '?')}`  Commit: `{short_sha(result.get('sha', ''))}`\n")
    if validation != "full":
        lines.append(f"Validation: `{validation}`\n")
        lines.append("_Smoke mode is a fast clean install/export preflight and does not run the full test suite._\n")
    lines.append("| Target | Status | Duration |")
    lines.append("|--------|--------|----------|")
    for item in result["results"]:
        status = item["status"].upper()
        s_icon = "white_check_mark" if status == "PASS" else "x"
        lines.append(f"| {item['target']} | :{s_icon}: {status} | {item.get('duration_secs', 0)}s |")

    if any(item["status"] != "pass" for item in result["results"]):
        lines.append("\n<details><summary>Failure details</summary>\n")
        for item in result["results"]:
            if item["status"] == "pass":
                continue
            lines.append(f"### {item['target']} (exit {item.get('exit_code', '?')})")
            stderr = item.get("stderr_tail", "")
            if stderr:
                lines.append(f"```\n{stderr[-500:]}\n```")
        lines.append("</details>")

    lines.append(f"\n*Run at {result.get('completed_at', 'unknown')}*")
    return "\n".join(lines)


# ── CLI Commands ─────────────────────────────────────────────────────────────


def resolve_submission_options(args: argparse.Namespace, command: str) -> tuple[dict, str, str, list[str], str, str]:
    config = load_config()
    branch = args.branch or current_branch()
    sha = args.sha or current_sha()
    targets = resolve_targets(config, parse_targets_arg(getattr(args, "targets", None)))
    priority = normalize_priority(getattr(args, "priority", None) or default_priority_for(command, config))
    validation = normalize_validation_mode("smoke" if getattr(args, "smoke", False) else "full")
    return config, branch, sha, targets, priority, validation


def cmd_enqueue(args: argparse.Namespace) -> int:
    try:
        _config, branch, sha, targets, priority, validation = resolve_submission_options(args, "enqueue")
    except (FileNotFoundError, ValueError) as exc:
        print(f"Error: {exc}")
        return 1

    job, created = enqueue_job(branch, sha, priority, targets, "enqueue", validation)
    if created:
        print(f"Enqueued: {summarize_job(job)}")
    else:
        print(f"Already queued/running: {summarize_job(job)}")
    return 0


def cmd_drain(_args: argparse.Namespace) -> int:
    try:
        config = load_config()
    except FileNotFoundError as exc:
        print(f"Error: {exc}")
        return 1

    acquired, any_failure = drain_pending_jobs(config, blocking=False)
    if not acquired:
        runner = current_runner_info()
        if runner and runner.get("active_job_id"):
            print(
                f"Another local CI runner is active [{runner['active_job_id']}] {runner.get('active_branch', '?')}."
            )
        else:
            print("Another local CI runner is active.")
        return 0

    notify("CI complete" + (" - PASSED" if not any_failure else " - FAILED"))
    return 1 if any_failure else 0


def cmd_run(args: argparse.Namespace) -> int:
    try:
        config, branch, sha, targets, priority, validation = resolve_submission_options(args, "run")
    except (FileNotFoundError, ValueError) as exc:
        print(f"Error: {exc}")
        return 1

    job, created = enqueue_job(branch, sha, priority, targets, "run", validation)
    print(("Enqueued" if created else "Already queued/running") + f": {summarize_job(job)}")

    result, exit_code = wait_for_job(job["id"], config)
    if result is not None:
        print_result(result, Path(load_job(job["id"])["result_file"]))
    notify("CI run complete" + (" - PASSED" if exit_code == 0 else " - FAILED"))
    return exit_code


def cmd_ship(args: argparse.Namespace) -> int:
    try:
        config, branch, sha, targets, priority, validation = resolve_submission_options(args, "ship")
    except (FileNotFoundError, ValueError) as exc:
        print(f"Error: {exc}")
        return 1
    if validation != "full":
        print("Error: ship only supports full validation. Use `run --smoke` or `check --smoke` for preflight.")
        return 1

    base = args.base or "main"
    if branch == base:
        print(f"Error: cannot ship {base} to itself. Checkout a feature branch first.")
        return 1

    if not gh_available():
        print("Error: gh CLI not available or not authenticated. Run: gh auth login")
        return 1

    print(f"\n=== Shipping {branch} -> {base} ===\n")
    print(f"  Pushing {branch}...")
    push = subprocess.run(
        ["git", "push", "-u", "origin", branch],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if push.returncode != 0:
        print(f"  Push failed: {push.stderr.strip()}")
        return 1

    print("  Creating PR...")
    pr_number = gh_pr_create(branch, base)
    if pr_number is None:
        print("  Failed to create or find PR.")
        return 1
    print(f"  PR #{pr_number} ready")

    job, _created = enqueue_job(branch, sha, priority, targets, "ship", validation)
    print(f"  Queueing CI: {summarize_job(job)}")
    result, exit_code = wait_for_job(job["id"], config)
    if result is None:
        return 1

    gh_pr_comment(pr_number, format_ci_comment(result))
    if result["overall"] == "pass":
        print(f"  All targets passed. Merging PR #{pr_number}...")
        if gh_pr_merge(pr_number):
            print(f"  PR #{pr_number} merged and branch deleted.")
            notify(f"PR #{pr_number} shipped to {base}!")
            return 0
        print(f"  Merge failed. PR #{pr_number} is still open.")
        notify(f"PR #{pr_number} CI passed but merge failed")
        return 1

    print(f"  CI failed. PR #{pr_number} left open for review.")
    notify(f"PR #{pr_number} CI failed")
    return exit_code


def cmd_check(args: argparse.Namespace) -> int:
    if not gh_available():
        print("Error: gh CLI not available. Run: gh auth login")
        return 1

    pr_info = gh_pr_head(args.pr)
    if pr_info is None:
        return 1

    pr_number, branch, sha = pr_info
    print(f"  PR #{pr_number} -> branch: {branch} @ {short_sha(sha)}")

    try:
        config = load_config()
        targets = resolve_targets(config, parse_targets_arg(args.targets))
        priority = normalize_priority(args.priority or default_priority_for("check", config))
        validation = normalize_validation_mode("smoke" if getattr(args, "smoke", False) else "full")
    except (FileNotFoundError, ValueError) as exc:
        print(f"Error: {exc}")
        return 1

    job, _created = enqueue_job(branch, sha, priority, targets, "check", validation)
    print(f"  Queueing CI: {summarize_job(job)}")
    result, exit_code = wait_for_job(job["id"], config)
    if result is None:
        return 1

    gh_pr_comment(pr_number, format_ci_comment(result))
    notify("CI check complete" + (" - PASSED" if exit_code == 0 else " - FAILED"))
    return exit_code


def cmd_bump(args: argparse.Namespace) -> int:
    try:
        requested_priority = normalize_priority(args.priority)
    except ValueError as exc:
        print(f"Error: {exc}")
        return 1

    try:
        with file_lock(queue_lock_path(), blocking=True):
            queue = load_queue_unlocked()
            job = find_job_unlocked(queue, args.job, statuses={"pending", "running"})
            if job is None:
                print(f"No active job matches '{args.job}'.")
                return 1
            if job["status"] != "pending":
                print(f"Job is already {job['status']}; only pending jobs can be reprioritized.")
                return 1
            job["priority"] = requested_priority
            job["bumped_at"] = now_iso()
            save_queue_unlocked(queue)
            print(f"Updated priority: {summarize_job(job)}")
            return 0
    except ValueError as exc:
        print(f"Error: {exc}")
        return 1


def cmd_cancel(args: argparse.Namespace) -> int:
    try:
        with file_lock(queue_lock_path(), blocking=True):
            queue = load_queue_unlocked()
            job = find_job_unlocked(queue, args.job, statuses={"pending", "running"})
            if job is None:
                print(f"No active job matches '{args.job}'.")
                return 1
            if job["status"] != "pending":
                print(f"Job is already {job['status']}; only pending jobs can be canceled safely.")
                return 1
            cancel_job_unlocked(job)
            save_queue_unlocked(trim_completed_jobs(queue))
            print(f"Canceled: {summarize_job(job)}")
            return 0
    except ValueError as exc:
        print(f"Error: {exc}")
        return 1


def cmd_list(_args: argparse.Namespace) -> int:
    if not gh_available():
        print("Error: gh CLI not available. Run: gh auth login")
        return 1

    prs = gh_pr_list_open()
    if not prs:
        print("No open PRs.")
        return 0

    print(f"Open PRs ({len(prs)}):\n")
    for pr in prs:
        author = pr.get("author", {}).get("login", "?")
        labels = ", ".join(label.get("name", "") for label in pr.get("labels", []))
        label_str = f" [{labels}]" if labels else ""
        print(f"  #{pr['number']:4d}  {pr['title']}")
        print(f"         {pr['headRefName']} by {author}{label_str}")
    return 0


def resolve_job_for_logs(job_ref: str | None) -> dict | None:
    queue = load_queue()
    runner = current_runner_info()

    if job_ref:
        return find_job_unlocked(queue, job_ref)

    if runner and runner.get("active_job_id"):
        return find_job_unlocked(queue, runner["active_job_id"])

    completed = [job for job in queue if job.get("status") == "completed"]
    if completed:
        return completed[-1]
    return None


def cmd_logs(args: argparse.Namespace) -> int:
    try:
        job = resolve_job_for_logs(args.job)
    except ValueError as exc:
        print(f"Error: {exc}")
        return 1

    if job is None:
        print("No matching job logs found.")
        return 1

    paths: list[Path]
    if args.target:
        path = target_log_path(job["id"], args.target)
        paths = [path]
    else:
        log_dir = job_logs_dir(job["id"])
        paths = sorted(log_dir.glob("*.log"))

    if not paths:
        print(f"No logs found for job [{job['id']}] {job['branch']}.")
        return 1

    print(f"Logs for [{job['id']}] {job['branch']} @ {short_sha(job.get('sha', ''))}\n")
    for path in paths:
        print(f"== {path.stem} ==")
        lines = tail_lines(path, args.lines)
        if lines:
            print("".join(lines).rstrip())
        else:
            print("(empty)")
        print()
    return 0


def cmd_evidence(args: argparse.Namespace) -> int:
    branch = args.branch or current_branch()
    printed_header = False

    if branch:
        print(f"Evidence for branch `{branch}`:")
        printed_header = True
    elif args.sha:
        print(f"Evidence for sha `{short_sha(args.sha)}`:")
        printed_header = True

    found = print_evidence_summary(branch=branch, sha=args.sha, limit=args.limit)
    if not found:
        if printed_header:
            print("  (none)")
        else:
            print("No local CI evidence recorded.")
        return 1
    return 0


def cmd_status(_args: argparse.Namespace) -> int:
    try:
        config = load_config()
    except FileNotFoundError as exc:
        print(f"Error: {exc}")
        return 1

    queue = load_queue()
    pending = sorted([job for job in queue if job.get("status") == "pending"], key=job_sort_key)
    running = [job for job in queue if job.get("status") == "running"]
    completed = [job for job in queue if job.get("status") == "completed"]
    runner = current_runner_info()

    print(f"State: {state_dir()}")
    print(f"Config: {config_path()}")

    if runner:
        active_job = runner.get("active_job_id") or "?"
        active_branch = runner.get("active_branch") or "?"
        print(f"\nRunner: pid={runner.get('pid', '?')} active=[{active_job}] {active_branch}")
    else:
        print("\nRunner: idle")

    if running:
        print(f"\nRunning ({len(running)}):")
        for job in running:
            print(f"  {summarize_job(job)} started {job.get('started_at', '?')}")
            active_targets = job.get("active_targets") or (
                runner.get("active_targets") if runner and runner.get("active_job_id") == job["id"] else None
            )
            target_summary = summarize_active_targets(active_targets, job.get("targets"))
            if target_summary:
                print(f"    live targets: {target_summary}")
            for name in job.get("targets") or []:
                state = (active_targets or {}).get(name)
                if not state:
                    continue
                details = []
                if state.get("phase"):
                    details.append(f"phase={state['phase']}")
                if state.get("validation_mode"):
                    details.append(f"mode={state['validation_mode']}")
                if state.get("test_policy"):
                    details.append(f"tests={state['test_policy']}")
                if state.get("prepared_state"):
                    details.append(f"prepared={state['prepared_state']}")
                if state.get("wait_reason"):
                    details.append(f"wait={state['wait_reason']}")
                if state.get("cleanup_status"):
                    details.append(f"cleanup={state['cleanup_status']}")
                if state.get("last_output_at"):
                    details.append(f"output={state['last_output_at']}")
                if state.get("last_heartbeat_at"):
                    details.append(f"heartbeat={state['last_heartbeat_at']}")
                if state.get("quiet_for_secs") is not None:
                    details.append(f"idle={state['quiet_for_secs']}s")
                if state.get("liveness"):
                    details.append(f"liveness={state['liveness']}")
                if state.get("log_path"):
                    details.append(f"log={Path(state['log_path']).name}")
                if details:
                    print(f"    {name}: " + ", ".join(details))
                if state.get("last_line"):
                    print(f"      {state['last_line']}")
                if state.get("cleanup_result"):
                    print(f"      cleanup: {state['cleanup_result']}")
    else:
        print("\nNo running jobs.")

    if pending:
        print(f"\nPending ({len(pending)}):")
        for job in pending:
            print(f"  {summarize_job(job)} queued {job.get('queued_at', '?')}")
            active_targets = job.get("active_targets")
            target_summary = summarize_active_targets(active_targets, job.get("targets"))
            if target_summary:
                progress_at = job.get("last_progress_at") or job.get("requeued_at") or "?"
                print(f"    last known targets: {target_summary} (updated {progress_at})")
            for name in job.get("targets") or []:
                state = (active_targets or {}).get(name)
                if not state:
                    continue
                details = []
                if state.get("phase"):
                    details.append(f"phase={state['phase']}")
                if state.get("validation_mode"):
                    details.append(f"mode={state['validation_mode']}")
                if state.get("test_policy"):
                    details.append(f"tests={state['test_policy']}")
                if state.get("prepared_state"):
                    details.append(f"prepared={state['prepared_state']}")
                if state.get("wait_reason"):
                    details.append(f"wait={state['wait_reason']}")
                if state.get("cleanup_status"):
                    details.append(f"cleanup={state['cleanup_status']}")
                if state.get("last_output_at"):
                    details.append(f"output={state['last_output_at']}")
                if state.get("last_heartbeat_at"):
                    details.append(f"heartbeat={state['last_heartbeat_at']}")
                if state.get("quiet_for_secs") is not None:
                    details.append(f"idle={state['quiet_for_secs']}s")
                if state.get("liveness"):
                    details.append(f"liveness={state['liveness']}")
                if state.get("log_path"):
                    details.append(f"log={Path(state['log_path']).name}")
                if details:
                    print(f"    {name}: " + ", ".join(details))
                if state.get("last_line"):
                    print(f"      {state['last_line']}")
                if state.get("cleanup_result"):
                    print(f"      cleanup: {state['cleanup_result']}")
    else:
        print("\nNo pending jobs.")

    if completed:
        print(f"\nRecent ({min(len(completed), 5)}):")
        for job in completed[-5:]:
            result_file = job.get("result_file")
            if result_file and Path(result_file).exists():
                result = load_result(Path(result_file))
                targets = ", ".join(
                    f"{item['target']}={item['status']}" for item in result.get("results", [])
                )
                print(
                    f"  [{job['id']}] {job['branch']} @ {short_sha(job.get('sha', ''))} "
                    f"{result.get('overall', '?').upper()} [{targets}]"
                )
            else:
                print(f"  {summarize_job(job)} (result file missing)")

    branch = current_branch()
    if branch:
        print(f"\nEvidence ({branch}):")
        if not print_evidence_summary(branch=branch, limit=2, indent="  "):
            print("  (none)")

    print("\nVM Status:")
    for vm_name in ["Ubuntu 24.04 desktop", "Windows"]:
        print(f"  {vm_name}: {utmctl_vm_status(vm_name) or 'not found'}")

    for host in [target_cfg.get("host") for target_cfg in config.get("targets", {}).values() if target_cfg.get("type") == "ssh"]:
        if host:
            print(f"  ssh {host}: {'up' if ssh_reachable(host, 3) else 'down'}")

    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Local CI runner for Pulp",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    sub = parser.add_subparsers(dest="command")

    def add_submission_args(
        command_parser: argparse.ArgumentParser,
        *,
        include_sha: bool = False,
        allow_smoke: bool = False,
    ) -> None:
        command_parser.add_argument("branch", nargs="?", help="Branch name (default: current)")
        command_parser.add_argument(
            "--priority",
            choices=sorted(PRIORITY_VALUES),
            help="Queue priority (default from config; ship/check default to high)",
        )
        command_parser.add_argument(
            "--targets",
            help="Comma-separated target list (for example: mac or mac,ubuntu)",
        )
        if include_sha:
            command_parser.add_argument("--sha", help="Exact commit SHA to validate (default: current HEAD)")
        if allow_smoke:
            command_parser.add_argument(
                "--smoke",
                action="store_true",
                help="Run the fast clean install/export preflight instead of full validation",
            )

    p_enqueue = sub.add_parser("enqueue", help="Queue a branch for validation")
    add_submission_args(p_enqueue, include_sha=True, allow_smoke=True)

    sub.add_parser("drain", help="Process pending jobs if no other runner is active")

    p_run = sub.add_parser("run", help="Queue validation and wait for completion")
    add_submission_args(p_run, include_sha=True, allow_smoke=True)

    p_ship = sub.add_parser("ship", help="PR -> queued CI -> merge on green")
    add_submission_args(p_ship, include_sha=True)
    p_ship.add_argument("--base", default="main", help="Base branch (default: main)")

    p_check = sub.add_parser("check", help="Validate an existing PR")
    p_check.add_argument("pr", help="PR number, GitHub URL, or 'latest'")
    p_check.add_argument(
        "--priority",
        choices=sorted(PRIORITY_VALUES),
        help="Queue priority (default: high)",
    )
    p_check.add_argument(
        "--targets",
        help="Comma-separated target list (for example: mac or mac,ubuntu)",
    )
    p_check.add_argument(
        "--smoke",
        action="store_true",
        help="Run the fast clean install/export preflight instead of full validation",
    )

    sub.add_parser("list", help="Show open PRs")

    p_bump = sub.add_parser("bump", help="Reprioritize a pending job")
    p_bump.add_argument("job", help="Job id, unique id prefix, or exact branch name")
    p_bump.add_argument("priority", choices=sorted(PRIORITY_VALUES), help="New priority")

    p_cancel = sub.add_parser("cancel", help="Cancel a pending job")
    p_cancel.add_argument("job", help="Job id, unique id prefix, or exact branch name")

    p_logs = sub.add_parser("logs", help="Tail saved logs for a running or completed job")
    p_logs.add_argument("job", nargs="?", help="Job id, unique id prefix, or exact branch name (default: active/latest)")
    p_logs.add_argument("--target", help="Target name to show (mac, ubuntu, windows)")
    p_logs.add_argument("--lines", type=int, default=80, help="Number of log lines to show (default: 80)")

    p_evidence = sub.add_parser("evidence", help="Show accumulated last-good target results by exact SHA")
    p_evidence.add_argument("branch", nargs="?", help="Branch name (default: current)")
    p_evidence.add_argument("--sha", help="Filter to one exact SHA")
    p_evidence.add_argument("--limit", type=int, default=5, help="Shas to show per validation mode (default: 5)")

    sub.add_parser("status", help="Show queue, runner, results, and VM status")
    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()

    commands = {
        "enqueue": cmd_enqueue,
        "drain": cmd_drain,
        "run": cmd_run,
        "ship": cmd_ship,
        "check": cmd_check,
        "list": cmd_list,
        "bump": cmd_bump,
        "cancel": cmd_cancel,
        "logs": cmd_logs,
        "evidence": cmd_evidence,
        "status": cmd_status,
    }

    if args.command in commands:
        return commands[args.command](args)

    parser.print_help()
    return 1


if __name__ == "__main__":
    sys.exit(main())
