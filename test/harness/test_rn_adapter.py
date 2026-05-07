"""Tests for the rn (React Native ViewStyle) catalog harness adapter.

Per CLAUDE.md "tests ship with fixes" — this is the same-PR test surface
for the rn adapter (#1392, week 1 cut, third of 5 surfaces).

Run via::

    cd /path/to/pulp
    python3 -m pytest test/harness/test_rn_adapter.py -v
    # or, without pytest installed:
    python3 -m unittest test.harness.test_rn_adapter -v
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import unittest
from pathlib import Path

# Make the harness package importable.
REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools.harness.adapters.base import CatalogEntry  # noqa: E402
from tools.harness.adapters.rn import RnAdapter  # noqa: E402
from tools.harness.status import Status, StatusCounts  # noqa: E402
from tools.harness.verifier import (  # noqa: E402
    collect_entries,
    load_compat,
    run_surface,
)


class RnAdapterClassifyTest(unittest.TestCase):
    """Classify each catalog status family on known-good and known-bad fixtures."""

    @classmethod
    def setUpClass(cls):
        cls.adapter = RnAdapter(REPO_ROOT)

    # ── Known-good entries ───────────────────────────────────────────

    def test_supported_non_enum_clean_is_PASS(self):
        """opacity is `supported`, prop-applier wires it, no unsupportedValues."""
        e = CatalogEntry(
            surface="rn",
            name="rn/opacity",
            status="supported",
            maps_to="prop-applier.ts -> setOpacity(id, n)",
            supported_values=["0..1"],
            unsupported_values=[],
        )
        result = self.adapter.run(e)
        self.assertEqual(result.status, Status.PASS, msg=result.detail)
        self.assertFalse(result.drifts, msg=result.detail)

    def test_supported_full_enum_is_PASS(self):
        """fontStyle's only RN values are normal+italic and prop-applier handles
        it through setFontStyle."""
        e = CatalogEntry(
            surface="rn",
            name="rn/fontStyle",
            status="supported",
            maps_to="setFontStyle",
            supported_values=["normal", "italic"],
            unsupported_values=[],
        )
        result = self.adapter.run(e)
        self.assertEqual(result.status, Status.PASS, msg=result.detail)
        self.assertFalse(result.drifts, msg=result.detail)

    # ── DIVERGE (catalog claims supported, but values incomplete) ──

    def test_partial_enum_is_DIVERGE(self):
        """alignItems is `supported` but missing baseline + flex-* long forms."""
        e = CatalogEntry(
            surface="rn",
            name="rn/alignItems",
            status="supported",
            maps_to="prop-applier.ts -> setFlex(id, 'align_items', value)",
            supported_values=["start", "center", "end", "stretch"],
            unsupported_values=["baseline", "flex-start", "flex-end (long forms)"],
        )
        result = self.adapter.run(e)
        self.assertEqual(result.status, Status.DIVERGE, msg=result.detail)
        # Catalog said supported, harness says DIVERGE → drift
        self.assertTrue(result.drifts, msg=result.detail)
        self.assertIn("center", result.matched_supported)
        self.assertIn("baseline", result.unmatched_supported)
        self.assertIn("flex-start", result.unmatched_supported)

    def test_partial_enum_status_partial_is_DIVERGE_no_drift(self):
        """flexDirection catalog status=partial → expected DIVERGE → no drift."""
        e = CatalogEntry(
            surface="rn",
            name="rn/flexDirection",
            status="partial",
            maps_to=(
                "prop-applier 'direction' -> setFlex(id, 'direction', value); "
                "but RN says 'row'|'row-reverse'|'column'|'column-reverse'"
            ),
            supported_values=["row"],
            unsupported_values=["row-reverse", "column", "column-reverse"],
        )
        result = self.adapter.run(e)
        self.assertEqual(result.status, Status.DIVERGE, msg=result.detail)
        # partial maps to DIVERGE in expected_status, so this is NOT a drift
        self.assertFalse(result.drifts, msg=result.detail)

    # ── NOT_IMPL ─────────────────────────────────────────────────────

    def test_missing_no_branch_marker_is_NOT_IMPL(self):
        """borderEndWidth is `missing` with `no branch` marker."""
        e = CatalogEntry(
            surface="rn",
            name="rn/borderEndWidth",
            status="missing",
            maps_to="no branch",
            supported_values=[],
            unsupported_values=["all"],
        )
        result = self.adapter.run(e)
        self.assertEqual(result.status, Status.NOT_IMPL, msg=result.detail)
        self.assertFalse(result.drifts, msg=result.detail)

    def test_missing_at_pulp_react_marker_is_NOT_IMPL(self):
        """alignContent — bridge field exists but @pulp/react TS doesn't expose it."""
        e = CatalogEntry(
            surface="rn",
            name="rn/alignContent",
            status="missing",
            maps_to="@pulp/react does not expose alignContent on FlexProps",
            supported_values=[],
            unsupported_values=["all"],
        )
        result = self.adapter.run(e)
        self.assertEqual(result.status, Status.NOT_IMPL, msg=result.detail)

    def test_missing_intentionally_not_dispatched_is_NOT_IMPL(self):
        """fontFamily — explicitly NOT dispatched in prop-applier."""
        e = CatalogEntry(
            surface="rn",
            name="rn/fontFamily",
            status="missing",
            maps_to=(
                "intentionally NOT dispatched in prop-applier.ts (line 152 "
                "comment) -- pending #932 SkFontMgr fix"
            ),
            supported_values=[],
            unsupported_values=["all"],
        )
        result = self.adapter.run(e)
        self.assertEqual(result.status, Status.NOT_IMPL, msg=result.detail)

    # ── OOS ──────────────────────────────────────────────────────────

    def test_wontfix_is_OOS(self):
        e = CatalogEntry(
            surface="rn",
            name="rn/borderCurve",
            status="wontfix",
            maps_to="n/a",
        )
        result = self.adapter.run(e)
        self.assertEqual(result.status, Status.OOS)
        self.assertFalse(result.drifts)

    def test_ios_only_prop_is_OOS(self):
        """shadowColor is iOS-only in RN; cross-platform pulp surface = OOS."""
        e = CatalogEntry(
            surface="rn",
            name="rn/shadowColor",
            status="missing",
            maps_to="no branch -- would route to setShadow / setBoxShadow",
        )
        result = self.adapter.run(e)
        self.assertEqual(result.status, Status.OOS, msg=result.detail)
        self.assertIn("ios-only", result.detail.lower())

    def test_android_only_prop_is_OOS(self):
        """textAlignVertical is Android-only in RN."""
        e = CatalogEntry(
            surface="rn",
            name="rn/textAlignVertical",
            status="missing",
            maps_to="no branch",
        )
        result = self.adapter.run(e)
        self.assertEqual(result.status, Status.OOS, msg=result.detail)
        self.assertIn("android-only", result.detail.lower())

    def test_unknown_property_is_OOS(self):
        """A made-up property the RN oracle doesn't define is OOS."""
        e = CatalogEntry(
            surface="rn",
            name="rn/madeUpProp",
            status="missing",
            maps_to="placeholder",
        )
        result = self.adapter.run(e)
        self.assertEqual(result.status, Status.OOS)
        self.assertIn("unknown surface", result.detail)

    # ── Aliasing ─────────────────────────────────────────────────────

    def test_alt_prop_aliasing_is_recognized(self):
        """rn/backgroundColor maps to @pulp/react's `background` prop —
        the adapter should recognize the alias rather than calling NOT_IMPL."""
        e = CatalogEntry(
            surface="rn",
            name="rn/backgroundColor",
            status="supported",
            maps_to="background prop -> setBackground",
            supported_values=["#rgb", "#rrggbb"],
            unsupported_values=[],
        )
        result = self.adapter.run(e)
        # Should be PASS (color kind, no unsupported listed)
        self.assertEqual(result.status, Status.PASS, msg=result.detail)

    def test_prop_applier_quoted_alias_is_recognized(self):
        """flexDirection's mapsTo references "prop-applier 'direction' -> ...";
        the adapter must recognize 'direction' as the alias, even though the
        prop name is flexDirection."""
        e = CatalogEntry(
            surface="rn",
            name="rn/flexDirection",
            status="partial",
            maps_to="prop-applier 'direction' -> setFlex(id, 'direction', value)",
            supported_values=["row"],
            unsupported_values=["row-reverse", "column", "column-reverse"],
        )
        result = self.adapter.run(e)
        # Adapter should NOT say NOT_IMPL — it should detect the alias and
        # then DIVERGE on the missing values.
        self.assertEqual(result.status, Status.DIVERGE, msg=result.detail)

    # ── Bridge-fn drift detection ────────────────────────────────────

    def test_unregistered_bridge_fn_is_DIVERGE(self):
        """A catalog claim citing a setX bridge fn that doesn't actually exist
        should be flagged as DIVERGE (catalog drift)."""
        e = CatalogEntry(
            surface="rn",
            name="rn/cursor",
            status="supported",
            # setCursor IS registered; setMadeUpFn is NOT. Claim this entry
            # routes through a fake fn — should DIVERGE.
            maps_to="cursor prop -> setMadeUpFn(id, value)",
            supported_values=["auto", "pointer"],
            unsupported_values=[],
        )
        result = self.adapter.run(e)
        self.assertEqual(result.status, Status.DIVERGE, msg=result.detail)
        self.assertIn("setMadeUpFn", result.detail)


class VerifierEndToEndTest(unittest.TestCase):
    """Full pipeline — compat.json -> all 120 rn entries -> coverage report."""

    def test_collects_all_120_rn_entries(self):
        compat = load_compat(REPO_ROOT)
        entries = collect_entries(compat, "rn")
        self.assertEqual(len(entries), 120, "compat.json rn/ must have 120 entries")

    def test_runs_rn_surface_end_to_end(self):
        results = run_surface(REPO_ROOT, "rn")
        self.assertEqual(len(results), 120)
        for r in results:
            self.assertIsInstance(r.status, Status)

    def test_no_rn_entry_crashes(self):
        """All 120 rn catalog entries must classify without exception."""
        compat = load_compat(REPO_ROOT)
        entries = collect_entries(compat, "rn")
        adapter = RnAdapter(REPO_ROOT)
        for e in entries:
            r = adapter.run(e)
            self.assertIsInstance(r.status, Status, msg=e.name)

    def test_coverage_distribution_is_nonzero(self):
        """Sanity: with the catalog as-is, we have at least some PASS, some
        DIVERGE, some NOT-IMPL, and some OOS."""
        results = run_surface(REPO_ROOT, "rn")
        statuses = [r.status for r in results]
        counts = StatusCounts.from_results(statuses)
        self.assertGreater(counts.pass_, 0, "expected at least 1 PASS")
        self.assertGreater(counts.diverge, 0, "expected at least 1 DIVERGE")
        self.assertGreater(counts.not_impl, 0, "expected at least 1 NOT-IMPL")
        self.assertGreater(counts.oos, 0, "expected at least 1 OOS (iOS/Android only props)")

    def test_json_output_contains_all_entries(self):
        results = run_surface(REPO_ROOT, "rn")
        from tools.harness.verifier import render_json

        payload = render_json({"rn": results}, sha="test")
        self.assertIn("rn", payload["surfaces"])
        self.assertEqual(payload["surfaces"]["rn"]["total"], 120)
        self.assertEqual(
            payload["surfaces"]["rn"]["total"],
            len(payload["surfaces"]["rn"]["results"]),
        )


class VerifierCliTest(unittest.TestCase):
    """Smoke-tests the CLI entrypoint via subprocess."""

    def test_json_subcommand_exits_zero(self):
        env = os.environ.copy()
        env["PYTHONPATH"] = str(REPO_ROOT)
        result = subprocess.run(
            [
                sys.executable,
                str(REPO_ROOT / "tools" / "harness" / "verifier.py"),
                "--surface=rn",
                "--json",
            ],
            cwd=REPO_ROOT,
            env=env,
            capture_output=True,
            text=True,
            timeout=60,
        )
        self.assertEqual(result.returncode, 0, msg=result.stderr)
        payload = json.loads(result.stdout)
        self.assertEqual(payload["surfaces"]["rn"]["total"], 120)


if __name__ == "__main__":
    unittest.main()
