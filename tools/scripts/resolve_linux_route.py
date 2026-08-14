#!/usr/bin/env python3
"""Describe and enforce the Build-and-Test Linux runner route.

The build workflow deliberately keeps the configured Mac Pro selector separate
from the selector authorized for the current event. The organization runner
group restricts trusted workflow refs to protected default-branch copies, so
only protected merge-group orchestration may use this selector. A separate
main-owned reusable workflow owns the PR-safe lane. Shipyard maintains a short
operator health lease for each pool; an absent or expired lease restores hosted
fallback before job dispatch.

This helper turns that policy decision into machine-readable metadata and
fails a dispatch if its configured selector is silently replaced by a hosted
fallback. It prints one compact JSON object on stdout.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import re
import sys
from typing import Any, Optional


ROUTE_REASONS = (
    "explicit-dispatch",
    "local-enabled",
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
MAC_PRO_LINUX_LABELS = (
    "self-hosted",
    "linux",
    "x64",
    "pulp-build-linux-x64",
    "pulp-host-macpro",
    "pulp-auto-linux-x64",
)
MAX_LEASE_HORIZON = dt.timedelta(minutes=15)


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


def is_macpro_linux_selector(selector: Any) -> bool:
    """Return whether *selector* is the reviewed unprivileged Mac Pro lane."""
    labels = _normalized_labels(selector)
    return (
        len(labels) == len(MAC_PRO_LINUX_LABELS)
        and set(labels) == set(MAC_PRO_LINUX_LABELS)
    )


def operator_lease_active(raw: str, now: dt.datetime | None = None) -> bool:
    """Accept a short, unexpired RFC 3339 lease issued by runner operations."""
    raw = raw.strip()
    if not raw:
        return False
    try:
        expires = dt.datetime.fromisoformat(raw.replace("Z", "+00:00"))
        if expires.tzinfo is None:
            return False
        current = now or dt.datetime.now(dt.timezone.utc)
        current = current.astimezone(dt.timezone.utc)
        expires = expires.astimezone(dt.timezone.utc)
    except (ValueError, OverflowError):
        return False
    return current < expires <= current + MAX_LEASE_HORIZON


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
    local_enabled: bool = False,
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

    automatic_local = event_name == "merge_group"
    if event_name == "workflow_dispatch" and dispatch is not None:
        if is_macpro_linux_selector(dispatch):
            if authorized is not None or resolved != "ubuntu-latest":
                raise ValueError(
                    "workflow_dispatch cannot directly target the protected "
                    "Mac Pro selector and must resolve exactly to ubuntu-latest"
                )
            reason = "security-hosted"
        else:
            reason = "explicit-dispatch"
            if authorized != dispatch:
                raise ValueError(
                    "workflow_dispatch authorized selector does not match its "
                    "dispatch override"
                )
    elif event_name == "workflow_dispatch":
        if authorized is not None:
            raise ValueError(
                "workflow_dispatch unexpectedly authorized a configured selector"
            )
        reason = "security-hosted" if configured is not None else "unconfigured-hosted"
    elif automatic_local and authorized is not None:
        if configured is None or authorized != configured:
            raise ValueError(
                "automatic Linux routing must use the configured selector"
            )
        if not is_macpro_linux_selector(authorized):
            raise ValueError(
                "automatic Linux routing only authorizes the reviewed Mac Pro "
                "unprivileged selector"
            )
        if not local_enabled:
            raise ValueError(
                "automatic Linux routing authorized a local selector without "
                "the operator health lease"
            )
        reason = "local-enabled"
    elif authorized is not None:
        raise ValueError(
            f"{event_name} unexpectedly authorized a Linux selector"
        )
    elif automatic_local:
        if resolved != "ubuntu-latest":
            raise ValueError(
                "automatic Linux routing without an active local lease must "
                "resolve exactly to ubuntu-latest"
            )
        reason = (
            "security-hosted"
            if configured is not None
            else "unconfigured-hosted"
        )
    elif configured is not None:
        reason = "security-hosted"
    else:
        reason = "unconfigured-hosted"

    configured_local = configured is not None and reason == "local-enabled"
    return {
        "linux_route_reason": reason,
        "linux_provider": provider_for_selector(
            resolved,
            requested_provider=(
                "github-hosted"
                if automatic_local and reason != "local-enabled"
                else requested_provider
            ),
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
    parser.add_argument(
        "--local-enabled",
        action="store_true",
        help="pass the workflow's single validated lease snapshot",
    )
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
            local_enabled=args.local_enabled,
        )
    except ValueError as exc:
        print(f"Linux route policy error: {exc}", file=sys.stderr)
        return 1
    print(json.dumps(metadata, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
