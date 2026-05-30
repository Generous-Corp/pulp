#!/usr/bin/env python3
"""Parity test: core/format/host-quirks.json vs. the C++ HostQuirksMeta.

The C++ struct ``pulp::format::HostQuirksMeta`` in
``core/format/include/pulp/format/host_quirks.hpp`` is the single source
of truth for the set of host-quirk flags and each flag's validation tier
(``QuirkStatus::{Validated|Speculative|LessonOnly}``). The machine-readable
catalog ``core/format/host-quirks.json`` is a parallel index consumed by
provenance / staleness tooling (host-quirks integration plan, P2+).

This test fails if the two ever drift:

  * a flag present in one but not the other, or
  * a flag whose JSON ``tier`` disagrees with its ``kHostQuirksMeta`` default.

It is intentionally a pure-Python parser (no C++ build needed): it scans
the ``struct HostQuirksMeta { ... }`` body for ``QuirkStatus <name> =
QuirkStatus::<Tier>;`` defaults and compares them to the JSON. That keeps
the parity gate cheap enough to run on every PR (mirrors the pattern in
tools/scripts/test_local_diff_cover.py).

Run:
    python3 tools/scripts/test_host_quirks_catalog_parity.py
"""

from __future__ import annotations

import json
import pathlib
import re
import unittest


REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
HEADER = REPO_ROOT / "core" / "format" / "include" / "pulp" / "format" / "host_quirks.hpp"
CATALOG = REPO_ROOT / "core" / "format" / "host-quirks.json"

VALID_TIERS = {"Validated", "Speculative", "LessonOnly"}
VALID_SOURCE_TYPES = {"spec", "release-notes", "repro", "in-DAW-bench", "unknown"}


def parse_meta_tiers() -> dict[str, str]:
    """Return {flag: tier} parsed from the HostQuirksMeta struct body.

    Looks for the ``struct HostQuirksMeta { ... };`` block and extracts
    every ``QuirkStatus <field> = QuirkStatus::<Tier>;`` default. Comments
    (``// ...``) are stripped first so commented-out fields never count.
    """
    text = HEADER.read_text(encoding="utf-8")

    match = re.search(r"struct\s+HostQuirksMeta\s*\{(.*?)\n\};", text, re.DOTALL)
    if not match:
        raise AssertionError(
            "Could not locate `struct HostQuirksMeta { ... };` in "
            f"{HEADER} — did the struct get renamed?"
        )
    body = match.group(1)
    # Strip line comments so a commented `// QuirkStatus foo = ...` line is
    # never mistaken for a real field default.
    body = re.sub(r"//[^\n]*", "", body)

    tiers: dict[str, str] = {}
    for name, tier in re.findall(
        r"QuirkStatus\s+(\w+)\s*=\s*QuirkStatus::(\w+)\s*;", body
    ):
        tiers[name] = tier
    return tiers


def load_catalog() -> dict:
    return json.loads(CATALOG.read_text(encoding="utf-8"))


class CatalogWellFormed(unittest.TestCase):
    """host-quirks.json parses and every entry has the required shape."""

    def test_files_exist(self) -> None:
        self.assertTrue(HEADER.is_file(), f"missing {HEADER}")
        self.assertTrue(CATALOG.is_file(), f"missing {CATALOG}")

    def test_entries_have_required_fields(self) -> None:
        catalog = load_catalog()
        self.assertIn("quirks", catalog, "catalog must have a `quirks` array")
        required = {"flag", "tier", "host", "symptom_area", "note"}
        seen: set[str] = set()
        for entry in catalog["quirks"]:
            missing = required - set(entry)
            self.assertFalse(
                missing, f"quirk {entry.get('flag', '?')} missing fields: {missing}"
            )
            self.assertIn(
                entry["tier"],
                VALID_TIERS,
                f"quirk {entry['flag']} has invalid tier {entry['tier']!r}",
            )
            # Provenance fields are optional but, when present, must use the
            # documented vocabulary.
            if "source_type" in entry:
                self.assertIn(
                    entry["source_type"],
                    VALID_SOURCE_TYPES,
                    f"quirk {entry['flag']} has invalid source_type "
                    f"{entry['source_type']!r}",
                )
            self.assertNotIn(
                entry["flag"],
                seen,
                f"duplicate flag {entry['flag']!r} in host-quirks.json",
            )
            seen.add(entry["flag"])


class CatalogStructParity(unittest.TestCase):
    """The JSON lists EXACTLY the HostQuirksMeta flags, with matching tiers."""

    def test_flag_sets_match(self) -> None:
        meta = parse_meta_tiers()
        self.assertTrue(
            meta, "parsed zero fields from HostQuirksMeta — parser likely broke"
        )
        catalog = {q["flag"]: q["tier"] for q in load_catalog()["quirks"]}

        only_in_meta = set(meta) - set(catalog)
        only_in_json = set(catalog) - set(meta)
        self.assertFalse(
            only_in_meta,
            "Flags in HostQuirksMeta but NOT in host-quirks.json "
            f"(add catalog entries): {sorted(only_in_meta)}",
        )
        self.assertFalse(
            only_in_json,
            "Flags in host-quirks.json but NOT in HostQuirksMeta "
            f"(remove stale catalog entries): {sorted(only_in_json)}",
        )

    def test_tiers_match(self) -> None:
        meta = parse_meta_tiers()
        catalog = {q["flag"]: q["tier"] for q in load_catalog()["quirks"]}
        mismatches = {
            flag: (meta[flag], catalog[flag])
            for flag in meta
            if flag in catalog and catalog[flag] != meta[flag]
        }
        self.assertFalse(
            mismatches,
            "Tier mismatch between HostQuirksMeta (left) and host-quirks.json "
            f"(right): {mismatches}",
        )


if __name__ == "__main__":
    unittest.main()
