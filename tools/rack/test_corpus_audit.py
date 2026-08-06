#!/usr/bin/env python3
"""Focused checks for Patchstorage usage priors and their quarantine floor."""

from __future__ import annotations

import corpus_audit as audit


def observation(patch: str, signal_name: str = "V/OCT",
                signal_role: str = "Pitch") -> dict:
    return {
        "patch": patch,
        "patch_sha256": patch,
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
    print("  ok  three distinct patch bodies corroborate one pitch prior")

    duplicate = rows + [observation("three")]
    report = audit.usage_prior_report(duplicate, min_support=4)
    assert not report["admitted"]
    assert report["quarantine"][0]["support"] == 3
    print("  ok  a duplicate body does not inflate support")

    conflict = rows + [{
        **observation("four", "GATE", "Gate"),
        "dst": ("GATE", "Gate"),
    }]
    report = audit.usage_prior_report(conflict, min_support=1)
    assert not report["admitted"]
    assert all("conflicting inferred meanings" in row["reason"]
               for row in report["quarantine"])
    print("  ok  conflicting meanings quarantine the port instead of voting")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
