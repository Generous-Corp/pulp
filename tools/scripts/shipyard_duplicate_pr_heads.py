#!/usr/bin/env python3
"""Detect duplicate open PR heads in one Shipyard steward census.

This is deliberately pure and read-only.  It validates the complete REST
snapshot before grouping, so a truncated or malformed census fails closed
instead of being reported as clean.  A REST snapshot is a diagnostic guard;
Shipyard's atomic exact-head handoff remains the merge-time authority.
"""
from __future__ import annotations

import argparse
import json
import sys
from collections import defaultdict
from pathlib import Path
from typing import Any


REQUIRED = (
    ("number",),
    ("head", "repo", "full_name"),
    ("head", "sha"),
    ("base", "repo", "full_name"),
    ("base", "ref"),
)


def value(row: dict[str, Any], path: tuple[str, ...]) -> Any:
    current: Any = row
    for part in path:
        if not isinstance(current, dict) or part not in current:
            raise ValueError(f"missing census field: {'.'.join(path)}")
        current = current[part]
    if current in (None, ""):
        raise ValueError(f"empty census field: {'.'.join(path)}")
    return current


def load(path: Path) -> list[dict[str, Any]]:
    try:
        rows = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError(f"cannot read census: {exc}") from exc
    if not isinstance(rows, list):
        raise ValueError("open PR census must be an array")
    for index, row in enumerate(rows):
        if not isinstance(row, dict):
            raise ValueError(f"census row {index} must be an object")
        for path_parts in REQUIRED:
            value(row, path_parts)
    return rows


def report(rows: list[dict[str, Any]], commit_sets: dict[str, list[str]] | None = None) -> dict[str, Any]:
    groups: defaultdict[tuple[str, str, str, str], list[int]] = defaultdict(list)
    for row in rows:
        key = (
            value(row, ("head", "repo", "full_name")),
            value(row, ("head", "sha")),
            value(row, ("base", "repo", "full_name")),
            value(row, ("base", "ref")),
        )
        groups[key].append(value(row, ("number",)))

    duplicates = []
    for (head_repo, head_sha, base_repo, base_ref), numbers in sorted(groups.items()):
        if len(numbers) > 1:
            duplicates.append({
                "kind": "duplicate_head",
                "head_repo": head_repo,
                "head_sha": head_sha,
                "base_repo": base_repo,
                "base_ref": base_ref,
                "pull_requests": sorted(numbers),
            })

    if commit_sets:
        by_commit: defaultdict[str, list[int]] = defaultdict(list)
        for row in rows:
            number = value(row, ("number",))
            for commit in commit_sets.get(str(number), []):
                if commit not in by_commit[commit]:
                    by_commit[commit].append(number)
        for commit, numbers in sorted(by_commit.items()):
            if len(numbers) > 1:
                duplicates.append({
                    "kind": "shared_unmerged_commit",
                    "commit": commit,
                    "pull_requests": sorted(numbers),
                })
        duplicates.sort(key=lambda item: (item["kind"], item["pull_requests"]))
    return {
        "status": "duplicate_heads" if duplicates else "clean",
        "open_pr_count": len(rows),
        "duplicates": duplicates,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pulls", required=True, type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--commits", type=Path,
                        help="optional JSON object mapping PR number to unmerged commit SHAs")
    args = parser.parse_args()
    try:
        commit_sets = None
        if args.commits:
            commit_sets = json.loads(args.commits.read_text(encoding="utf-8"))
            if not isinstance(commit_sets, dict):
                raise ValueError("commit census must be an object")
        result = report(load(args.pulls), commit_sets)
    except ValueError as exc:
        result = {"status": "invalid_census", "error": str(exc)}
        status = 2
    else:
        status = 1 if result["duplicates"] else 0
    rendered = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(rendered, encoding="utf-8")
    print(rendered, end="")
    return status


if __name__ == "__main__":
    sys.exit(main())
