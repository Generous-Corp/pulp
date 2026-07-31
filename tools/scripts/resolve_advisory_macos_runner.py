#!/usr/bin/env python3
"""Resolve an advisory macOS runner without touching merge-gate capacity."""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path


DENIED_LABEL_PREFIXES = ("pulp-build", "pulp-preamble")
ADVISORY_LABEL_PREFIX = "pulp-advisory-"
KNOWN_HOSTED_SELECTORS = {
    "macos-14",
    "macos-15",
    "macos-26",
    "macos-latest",
}


def resolve_selector(
    configured_json: str,
    fallback_json: str,
    *,
    require_self_hosted: bool = False,
    event_name: str = "",
) -> tuple[bool, str]:
    configured = bool(configured_json.strip())
    if configured:
        configured_selector = _parse_selector(
            configured_json, require_self_hosted=require_self_hosted, configured=True
        )
        configured_is_hosted = (
            isinstance(configured_selector, str)
            and configured_selector.casefold() in KNOWN_HOSTED_SELECTORS
        )
        # Pull-request workflow YAML is contributor-controlled, so a condition
        # in that YAML is not an access-control boundary. Keep private
        # selectors dispatch-only until an organization runner group restricts
        # the pool to selected trusted workflow refs.
        if event_name and event_name != "workflow_dispatch" and not configured_is_hosted:
            print(
                "advisory-runner: ignoring configured private selector for "
                f"{event_name}; private pools are workflow_dispatch-only",
                file=sys.stderr,
            )
            configured = False
        else:
            return True, json.dumps(configured_selector, separators=(",", ":"))

    raw = fallback_json.strip()
    if not raw:
        # Keep runs-on syntactically valid even when the consumer job is
        # skipped by `enabled == false`; GitHub may still parse fromJSON().
        return False, json.dumps("macos-15")

    selector = _parse_selector(
        raw, require_self_hosted=require_self_hosted, configured=False
    )
    return True, json.dumps(selector, separators=(",", ":"))


def _parse_selector(
    raw: str, *, require_self_hosted: bool, configured: bool
) -> str | list[str]:
    try:
        selector = json.loads(raw)
    except json.JSONDecodeError as exc:
        raise ValueError(f"runner selector is not valid JSON: {exc}") from exc

    if isinstance(selector, str):
        labels = [selector]
    elif isinstance(selector, list) and selector and all(
        isinstance(label, str) and label for label in selector
    ):
        labels = selector
    else:
        raise ValueError("runner selector must be a non-empty JSON string or string array")

    lowered = [label.casefold() for label in labels]
    denied = [
        label
        for label in labels
        if any(label.casefold().startswith(prefix) for prefix in DENIED_LABEL_PREFIXES)
    ]
    if denied:
        raise ValueError(
            "advisory jobs may not use required merge-gate labels: "
            + ", ".join(denied)
        )
    if require_self_hosted and "self-hosted" not in lowered:
        raise ValueError("this advisory lane requires a distinct self-hosted selector")
    known_hosted = (
        isinstance(selector, str)
        and selector.casefold() in KNOWN_HOSTED_SELECTORS
    )
    if configured and not known_hosted and not any(
        label.startswith(ADVISORY_LABEL_PREFIX) for label in lowered
    ):
        raise ValueError(
            f"configured non-hosted advisory selectors require a "
            f"{ADVISORY_LABEL_PREFIX}* identity label"
        )

    return selector


def write_outputs(path: Path, enabled: bool, selector_json: str) -> None:
    with path.open("a", encoding="utf-8") as handle:
        handle.write(f"enabled={'true' if enabled else 'false'}\n")
        handle.write(f"selector_json={selector_json}\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--configured-json", default="")
    parser.add_argument("--fallback-json", default="")
    parser.add_argument("--require-self-hosted", action="store_true")
    parser.add_argument("--event-name", default="")
    args = parser.parse_args()

    try:
        enabled, selector_json = resolve_selector(
            args.configured_json,
            args.fallback_json,
            require_self_hosted=args.require_self_hosted,
            event_name=args.event_name,
        )
        output_path = os.environ.get("GITHUB_OUTPUT")
        if output_path:
            write_outputs(Path(output_path), enabled, selector_json)
        else:
            print(
                json.dumps(
                    {"enabled": enabled, "selector_json": selector_json},
                    separators=(",", ":"),
                )
            )
        return 0
    except (OSError, ValueError) as exc:
        print(f"resolve-advisory-macos-runner: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
