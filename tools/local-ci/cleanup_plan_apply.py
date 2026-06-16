"""Cleanup artifact deletion helpers."""

from __future__ import annotations

import shutil
from pathlib import Path


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
