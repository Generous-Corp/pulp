"""Cleanup artifact identity and sorting helpers."""

from __future__ import annotations

from pathlib import Path


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
