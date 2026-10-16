#!/usr/bin/env python3
"""Validate the immutable evidence envelope consumed by Shipyard merge receipts.

This module deliberately only validates evidence; it does not query GitHub or
publish attestations. A producer must supply every field and the digest binds
the complete canonical payload. The digest is an integrity check, not an
authenticity proof; a producer/attestation layer must establish provenance.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import re
from pathlib import Path
from typing import Any

SCHEMA = "shipyard-merge-gate-evidence/v1"
DOMAIN = b"shipyard-merge-gate-evidence-v1\0"
SHA = re.compile(r"^[0-9a-f]{40}$")
HASH = re.compile(r"^[0-9a-f]{64}$")
REPOSITORY = re.compile(r"^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$")
RFC3339_UTC = re.compile(
    r"^\d{4}-\d{2}-\d{2}T(?:[01]\d|2[0-3]):[0-5]\d:[0-5]\d(\.\d{1,6})?Z$"
)


class EvidenceError(ValueError):
    pass


def canonical(value: Any) -> bytes:
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode()


def digest(value: Any) -> str:
    return hashlib.sha256(DOMAIN + canonical(value)).hexdigest()


def _keys(value: dict[str, Any], expected: set[str], label: str) -> None:
    if set(value) != expected:
        raise EvidenceError(f"{label} fields do not match schema")


def _sha(value: Any, label: str) -> None:
    if not isinstance(value, str) or not SHA.fullmatch(value) or value == "0" * 40:
        raise EvidenceError(f"{label} must be a lowercase 40-character SHA")


def _hash(value: Any, label: str) -> None:
    if not isinstance(value, str) or not HASH.fullmatch(value) or value == "0" * 64:
        raise EvidenceError(f"{label} must be a lowercase SHA-256")


def _merged_at(value: Any) -> None:
    if not isinstance(value, str) or not RFC3339_UTC.fullmatch(value):
        raise EvidenceError("merged_at must be an RFC 3339 UTC timestamp")
    try:
        parsed = datetime.fromisoformat(value[:-1] + "+00:00")
    except ValueError as error:
        raise EvidenceError("merged_at must be an RFC 3339 UTC timestamp") from error
    if parsed.tzinfo is None or parsed.utcoffset() != timezone.utc.utcoffset(parsed):
        raise EvidenceError("merged_at must be an RFC 3339 UTC timestamp")


def _reject_duplicate_pairs(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, item in pairs:
        if key in result:
            raise EvidenceError(f"duplicate JSON key: {key}")
        result[key] = item
    return result


def _reject_constant(value: str) -> None:
    raise EvidenceError(f"non-finite JSON number: {value}")


def load(path: Path) -> Any:
    try:
        return json.loads(
            path.read_text(encoding="utf-8"),
            object_pairs_hook=_reject_duplicate_pairs,
            parse_constant=_reject_constant,
        )
    except (OSError, json.JSONDecodeError, EvidenceError) as error:
        raise EvidenceError(str(error)) from error


def validate(value: Any) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise EvidenceError("evidence must be a JSON object")
    _keys(value, {"schema", "repository", "base_sha", "head_sha", "merge_group_sha", "merge_commit_sha", "merged_at", "review", "required_checks", "source_digests", "evidence_digest"}, "evidence")
    if value["schema"] != SCHEMA:
        raise EvidenceError("unsupported evidence schema")
    if not isinstance(value["repository"], str) or not REPOSITORY.fullmatch(value["repository"]):
        raise EvidenceError("repository must be owner/name")
    for field in ("base_sha", "head_sha", "merge_group_sha", "merge_commit_sha"):
        _sha(value[field], field)
    # A fast-forward merge may retain the PR head as the merge commit, and a
    # queue's tested merge-group commit may be the final merge commit. The
    # producer must prove those identities against live GitHub state; the
    # schema must not reject valid equality before that check occurs.
    if value["base_sha"] == value["head_sha"]:
        raise EvidenceError("base and head identities must differ")
    _merged_at(value["merged_at"])
    review = value["review"]
    if not isinstance(review, dict):
        raise EvidenceError("review evidence is required")
    _keys(review, {"verdict", "digest"}, "review")
    if review["verdict"] != "approved":
        raise EvidenceError("review verdict is not approved")
    _hash(review["digest"], "review.digest")
    checks = value["required_checks"]
    if not isinstance(checks, list) or not checks:
        raise EvidenceError("required_checks must be non-empty")
    names: set[str] = set()
    for check in checks:
        if not isinstance(check, dict):
            raise EvidenceError("required check evidence is malformed")
        _keys(check, {"name", "conclusion", "source_digest"}, "required check")
        if not isinstance(check["name"], str) or not check["name"].strip() or check["name"] in names:
            raise EvidenceError("required check names must be unique and non-empty")
        names.add(check["name"])
        if check["conclusion"] != "success":
            raise EvidenceError(f"required check {check['name']} is not successful")
        _hash(check["source_digest"], f"required check {check['name']} source_digest")
    source_digests = value["source_digests"]
    if not isinstance(source_digests, list) or not source_digests:
        raise EvidenceError("source_digests must be non-empty")
    for item in source_digests:
        _hash(item, "source_digest")
    if len(set(source_digests)) != len(source_digests):
        raise EvidenceError("source_digests must be unique")
    referenced = {review["digest"], *(check["source_digest"] for check in checks)}
    if not referenced.issubset(set(source_digests)):
        raise EvidenceError("source_digests must contain every referenced source digest")
    supplied = value["evidence_digest"]
    _hash(supplied, "evidence_digest")
    unsigned = dict(value)
    del unsigned["evidence_digest"]
    if supplied != digest(unsigned):
        raise EvidenceError("evidence_digest does not match payload")
    return value


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate shipyard-merge-gate-evidence/v1 JSON")
    parser.add_argument("path", type=Path)
    args = parser.parse_args()
    try:
        validate(load(args.path))
    except EvidenceError as error:
        parser.error(str(error))
    print("valid")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
