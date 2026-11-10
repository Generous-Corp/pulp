"""Construction helpers for declarative agent capability records."""
from __future__ import annotations

from typing import Any


def availability() -> dict[str, Any]:
    return {
        "state": "available",
        "platforms": ["all"],
        "required_features": [],
    }


def binding(
    *,
    role: str,
    kind: str,
    include: str,
    qualified_name: str,
    target: str,
    address_expression: str | None = None,
    header_fingerprint: str | None = None,
) -> dict[str, Any]:
    """One typed binding. The header's reviewed fingerprint is not passed here:
    it lives once per header in tools/agent-capabilities/fingerprints/, so two
    changes on different headers never rewrite the same catalog lines. A
    `header_fingerprint=` argument is kept only so a catalog row written for the
    old layout still imports; `agent_capability_manifest.py --check` refuses it
    and `--migrate-fingerprints` moves it into its fragment."""
    result = {
        "role": role,
        "kind": kind,
        "include": include,
        "qualified_name": qualified_name,
        "target": target,
        "availability": availability(),
    }
    if header_fingerprint is not None:
        result["_legacy_header_fingerprint"] = header_fingerprint
    if address_expression is not None:
        result["_address_expression"] = address_expression
    return result


def capability(**row: Any) -> dict[str, Any]:
    row.setdefault("contract_version", {"major": 1, "minor": 0})
    row.setdefault("status", "usable")
    row.setdefault(
        "evolution", {"state": "active", "introduced_in": {"major": 1, "minor": 0}}
    )
    return row
