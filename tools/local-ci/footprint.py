"""Disk-footprint accounting helpers for local CI.

Extracted from local_ci.py to give the `status` / cleanup paths a
focused seam without dragging in the orchestrator. Pure read-side
helpers; no mutation, no subprocess.

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


def describe_path_for_cleanup(path: Path) -> str:
    try:
        return str(path.relative_to(state_dir()))
    except ValueError:
        return str(path)
