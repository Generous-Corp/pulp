#!/usr/bin/env python3
"""Fail closed when a graph-agent status manifest stops proving progress.

The CI queue watchdogs observe GitHub jobs and merge throughput.  They cannot
see an agent lane whose status file remains ``ACTIVE`` after its owner has
disappeared.  This guard consumes a small, immutable status snapshot produced
by the coordinator and turns missing progress evidence into an actionable
``BLOCKED`` result.

The input is deliberately a snapshot rather than a GitHub mutation path.  A
lane may only be considered live when it has a recent heartbeat, a receipt
that exists and matches its recorded digest, and (when ``pr_required`` is
true) complete PR linkage.  ``BLOCKED`` lanes must carry both a blocker and a
next action so the result can be handed to the next owner.  Unknown fields are
ignored for forward compatibility; unknown statuses and malformed required
fields fail closed.

Example manifest shape::

    {
      "schema": "pulp.graph_agent_status.v1",
      "generated_at": "2026-11-08T10:00:00Z",
      "lanes": [{
        "lane": "G-7",
        "status": "PARTIAL",
        "heartbeat_at": "2026-11-08T09:55:00Z",
        "receipt_path": "docs/graph-program-status/G-7.json",
        "receipt_sha256": "<64 hex characters>",
        "pr_required": true,
        "pr": {"number": 11700, "url": "https://github.com/Generous-Corp/pulp/pull/11700",
               "head": "<40 hex characters>", "base": "main"}
      }]
    }

No network or write is performed.  Exit 0 means every lane is proven live or
explicitly complete.  Exit 1 means one or more lanes are ``BLOCKED``.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any
from urllib.parse import urlparse

SCHEMA = "pulp.graph_agent_status.v1"
STATUSES = {"ACTIVE", "PARTIAL", "BLOCKED", "DONE"}
HEARTBEAT_STATUSES = {"ACTIVE", "PARTIAL"}
DEFAULT_MAX_HEARTBEAT_MINUTES = 90.0
DEFAULT_MAX_SNAPSHOT_MINUTES = 180.0
SHA256_RE = re.compile(r"^[0-9a-fA-F]{64}$")
SHA1_RE = re.compile(r"^[0-9a-fA-F]{40}$")
REPO = "Generous-Corp/pulp"


class ManifestError(ValueError):
    """Input is malformed enough that health cannot be inferred."""


def parse_timestamp(value: Any, field: str) -> dt.datetime:
    if not isinstance(value, str) or not value.strip():
        raise ManifestError(f"{field} must be an ISO-8601 timestamp")
    text = value.strip()
    if text.endswith("Z"):
        text = text[:-1] + "+00:00"
    try:
        stamp = dt.datetime.fromisoformat(text)
    except ValueError as exc:
        raise ManifestError(f"{field} is not an ISO-8601 timestamp: {value!r}") from exc
    if stamp.tzinfo is None:
        raise ManifestError(f"{field} must include a timezone")
    return stamp.astimezone(dt.timezone.utc)


def parse_now(value: str | None) -> dt.datetime:
    if value is None:
        return dt.datetime.now(dt.timezone.utc)
    return parse_timestamp(value, "--now")


def age_minutes(now: dt.datetime, stamp: dt.datetime) -> float:
    return (now - stamp).total_seconds() / 60.0


def receipt_path(root: Path, value: Any) -> Path:
    if not isinstance(value, str) or not value:
        raise ManifestError("receipt_path must be a non-empty relative path")
    path = Path(value)
    if path.is_absolute() or ".." in path.parts:
        raise ManifestError("receipt_path must stay below the manifest directory")
    return (root / path).resolve()


def check_pr(lane: dict[str, Any], findings: list[str]) -> None:
    required = lane.get("pr_required", False)
    pr = lane.get("pr")
    if required and not isinstance(pr, dict):
        findings.append("PR linkage is required but missing")
        return
    if pr is None:
        return
    if not isinstance(pr, dict):
        findings.append("PR linkage must be an object")
        return
    number = pr.get("number")
    if not isinstance(number, int) or number <= 0:
        findings.append("PR number must be a positive integer")
    url = pr.get("url")
    parsed = urlparse(url) if isinstance(url, str) else None
    if (
        parsed is None
        or parsed.scheme != "https"
        or parsed.netloc != "github.com"
        or parsed.path != f"/{REPO}/pull/{number}"
    ):
        findings.append(f"PR URL must be the canonical {REPO} pull URL")
    head = pr.get("head")
    if not isinstance(head, str) or not SHA1_RE.fullmatch(head):
        findings.append("PR head must be the complete 40-hex commit SHA")
    base = pr.get("base")
    if base != "main":
        findings.append("PR base must be main")


def check_lane(
    lane: Any,
    *,
    root: Path,
    now: dt.datetime,
    max_heartbeat_minutes: float,
) -> dict[str, Any]:
    if not isinstance(lane, dict):
        return {"lane": "<invalid>", "status": "BLOCKED", "reasons": ["lane must be an object"]}

    name = lane.get("lane")
    lane_name = name if isinstance(name, str) and name.strip() else "<unnamed>"
    status = lane.get("status")
    reasons: list[str] = []
    next_action = lane.get("next_action")
    if not isinstance(name, str) or not name.strip():
        reasons.append("lane name is missing")
    if status not in STATUSES:
        reasons.append(f"status must be one of {sorted(STATUSES)}")

    if status in HEARTBEAT_STATUSES:
        try:
            heartbeat = parse_timestamp(lane.get("heartbeat_at"), f"{lane_name}.heartbeat_at")
            age = age_minutes(now, heartbeat)
            if age < 0:
                reasons.append("heartbeat is in the future")
            elif age > max_heartbeat_minutes:
                reasons.append(
                    f"heartbeat is {age:.1f} minutes old (limit {max_heartbeat_minutes:.1f})"
                )
        except ManifestError as exc:
            reasons.append(str(exc))

    if status == "BLOCKED":
        blocker = lane.get("blocker")
        if not isinstance(blocker, str) or not blocker.strip():
            reasons.append("BLOCKED status requires blocker")
        else:
            reasons.append(f"explicit blocker: {blocker.strip()}")
        if not isinstance(next_action, str) or not next_action.strip():
            reasons.append("BLOCKED status requires next_action")

    if status in HEARTBEAT_STATUSES | {"DONE"} or lane.get("receipt_path") is not None:
        try:
            path = receipt_path(root, lane.get("receipt_path"))
            if not path.is_file():
                reasons.append(f"receipt does not exist: {lane.get('receipt_path')}")
            else:
                expected = lane.get("receipt_sha256")
                if not isinstance(expected, str) or not SHA256_RE.fullmatch(expected):
                    reasons.append("receipt_sha256 must be a complete 64-hex digest")
                else:
                    actual = hashlib.sha256(path.read_bytes()).hexdigest()
                    if actual.lower() != expected.lower():
                        reasons.append("receipt SHA-256 does not match receipt_sha256")
        except ManifestError as exc:
            reasons.append(str(exc))

    if "pr_required" in lane and not isinstance(lane.get("pr_required"), bool):
        reasons.append("pr_required must be boolean")
    check_pr(lane, reasons)
    verdict = "BLOCKED" if reasons else status
    result: dict[str, Any] = {"lane": lane_name, "status": verdict, "reasons": reasons}
    if isinstance(next_action, str) and next_action.strip():
        result["next_action"] = next_action.strip()
    return result


def analyze(
    manifest: dict[str, Any],
    *,
    root: Path,
    now: dt.datetime,
    max_heartbeat_minutes: float = DEFAULT_MAX_HEARTBEAT_MINUTES,
    max_snapshot_minutes: float = DEFAULT_MAX_SNAPSHOT_MINUTES,
) -> dict[str, Any]:
    reasons: list[str] = []
    if manifest.get("schema") != SCHEMA:
        reasons.append(f"schema must be {SCHEMA}")
    try:
        generated = parse_timestamp(manifest.get("generated_at"), "generated_at")
        snapshot_age = age_minutes(now, generated)
        if snapshot_age < 0:
            reasons.append("manifest generated_at is in the future")
        elif snapshot_age > max_snapshot_minutes:
            reasons.append(
                f"manifest is {snapshot_age:.1f} minutes old (limit {max_snapshot_minutes:.1f})"
            )
    except ManifestError as exc:
        reasons.append(str(exc))

    lanes = manifest.get("lanes")
    if not isinstance(lanes, list) or not lanes:
        reasons.append("lanes must be a non-empty array")
        lanes = []
    lane_results = [
        check_lane(
            lane,
            root=root,
            now=now,
            max_heartbeat_minutes=max_heartbeat_minutes,
        )
        for lane in lanes
    ]
    names = [item["lane"] for item in lane_results]
    duplicates = sorted({name for name in names if names.count(name) > 1 and name != "<invalid>"})
    if duplicates:
        reasons.append(f"duplicate lane names: {', '.join(duplicates)}")
    blocked = [item for item in lane_results if item["status"] == "BLOCKED"]
    result = {
        "schema": SCHEMA,
        "generated_at": manifest.get("generated_at"),
        "verdict": "BLOCKED" if reasons or blocked else "PASS",
        "reasons": reasons,
        "lanes": lane_results,
    }
    return result


def load_manifest(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ManifestError(f"cannot read manifest {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise ManifestError("manifest root must be an object")
    return value


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--now", help="UTC ISO-8601 time (for replayable checks)")
    parser.add_argument("--max-heartbeat-minutes", type=float, default=DEFAULT_MAX_HEARTBEAT_MINUTES)
    parser.add_argument("--max-snapshot-minutes", type=float, default=DEFAULT_MAX_SNAPSHOT_MINUTES)
    parser.add_argument("--json-out", type=Path, help="write the verdict JSON")
    args = parser.parse_args()
    if args.max_heartbeat_minutes <= 0 or args.max_snapshot_minutes <= 0:
        parser.error("age limits must be positive")
    return args


def main() -> int:
    args = parse_args()
    try:
        manifest = load_manifest(args.manifest)
        result = analyze(
            manifest,
            root=args.manifest.parent,
            now=parse_now(args.now),
            max_heartbeat_minutes=args.max_heartbeat_minutes,
            max_snapshot_minutes=args.max_snapshot_minutes,
        )
    except ManifestError as exc:
        result = {
            "schema": SCHEMA,
            "verdict": "BLOCKED",
            "reasons": [str(exc)],
            "lanes": [],
        }
    text = json.dumps(result, indent=2, sort_keys=True)
    if args.json_out:
        args.json_out.write_text(text + "\n", encoding="utf-8")
    print(text)
    for lane in result.get("lanes", []):
        if lane.get("status") == "BLOCKED":
            action = lane.get("next_action", "assign an owner and record the next action")
            reasons = "; ".join(lane.get("reasons", [])) or "lane is blocked"
            print(f"BLOCKED {lane.get('lane')}: {reasons}. Next action: {action}", file=sys.stderr)
    return 0 if result.get("verdict") == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
