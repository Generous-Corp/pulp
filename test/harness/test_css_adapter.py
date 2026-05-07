"""Tests for the css catalog harness adapter.

Per CLAUDE.md "tests ship with fixes" — this is the same-PR test surface
for the css adapter (#1392, second of 5 surfaces).

Run via::

    cd /path/to/pulp
    python3 -m pytest test/harness/ -v
    # or, without pytest installed:
    python3 -m unittest discover -s test/harness -v
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
from tools.harness.adapters.css import CssAdapter  # noqa: E402
from tools.harness.status import Status, StatusCounts  # noqa: E402
from tools.harness.verifier import (  # noqa: E402
    collect_entries,
    load_compat,
    run_surface,
)


class CssAdapterClassifyTest(unittest.TestCase):
    """Classify each catalog status family on known-good and known-bad fixtures.

    All fixtures use canonical-looking property names and `mapsTo` strings
    pulled directly from `compat.json` so the tests fail loudly if the
    adapter heuristics drift away from the catalog vocabulary.
    """

    @classmethod
    def setUpClass(cls):
        cls.adapter = CssAdapter(REPO_ROOT)

    # ── Known-good entries (PASS-shaped) ─────────────────────────────

    def test_supported_non_enum_wired_is_PASS(self):
        """`backgroundColor` is wired through web-compat-style-decl.js and
        has no meaningful unsupportedValues — the harness must say PASS."""
        e = CatalogEntry(
            surface="css",
            name="css/marginTop",
            status="supported",
            maps_to="parseCSSLength -> setFlex(id, 'margin_top', value)",
            supported_values=["px", "em"],
            unsupported_values=[],
        )
        result = self.adapter.run(e)
        self.assertEqual(result.status, Status.PASS, msg=result.detail)
        self.assertFalse(result.drifts, msg=result.detail)

    def test_supported_enum_full_coverage_is_PASS(self):
        """justifyContent has all 6 CSS-spec values supported in compat.json."""
        e = CatalogEntry(
            surface="css",
            name="css/justifyContent",
            status="supported",
            maps_to="setFlex(id, 'justify_content', value)",
            supported_values=[
                "flex-start", "flex-end", "center",
                "space-between", "space-around", "space-evenly",
            ],
            unsupported_values=[],
        )
        result = self.adapter.run(e)
        self.assertEqual(result.status, Status.PASS, msg=result.detail)

    # ── DIVERGE-shaped entries ───────────────────────────────────────

    def test_partial_enum_is_DIVERGE_with_value_list(self):
        """alignItems is `partial` with 4 of 5 CSS-spec values supported
        (missing baseline)."""
        e = CatalogEntry(
            surface="css",
            name="css/alignItems",
            status="partial",
            maps_to="setFlex(id, 'align_items', value)",
            supported_values=["flex-start", "flex-end", "center", "stretch"],
            unsupported_values=["baseline"],
        )
        result = self.adapter.run(e)
        self.assertEqual(result.status, Status.DIVERGE, msg=result.detail)
        self.assertIn("baseline", result.unmatched_supported)
        self.assertEqual(
            set(result.matched_supported),
            {"flex-start", "flex-end", "center", "stretch"},
        )

    def test_supported_with_unsupported_values_is_DIVERGE(self):
        """A non-enum 'supported' prop with unsupportedValues populated
        is real drift — the catalog over-claims."""
        e = CatalogEntry(
            surface="css",
            name="css/boxShadow",
            status="supported",
            maps_to="parseCSSColor -> setBoxShadow(id, ...)",
            supported_values=["single shadow"],
            unsupported_values=["multiple shadows"],
        )
        result = self.adapter.run(e)
        self.assertEqual(result.status, Status.DIVERGE, msg=result.detail)
        self.assertIn("multiple shadows", result.extra_unsupported)
        self.assertTrue(result.drifts)

    def test_supported_but_not_wired_is_NOT_IMPL_with_drift(self):
        """`backdropFilter` and `backfaceVisibility` are flagged supported
        in the catalog but not wired through web-compat-style-decl.js — the
        harness must surface this as drift."""
        e = CatalogEntry(
            surface="css",
            name="css/backdropFilter",
            status="supported",
            maps_to="setBackdropFilter(id, blur_px)",
        )
        result = self.adapter.run(e)
        self.assertEqual(result.status, Status.NOT_IMPL, msg=result.detail)
        self.assertTrue(result.drifts, "supported-but-not-wired must drift")

    # ── NOT_IMPL-shaped entries ──────────────────────────────────────

    def test_missing_with_no_branch_marker_is_NOT_IMPL(self):
        """The `no branch` marker is the catalog vocabulary for 'unwired'."""
        e = CatalogEntry(
            surface="css",
            name="css/animationPlayState",
            status="missing",
            maps_to="no branch",
        )
        result = self.adapter.run(e)
        self.assertEqual(result.status, Status.NOT_IMPL, msg=result.detail)
        self.assertFalse(result.drifts, msg=result.detail)

    def test_missing_with_silently_dropped_is_NOT_IMPL(self):
        """alignContent is the canonical 'silently dropped' entry — switch
        case is missing, value is silently discarded."""
        e = CatalogEntry(
            surface="css",
            name="css/alignContent",
            status="missing",
            maps_to=(
                "web-compat calls setFlex(id, 'align_content', ...) but "
                "setFlex switch has NO 'align_content' branch"
            ),
        )
        result = self.adapter.run(e)
        # alignContent IS wired in the JS now (we verified — it has a
        # `case "alignContent":`), so the harness should classify it NOT_IMPL
        # via the no-supported-values branch rather than the no-branch one.
        # Either NOT_IMPL classification is acceptable; both reflect the
        # catalog's missing claim.
        self.assertEqual(result.status, Status.NOT_IMPL, msg=result.detail)

    # ── OOS-shaped entries ───────────────────────────────────────────

    def test_wontfix_is_OOS(self):
        """All 31 wontfix entries must be OOS regardless of any other field."""
        e = CatalogEntry(
            surface="css",
            name="css/all",
            status="wontfix",
            maps_to="n/a",
            unsupported_values=["initial", "inherit", "unset", "revert"],
        )
        result = self.adapter.run(e)
        self.assertEqual(result.status, Status.OOS)
        self.assertFalse(result.drifts)

    def test_non_mdn_unwired_is_OOS(self):
        """A made-up property that's not in MDN and not wired is OOS."""
        e = CatalogEntry(
            surface="css",
            name="css/madeUpCssProperty",
            status="missing",
            maps_to="placeholder",
        )
        result = self.adapter.run(e)
        self.assertEqual(result.status, Status.OOS)

    # ── NO-OP-shaped entries ─────────────────────────────────────────

    def test_no_op_marker_is_NO_OP(self):
        """The `stored on element` / `accepted silently` markers are the
        canonical NO-OP signal — touchAction is exactly this case."""
        e = CatalogEntry(
            surface="css",
            name="css/touchAction",
            status="partial",
            maps_to="stored on element instance; not propagated to bridge",
        )
        result = self.adapter.run(e)
        self.assertEqual(result.status, Status.NO_OP)

    # ── Synthetic entries (`__*`) ────────────────────────────────────

    def test_synthetic_entry_passes_through_catalog_status(self):
        """css/__hover_pseudo and css/__pseudo_classes_note represent
        CSS *features*, not properties. The harness should mirror the
        catalog status without trying to look up an oracle."""
        for status, expected in [
            ("supported", Status.PASS),
            ("missing", Status.NOT_IMPL),
            ("wontfix", Status.OOS),
        ]:
            with self.subTest(status=status):
                e = CatalogEntry(
                    surface="css",
                    name="css/__test_synthetic",
                    status=status,
                    maps_to="synthetic feature",
                )
                result = self.adapter.run(e)
                self.assertEqual(result.status, expected, msg=result.detail)

    # ── Wiring extraction ────────────────────────────────────────────

    def test_wired_set_includes_known_routes(self):
        """The adapter parses `case \"X\":` keys out of
        web-compat-style-decl.js. Sanity-check that the obvious routes
        landed."""
        for prop in (
            "display",
            "flexDirection",
            "alignItems",
            "alignContent",
            "backgroundColor",
            "boxShadow",
            "transform",
            "marginTop",
            "borderRadius",
            "opacity",
        ):
            with self.subTest(prop=prop):
                self.assertIn(
                    prop,
                    self.adapter._wired,
                    msg=f"{prop!r} should be wired in web-compat-style-decl.js",
                )

    def test_wired_set_excludes_unwired_props(self):
        """Conversely, the adapter must not mis-claim wiring for props
        the catalog says are missing — `borderStyle` has no JS case."""
        for prop in (
            "borderStyle",
            "clipPath",
            "mask",
            "writingMode",
            "verticalAlign",
        ):
            with self.subTest(prop=prop):
                self.assertNotIn(
                    prop,
                    self.adapter._wired,
                    msg=f"{prop!r} unexpectedly classified as wired",
                )


class CssVerifierEndToEndTest(unittest.TestCase):
    """Full pipeline — compat.json -> all 195 css entries -> coverage report."""

    def test_collects_all_195_css_entries(self):
        compat = load_compat(REPO_ROOT)
        entries = collect_entries(compat, "css")
        self.assertEqual(len(entries), 195, "compat.json css/ must have 195 entries")

    def test_runs_css_surface_end_to_end(self):
        results = run_surface(REPO_ROOT, "css")
        self.assertEqual(len(results), 195)
        for r in results:
            self.assertIsInstance(r.status, Status)

    def test_no_css_entry_crashes(self):
        """All 195 css catalog entries must classify without exception."""
        compat = load_compat(REPO_ROOT)
        entries = collect_entries(compat, "css")
        adapter = CssAdapter(REPO_ROOT)
        for e in entries:
            r = adapter.run(e)
            self.assertIsInstance(r.status, Status, msg=e.name)

    def test_coverage_distribution_is_nonzero(self):
        """Sanity: with the catalog as-is, we have at least some entries
        in PASS / DIVERGE / OOS buckets. Otherwise the harness is broken.
        (NOT-IMPL is no longer required to be > 0 — pulp #1615 closed
        the last NOT-IMPL css entries by reconciling catalog claims with
        the oracle.)"""
        results = run_surface(REPO_ROOT, "css")
        statuses = [r.status for r in results]
        counts = StatusCounts.from_results(statuses)
        self.assertGreater(counts.pass_, 0, "expected at least 1 PASS")
        self.assertGreater(counts.diverge, 0, "expected at least 1 DIVERGE")
        self.assertGreater(counts.oos, 0, "expected at least 1 OOS")

    def test_wontfix_count_matches_catalog(self):
        """All 31 wontfix entries must produce OOS verdicts. Anything
        else means the wontfix short-circuit broke."""
        results = run_surface(REPO_ROOT, "css")
        wontfix_oos = sum(
            1 for r in results
            if r.entry.status == "wontfix" and r.status is Status.OOS
        )
        wontfix_total = sum(
            1 for r in results if r.entry.status == "wontfix"
        )
        self.assertEqual(wontfix_oos, wontfix_total)
        self.assertEqual(wontfix_total, 31, "compat.json css/ wontfix count drift")

    def test_json_output_contains_all_entries(self):
        """JSON shape stability — `total` matches `len(results)`."""
        from tools.harness.verifier import render_json
        results = run_surface(REPO_ROOT, "css")
        payload = render_json({"css": results}, sha="test")
        self.assertIn("css", payload["surfaces"])
        self.assertEqual(payload["surfaces"]["css"]["total"], 195)
        self.assertEqual(
            payload["surfaces"]["css"]["total"],
            len(payload["surfaces"]["css"]["results"]),
        )


class CssVerifierCliTest(unittest.TestCase):
    """Smoke-tests the CLI entrypoint via subprocess."""

    def test_css_surface_via_cli_exits_zero(self):
        env = os.environ.copy()
        env["PYTHONPATH"] = str(REPO_ROOT)
        result = subprocess.run(
            [
                sys.executable,
                str(REPO_ROOT / "tools" / "harness" / "verifier.py"),
                "--surface=css",
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
        self.assertEqual(payload["surfaces"]["css"]["total"], 195)


if __name__ == "__main__":
    unittest.main()
