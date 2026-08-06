#!/usr/bin/env python3
"""Candidate quarantine and canonical-claim dedupe controls."""
import copy
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import idiom_check  # noqa: E402
import knowledge  # noqa: E402


def main() -> int:
    bad = 0
    admitted = knowledge.load()
    all_entries = knowledge.load(include_candidates=True)
    candidate = all_entries.get("post-multiplier-low-pass")
    if "post-multiplier-low-pass" in admitted or not candidate:
        print("  WRONG  candidate guidance is not quarantined from generation")
        bad += 1
    else:
        print("  ok     candidate guidance is absent from the generation loader")

    problems = knowledge.problems(all_entries, idiom_check.load_idioms())
    if problems:
        print(f"  WRONG  admission metadata failed lint: {problems[:3]}")
        bad += 1
    else:
        print("  ok     canonical locator and canonical claim fingerprint pass lint")

    duplicate = copy.deepcopy(all_entries)
    clone = copy.deepcopy(candidate)
    clone["id"] = "duplicate-claim"
    clone["evidence"][0]["page"] = 231
    clone["anchor"]["page"] = 231
    duplicate[clone["id"]] = clone
    got = knowledge.problems(duplicate, idiom_check.load_idioms())
    if not any("duplicates the canonical semantic identity" in problem for problem in got):
        print("  WRONG  a duplicate guidance row escaped canonical dedupe")
        bad += 1
    else:
        print("  ok     semantic duplicates attach evidence to one row despite wording")

    broken = copy.deepcopy(all_entries)
    broken["post-multiplier-low-pass"]["canonical_claim_fingerprint"] = "0" * 64
    got = knowledge.problems(broken, idiom_check.load_idioms())
    if not any("does not match its canonical claim" in problem for problem in got):
        print("  WRONG  a stale canonical claim fingerprint escaped lint")
        bad += 1
    else:
        print("  ok     canonical claim edits require a new fingerprint")
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
