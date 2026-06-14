"""Pure evidence index record helpers."""

from __future__ import annotations

from pathlib import Path

from provenance import normalize_provenance


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
