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


def processed_upload_counts(
    payload: dict[str, Any], repository: str, run_id: int, required_flags: list[str]
) -> dict[str, Any]:
    """Count exact-run Codecov uploads that reached the merged state."""
    uploads = payload.get("results")
    declared_count = payload.get("count")
    if not isinstance(uploads, list) or not isinstance(declared_count, int):
        raise ValueError("Codecov upload payload must contain count and results")
    complete_page = declared_count == len(uploads)
    expected_build_url = f"https://github.com/{repository}/actions/runs/{run_id}"
    counts = {
        flag: sum(
            upload.get("build_url") == expected_build_url
            and upload.get("state") == "merged"
            and isinstance(upload.get("flags"), list)
            and flag in upload["flags"]
            for upload in uploads
        )
        for flag in required_flags
    }
    return {"counts": counts, "complete_page": complete_page}


def _parse_github_timestamp(value: str) -> dt.datetime | None:
    try:
        return dt.datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError:
        return None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sha")
    parser.add_argument(
        "--required-axis",
        nargs="+",
        metavar="AXIS=ATTEMPT",
        help="Required receipt axes and the latest workflow attempt that ran each axis.",
    )
    parser.add_argument("--codecov-processed-run-id", type=int)
    parser.add_argument("--repository")
    parser.add_argument("--required-flag", nargs="+")
    args = parser.parse_args()

    payload = json.load(sys.stdin)

    if args.codecov_processed_run_id is not None:
        if args.sha or args.required_axis:
            raise SystemExit("processed-upload mode cannot be combined with receipt mode")
        if not args.repository or not args.required_flag:
            raise SystemExit(
                "processed-upload mode requires --repository and --required-flag"
            )
        if not isinstance(payload, dict):
            raise SystemExit("Codecov upload payload must be an object")
        try:
            summary = processed_upload_counts(
                payload,
                args.repository,
                args.codecov_processed_run_id,
                args.required_flag,
            )
        except ValueError as error:
            raise SystemExit(str(error)) from None
        print(json.dumps(summary, sort_keys=True))
        return 0 if (
            summary["complete_page"]
            and all(count >= 1 for count in summary["counts"].values())
        ) else 1

    if not args.sha or not args.required_axis:
        raise SystemExit("receipt mode requires --sha and --required-axis")
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
