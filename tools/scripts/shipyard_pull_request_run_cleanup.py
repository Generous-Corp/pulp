#!/usr/bin/env python3
"""Plan bounded cancellation of superseded pull-request workflow runs.

GitHub can leave a workflow run queued after a newer commit replaces the PR
head, even when the workflow uses ``cancel-in-progress``. Those obsolete runs
consume hosted or scarce self-hosted capacity while their conclusions can no
longer satisfy the current PR head. The steward may cancel only nonterminal
``pull_request`` runs whose exact head SHA is absent from every currently open
pull request. ``pull_request_target`` is deliberately out of scope because its
workflow run head is the trusted base commit, not the untrusted PR head.
"""

from __future__ import annotations

import argparse
from datetime import datetime
import json
from pathlib import Path
import re
from typing import Any


SHA_RE = re.compile(r"^[0-9a-f]{40}$")
CANCELLABLE_STATES = {"in_progress", "pending", "queued", "requested", "waiting"}


def _load(path: Path) -> Any:
    with path.open(encoding="utf-8") as handle:
        return json.load(handle)


def current_heads(pulls: Any) -> set[str]:
    if not isinstance(pulls, list):
        raise ValueError("open pull-request census must be an array")
    heads: set[str] = set()
    for pull in pulls:
        if not isinstance(pull, dict):
            raise ValueError("open pull request must be an object")
        head_payload = pull.get("head")
        if not isinstance(head_payload, dict):
            raise ValueError("open pull request must contain a head object")
        head = str(head_payload.get("sha") or "").lower()
        if not SHA_RE.fullmatch(head):
            raise ValueError("open pull-request head must be a full lowercase SHA")
        heads.add(head)
    return heads


def _created_sort_key(value: Any) -> tuple[int, str]:
    text = str(value or "")
    try:
        parsed = datetime.fromisoformat(text.replace("Z", "+00:00"))
    except ValueError:
        return (1, text)
    return (0, parsed.isoformat())


def plan_cleanup(pulls: Any, runs: Any, *, limit: int = 20) -> dict[str, Any]:
    if limit < 1:
        raise ValueError("cancellation limit must be positive")
    heads = current_heads(pulls)
    if not isinstance(runs, list):
        raise ValueError("workflow run census must be an array")

    seen_ids: set[int] = set()
    candidates: list[dict[str, Any]] = []
    for run in runs:
        if not isinstance(run, dict):
            raise ValueError("workflow run must be an object")
        if str(run.get("event") or "") != "pull_request":
            continue
        status = str(run.get("status") or "").lower()
        if status == "completed":
            continue
        if status not in CANCELLABLE_STATES:
            raise ValueError(f"unexpected nonterminal pull-request run status: {status!r}")
        run_id = run.get("id")
        if isinstance(run_id, bool) or not isinstance(run_id, int) or run_id < 1:
            raise ValueError("nonterminal pull-request run must have a positive integer id")
        if run_id in seen_ids:
            raise ValueError(f"duplicate workflow run id: {run_id}")
        seen_ids.add(run_id)
        head = str(run.get("head_sha") or "").lower()
        if not SHA_RE.fullmatch(head):
            raise ValueError("nonterminal pull-request run must have a full lowercase head SHA")
        if head in heads:
            continue
        candidates.append(
            {
                "run_id": run_id,
                "head_sha": head,
                "head_branch": str(run.get("head_branch") or ""),
                "workflow": str(run.get("name") or ""),
                "status": status,
                "created_at": str(run.get("created_at") or ""),
            }
        )

    candidates.sort(key=lambda row: (_created_sort_key(row["created_at"]), row["run_id"]))
    total = len(candidates)
    return {
        "schema_version": 1,
        "current_head_count": len(heads),
        "candidate_count": total,
        "cancellation_limit": limit,
        "truncated": total > limit,
        "candidates": candidates[:limit],
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pulls", type=Path, required=True)
    parser.add_argument("--runs", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--limit", type=int, default=20)
    args = parser.parse_args()
    result = plan_cleanup(_load(args.pulls), _load(args.runs), limit=args.limit)
    args.output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
