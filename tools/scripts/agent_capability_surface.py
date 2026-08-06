#!/usr/bin/env python3
"""Inventory and validate the public headers covered by agent capabilities.

The inventory is deliberately conservative: a byte change to a public header
is a surface change. A future pinned-Clang AST pass can make the fingerprint
more semantic without weakening the current fail-closed behavior.
"""
from __future__ import annotations

import hashlib
import json
import pathlib
from typing import Any, Iterable


SURFACE_SCHEMA = "pulp.agent-capability-surface.v1"
BASELINE_SCHEMA = "pulp.agent-capability-legacy-baseline.v1"
SURFACE_SNAPSHOT = pathlib.Path("docs/status/agent-capability-surface.json")
LEGACY_BASELINE = pathlib.Path(
    "tools/agent-capabilities/legacy-unreviewed-baseline.json"
)
SURFACE_SCHEMA_FILE = pathlib.Path(
    "docs/status/agent-capability-surface.schema.json"
)
FROZEN_LEGACY_COUNT = 339
FROZEN_LEGACY_DIGEST = (
    "sha256:880d2694d8dab7fdb041297ea4d7fe2c70c7b8bd53ca6bf7b47cd9954bbdd8c7"
)

PUBLIC_ROOTS = (
    {
        "domain": "audio",
        "source": "core/audio/include/pulp/audio",
        "install_prefix": "pulp/audio",
    },
    {
        "domain": "midi",
        "source": "core/midi/include/pulp/midi",
        "install_prefix": "pulp/midi",
    },
    {
        "domain": "sequence",
        "source": "core/sequence/include/pulp/sequence",
        "install_prefix": "pulp/sequence",
    },
    {
        "domain": "signal",
        "source": "core/signal/include/pulp/signal",
        "install_prefix": "pulp/signal",
    },
    {
        "domain": "timebase",
        "source": "core/timebase/include/pulp/timebase",
        "install_prefix": "pulp/timebase",
    },
)

DISPOSITIONS = {
    "capability_entrypoint",
    "capability_support",
    "infrastructure",
    "legacy_unreviewed",
    "unsupported_capability",
}
REVIEWED_DISPOSITIONS = {
    "capability_support",
    "infrastructure",
    "unsupported_capability",
}


def canonical_digest(value: Any) -> str:
    payload = json.dumps(
        value, sort_keys=True, separators=(",", ":"), ensure_ascii=True
    ).encode("utf-8")
    return "sha256:" + hashlib.sha256(payload).hexdigest()


def file_fingerprint(path: pathlib.Path) -> str:
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def discover_headers(root: pathlib.Path) -> dict[str, dict[str, str]]:
    headers: dict[str, dict[str, str]] = {}
    for public_root in PUBLIC_ROOTS:
        directory = root / public_root["source"]
        if not directory.is_dir():
            continue
        include_root = root / "core" / public_root["domain"] / "include"
        for path in sorted(directory.rglob("*")):
            if not path.is_file():
                continue
            include = path.relative_to(include_root).as_posix()
            headers[include] = {
                "domain": public_root["domain"],
                "source": path.relative_to(root).as_posix(),
                "fingerprint": file_fingerprint(path),
            }
    return headers


def baseline_document(
    root: pathlib.Path, reviewed_includes: Iterable[str]
) -> dict[str, Any]:
    reviewed = set(reviewed_includes)
    entries = [
        {"include": include, "fingerprint": item["fingerprint"]}
        for include, item in sorted(discover_headers(root).items())
        if include not in reviewed
    ]
    return {
        "schema": BASELINE_SCHEMA,
        "frozen_count": len(entries),
        "entries_digest": canonical_digest(entries),
        "entries": entries,
    }


def validate_baseline(
    document: Any, expected_digest: str = FROZEN_LEGACY_DIGEST
) -> list[str]:
    problems: list[str] = []
    if not isinstance(document, dict):
        return ["legacy baseline must be an object"]
    expected_fields = {"schema", "frozen_count", "entries_digest", "entries"}
    if set(document) != expected_fields:
        problems.append("legacy baseline fields are not exact")
    if document.get("schema") != BASELINE_SCHEMA:
        problems.append(f"legacy baseline schema must be {BASELINE_SCHEMA}")
    entries = document.get("entries")
    if not isinstance(entries, list):
        return problems + ["legacy baseline entries must be an array"]
    seen: set[str] = set()
    for index, entry in enumerate(entries):
        where = f"legacy baseline entries[{index}]"
        if not isinstance(entry, dict) or set(entry) != {"include", "fingerprint"}:
            problems.append(f"{where} must contain exactly include and fingerprint")
            continue
        include = entry.get("include")
        fingerprint = entry.get("fingerprint")
        if not isinstance(include, str) or not include.startswith("pulp/"):
            problems.append(f"{where}.include is invalid")
        elif include in seen:
            problems.append(f"duplicate legacy baseline include: {include}")
        else:
            seen.add(include)
        if not _valid_digest(fingerprint):
            problems.append(f"{where}.fingerprint is invalid")
    actual_digest = canonical_digest(entries)
    if document.get("entries_digest") != actual_digest:
        problems.append("legacy baseline entries_digest does not match its entries")
    if actual_digest != expected_digest:
        problems.append(
            "legacy baseline digest changed; the frozen legacy_unreviewed set may "
            "only shrink through explicit reviewed classifications"
        )
    if document.get("frozen_count") != len(entries):
        problems.append("legacy baseline frozen_count does not match its entries")
    if expected_digest == FROZEN_LEGACY_DIGEST and len(entries) != FROZEN_LEGACY_COUNT:
        problems.append(
            f"legacy baseline must retain exactly {FROZEN_LEGACY_COUNT} frozen entries"
        )
    return problems


def load_baseline(root: pathlib.Path) -> dict[str, Any]:
    path = root / LEGACY_BASELINE
    if not path.is_file():
        raise RuntimeError(f"missing frozen legacy baseline: {path}")
    return json.loads(path.read_text())


def build_surface_document(
    root: pathlib.Path,
    *,
    binding_claims: list[dict[str, Any]],
    reviewed_headers: list[dict[str, Any]],
    tombstones: list[dict[str, Any]],
    baseline: dict[str, Any],
    known_capability_keys: Iterable[str] = (),
    expected_baseline_digest: str = FROZEN_LEGACY_DIGEST,
    inventory_version: int = 1,
) -> tuple[dict[str, Any], list[str]]:
    problems = validate_baseline(baseline, expected_baseline_digest)
    baseline_entries = {
        entry["include"]: entry["fingerprint"]
        for entry in baseline.get("entries", [])
        if isinstance(entry, dict)
        and isinstance(entry.get("include"), str)
        and isinstance(entry.get("fingerprint"), str)
    }
    current = discover_headers(root)
    known_keys = set(known_capability_keys)
    claims, claim_problems = _normalize_binding_claims(binding_claims, known_keys)
    reviewed, reviewed_problems = _normalize_reviewed_headers(
        reviewed_headers, known_keys
    )
    removed, tombstone_problems = _normalize_tombstones(tombstones)
    problems.extend(claim_problems)
    problems.extend(reviewed_problems)
    problems.extend(tombstone_problems)
    if isinstance(inventory_version, bool) or not isinstance(
        inventory_version, int
    ) or inventory_version < 1:
        problems.append("surface inventory_version must be a positive integer")
    for include, tombstone in removed.items():
        if tombstone["removed_in_inventory_version"] != inventory_version:
            problems.append(
                f"surface tombstone must name the current inventory version: {include}"
            )

    overlap = sorted(set(claims) & set(reviewed))
    if overlap:
        problems.append(
            "headers cannot be both capability entrypoints and separately reviewed: "
            + ", ".join(overlap)
        )
    overlap = sorted((set(claims) | set(reviewed)) & set(removed))
    if overlap:
        problems.append(
            "surface tombstones overlap live reviewed headers: " + ", ".join(overlap)
        )

    records: list[dict[str, Any]] = []
    for include, discovered in sorted(current.items()):
        if include in removed:
            problems.append(f"surface tombstone still exists: {include}")
            continue
        if include in claims:
            claim = claims[include]
            _check_fingerprint(include, discovered["fingerprint"], claim["fingerprint"], problems)
            record = {
                **discovered,
                "include": include,
                "disposition": "capability_entrypoint",
                "capability_keys": claim["capability_keys"],
                "rationale": "Public binding for a curated consumer capability.",
            }
        elif include in reviewed:
            review = reviewed[include]
            _check_fingerprint(include, discovered["fingerprint"], review["fingerprint"], problems)
            record = {
                **discovered,
                "include": include,
                "disposition": review["disposition"],
                "capability_keys": review["capability_keys"],
                "rationale": review["rationale"],
            }
        elif include in baseline_entries:
            _check_fingerprint(
                include, discovered["fingerprint"], baseline_entries[include], problems
            )
            record = {
                **discovered,
                "include": include,
                "disposition": "legacy_unreviewed",
                "capability_keys": [],
                "rationale": (
                    "Frozen public surface with no reviewed machine-readable "
                    "capability claim."
                ),
            }
        else:
            problems.append(
                f"unclassified public header: {include}; register a capability or "
                "add an explicit reviewed disposition"
            )
            continue
        records.append(record)

    for include in sorted(set(claims) | set(reviewed)):
        if include not in current:
            problems.append(f"reviewed public header is missing: {include}")
    for include, fingerprint in sorted(baseline_entries.items()):
        if include not in current and include not in removed:
            problems.append(
                f"frozen legacy public header was removed without a surface tombstone: {include}"
            )
        if include in removed and removed[include]["last_fingerprint"] != fingerprint:
            problems.append(f"surface tombstone fingerprint does not match baseline: {include}")

    counts = {name: 0 for name in sorted(DISPOSITIONS)}
    by_domain: dict[str, dict[str, int]] = {}
    for public_root in PUBLIC_ROOTS:
        by_domain[public_root["domain"]] = {
            "public_headers": 0,
            "reviewed_headers": 0,
            "legacy_unreviewed_headers": 0,
            "unsupported_headers": 0,
        }
    for record in records:
        counts[record["disposition"]] += 1
        domain_counts = by_domain[record["domain"]]
        domain_counts["public_headers"] += 1
        if record["disposition"] == "legacy_unreviewed":
            domain_counts["legacy_unreviewed_headers"] += 1
        else:
            domain_counts["reviewed_headers"] += 1
        if record["disposition"] == "unsupported_capability":
            domain_counts["unsupported_headers"] += 1

    document = {
        "$schema": "agent-capability-surface.schema.json",
        "schema": SURFACE_SCHEMA,
        "inventory_version": inventory_version,
        "fingerprint_algorithm": "sha256-file-bytes",
        "coverage_state": "partial",
        "roots": [dict(item) for item in PUBLIC_ROOTS],
        "counts": {
            "public_headers": len(records),
            "reviewed_headers": len(records) - counts["legacy_unreviewed"],
            "legacy_unreviewed_headers": counts["legacy_unreviewed"],
            "unsupported_headers": counts["unsupported_capability"],
            "by_disposition": counts,
            "by_domain": by_domain,
        },
        "headers": records,
        "tombstones": sorted(removed.values(), key=lambda item: item["include"]),
    }
    return document, problems


def rendered(document: dict[str, Any]) -> str:
    return json.dumps(document, indent=2, ensure_ascii=False) + "\n"


def _normalize_binding_claims(
    claims: list[dict[str, Any]], known_capability_keys: set[str]
) -> tuple[dict[str, dict[str, Any]], list[str]]:
    normalized: dict[str, dict[str, Any]] = {}
    problems: list[str] = []
    for claim in claims:
        include = claim.get("include")
        fingerprint = claim.get("fingerprint")
        key = claim.get("capability_key")
        if not isinstance(include, str) or not include.startswith("pulp/"):
            problems.append("capability binding has an invalid include")
            continue
        if not _valid_digest(fingerprint):
            problems.append(f"capability binding has an invalid header fingerprint: {include}")
            continue
        if not isinstance(key, str) or not key:
            problems.append(f"capability binding has an invalid capability key: {include}")
            continue
        if key not in known_capability_keys:
            problems.append(f"capability binding references an unknown key: {key}")
            continue
        existing = normalized.setdefault(
            include, {"fingerprint": fingerprint, "capability_keys": []}
        )
        if existing["fingerprint"] != fingerprint:
            problems.append(f"capability bindings disagree on fingerprint: {include}")
        existing["capability_keys"].append(key)
    for item in normalized.values():
        item["capability_keys"] = sorted(set(item["capability_keys"]))
    return normalized, problems


def _normalize_reviewed_headers(
    entries: list[dict[str, Any]], known_capability_keys: set[str]
) -> tuple[dict[str, dict[str, Any]], list[str]]:
    normalized: dict[str, dict[str, Any]] = {}
    problems: list[str] = []
    required = {"include", "fingerprint", "disposition", "capability_keys", "rationale"}
    for index, entry in enumerate(entries):
        where = f"reviewed_headers[{index}]"
        if not isinstance(entry, dict) or set(entry) != required:
            problems.append(f"{where} fields must be exactly {', '.join(sorted(required))}")
            continue
        include = entry["include"]
        if not isinstance(include, str) or not include.startswith("pulp/"):
            problems.append(f"{where}.include is invalid")
            continue
        if include in normalized:
            problems.append(f"duplicate reviewed public header: {include}")
            continue
        if not _valid_digest(entry["fingerprint"]):
            problems.append(f"{where}.fingerprint is invalid")
        if (
            not isinstance(entry["disposition"], str)
            or entry["disposition"] not in REVIEWED_DISPOSITIONS
        ):
            problems.append(f"{where}.disposition is invalid")
        keys = entry["capability_keys"]
        if not isinstance(keys, list) or not all(isinstance(key, str) and key for key in keys):
            problems.append(f"{where}.capability_keys must be an array of strings")
        elif len(keys) != len(set(keys)):
            problems.append(f"{where}.capability_keys must be unique")
        elif any(key not in known_capability_keys for key in keys):
            problems.append(f"{where}.capability_keys references an unknown key")
        elif entry["disposition"] == "capability_support" and not keys:
            problems.append(f"{where}.capability_support requires capability_keys")
        elif entry["disposition"] != "capability_support" and keys:
            problems.append(
                f"{where}.{entry['disposition']} may not claim capability_keys"
            )
        rationale = entry["rationale"]
        if not isinstance(rationale, str) or len(rationale.strip()) < 20:
            problems.append(f"{where}.rationale must explain the classification")
        normalized[include] = dict(entry)
    return normalized, problems


def _normalize_tombstones(
    entries: list[dict[str, Any]],
) -> tuple[dict[str, dict[str, Any]], list[str]]:
    normalized: dict[str, dict[str, Any]] = {}
    problems: list[str] = []
    required = {
        "include",
        "last_fingerprint",
        "removed_in_inventory_version",
        "reason",
    }
    for index, entry in enumerate(entries):
        where = f"surface tombstones[{index}]"
        if not isinstance(entry, dict) or set(entry) != required:
            problems.append(f"{where} fields are not exact")
            continue
        include = entry["include"]
        if not isinstance(include, str) or not include.startswith("pulp/"):
            problems.append(f"{where}.include is invalid")
            continue
        if include in normalized:
            problems.append(f"duplicate surface tombstone: {include}")
            continue
        if not _valid_digest(entry["last_fingerprint"]):
            problems.append(f"{where}.last_fingerprint is invalid")
        revision = entry["removed_in_inventory_version"]
        if isinstance(revision, bool) or not isinstance(revision, int) or revision < 1:
            problems.append(f"{where}.removed_in_inventory_version is invalid")
        reason = entry["reason"]
        if not isinstance(reason, str) or len(reason.strip()) < 20:
            problems.append(f"{where}.reason must explain the removal")
        normalized[include] = dict(entry)
    return normalized, problems


def _check_fingerprint(
    include: str, actual: str, expected: str, problems: list[str]
) -> None:
    if actual != expected:
        problems.append(
            f"public header fingerprint changed: {include}; expected {expected}, got {actual}"
        )


def _valid_digest(value: Any) -> bool:
    if not isinstance(value, str) or not value.startswith("sha256:"):
        return False
    digest = value.removeprefix("sha256:")
    return len(digest) == 64 and all(char in "0123456789abcdef" for char in digest)
