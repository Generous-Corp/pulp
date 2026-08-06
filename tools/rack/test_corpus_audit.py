#!/usr/bin/env python3
"""Focused checks for Patchstorage usage priors and their quarantine floor."""

from __future__ import annotations

import tempfile

import corpus_audit as audit
import patch_corpus


def observation(patch: str, signal_name: str = "V/OCT",
                signal_role: str = "Pitch", author_id: int | None = None) -> dict:
    return {
        "patch": patch,
        "patch_sha256": patch,
        "source_author": patch,
        "source_author_id": author_id if author_id is not None else {
            "one": 1, "two": 2, "three": 3, "four": 4, "revision": 3,
        }.get(patch),
        "src": ("Unmapped", "Sequencer"),
        "dst": ("Fundamental", "VCO"),
        "s": None,
        "d": (signal_name, signal_role),
        "s_index": 0,
        "d_index": 0,
    }


def main() -> int:
    assert audit._signal("V/OCT", "Pitch", "in") == "pitch"
    assert audit._signal("Clock", "Clock", "in") == "clock"
    assert audit._signal("Gate 1 CV", "Cv", "in") == "gate"
    assert audit._signal("Wobble", "Cv", "in") is None
    print("  ok  a generic CV name cannot manufacture a usage prior")

    rows = [observation("one"), observation("two"), observation("three")]
    report = audit.usage_prior_report(rows, min_support=3)
    assert len(report["admitted"]) == 1
    assert report["admitted"][0]["signal"] == "pitch"
    assert report["admitted"][0]["support"] == 3
    print("  ok  three distinct authors corroborate one pitch prior")

    duplicate = rows + [{**observation("revision"), "source_author": "renamed"}]
    report = audit.usage_prior_report(duplicate, min_support=4)
    assert not report["admitted"]
    assert report["quarantine"][0]["support"] == 3
    print("  ok  a second body from one author does not inflate support")

    conflict = rows + [{
        **observation("four", "GATE", "Gate"),
        "dst": ("GATE", "Gate"),
    }]
    report = audit.usage_prior_report(conflict, min_support=1)
    assert not report["admitted"]
    assert all("conflicting inferred meanings" in row["reason"]
               for row in report["quarantine"])
    print("  ok  conflicting meanings quarantine the port instead of voting")

    saved = (patch_corpus.ROOT, patch_corpus.PATCH_DIR, patch_corpus.load_index,
             patch_corpus.save_index, patch_corpus.listing,
             patch_corpus.detail, patch_corpus._get)
    with tempfile.TemporaryDirectory() as tmp:
        captured = {}
        patch_corpus.ROOT = tmp
        patch_corpus.PATCH_DIR = tmp
        patch_corpus.load_index = lambda: {"patches": {}}
        patch_corpus.save_index = lambda index: captured.update(index)
        patch_corpus.listing = lambda page: ([{"id": 7}] if page == 1 else [])
        patch_corpus.detail = lambda _pid: {
            "id": 7, "title": "unknown terms", "license": {},
            "author": {"id": 99, "name": "fixture"},
            "files": [{"filename": "x.vcv", "url": "body"}]}
        patch_corpus._get = lambda _url: (_ for _ in ()).throw(
            AssertionError("an unlicensed body was downloaded"))
        try:
            patch_corpus.fetch(1)
        finally:
            (patch_corpus.ROOT, patch_corpus.PATCH_DIR, patch_corpus.load_index,
             patch_corpus.save_index, patch_corpus.listing,
             patch_corpus.detail, patch_corpus._get) = saved
        row = captured["patches"]["7"]
        assert "body_quarantined" in row and "file" not in row
        assert row["author_id"] == 99
    print("  ok  an unlicensed body is never downloaded or written")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
