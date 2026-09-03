#!/usr/bin/env python3
"""Focused checks for corpus usage priors and their quarantine floor."""

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

    anonymous = [observation("anon-a", author_id=0),
                 observation("anon-b", author_id=0),
                 observation("anon-c", author_id=0)]
    report = audit.usage_prior_report(anonymous, min_support=2)
    assert not report["admitted"]
    assert report["quarantine"][0]["support"] == 1
    print("  ok  uploads with no identified contributor count as one source")

    conflict = rows + [{
        **observation("four", "GATE", "Gate"),
        "dst": ("GATE", "Gate"),
    }]
    report = audit.usage_prior_report(conflict, min_support=1)
    assert not report["admitted"]
    assert all("conflicting inferred meanings" in row["reason"]
               for row in report["quarantine"])
    print("  ok  conflicting meanings quarantine the port instead of voting")

    # The stored index records a contributor as a plain name and carries no
    # numeric id, so an identity that reads the id alone finds nothing on every
    # row, collapses all evidence into one bucket, and pins support at 1 -- a
    # floor above 1 then cannot fire and the lane reports that the corpus
    # corroborates nothing. Rows here are shaped like the store, not like the
    # writer, so this fails if the identity ever narrows back to the id.
    stored_shape = [{k: v for k, v in observation(name).items()
                     if k != "source_author_id"}
                    for name in ("alfa", "bravo", "charlie")]
    assert all("source_author_id" not in row for row in stored_shape)
    report = audit.usage_prior_report(stored_shape, min_support=3)
    assert len(report["admitted"]) == 1, report
    assert report["admitted"][0]["support"] == 3
    print("  ok  named contributors corroborate when no numeric id is stored")

    one_author = [{k: v for k, v in observation(name).items()
                   if k != "source_author_id"} | {"source_author": "  Same  "}
                  for name in ("alfa", "bravo", "charlie")]
    report = audit.usage_prior_report(one_author, min_support=2)
    assert not report["admitted"]
    assert report["quarantine"][0]["support"] == 1
    print("  ok  one named contributor stays one vote across several bodies")

    blind = [dict(row, source_author="", source_author_id=None)
             for row in stored_shape] + [
        dict(observation("delta", "GATE", "Gate"), source_author="",
             source_author_id=None, dst=("GATE", "Gate"))]
    report = audit.usage_prior_report(blind, min_support=3)
    assert report["degenerate_support"] is True, report
    assert not report["admitted"]
    report = audit.usage_prior_report(stored_shape, min_support=3)
    assert report["degenerate_support"] is False, report
    print("  ok  an identity that cannot tell contributors apart says so")

    keys = {audit._contributor_key(row) for row in stored_shape}
    assert len(keys) == 3
    assert not any("alfa" in key for key in keys), keys
    print("  ok  a contributor key compares equal without carrying a name")

    # Both ends mapped, so the far end's class is known and the inference can
    # be scored. Three contributors agree the sequencer output is pitch, and
    # the port map says so too.
    def scored(name: str, out_name: str = "V/OCT") -> dict:
        return dict(observation(name), s=(out_name, "Pitch"),
                    d=("V/OCT", "Pitch"), source_author_id=None)

    truthful = [scored(n) for n in ("alfa", "bravo", "charlie")]
    report = audit.prior_accuracy(truthful, floors=(1, 3))
    assert report[0]["correct"] and not report[0]["wrong"], report
    assert report[1]["correct"], report
    print("  ok  a prior that matches the mapped port scores as correct")

    wrong = [dict(scored(n), s=("Clock", "Clock")) for n in ("a", "b", "c")]
    report = audit.prior_accuracy(wrong, floors=(1,))
    # Both directions are scored, so one disagreeing cable is two wrong
    # answers: the output read as pitch and the input read as clock.
    assert report[0]["wrong"] == 2 and report[0]["correct"] == 0, report
    print("  ok  a prior that contradicts the mapped port scores as wrong")

    unmapped = [dict(observation(n), source_author_id=None)
                for n in ("alfa", "bravo")]
    assert all(row["s"] is None for row in unmapped)
    report = audit.prior_accuracy(unmapped, floors=(1,))
    assert report[0]["scored"] == 0, report
    print("  ok  a port with no known class is not scored at all")

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
