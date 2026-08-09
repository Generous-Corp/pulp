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
    header_fingerprint: str,
    address_expression: str | None = None,
) -> dict[str, Any]:
    result = {
        "role": role,
        "kind": kind,
        "include": include,
        "qualified_name": qualified_name,
        "target": target,
        "availability": availability(),
        "_header_fingerprint": header_fingerprint,
    }
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
