#!/usr/bin/env python3
"""Plan truthful sentinel-label convergence for otherwise unlabeled open PRs."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
from typing import Any


SHA_RE = re.compile(r"^[0-9a-f]{40}$")
SENTINEL = "shipyard:provenance-missing"


def _load(path: Path) -> Any:
    with path.open(encoding="utf-8") as handle:
        return json.load(handle)


def plan_labels(pulls: Any) -> dict[str, Any]:
    if not isinstance(pulls, list):
        raise ValueError("open pull-request census must be an array")

    seen_numbers: set[int] = set()
    candidates: list[dict[str, Any]] = []
    for pull in pulls:
        if not isinstance(pull, dict):
            raise ValueError("open pull request must be an object")
        number = pull.get("number")
        if isinstance(number, bool) or not isinstance(number, int) or number < 1:
            raise ValueError("open pull request must have a positive integer number")
        if number in seen_numbers:
            raise ValueError(f"duplicate open pull request number: {number}")
        seen_numbers.add(number)
        if pull.get("state") != "open":
            raise ValueError("pull-request census must contain only open PRs")
        head_payload = pull.get("head")
        if not isinstance(head_payload, dict):
            raise ValueError("open pull request must contain a head object")
        head = str(head_payload.get("sha") or "").lower()
        if not SHA_RE.fullmatch(head):
            raise ValueError("open pull-request head must be a full lowercase SHA")
        raw_labels = pull.get("labels")
        if not isinstance(raw_labels, list):
            raise ValueError("open pull-request labels must be an array")
        labels: list[str] = []
        for label in raw_labels:
            if not isinstance(label, dict) or not isinstance(label.get("name"), str):
                raise ValueError("open pull-request label must contain a string name")
            labels.append(label["name"])
        if len(set(labels)) != len(labels):
            raise ValueError(f"duplicate label on pull request {number}")

        mutation = None
        if not labels:
            mutation = "add"
        elif SENTINEL in labels and len(labels) > 1:
            mutation = "remove"
        if mutation:
            candidates.append(
                {
                    "number": number,
                    "expected_head": head,
                    "mutation": mutation,
                    "label": SENTINEL,
                }
            )

    candidates.sort(key=lambda row: row["number"])
    return {
        "schema_version": 1,
        "open_pr_count": len(pulls),
        "candidate_count": len(candidates),
        "sentinel": SENTINEL,
        "candidates": candidates,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pulls", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.write_text(
        json.dumps(plan_labels(_load(args.pulls)), indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
