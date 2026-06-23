"""Disk-footprint accounting helpers for local CI.

These are focused read-side helpers for the `status` / cleanup paths: no
mutation, no subprocess.

`local_ci_state_footprint()` summarizes the on-disk sizes of the
state subdirectories that grow over time (bundles, prepared
checkouts, target logs, results, cloud-run records). Use it to
populate `pulp ci-local status` and to detect runaway disk usage.
"""

from __future__ import annotations

import os
from pathlib import Path

from state_paths import (
    bundles_dir,
    cloud_runs_dir,
    logs_dir,
    prepared_dir,
    results_dir,
    state_dir,
)


def format_size_bytes(value: int | float | None) -> str:
    if value in (None, ""):
        return ""
    amount = float(value)
    units = ["B", "KB", "MB", "GB", "TB"]
    for unit in units:
        if amount < 1024.0 or unit == units[-1]:
            if unit == "B":
                return f"{int(amount)} {unit}"
            return f"{amount:.1f} {unit}"
        amount /= 1024.0
    return f"{amount:.1f} TB"


def path_size_bytes(path: Path) -> int:
    try:
        if not path.exists():
            return 0
        if path.is_file():
            return int(path.stat().st_size)
    except OSError:
        return 0

    total = 0
    for root, _dirs, files in os.walk(path):
        for filename in files:
            try:
                total += int((Path(root) / filename).stat().st_size)
            except OSError:
                continue
    return total


def local_ci_state_footprint() -> dict:
    entries = {}
    total = 0
    for label, path in (
        ("bundles", bundles_dir()),
        ("prepared", prepared_dir()),
        ("logs", logs_dir()),
        ("results", results_dir()),
        ("cloud-runs", cloud_runs_dir()),
    ):
        size_bytes = path_size_bytes(path)
        entries[label] = {
            "path": path,
            "size_bytes": size_bytes,
        }
        total += size_bytes
    return {
        "entries": entries,
        "total_bytes": total,
    }


def state_footprint_lines(footprint: dict, *, indent: str = "") -> list[str]:
    lines = [
        f"{indent}Local CI footprint: total={format_size_bytes(footprint.get('total_bytes', 0))}",
    ]
    for label in ("bundles", "prepared", "logs", "results", "cloud-runs"):
        entry = (footprint.get("entries") or {}).get(label) or {}
        lines.append(
            f"{indent}  {label}: {format_size_bytes(entry.get('size_bytes', 0))} "
            f"({describe_path_for_cleanup(entry.get('path', state_dir()))})"
        )
    return lines


def describe_path_for_cleanup(path: Path) -> str:
    try:
        return str(path.relative_to(state_dir()))
    except ValueError:
        return str(path)
