#!/usr/bin/env python3
"""Keep screened source examples from returning as duplicate idioms.

The quarantine records a source coordinate and the canonical semantic identity
that already represents it. It contains no source prose or images. A canonical
record changing shape makes the quarantine fail until somebody re-checks the
source example, so "duplicate" cannot quietly become a stale assertion.
"""

from __future__ import annotations

import hashlib
import json
import os
import sys

import idiom_check

HERE = os.path.dirname(os.path.abspath(__file__))
QUARANTINE = os.path.join(HERE, "patch_idioms", "_source_quarantine.json")


def semantic_identity(idiom: dict) -> str:
    """Stable hash of exactly the structure duplicate detection compares."""
    shape = {
        "topology": sorted((r.get("from_module", "any"),
                            r.get("from_port", "any_out"),
                            r.get("to_module", "any"),
                            r.get("to_port", "any_in"),
                            bool(r.get("same_module")),
                            bool(r.get("different_module")))
                           for r in idiom.get("topology") or []),
        "at_least": sorted((idiom.get("at_least") or {}).items()),
        "forbidden_topology": sorted((r.get("from_module", "any"),
                                      r.get("to_module", "any"),
                                      r.get("to_port", "any_in"))
                                     for r in idiom.get("forbidden_topology") or []),
    }
    raw = json.dumps(shape, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(raw).hexdigest()


def load(path: str = QUARANTINE) -> dict:
    with open(path) as f:
        return json.load(f)


def problems(doc: dict | None = None, idioms: dict | None = None) -> list[str]:
    doc = doc if doc is not None else load()
    idioms = idioms if idioms is not None else idiom_check.load_idioms()
    bad: list[str] = []
    records = list(doc.get("accepted") or []) + list(doc.get("duplicates") or [])

    total = doc.get("source_total")
    remaining = doc.get("remaining")
    if not isinstance(total, int) or remaining != total - len(records):
        bad.append("source denominator does not equal accepted + duplicate + remaining")

    patches = set()
    for record in records:
        patch = record.get("patch")
        page = record.get("page")
        where = f"patch {patch}"
        if patch in patches:
            bad.append(f"{where} appears more than once")
        patches.add(patch)
        if not isinstance(patch, int) or page != patch + doc.get("page_offset", 0):
            bad.append(f"{where} does not satisfy page = patch + page_offset")
        digest = record.get("page_sha256", "")
        if len(digest) != 64 or any(c not in "0123456789abcdef" for c in digest):
            bad.append(f"{where} has no valid page SHA-256")
        slug = record.get("canonical_slug")
        if slug not in idioms:
            bad.append(f"{where} points at missing canonical idiom {slug!r}")
            continue
        actual = semantic_identity(idioms[slug])
        if record.get("semantic_identity") != actual:
            bad.append(f"{where}'s semantic identity no longer matches {slug}; "
                       "re-screen it before keeping the duplicate disposition")
    return bad


def main() -> int:
    bad = problems()
    for item in bad:
        print(f"  {item}")
    if not bad:
        doc = load()
        print(f"  {len(doc['accepted'])} accepted, {len(doc['duplicates'])} "
              f"quarantined duplicates, {doc['remaining']} remaining")
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
