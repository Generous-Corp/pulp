#!/usr/bin/env python3
"""Select an eligible local runner only when live capacity is proven.

The selector remains a repository variable so the profile is reviewable.  This
helper adds the missing runtime check: a self-hosted selector is used only for
an eligible same-repository event when an online, idle runner currently carries
every requested label.  Otherwise the job receives its hosted fallback before
GitHub assigns it, avoiding an unsatisfiable queue.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from typing import Any


def parse_selector(raw: str) -> Any:
    raw = raw.strip()
    if not raw:
        return None
    value = json.loads(raw)
    if not isinstance(value, (str, list)):
        raise ValueError("selector must be a JSON string or array")
    return value


def labels(selector: Any) -> set[str]:
    values = selector if isinstance(selector, list) else [selector]
    return {str(value).strip() for value in values}


def local_healthy(repo: str, selector: Any) -> bool:
    wanted = labels(selector)
    if "self-hosted" not in {value.lower() for value in wanted}:
        return False
    result = subprocess.run(
        ["gh", "api", f"repos/{repo}/actions/runners?per_page=100"],
        check=True,
        capture_output=True,
        text=True,
        timeout=15,
    )
    payload = json.loads(result.stdout)
    return any(
        runner.get("status") == "online"
        and runner.get("busy") is False
        and wanted.issubset(
            {str(label.get("name", "")).strip() for label in runner.get("labels", [])}
        )
        for runner in payload.get("runners", [])
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", required=True)
    parser.add_argument("--configured-json", default="")
    parser.add_argument("--fallback-json", required=True)
    parser.add_argument("--allow-local", action="store_true")
    args = parser.parse_args()

    try:
        configured = parse_selector(args.configured_json)
        fallback = parse_selector(args.fallback_json)
        if configured is not None and args.allow_local and local_healthy(args.repo, configured):
            selected = configured
            reason = "local-healthy"
        else:
            selected = fallback
            reason = "hosted-fallback"
    except (OSError, subprocess.SubprocessError, ValueError, json.JSONDecodeError) as exc:
        selected = parse_selector(args.fallback_json)
        reason = f"hosted-fallback:{exc}"

    compact = json.dumps(selected, separators=(",", ":"))
    print(f"selector_json={compact}")
    print(f"route_reason={reason}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
