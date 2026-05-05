"""Tests for the yoga catalog harness adapter.

Per CLAUDE.md "tests ship with fixes" — this is the same-PR test surface
for the verifier core (#1391) and yoga adapter (#1392, week 1 cut).

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
from tools.harness.adapters.yoga import YogaAdapter  # noqa: E402
from tools.harness.status import Status, StatusCounts  # noqa: E402
from tools.harness.verifier import (  # noqa: E402
    collect_entries,
    load_compat,
    main as verifier_main,
    run_surface,
)


class YogaAdapterClassifyTest(unittest.TestCase):
    """Classify each catalog status family on known-good and known-bad fixtures."""

    @classmethod
    def setUpClass(cls):
        cls.adapter = YogaAdapter(REPO_ROOT)

    # ── Known-good entries ───────────────────────────────────────────

    def test_supported_full_enum_is_PASS(self):
        """flexGrow is a `supported` numeric prop with no unsupported values
        listed — the harness must say PASS."""
        e = CatalogEntry(
            surface="yoga",
            name="yoga/flexGrow",
            status="supported",
            maps_to="FlexStyle.flex_grow (float)",
            supported_values=["any number"],
            unsupported_values=[],
        )
        result = self.adapter.run(e)
        self.assertEqual(result.status, Status.PASS, msg=result.detail)
        self.assertFalse(result.drifts, msg=result.detail)

    def test_partial_enum_is_DIVERGE(self):
        """flexDirection is `partial` with 2 of Yoga's 4 values supported."""
        e = CatalogEntry(
            surface="yoga",
            name="yoga/flexDirection",
            status="partial",
            maps_to="FlexStyle.direction",
            supported_values=["row", "column"],
            unsupported_values=["row-reverse", "column-reverse"],
        )
        result = self.adapter.run(e)
        self.assertEqual(result.status, Status.DIVERGE, msg=result.detail)
        self.assertEqual(set(result.matched_supported), {"row", "column"})
        self.assertIn("row-reverse", result.unmatched_supported)
        self.assertIn("column-reverse", result.unmatched_supported)

    # ── Known-bad entries ────────────────────────────────────────────

    def test_missing_with_no_field_marker_is_NOT_IMPL(self):
        """alignContent is the canonical missing entry — `mapsTo` says
        'no field on FlexStyle and no setFlex case'."""
        e = CatalogEntry(
            surface="yoga",
            name="yoga/alignContent",
            status="missing",
            maps_to="no field on FlexStyle and no setFlex case for 'align_content'",
            supported_values=[],
            unsupported_values=[],
        )
        result = self.adapter.run(e)
        self.assertEqual(result.status, Status.NOT_IMPL, msg=result.detail)
        self.assertFalse(result.drifts, msg=result.detail)

    def test_wontfix_is_OOS(self):
        e = CatalogEntry(
            surface="yoga",
            name="yoga/flex",
            status="wontfix",
            maps_to="explicitly out of scope",
        )
        result = self.adapter.run(e)
        self.assertEqual(result.status, Status.OOS)

    def test_property_not_in_oracle_is_OOS(self):
        """A made-up property the Yoga oracle doesn't define is OOS."""
        e = CatalogEntry(
            surface="yoga",
            name="yoga/madeUpProp",
            status="missing",
            maps_to="placeholder",
        )
        result = self.adapter.run(e)
        self.assertEqual(result.status, Status.OOS)

    def test_no_op_marker_is_NO_OP(self):
        e = CatalogEntry(
            surface="yoga",
            name="yoga/aspectRatio",
            status="partial",
            maps_to="accepted silently for now",
        )
        result = self.adapter.run(e)
        self.assertEqual(result.status, Status.NO_OP)

    def test_view_accessor_counts_as_binding(self):
        """top/right/bottom/left are bound through View accessors, not
        FlexStyle; the adapter must recognize this so they aren't
        misclassified as NOT-IMPL."""
        for prop in ("top", "right", "bottom", "left"):
            e = CatalogEntry(
                surface="yoga",
                name=f"yoga/{prop}",
                status="supported",
                maps_to=f"View::set_{prop}",
                supported_values=["px"],
                unsupported_values=[],
            )
            with self.subTest(prop=prop):
                result = self.adapter.run(e)
                self.assertIn(
                    result.status,
                    {Status.PASS, Status.DIVERGE},
                    msg=f"{prop}: {result.status} ({result.detail})",
                )

    def test_pass_requires_all_oracle_values_supported(self):
        """A new prop claimed `supported` but missing oracle values stays DIVERGE."""
        e = CatalogEntry(
            surface="yoga",
            name="yoga/justifyContent",
            status="supported",
            maps_to="FlexStyle.justify_content",
            supported_values=[
                "center",
                "space-between",
                "space-around",
                "space-evenly",
            ],
            unsupported_values=[],
        )
        result = self.adapter.run(e)
        self.assertEqual(result.status, Status.DIVERGE, msg=result.detail)
        self.assertTrue(result.drifts)


class VerifierEndToEndTest(unittest.TestCase):
    """Full pipeline — compat.json -> all 53 yoga entries -> coverage report."""

    def test_collects_all_53_yoga_entries(self):
        compat = load_compat(REPO_ROOT)
        entries = collect_entries(compat, "yoga")
        self.assertEqual(len(entries), 53, "compat.json yoga/ must have 53 entries")

    def test_runs_yoga_surface_end_to_end(self):
        results = run_surface(REPO_ROOT, "yoga")
        self.assertEqual(len(results), 53)
        # Every result must have a Status enum (no exceptions / Nones)
        for r in results:
            self.assertIsInstance(r.status, Status)

    def test_no_yoga_entry_crashes(self):
        """All 53 yoga catalog entries must classify without exception."""
        compat = load_compat(REPO_ROOT)
        entries = collect_entries(compat, "yoga")
        adapter = YogaAdapter(REPO_ROOT)
        for e in entries:
            r = adapter.run(e)
            self.assertIsInstance(r.status, Status, msg=e.name)

    def test_coverage_distribution_is_nonzero(self):
        """Sanity: with the catalog as-is, we have at least some PASS, some
        DIVERGE, and some NOT-IMPL. Otherwise the harness is broken."""
        results = run_surface(REPO_ROOT, "yoga")
        statuses = [r.status for r in results]
        counts = StatusCounts.from_results(statuses)
        self.assertGreater(counts.pass_, 0, "expected at least 1 PASS")
        self.assertGreater(counts.diverge, 0, "expected at least 1 DIVERGE")
        self.assertGreater(counts.not_impl, 0, "expected at least 1 NOT-IMPL")

    def test_json_output_contains_all_entries(self):
        results = run_surface(REPO_ROOT, "yoga")
        # Build a json-shaped dict ourselves rather than re-running main()
        from tools.harness.verifier import render_json

        payload = render_json({"yoga": results}, sha="test")
        self.assertIn("yoga", payload["surfaces"])
        self.assertEqual(payload["surfaces"]["yoga"]["total"], 53)
        self.assertEqual(payload["surfaces"]["yoga"]["total"],
                         len(payload["surfaces"]["yoga"]["results"]))


class VerifierCliTest(unittest.TestCase):
    """Smoke-tests the CLI entrypoint via subprocess."""

    def test_json_subcommand_exits_zero(self):
        env = os.environ.copy()
        env["PYTHONPATH"] = str(REPO_ROOT)
        result = subprocess.run(
            [
                sys.executable,
                str(REPO_ROOT / "tools" / "harness" / "verifier.py"),
                "--surface=yoga",
                "--json",
            ],
            cwd=REPO_ROOT,
            env=env,
            capture_output=True,
            text=True,
            timeout=60,
        )
        self.assertEqual(result.returncode, 0, msg=result.stderr)
        # stdout must be parseable JSON
        payload = json.loads(result.stdout)
        self.assertEqual(payload["surfaces"]["yoga"]["total"], 53)

    def test_unknown_surface_errors_cleanly(self):
        env = os.environ.copy()
        env["PYTHONPATH"] = str(REPO_ROOT)
        result = subprocess.run(
            [
                sys.executable,
                str(REPO_ROOT / "tools" / "harness" / "verifier.py"),
                "--surface=does-not-exist",
                "--json",
            ],
            cwd=REPO_ROOT,
            env=env,
            capture_output=True,
            text=True,
            timeout=30,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("does-not-exist", result.stderr)


if __name__ == "__main__":
    unittest.main()
