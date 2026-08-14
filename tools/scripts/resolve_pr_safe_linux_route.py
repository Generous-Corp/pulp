#!/usr/bin/env python3
"""Select the main-owned PR-safe Linux workflow while its lease is valid."""

from __future__ import annotations

import argparse
import json
import sys
from datetime import datetime, timezone
from typing import Optional


MAX_LEASE_HORIZON_SECONDS = 15 * 60

PR_SAFE_SELECTOR = [
    "self-hosted",
    "Linux",
    "X64",
    "pulp-build-linux-x64",
    "pulp-host-macpro",
    "pulp-pr-safe-linux-x64",
]


def _parse_selector(raw: str) -> list[str]:
    try:
        value = json.loads(raw)
    except json.JSONDecodeError:
        return []
    if not isinstance(value, list) or not all(isinstance(item, str) for item in value):
        return []
    return value


def _lease_is_live(raw: str, now: datetime) -> bool:
    try:
        expiry = datetime.fromisoformat(raw.replace("Z", "+00:00"))
    except (TypeError, ValueError):
        return False
    if expiry.tzinfo is None:
        return False
    remaining = expiry.astimezone(timezone.utc) - now.astimezone(timezone.utc)
    return 0 < remaining.total_seconds() <= MAX_LEASE_HORIZON_SECONDS


def resolve(
    *,
    event_name: str,
    repository: str,
    pr_head_repository: str,
    selector_json: str,
    lease_until: str,
    enabled: str,
    now: Optional[datetime] = None,
) -> dict[str, object]:
    now = now or datetime.now(timezone.utc)
    reason = "hosted-default"
    use_reusable = False
    if event_name != "pull_request":
        reason = "event-not-pull-request"
    elif not repository or pr_head_repository != repository:
        reason = "fork-or-missing-head-repository"
    elif enabled.strip().lower() != "true":
        reason = "not-enabled"
    elif _parse_selector(selector_json) != PR_SAFE_SELECTOR:
        reason = "selector-invalid"
    elif not _lease_is_live(lease_until.strip(), now):
        reason = "lease-missing-or-expired"
    else:
        reason = "pr-safe-local"
        use_reusable = True
    return {
        "use_reusable": use_reusable,
        "reason": reason,
        "selector_json": json.dumps(PR_SAFE_SELECTOR, separators=(",", ":"))
        if use_reusable
        else "",
    }


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--event-name", required=True)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--pr-head-repository", default="")
    parser.add_argument("--selector-json", default="")
    parser.add_argument("--lease-until", default="")
    parser.add_argument("--enabled", default="false")
    args = parser.parse_args(argv)
    print(
        json.dumps(
            resolve(
                event_name=args.event_name,
                repository=args.repository,
                pr_head_repository=args.pr_head_repository,
                selector_json=args.selector_json,
                lease_until=args.lease_until,
                enabled=args.enabled,
            ),
            separators=(",", ":"),
        )
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
