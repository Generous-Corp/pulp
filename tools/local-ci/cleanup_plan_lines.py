"""Cleanup plan line rendering helpers."""

from __future__ import annotations

from collections.abc import Callable
from pathlib import Path


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
