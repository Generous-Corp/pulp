#!/usr/bin/env python3
"""Validate the immutable evidence envelope consumed by Shipyard merge receipts.

This module deliberately only validates evidence; it does not query GitHub or
publish attestations. A producer must supply every field and the digest binds
the complete canonical payload.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path
from typing import Any

SCHEMA = "shipyard-merge-gate-evidence/v1"
DOMAIN = b"shipyard-merge-gate-evidence-v1\0"
SHA = re.compile(r"^[0-9a-f]{40}$")
HASH = re.compile(r"^[0-9a-f]{64}$")


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
    if not isinstance(value, str) or not HASH.fullmatch(value):
        raise EvidenceError(f"{label} must be a lowercase SHA-256")


def validate(value: Any) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise EvidenceError("evidence must be a JSON object")
    _keys(value, {"schema", "repository", "base_sha", "head_sha", "merge_group_sha", "merge_commit_sha", "merged_at", "review", "required_checks", "source_digests", "evidence_digest"}, "evidence")
    if value["schema"] != SCHEMA:
        raise EvidenceError("unsupported evidence schema")
    if not isinstance(value["repository"], str) or "/" not in value["repository"]:
        raise EvidenceError("repository must be owner/name")
    for field in ("base_sha", "head_sha", "merge_group_sha", "merge_commit_sha"):
        _sha(value[field], field)
    if not isinstance(value["merged_at"], str) or not value["merged_at"].strip():
        raise EvidenceError("merged_at is required")
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
        validate(json.loads(args.path.read_text(encoding="utf-8")))
    except (OSError, json.JSONDecodeError, EvidenceError) as error:
        parser.error(str(error))
    print("valid")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
