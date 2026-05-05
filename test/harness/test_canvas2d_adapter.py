"""Tests for the canvas2d catalog harness adapter.

Per CLAUDE.md "tests ship with fixes" — this is the same-PR test surface
for the canvas2d adapter (#1392, week 1 cut, fourth surface).

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
from tools.harness.adapters.canvas2d import Canvas2dAdapter  # noqa: E402
from tools.harness.status import Status, StatusCounts  # noqa: E402
from tools.harness.verifier import (  # noqa: E402
    collect_entries,
    load_compat,
    run_surface,
)


class Canvas2dAdapterClassifyTest(unittest.TestCase):
    """Classify each catalog status family on known-good and known-bad fixtures."""

    @classmethod
    def setUpClass(cls):
        cls.adapter = Canvas2dAdapter(REPO_ROOT)

    # ── Known-good entries ───────────────────────────────────────────

    def test_supported_method_is_PASS(self):
        """fillRect: supported, mapsTo cites canvasRect (which is registered)."""
        e = CatalogEntry(
            surface="canvas2d",
            name="canvas2d/fillRect",
            status="supported",
            maps_to="ctx.fillRect(x,y,w,h) -> canvasRect(id,x,y,w,h).",
            supported_values=["any rectangle"],
            unsupported_values=[],
        )
        result = self.adapter.run(e)
        self.assertEqual(result.status, Status.PASS, msg=result.detail)
        self.assertFalse(result.drifts, msg=result.detail)

    def test_supported_attribute_is_PASS(self):
        """lineWidth: bridge has canvasSetLineWidth, no unsupported values."""
        e = CatalogEntry(
            surface="canvas2d",
            name="canvas2d/lineWidth",
            status="supported",
            maps_to="canvasSetLineWidth(id, value)",
            supported_values=[],
            unsupported_values=[],
        )
        result = self.adapter.run(e)
        self.assertEqual(result.status, Status.PASS, msg=result.detail)

    def test_pulp_extension_is_PASS(self):
        """_native_canvasFillCircle: Pulp-only convenience; bridge canvasFillCircle exists."""
        e = CatalogEntry(
            surface="canvas2d",
            name="canvas2d/_native_canvasFillCircle",
            status="supported",
            maps_to="Pulp-specific bridge fn (canvasFillCircle).",
        )
        result = self.adapter.run(e)
        self.assertEqual(result.status, Status.PASS, msg=result.detail)

    # ── Oracle-pinned partial (gotcha) ───────────────────────────────

    def test_oracle_partial_arc_is_DIVERGE(self):
        """ctx.arc is pinned at partial in the oracle (gotcha #1: arc-as-path)."""
        e = CatalogEntry(
            surface="canvas2d",
            name="canvas2d/arc",
            status="partial",
            maps_to="ctx.arc -> path-mode emit as cubic-bezier via canvasMoveTo + canvasCubicTo.",
        )
        result = self.adapter.run(e)
        self.assertEqual(result.status, Status.DIVERGE, msg=result.detail)
        self.assertIn("arc-as-path", result.detail)
        self.assertFalse(result.drifts, msg=result.detail)

    def test_oracle_partial_radial_is_DIVERGE(self):
        """createRadialGradient is pinned at partial (gotcha #3: single-circle)."""
        e = CatalogEntry(
            surface="canvas2d",
            name="canvas2d/createRadialGradient",
            status="partial",
            maps_to="ctx.createRadialGradient — bridge takes single circle (cx,cy,r).",
        )
        result = self.adapter.run(e)
        self.assertEqual(result.status, Status.DIVERGE, msg=result.detail)
        self.assertIn("radial-single-circle", result.detail)

    # ── Oracle-pinned missing ────────────────────────────────────────

    def test_oracle_missing_conic_is_NOT_IMPL(self):
        """createConicGradient: SKILL gotcha #5 — no canvasSetConicGradient registered."""
        e = CatalogEntry(
            surface="canvas2d",
            name="canvas2d/createConicGradient",
            status="missing",
            maps_to="ctx.createConicGradient -> shim returns an empty linear gradient.",
        )
        result = self.adapter.run(e)
        self.assertEqual(result.status, Status.NOT_IMPL, msg=result.detail)
        self.assertFalse(result.drifts, msg=result.detail)

    def test_oracle_missing_shadow_is_NOT_IMPL(self):
        """shadowBlur — not implemented in shim or bridge."""
        e = CatalogEntry(
            surface="canvas2d",
            name="canvas2d/shadowBlur",
            status="missing",
            maps_to="Not implemented.",
        )
        result = self.adapter.run(e)
        self.assertEqual(result.status, Status.NOT_IMPL, msg=result.detail)

    # ── Catalog `mapsTo` heuristics ──────────────────────────────────

    def test_unimpl_mapsTo_is_NOT_IMPL(self):
        """An entry whose mapsTo says 'Not implemented' is NOT-IMPL even
        if the oracle didn't pre-mark it missing."""
        e = CatalogEntry(
            surface="canvas2d",
            name="canvas2d/filter",
            status="missing",
            maps_to="Not implemented in shim or bridge.",
        )
        result = self.adapter.run(e)
        self.assertEqual(result.status, Status.NOT_IMPL, msg=result.detail)

    def test_wontfix_is_OOS(self):
        e = CatalogEntry(
            surface="canvas2d",
            name="canvas2d/fillRect",
            status="wontfix",
            maps_to="explicitly out of scope",
        )
        result = self.adapter.run(e)
        self.assertEqual(result.status, Status.OOS)

    def test_entry_not_in_oracle_is_OOS(self):
        """A made-up canvas2d entry the oracle doesn't define is OOS."""
        e = CatalogEntry(
            surface="canvas2d",
            name="canvas2d/madeUpMethod",
            status="missing",
            maps_to="placeholder",
        )
        result = self.adapter.run(e)
        self.assertEqual(result.status, Status.OOS)

    # ── DIVERGE detection ────────────────────────────────────────────

    def test_supported_with_unsupported_values_is_DIVERGE(self):
        """fillStyle: catalog says supported but lists CanvasPattern as
        unsupported — that's a drift signal the harness must catch."""
        e = CatalogEntry(
            surface="canvas2d",
            name="canvas2d/fillStyle",
            status="supported",
            maps_to="canvasSetFillColor or canvasSetLinearGradient",
            supported_values=["solid color", "CanvasGradient"],
            unsupported_values=["CanvasPattern"],
        )
        result = self.adapter.run(e)
        self.assertEqual(result.status, Status.DIVERGE, msg=result.detail)
        self.assertTrue(result.drifts, msg=result.detail)

    def test_mapsTo_cites_unregistered_bridge_fn_is_DIVERGE(self):
        """If catalog claims a route through canvasNonExistent (not registered),
        that's a strong DIVERGE — catalog and bridge disagree."""
        e = CatalogEntry(
            surface="canvas2d",
            name="canvas2d/measureText",
            status="supported",
            maps_to="ctx.measureText -> canvasNonExistentFakeFn(id, text)",
        )
        result = self.adapter.run(e)
        self.assertEqual(result.status, Status.DIVERGE, msg=result.detail)
        self.assertIn("canvasNonExistentFakeFn", str(result.extra_unsupported))


class Canvas2dBridgeAndShimIntrospectionTest(unittest.TestCase):
    """Verify the adapter actually reads bridge + shim correctly."""

    @classmethod
    def setUpClass(cls):
        cls.adapter = Canvas2dAdapter(REPO_ROOT)

    def test_bridge_fns_extracted(self):
        """Sanity: known canvas* register_function calls land in the set."""
        for fn in ("canvasRect", "canvasFillPath", "canvasSetFont", "canvasMoveTo"):
            with self.subTest(fn=fn):
                self.assertIn(fn, self.adapter._bridge_fns, msg=f"{fn} missing from bridge fn set")

    def test_shim_methods_extracted(self):
        """Sanity: known shim CRC2D methods land in the set."""
        for m in ("fillRect", "stroke", "arc", "createLinearGradient", "addColorStop"):
            with self.subTest(method=m):
                self.assertIn(m, self.adapter._shim_methods, msg=f"{m} missing from shim method set")

    def test_no_phantom_bridge_fn(self):
        """Negative: a clearly-fake bridge fn name must NOT appear."""
        self.assertNotIn("canvasTotallyFakeFn", self.adapter._bridge_fns)


class VerifierEndToEndTest(unittest.TestCase):
    """Full pipeline — compat.json -> all 63 canvas2d entries -> coverage report."""

    def test_collects_all_canvas2d_entries(self):
        compat = load_compat(REPO_ROOT)
        entries = collect_entries(compat, "canvas2d")
        # canvas2d has 63 real entries (the `_note` key is skipped by the
        # entry collector since it's a string, not a dict payload).
        self.assertEqual(
            len(entries),
            63,
            f"compat.json canvas2d/ must have 63 real entries (got {len(entries)})",
        )

    def test_runs_canvas2d_surface_end_to_end(self):
        results = run_surface(REPO_ROOT, "canvas2d")
        self.assertEqual(len(results), 63)
        # Every result must have a Status enum (no exceptions / Nones)
        for r in results:
            self.assertIsInstance(r.status, Status)

    def test_no_canvas2d_entry_crashes(self):
        """All 63 catalog entries must classify without exception."""
        compat = load_compat(REPO_ROOT)
        entries = collect_entries(compat, "canvas2d")
        adapter = Canvas2dAdapter(REPO_ROOT)
        for e in entries:
            r = adapter.run(e)
            self.assertIsInstance(r.status, Status, msg=e.name)

    def test_coverage_distribution_is_nonzero(self):
        """Sanity: with the catalog as-is, we have at least some PASS, some
        DIVERGE, and some NOT-IMPL. Otherwise the harness is broken."""
        results = run_surface(REPO_ROOT, "canvas2d")
        statuses = [r.status for r in results]
        counts = StatusCounts.from_results(statuses)
        self.assertGreater(counts.pass_, 0, "expected at least 1 PASS")
        self.assertGreater(counts.diverge, 0, "expected at least 1 DIVERGE")
        self.assertGreater(counts.not_impl, 0, "expected at least 1 NOT-IMPL")

    def test_known_gotcha_entries_classified_DIVERGE(self):
        """The four oracle-pinned gotcha entries must always be DIVERGE.
        Regression guard for the SKILL gotchas catalog."""
        results = run_surface(REPO_ROOT, "canvas2d")
        by_name = {r.entry.name: r for r in results}
        for name in (
            "canvas2d/arc",
            "canvas2d/arcTo",
            "canvas2d/ellipse",
            "canvas2d/createRadialGradient",
            "canvas2d/transform",
        ):
            with self.subTest(entry=name):
                self.assertIn(name, by_name)
                self.assertEqual(
                    by_name[name].status,
                    Status.DIVERGE,
                    msg=f"{name} must be DIVERGE per oracle gotcha pin: {by_name[name].detail}",
                )

    def test_known_unimpl_entries_classified_NOT_IMPL(self):
        """The shadow*, filter, miterLimit, imageSmoothing*, conic, pattern,
        direction entries must always be NOT-IMPL."""
        results = run_surface(REPO_ROOT, "canvas2d")
        by_name = {r.entry.name: r for r in results}
        for name in (
            "canvas2d/createConicGradient",
            "canvas2d/createPattern",
            "canvas2d/shadowBlur",
            "canvas2d/shadowColor",
            "canvas2d/shadowOffsetX",
            "canvas2d/shadowOffsetY",
            "canvas2d/filter",
            "canvas2d/miterLimit",
            "canvas2d/imageSmoothingEnabled",
            "canvas2d/imageSmoothingQuality",
            "canvas2d/direction",
        ):
            with self.subTest(entry=name):
                self.assertIn(name, by_name)
                self.assertEqual(
                    by_name[name].status,
                    Status.NOT_IMPL,
                    msg=f"{name} must be NOT-IMPL: {by_name[name].detail}",
                )

    def test_json_output_contains_all_entries(self):
        from tools.harness.verifier import render_json

        results = run_surface(REPO_ROOT, "canvas2d")
        payload = render_json({"canvas2d": results}, sha="test")
        self.assertIn("canvas2d", payload["surfaces"])
        self.assertEqual(payload["surfaces"]["canvas2d"]["total"], 63)
        self.assertEqual(
            payload["surfaces"]["canvas2d"]["total"],
            len(payload["surfaces"]["canvas2d"]["results"]),
        )


class VerifierCliTest(unittest.TestCase):
    """Smoke-tests the CLI entrypoint via subprocess."""

    def test_canvas2d_subcommand_exits_zero(self):
        env = os.environ.copy()
        env["PYTHONPATH"] = str(REPO_ROOT)
        result = subprocess.run(
            [
                sys.executable,
                str(REPO_ROOT / "tools" / "harness" / "verifier.py"),
                "--surface=canvas2d",
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
        self.assertEqual(payload["surfaces"]["canvas2d"]["total"], 63)


if __name__ == "__main__":
    unittest.main()
