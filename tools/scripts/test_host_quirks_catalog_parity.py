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
SOURCE = REPO_ROOT / "core" / "format" / "src" / "host_quirks.cpp"
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


def parse_struct_fields() -> set[str]:
    """Return the set of data-member names in the ``HostQuirks`` struct.

    Extracts every ``bool <name> =`` / ``int <name> =`` field from the
    ``struct HostQuirks { ... };`` body (comments stripped). This closes the
    struct↔meta/macro drift axis: a field added to the struct (+ meta + JSON)
    but forgotten in the PULP_HOST_QUIRK_FIELDS X-macro in host_quirks.cpp
    would otherwise compile + test green while being silently inert in
    apply_filter / overrides / `pulp doctor`. Since the macro must list every
    struct field, asserting struct == meta == JSON keeps the macro honest too
    (any struct field missing from meta/JSON trips this; the X-macro mirrors
    the struct 1:1 by construction + the in-file count static_assert).
    """
    text = HEADER.read_text(encoding="utf-8")
    match = re.search(r"struct\s+HostQuirks\s*\{(.*?)\n\};", text, re.DOTALL)
    if not match:
        raise AssertionError(
            "Could not locate `struct HostQuirks { ... };` in "
            f"{HEADER} — did the struct get renamed?"
        )
    body = re.sub(r"//[^\n]*", "", match.group(1))  # strip line comments
    return set(re.findall(r"(?:bool|int)\s+(\w+)\s*=", body))


def parse_macro_fields() -> set[str]:
    """Return the field names in the ``PULP_HOST_QUIRK_FIELDS`` X-macro.

    This is the THIRD list (host_quirks.cpp) that drives apply_filter, the
    per-quirk overrides, and `pulp doctor` enumeration. #3240 showed that
    comparing only struct↔meta↔JSON misses the case where a field is
    added everywhere EXCEPT this macro — then apply_filter / overrides /
    enumerate silently skip it while every other parity check passes and the
    self-counting `static_assert(count_quirk_fields() == N)` stays valid
    (the macro counts its own entries). Parsing the macro here closes that
    last drift axis.
    """
    text = SOURCE.read_text(encoding="utf-8")
    match = re.search(r"#define\s+PULP_HOST_QUIRK_FIELDS\(X\)(.*?)\n\n", text, re.DOTALL)
    if not match:
        raise AssertionError(
            "Could not locate the `#define PULP_HOST_QUIRK_FIELDS(X)` macro in "
            f"{SOURCE} — did it get renamed?"
        )
    body = re.sub(r"//[^\n]*", "", match.group(1))
    return set(re.findall(r"X\((\w+)\)", body))


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


class StructMetaParity(unittest.TestCase):
    """The HostQuirks struct fields EXACTLY match the HostQuirksMeta flags.

    Closes the struct↔meta/macro drift axis flagged by the 2026-05-30
    self-sweep: a field added to the struct but missed in meta/JSON (or the
    X-macro) would otherwise be silently inert.
    """

    def test_struct_fields_match_meta(self) -> None:
        struct_fields = parse_struct_fields()
        meta = set(parse_meta_tiers())
        self.assertTrue(
            struct_fields,
            "parsed zero fields from `struct HostQuirks` — parser likely broke",
        )
        only_in_struct = struct_fields - meta
        only_in_meta = meta - struct_fields
        self.assertFalse(
            only_in_struct,
            "Fields in `struct HostQuirks` but NOT in HostQuirksMeta "
            "(add a QuirkStatus + a catalog entry + the PULP_HOST_QUIRK_FIELDS "
            f"X-macro row): {sorted(only_in_struct)}",
        )
        self.assertFalse(
            only_in_meta,
            "Fields in HostQuirksMeta but NOT in `struct HostQuirks` "
            f"(stale meta entry): {sorted(only_in_meta)}",
        )


class MacroFieldParity(unittest.TestCase):
    """The PULP_HOST_QUIRK_FIELDS X-macro lists EXACTLY the struct fields.

    Closes the last drift axis (#3240): a field added to the
    struct + meta + JSON but omitted from the X-macro would otherwise be
    silently inert in apply_filter / overrides / enumerate, with every other
    parity check + the self-counting static_assert still green.
    """

    def test_macro_fields_match_struct(self) -> None:
        macro = parse_macro_fields()
        struct_fields = parse_struct_fields()
        self.assertTrue(
            macro, "parsed zero fields from PULP_HOST_QUIRK_FIELDS — parser broke"
        )
        only_in_struct = struct_fields - macro
        only_in_macro = macro - struct_fields
        self.assertFalse(
            only_in_struct,
            "Fields in `struct HostQuirks` but MISSING from the "
            "PULP_HOST_QUIRK_FIELDS X-macro (host_quirks.cpp) — they would be "
            "silently skipped by apply_filter / overrides / pulp doctor: "
            f"{sorted(only_in_struct)}",
        )
        self.assertFalse(
            only_in_macro,
            "Fields in the PULP_HOST_QUIRK_FIELDS X-macro but NOT in "
            f"`struct HostQuirks` (stale macro entry): {sorted(only_in_macro)}",
        )


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
