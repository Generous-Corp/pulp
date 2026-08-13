#!/usr/bin/env python3
"""Describe and enforce the Build-and-Test Linux runner route.

The build workflow deliberately keeps the configured Mac Pro selector separate
from the selector authorized for the current event. The organization runner
group restricts trusted workflow refs to protected default-branch copies, so
same-repository pull requests and merge groups may use the disposable Mac Pro
pool. Fork pull requests remain hosted.

This helper turns that policy decision into machine-readable metadata and
fails a dispatch if its configured selector is silently replaced by a hosted
fallback. It prints one compact JSON object on stdout.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from typing import Any, Optional


ROUTE_REASONS = (
    "explicit-dispatch",
    "trusted-local",
    "security-hosted",
    "unconfigured-hosted",
)
BARE_LABEL_RE = re.compile(r"^[A-Za-z0-9_.:-]+$")
GITHUB_HOSTED_LABELS = frozenset(
    {
        "ubuntu-latest",
        "ubuntu-slim",
        "ubuntu-22.04",
        "ubuntu-22.04-arm",
        "ubuntu-24.04",
        "ubuntu-24.04-arm",
        "windows-latest",
        "windows-2022",
        "windows-2025",
        "macos-latest",
        "macos-13",
        "macos-13-large",
        "macos-14",
        "macos-14-large",
        "macos-14-xlarge",
        "macos-15",
        "macos-15-large",
        "macos-15-xlarge",
        "macos-26",
        "macos-26-large",
        "macos-26-xlarge",
    }
)


def _selector(raw: str, name: str) -> tuple[Any, str]:
    """Parse a runs-on selector and return its value and compact JSON."""
    raw = raw.strip()
    if not raw:
        return None, ""
    try:
        value = json.loads(raw)
    except json.JSONDecodeError as exc:
        if BARE_LABEL_RE.fullmatch(raw):
            value = raw
        else:
            raise ValueError(f"{name} is not valid JSON: {exc}") from exc
    if not isinstance(value, (str, list)):
        raise ValueError(f"{name} must decode to a string or array")
    return value, json.dumps(value, separators=(",", ":"))


def _normalized_labels(selector: Any) -> list[str]:
    labels = selector if isinstance(selector, list) else [selector]
    return [str(label).strip().lower() for label in labels]


def _is_known_github_hosted(selector: Any) -> bool:
    labels = _normalized_labels(selector)
    return bool(labels) and all(label in GITHUB_HOSTED_LABELS for label in labels)


def provider_for_selector(
    selector: Any,
    *,
    requested_provider: str,
    configured_local: bool = False,
    operator_override: bool = False,
) -> str:
    """Derive a truthful display provider from selector plus route source."""
    normalized = _normalized_labels(selector)
    if "self-hosted" in normalized:
        return "local"
    # Self-hosted runners may suppress GitHub's default labels and register
    # only a custom label. The PULP_LOCAL_* variable is authoritative context
    # for that otherwise ambiguous selector form.
    if configured_local:
        return "local"
    if any(label.startswith("namespace-") for label in normalized):
        return "namespace"
    if not operator_override and requested_provider == "namespace":
        return "namespace"
    if operator_override and not _is_known_github_hosted(selector):
        return "operator"
    return "github-hosted"


def resolve_route(
    *,
    event_name: str,
    dispatch_selector_json: str,
    configured_selector_json: str,
    authorized_selector_json: str,
    resolved_selector_json: str,
    requested_provider: str = "github-hosted",
) -> dict[str, str]:
    """Return Linux route metadata or reject an inconsistent dispatch."""
    dispatch, _ = _selector(
        dispatch_selector_json, "workflow_dispatch Linux selector"
    )
    configured, configured_json = _selector(
        configured_selector_json, "configured Linux selector"
    )
    authorized, authorized_json = _selector(
        authorized_selector_json, "event-authorized Linux selector"
    )
    resolved, resolved_json = _selector(
        resolved_selector_json, "resolved Linux selector"
    )
    if resolved is None:
        raise ValueError("resolved Linux selector must not be empty")

    if event_name == "workflow_dispatch" and (
        dispatch is not None or configured is not None
    ):
        reason = "explicit-dispatch"
        expected = dispatch if dispatch is not None else configured
        if expected is not None and authorized != expected:
            raise ValueError(
                "workflow_dispatch authorized selector does not match its "
                "dispatch override or configured selector"
            )
        if dispatch is None and configured is not None:
            if resolved != configured or _is_known_github_hosted(configured):
                raise ValueError(
                    "workflow_dispatch configured Linux selector unexpectedly "
                    f"resolved to {resolved_json}"
                )
    elif event_name in ("pull_request", "merge_group") and authorized is not None:
        reason = "trusted-local"
    elif authorized is not None:
        raise ValueError(
            f"{event_name} unexpectedly authorized a Linux selector"
        )
    elif configured is not None:
        reason = "security-hosted"
    else:
        reason = "unconfigured-hosted"

    configured_local = (
        event_name in ("pull_request", "merge_group")
        and authorized is not None
    ) or (
        event_name == "workflow_dispatch"
        and dispatch is None
        and configured is not None
    )
    return {
        "linux_route_reason": reason,
        "linux_provider": provider_for_selector(
            resolved,
            requested_provider=requested_provider,
            configured_local=configured_local,
            operator_override=dispatch is not None,
        ),
        "configured_selector_json": configured_json,
        "event_authorized_selector_json": authorized_json,
    }


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--event-name", required=True)
    parser.add_argument("--dispatch-selector-json", default="")
    parser.add_argument("--configured-selector-json", default="")
    parser.add_argument("--authorized-selector-json", default="")
    parser.add_argument("--resolved-selector-json", required=True)
    parser.add_argument("--requested-provider", default="github-hosted")
    return parser


def main(argv: Optional[list[str]] = None) -> int:
    args = _build_parser().parse_args(argv)
    try:
        metadata = resolve_route(
            event_name=args.event_name,
            dispatch_selector_json=args.dispatch_selector_json,
            configured_selector_json=args.configured_selector_json,
            authorized_selector_json=args.authorized_selector_json,
            resolved_selector_json=args.resolved_selector_json,
            requested_provider=args.requested_provider,
        )
    except ValueError as exc:
        print(f"Linux route policy error: {exc}", file=sys.stderr)
        return 1
    print(json.dumps(metadata, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
