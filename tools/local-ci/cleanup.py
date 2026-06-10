"""Cleanup planning and deletion helpers for local CI state.

This module owns the disk-artifact retention rules. The `local_ci.py` entrypoint
keeps thin wrappers around these helpers so existing callers can keep using the
historical `local_ci.*` names.
"""

from __future__ import annotations

import json
import shutil
from pathlib import Path
from typing import Callable


DEFAULT_KEEP_COMPLETED_JOBS = 25


def result_file_job_id(path: Path) -> str | None:
    if path.suffix != ".json":
        return None
    stem = path.stem
    parts = stem.split("-", 3)
    if len(parts) < 3:
        return None
    return parts[2]


def artifact_entry_sort_key(entry: dict) -> tuple[float, str]:
    return (float(entry.get("mtime", 0.0)), str(entry.get("path", "")))


def collect_local_ci_cleanup_plan(
    queue: list[dict],
    *,
    keep_results: int = DEFAULT_KEEP_COMPLETED_JOBS,
    keep_logs: int = DEFAULT_KEEP_COMPLETED_JOBS,
    keep_bundles: int = 0,
    include_prepared: bool = False,
    bundles_dir_fn: Callable[[], Path],
    logs_dir_fn: Callable[[], Path],
    results_dir_fn: Callable[[], Path],
    prepared_dir_fn: Callable[[], Path],
    path_size_bytes_fn: Callable[[Path], int],
) -> dict:
    keep_results = max(0, int(keep_results))
    keep_logs = max(0, int(keep_logs))
    keep_bundles = max(0, int(keep_bundles))
    retained_job_ids = {job["id"] for job in queue}
    live_job_ids = {job["id"] for job in queue if job.get("status") in {"pending", "running"}}
    categories: dict[str, list[dict]] = {
        "bundles": [],
        "logs": [],
        "results": [],
        "prepared": [],
    }

    def add_file_entry(category: str, path: Path, job_id: str | None) -> None:
        try:
            stat = path.stat()
        except OSError:
            return
        categories[category].append(
            {
                "path": path,
                "job_id": job_id,
                "size_bytes": int(stat.st_size),
                "mtime": float(stat.st_mtime),
            }
        )

    def add_dir_entry(category: str, path: Path, job_id: str | None) -> None:
        if not path.exists() or not path.is_dir():
            return
        try:
            stat = path.stat()
        except OSError:
            return
        categories[category].append(
            {
                "path": path,
                "job_id": job_id,
                "size_bytes": path_size_bytes_fn(path),
                "mtime": float(stat.st_mtime),
            }
        )

    for path in bundles_dir_fn().glob("*.bundle"):
        add_file_entry("bundles", path, path.stem)
    log_root = logs_dir_fn()
    for path in (log_root.iterdir() if log_root.exists() else []):
        if path.is_dir():
            add_dir_entry("logs", path, path.name)
    for path in results_dir_fn().glob("*.json"):
        add_file_entry("results", path, result_file_job_id(path))
    prepared_root = prepared_dir_fn()
    if include_prepared and prepared_root.exists():
        for target_dir in prepared_root.iterdir():
            if not target_dir.is_dir():
                continue
            for mode_dir in target_dir.iterdir():
                if mode_dir.is_dir():
                    add_dir_entry("prepared", mode_dir, None)

    plan_categories: dict[str, list[dict]] = {
        "bundles": [],
        "logs": [],
        "results": [],
        "prepared": [],
    }

    bundle_candidates = [
        entry
        for entry in sorted(categories["bundles"], key=artifact_entry_sort_key, reverse=True)
        if entry.get("job_id") not in live_job_ids
    ]
    plan_categories["bundles"] = bundle_candidates[keep_bundles:]

    def select_queue_orphans(entries: list[dict], keep_count: int) -> list[dict]:
        always_keep = [entry for entry in entries if entry.get("job_id") in retained_job_ids]
        orphaned = [entry for entry in entries if entry.get("job_id") not in retained_job_ids]
        orphaned.sort(key=artifact_entry_sort_key, reverse=True)
        del always_keep  # clarity: retained-job artifacts are never candidates
        return orphaned[keep_count:]

    plan_categories["logs"] = select_queue_orphans(categories["logs"], keep_logs)
    plan_categories["results"] = select_queue_orphans(categories["results"], keep_results)
    plan_categories["prepared"] = sorted(
        categories["prepared"],
        key=artifact_entry_sort_key,
        reverse=True,
    )

    total_bytes = sum(
        int(entry.get("size_bytes", 0))
        for entries in plan_categories.values()
        for entry in entries
    )
    total_paths = sum(len(entries) for entries in plan_categories.values())
    return {
        "categories": plan_categories,
        "total_bytes": total_bytes,
        "total_paths": total_paths,
        "keep_results": keep_results,
        "keep_logs": keep_logs,
        "keep_bundles": keep_bundles,
        "include_prepared": include_prepared,
    }


def apply_local_ci_cleanup_plan(plan: dict) -> dict:
    removed: list[dict] = []
    failed: list[dict] = []
    for category, entries in (plan.get("categories") or {}).items():
        for entry in entries:
            path = Path(entry["path"])
            try:
                if path.is_dir():
                    shutil.rmtree(path)
                else:
                    path.unlink(missing_ok=True)
                removed.append(
                    {
                        "category": category,
                        "path": path,
                        "size_bytes": int(entry.get("size_bytes", 0)),
                    }
                )
            except OSError as exc:
                failed.append(
                    {
                        "category": category,
                        "path": path,
                        "error": str(exc),
                    }
                )
    return {
        "removed": removed,
        "failed": failed,
        "removed_bytes": sum(item["size_bytes"] for item in removed),
    }


def cleanup_plan_lines(
    plan: dict,
    *,
    dry_run: bool,
    format_size_fn: Callable[[int], str],
    describe_path_fn: Callable[[Path], str],
    entry_limit: int = 10,
) -> list[str]:
    lines = [
        "Local CI cleanup:",
        "",
        f"  reclaimable: {format_size_fn(plan.get('total_bytes', 0))} "
        f"across {plan.get('total_paths', 0)} path(s)",
    ]
    for category in ("bundles", "logs", "results", "prepared"):
        entries = (plan.get("categories") or {}).get(category) or []
        if not entries:
            continue
        category_bytes = sum(int(entry.get("size_bytes", 0)) for entry in entries)
        lines.extend(
            [
                "",
                f"  {category}: {format_size_fn(category_bytes)} "
                f"across {len(entries)} path(s)",
            ]
        )
        for entry in entries[:entry_limit]:
            lines.append(
                f"    {describe_path_fn(Path(entry['path']))} "
                f"({format_size_fn(entry.get('size_bytes', 0))})"
            )
        if len(entries) > entry_limit:
            lines.append(f"    ... {len(entries) - entry_limit} more")

    lines.extend(
        [
            "",
            "  dry run only; re-run with --apply to delete these paths"
            if dry_run
            else "  applying cleanup now",
        ]
    )
    return lines


def collect_stale_windows_cleanup_candidates_unlocked(
    queue: list[dict],
    *,
    stale_running_jobs_fn: Callable[[list[dict]], list[dict]],
    now_fn: Callable[[], str],
) -> list[dict]:
    candidates: list[dict] = []
    for job in stale_running_jobs_fn(queue):
        active_targets = job.get("active_targets") or {}
        state = dict(active_targets.get("windows") or {})
        host = state.get("host")
        validator_pid = state.get("validator_pid")
        validator_started_at = state.get("validator_started_at")
        if not host or validator_pid is None or not validator_started_at:
            continue
        if state.get("cleanup_requested_at"):
            continue

        state["cleanup_requested_at"] = now_fn()
        state["cleanup_status"] = "requested"
        state["cleanup_reason"] = "stale_runner_recovery"
        active_targets["windows"] = state
        job["active_targets"] = active_targets
        job["last_progress_at"] = now_fn()
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


def stale_windows_validator_cleanup_script(
    pid: int,
    started_at: str,
    *,
    ps_literal_fn: Callable[[str], str],
) -> str:
    return f"""
$PidToKill = {pid}
$ExpectedStart = '{ps_literal_fn(started_at)}'

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


def cleanup_stale_windows_validator(
    host: str,
    pid: int,
    started_at: str,
    *,
    ps_literal_fn: Callable[[str], str],
    run_logged_command_fn: Callable,
    windows_ssh_powershell_command_fn: Callable[[str], list[str]],
    trim_line_fn: Callable[[str], str],
) -> dict:
    ps_script = stale_windows_validator_cleanup_script(
        pid,
        started_at,
        ps_literal_fn=ps_literal_fn,
    )
    run = run_logged_command_fn(
        windows_ssh_powershell_command_fn(host),
        input_text=ps_script,
        timeout=120,
    )
    lines = [line.strip() for line in run.get("output", "").splitlines() if line.strip()]
    payload = {}
    if lines:
        try:
            payload = json.loads(lines[-1])
        except json.JSONDecodeError:
            payload = {"error": trim_line_fn(lines[-1])}
    if run.get("returncode") != 0:
        payload.setdefault("error", f"cleanup command exited {run.get('returncode')}")
    return payload


def stale_windows_validator_cleanup_status(result: dict) -> str:
    if result.get("killed"):
        return "killed"
    if not result.get("found", True):
        return "not-found"
    if result.get("found") and not result.get("matched", True):
        return "mismatch"
    if result.get("error"):
        return "error"
    return "checked"


def stale_windows_validator_update_fields(
    candidate: dict,
    result: dict,
    *,
    now_fn: Callable[[], str],
    trim_line_fn: Callable[[str], str],
) -> dict:
    clear_process = bool(result.get("killed") or not result.get("found", True))
    return {
        "cleanup_completed_at": now_fn(),
        "cleanup_status": stale_windows_validator_cleanup_status(result),
        "cleanup_result": trim_line_fn(json.dumps(result, sort_keys=True)),
        "validator_pid": None if clear_process else candidate["validator_pid"],
        "validator_started_at": None if clear_process else candidate["validator_started_at"],
    }


def reclaim_stale_remote_validator_candidates(
    candidates: list[dict],
    *,
    cleanup_validator_fn: Callable[[str, int, str], dict],
    update_job_target_state_fn: Callable,
    now_fn: Callable[[], str],
    trim_line_fn: Callable[[str], str],
) -> int:
    for candidate in candidates:
        result = cleanup_validator_fn(
            candidate["host"],
            candidate["validator_pid"],
            candidate["validator_started_at"],
        )
        update_job_target_state_fn(
            candidate["job_id"],
            candidate["target"],
            **stale_windows_validator_update_fields(
                candidate,
                result,
                now_fn=now_fn,
                trim_line_fn=trim_line_fn,
            ),
        )
    return len(candidates)
