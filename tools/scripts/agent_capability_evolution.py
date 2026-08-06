#!/usr/bin/env python3
"""Evolution rules for installed capability and public-surface snapshots."""
from __future__ import annotations

import copy
import json
from typing import Any


def contract_payload(row: dict[str, Any]) -> dict[str, Any]:
    return {
        key: copy.deepcopy(value)
        for key, value in row.items()
        if key not in {"contract_digest", "contract_version", "summary"}
    }


def manifest_evolution_problems(
    previous: Any, current: dict[str, Any], *, allow_unpublished_migration: bool
) -> list[str]:
    if not isinstance(previous, dict):
        return []
    old_rows = previous.get("capabilities")
    versioned = isinstance(old_rows, list) and all(
        isinstance(row, dict)
        and "contract_version" in row
        and "contract_digest" in row
        for row in old_rows
    )
    if not versioned:
        if allow_unpublished_migration:
            return []
        return [
            "existing snapshot predates contract versions; the unpublished-v1 "
            "migration flag is required exactly once"
        ]
    if allow_unpublished_migration:
        return ["unpublished-v1 migration is forbidden after versions exist"]

    problems: list[str] = []
    old_by_key = {row["key"]: row for row in old_rows}
    new_by_key = {row["key"]: row for row in current["capabilities"]}
    new_tombstones = {row["key"]: row for row in current["tombstones"]}
    old_tombstones = {row["key"]: row for row in previous.get("tombstones", [])}

    for key in sorted(set(old_by_key) & set(new_by_key)):
        old = old_by_key[key]
        new = new_by_key[key]
        old_version = _version_tuple(old["contract_version"])
        new_version = _version_tuple(new["contract_version"])
        changed = contract_payload(old) != contract_payload(new)
        if changed and new_version <= old_version:
            problems.append(f"{key} changed without a contract_version increase")
        if not changed and new_version != old_version:
            problems.append(f"{key} contract_version changed without a contract change")
        old_bindings = {_binding_identity(item) for item in old["bindings"]}
        new_bindings = {_binding_identity(item) for item in new["bindings"]}
        breaking = not old_bindings.issubset(new_bindings)
        old_nonbinding = contract_payload(old)
        new_nonbinding = contract_payload(new)
        old_nonbinding.pop("bindings", None)
        new_nonbinding.pop("bindings", None)
        breaking = breaking or old_nonbinding != new_nonbinding
        if breaking and new_version[0] <= old_version[0]:
            problems.append(f"{key} has a breaking change without a major increase")

    for key in sorted(set(new_by_key) - set(old_by_key)):
        if _version_tuple(new_by_key[key]["contract_version"]) != (1, 0):
            problems.append(f"new capability must start at contract version 1.0: {key}")

    for key in sorted(set(old_by_key) - set(new_by_key)):
        old = old_by_key[key]
        tombstone = new_tombstones.get(key)
        if tombstone is None:
            problems.append(f"removed capability lacks a tombstone: {key}")
            continue
        if tombstone.get("last_contract_version") != old.get("contract_version"):
            problems.append(f"{key} tombstone has the wrong last_contract_version")
        if tombstone.get("last_contract_digest") != old.get("contract_digest"):
            problems.append(f"{key} tombstone has the wrong last_contract_digest")
        old_evolution = old.get("evolution", {})
        if tombstone.get("introduced_in") != old_evolution.get("introduced_in"):
            problems.append(f"{key} tombstone has the wrong introduced_in version")
        if tombstone.get("deprecated_in") != old_evolution.get("deprecated_in"):
            problems.append(f"{key} tombstone has the wrong deprecated_in version")
        if tombstone.get("removed_in_manifest_revision") != current.get(
            "manifest_revision"
        ):
            problems.append(
                f"{key} tombstone must name the current manifest revision"
            )
        if old.get("status") != "deprecated" or old.get("evolution", {}).get(
            "state"
        ) != "deprecated":
            problems.append(
                f"{key} may be removed only after a published deprecated revision"
            )

    removed_keys = set(old_by_key) - set(new_by_key)
    for key in sorted(set(new_tombstones) - set(old_tombstones)):
        if key not in removed_keys:
            problems.append(f"capability tombstone has no removed prior key: {key}")

    for key, old in old_tombstones.items():
        if new_tombstones.get(key) != old:
            problems.append(f"capability tombstone may not be removed or rewritten: {key}")
    for key in sorted(set(new_by_key) & set(old_tombstones)):
        problems.append(f"tombstoned capability key may not be reused: {key}")

    old_material = copy.deepcopy(previous)
    new_material = copy.deepcopy(current)
    old_revision = old_material.pop("manifest_revision", None)
    new_revision = new_material.pop("manifest_revision", None)
    if old_material != new_material and (
        not isinstance(old_revision, int)
        or not isinstance(new_revision, int)
        or new_revision <= old_revision
    ):
        problems.append("manifest changed without a manifest_revision increase")
    if old_material == new_material and new_revision != old_revision:
        problems.append("manifest_revision changed without a manifest change")
    return problems


def surface_evolution_problems(
    previous: Any, current: dict[str, Any], surface_schema: str
) -> list[str]:
    if not isinstance(previous, dict) or previous.get("schema") != surface_schema:
        return []
    problems: list[str] = []
    old_version = previous.get("inventory_version")
    new_version = current.get("inventory_version")
    old_material = copy.deepcopy(previous)
    new_material = copy.deepcopy(current)
    old_material.pop("inventory_version", None)
    new_material.pop("inventory_version", None)
    if old_material != new_material and (
        not isinstance(old_version, int)
        or not isinstance(new_version, int)
        or new_version <= old_version
    ):
        problems.append("public surface changed without an inventory_version increase")
    if old_material == new_material and old_version != new_version:
        problems.append("surface inventory_version changed without a surface change")

    old_headers = {row["include"]: row for row in previous.get("headers", [])}
    new_headers = {row["include"]: row for row in current.get("headers", [])}
    for include in sorted(set(old_headers) & set(new_headers)):
        if (
            old_headers[include].get("disposition") != "legacy_unreviewed"
            and new_headers[include].get("disposition") == "legacy_unreviewed"
        ):
            problems.append(
                f"reviewed header may not return to legacy_unreviewed: {include}"
            )
    old_tombstones = {
        row["include"]: row for row in previous.get("tombstones", [])
    }
    new_tombstones = {row["include"]: row for row in current.get("tombstones", [])}

    removed_headers = set(old_headers) - set(new_headers)
    for include in sorted(removed_headers):
        tombstone = new_tombstones.get(include)
        if tombstone is None:
            problems.append(f"removed reviewed header lacks a tombstone: {include}")
            continue
        if tombstone.get("last_fingerprint") != old_headers[include].get(
            "fingerprint"
        ):
            problems.append(
                f"surface tombstone fingerprint does not match prior ledger: {include}"
            )
        if tombstone.get("removed_in_inventory_version") != new_version:
            problems.append(
                f"surface tombstone must name the current inventory version: {include}"
            )

    for include in sorted(set(new_tombstones) - set(old_tombstones)):
        if include not in removed_headers:
            problems.append(
                f"surface tombstone has no removed prior header: {include}"
            )
    for include, old in old_tombstones.items():
        if new_tombstones.get(include) != old:
            problems.append(f"surface tombstone may not be removed or rewritten: {include}")
    return problems


def _version_tuple(value: dict[str, Any]) -> tuple[int, int]:
    return value["major"], value["minor"]


def _binding_identity(item: dict[str, Any]) -> tuple[Any, ...]:
    return (
        item.get("role"),
        item.get("kind"),
        item.get("include"),
        item.get("qualified_name"),
        item.get("target"),
        json.dumps(item.get("availability"), sort_keys=True),
    )
