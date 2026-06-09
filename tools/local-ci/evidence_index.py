"""Evidence index persistence and grouping for local CI.

This module owns the durable "last good evidence" index derived from local CI
result files. It intentionally does not know about queue claiming, runner
liveness, or target execution.
"""

from __future__ import annotations

import json
from collections import defaultdict
from pathlib import Path

from git_helpers import short_sha
from io_utils import atomic_write_text, file_lock
from provenance import normalize_provenance, normalize_result, provenance_summary
from state_paths import evidence_lock_path, evidence_path, results_dir


def empty_evidence_index() -> dict:
    return {"version": 3, "entries": {}}


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
        "provenance": normalize_provenance(result.get("provenance")),
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


def load_result(path: Path) -> dict:
    return normalize_result(json.loads(path.read_text()))


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


def collect_evidence_groups_from_index(
    index: dict,
    *,
    branch: str | None = None,
    sha: str | None = None,
) -> dict[str, list[dict]]:
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


def collect_evidence_groups(branch: str | None = None, sha: str | None = None) -> dict[str, list[dict]]:
    return collect_evidence_groups_from_index(load_evidence_index(), branch=branch, sha=sha)


def print_evidence_summary_from_groups(
    groups: dict[str, list[dict]],
    *,
    limit: int = 3,
    indent: str = "",
) -> bool:
    if not groups:
        return False

    for validation in sorted(groups):
        print(f"{indent}{validation}:")
        for item in groups[validation][:limit]:
            targets = ", ".join(f"{target}=pass" for target in sorted(item.get("targets", {})))
            print(
                f"{indent}  {short_sha(item.get('sha', ''))} [{targets}] "
                f"last={item.get('completed_at', '?')} "
                f"via {provenance_summary(item.get('provenance'))}"
            )
    return True


def print_evidence_summary(
    *,
    branch: str | None = None,
    sha: str | None = None,
    limit: int = 3,
    indent: str = "",
) -> bool:
    return print_evidence_summary_from_groups(
        collect_evidence_groups(branch=branch, sha=sha),
        limit=limit,
        indent=indent,
    )
