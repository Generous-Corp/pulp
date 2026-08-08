#!/usr/bin/env python3
"""Verify exact, non-expired Codecov upload receipts for one commit."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import sys
from typing import Any


def receipt_counts(
    artifacts: list[dict[str, Any]], sha: str, axis_attempts: dict[str, int]
) -> dict[str, int]:
    """Count exact, non-expired receipt artifacts for each required axis."""
    expected = {
        axis: f"codecov-upload-{axis}-{sha}-attempt-{attempt}"
        for axis, attempt in axis_attempts.items()
    }
    return {
        axis: sum(
            artifact.get("name") == name and artifact.get("expired") is False
            for artifact in artifacts
        )
        for axis, name in expected.items()
    }


def receipt_summary(
    artifacts: list[dict[str, Any]], sha: str, axis_attempts: dict[str, int]
) -> dict[str, Any]:
    """Return exact counts and the oldest creation time in a complete set."""
    expected = {
        axis: f"codecov-upload-{axis}-{sha}-attempt-{attempt}"
        for axis, attempt in axis_attempts.items()
    }
    matches = {
        axis: [
            artifact
            for artifact in artifacts
            if artifact.get("name") == name and artifact.get("expired") is False
        ]
        for axis, name in expected.items()
    }
    created_at = [
        artifact.get("created_at")
        for axis_matches in matches.values()
        for artifact in axis_matches
    ]
    valid_times = [
        value
        for value in created_at
        if isinstance(value, str)
        and _parse_github_timestamp(value) is not None
    ]
    return {
        "counts": {axis: len(axis_matches) for axis, axis_matches in matches.items()},
        "oldest_created_at": (
            min(valid_times) if len(valid_times) == len(axis_attempts) else None
        ),
    }


def _parse_github_timestamp(value: str) -> dt.datetime | None:
    try:
        return dt.datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError:
        return None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sha", required=True)
    parser.add_argument(
        "--required-axis",
        nargs="+",
        required=True,
        metavar="AXIS=ATTEMPT",
        help="Required receipt axes and the latest workflow attempt that ran each axis.",
    )
    args = parser.parse_args()

    payload = json.load(sys.stdin)
    artifacts = payload.get("artifacts", payload) if isinstance(payload, dict) else payload
    if not isinstance(artifacts, list):
        raise SystemExit("artifact payload must be a list or contain an artifacts list")

    axis_attempts: dict[str, int] = {}
    for spec in args.required_axis:
        try:
            axis, attempt_text = spec.rsplit("=", 1)
            attempt = int(attempt_text)
        except (ValueError, TypeError):
            raise SystemExit(f"invalid --required-axis value: {spec!r}") from None
        if not axis or attempt < 1:
            raise SystemExit(f"invalid --required-axis value: {spec!r}")
        axis_attempts[axis] = attempt

    summary = receipt_summary(artifacts, args.sha, axis_attempts)
    print(json.dumps(summary, sort_keys=True))
    return 0 if (
        all(count == 1 for count in summary["counts"].values())
        and summary["oldest_created_at"] is not None
    ) else 1


if __name__ == "__main__":
    raise SystemExit(main())
