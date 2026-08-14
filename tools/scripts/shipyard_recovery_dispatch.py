#!/usr/bin/env python3
"""Plan deduplicated exact-head Shipyard recovery assignments.

This renderer is intentionally pure: it reads a Shipyard report plus a census
of commit statuses and emits a dispatch plan. GitHub mutations belong to the
single serialized controller workflow.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
from typing import Any


CONTEXT = "shipyard/recovery-dispatch"
RECOVERY_ACTIONS = {"needs_update", "required_failed"}
FULL_SHA = re.compile(r"^[0-9a-f]{40}$")


def _load(path: Path) -> Any:
    with path.open(encoding="utf-8") as handle:
        return json.load(handle)


def _repos(report: dict[str, Any]) -> list[dict[str, Any]]:
    repos = report.get("repos")
    if repos is None and isinstance(report.get("data"), dict):
        repos = report["data"].get("repos")
    if not isinstance(repos, list):
        raise ValueError("steward report has no repository list")
    return [repo for repo in repos if isinstance(repo, dict)]


def _status_map(raw: Any) -> dict[str, list[dict[str, Any]]]:
    if not isinstance(raw, list):
        raise ValueError("recovery status census must be an array")
    result: dict[str, list[dict[str, Any]]] = {}
    for row in raw:
        if not isinstance(row, dict):
            continue
        head = str(row.get("head_sha") or "").lower()
        statuses = row.get("statuses")
        if FULL_SHA.fullmatch(head) and isinstance(statuses, list):
            result[head] = [status for status in statuses if isinstance(status, dict)]
    return result


def _latest_dispatch_status(statuses: list[dict[str, Any]]) -> dict[str, Any] | None:
    matching = [
        status
        for status in statuses
        if str(status.get("context") or "").lower() == CONTEXT
    ]
    if not matching:
        return None
    return max(
        matching,
        key=lambda status: (
            str(status.get("created_at") or ""),
            int(status.get("id") or 0),
        ),
    )


def _retry_attempt(status: dict[str, Any] | None) -> int | None:
    if status is None:
        return 1
    if str(status.get("state") or "").lower() != "error":
        return None
    match = re.search(r"\battempt=(\d+)\b", str(status.get("description") or ""))
    previous = int(match.group(1)) if match else 1
    return previous + 1 if previous < 2 else None


def _fingerprint(repo: str, number: int, head: str, decision: dict[str, Any]) -> str:
    material = json.dumps(
        {
            "repo": repo,
            "pr": number,
            "head": head,
            "decision": decision,
        },
        sort_keys=True,
        separators=(",", ":"),
    ).encode()
    return hashlib.sha256(material).hexdigest()


def build_plan(
    report: dict[str, Any], statuses_raw: Any, epoch: int
) -> dict[str, Any]:
    if epoch <= 0:
        raise ValueError("assignment epoch must be positive")
    statuses = _status_map(statuses_raw)
    candidates: list[dict[str, Any]] = []
    errors: list[str] = []
    for repo_row in _repos(report):
        repo = str(repo_row.get("repo") or "")
        if "/" not in repo:
            errors.append("steward report contains malformed repository")
            continue
        for pr in repo_row.get("prs") or []:
            if not isinstance(pr, dict):
                continue
            decision = pr.get("decision")
            if not isinstance(decision, dict):
                continue
            action = str(decision.get("action") or "")
            if action not in RECOVERY_ACTIONS or pr.get("error"):
                continue
            number = pr.get("number")
            head = str(pr.get("head_sha") or "").lower()
            if not isinstance(number, int) or number <= 0 or not FULL_SHA.fullmatch(head):
                errors.append(f"{repo} has invalid recovery target")
                continue
            attempt = _retry_attempt(_latest_dispatch_status(statuses.get(head, [])))
            if attempt is None:
                continue
            candidates.append(
                {
                    "repo": repo,
                    "pr_number": number,
                    "expected_head": head,
                    "assignment_epoch": epoch,
                    "dispatch_attempt": attempt,
                    "decision": decision,
                    "fingerprint": _fingerprint(repo, number, head, decision),
                }
            )
    candidates.sort(key=lambda row: (row["repo"], row["pr_number"]))
    return {
        "schema_version": 1,
        "context": CONTEXT,
        "candidate_count": len(candidates),
        "candidates": candidates,
        "errors": errors,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--statuses", type=Path, required=True)
    parser.add_argument("--epoch", type=int, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    plan = build_plan(_load(args.report), _load(args.statuses), args.epoch)
    args.output.write_text(json.dumps(plan, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return 0 if not plan["errors"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
