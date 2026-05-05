"""Tests for the HTML catalog harness adapter.

Per CLAUDE.md "tests ship with fixes" — this is the same-PR test surface
for the HTML adapter (#1392 partial, week 1 cut). Mirrors
`test_yoga_adapter.py` so the test conventions stay aligned across
parallel adapters (#1391 / #1392 / #1393).

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
from tools.harness.adapters.html import HtmlAdapter  # noqa: E402
from tools.harness.status import Status, StatusCounts  # noqa: E402
from tools.harness.verifier import (  # noqa: E402
    collect_entries,
    load_compat,
    run_surface,
)


class HtmlAdapterClassifyTest(unittest.TestCase):
    """Classify each catalog status family on known-good and known-bad fixtures."""

    @classmethod
    def setUpClass(cls):
        cls.adapter = HtmlAdapter(REPO_ROOT)

    # ── Known-good entries ───────────────────────────────────────────

    def test_element_method_with_no_unsupported_is_PASS(self):
        """`Element_classList` is the canonical supported entry —
        `Object.defineProperty(Element.prototype, 'classList', ...)` exists
        and the catalog records no unsupported values."""
        e = CatalogEntry(
            surface="html",
            name="html/Element_classList",
            status="supported",
            maps_to="Element.classList — add/remove/toggle/contains.",
            supported_values=[],
            unsupported_values=[],
        )
        result = self.adapter.run(e)
        self.assertEqual(result.status, Status.PASS, msg=result.detail)
        self.assertFalse(result.drifts, msg=result.detail)

    def test_html_tag_with_full_bridge_is_PASS(self):
        """`<div>` wires through `_ensureNative` to `createCol` — and
        `createCol` is registered in widget_bridge.cpp. Both halves pass,
        so the row is PASS."""
        e = CatalogEntry(
            surface="html",
            name="html/div",
            status="supported",
            maps_to="Element('div')._ensureNative -> createCol(id, parent).",
            supported_values=["plain container"],
            unsupported_values=[],
        )
        result = self.adapter.run(e)
        self.assertEqual(result.status, Status.PASS, msg=result.detail)

    def test_document_member_is_PASS(self):
        e = CatalogEntry(
            surface="html",
            name="html/document_getElementById",
            status="supported",
            maps_to="document.getElementById -> __elements__['#'+id] lookup.",
            supported_values=[],
            unsupported_values=[],
        )
        result = self.adapter.run(e)
        self.assertEqual(result.status, Status.PASS, msg=result.detail)

    # ── DIVERGE entries (binding present + listed gaps) ──────────────

    def test_element_method_with_unsupported_values_is_DIVERGE(self):
        """`addEventListener` is wired but the catalog lists a few event
        types that route differently. The harness must say DIVERGE so the
        gap shows up in the drift list."""
        e = CatalogEntry(
            surface="html",
            name="html/Element_addEventListener",
            status="supported",
            maps_to="Element.addEventListener — registers click/mouseenter/...",
            supported_values=["click", "mouseenter"],
            unsupported_values=[
                "dragstart/drag/dragend (use registerDrop)",
                "wheel (use registerWheel)",
            ],
        )
        result = self.adapter.run(e)
        self.assertEqual(result.status, Status.DIVERGE, msg=result.detail)
        self.assertEqual(len(result.extra_unsupported), 2)

    def test_html_tag_with_unsupported_values_is_DIVERGE(self):
        """`<style>` is wired, but the catalog lists @media/@import/etc. as
        unsupported — the harness must surface that as DIVERGE."""
        e = CatalogEntry(
            surface="html",
            name="html/style",
            status="supported",
            maps_to="Element('style')._ensureNative -> createCol + setVisible(false).",
            supported_values=["tag selectors"],
            unsupported_values=["@media", "@import", "@keyframes"],
        )
        result = self.adapter.run(e)
        self.assertEqual(result.status, Status.DIVERGE, msg=result.detail)

    # ── Known-bad entries ────────────────────────────────────────────

    def test_missing_with_unimpl_marker_is_NOT_IMPL(self):
        """`html/ARIA` is the canonical missing entry — `mapsTo` says
        'NOT routed to platform accessibility APIs'."""
        e = CatalogEntry(
            surface="html",
            name="html/ARIA",
            status="missing",
            maps_to=(
                "ARIA attributes (aria-label, role, etc.) are stored in "
                "_attributes but NOT routed to platform accessibility APIs."
            ),
            supported_values=[],
            unsupported_values=["all accessibility consumers"],
        )
        result = self.adapter.run(e)
        self.assertEqual(result.status, Status.NOT_IMPL, msg=result.detail)
        self.assertFalse(result.drifts, msg=result.detail)

    def test_unknown_oracle_key_is_OOS(self):
        """A catalog row the oracle doesn't know about is OOS, not PASS."""
        e = CatalogEntry(
            surface="html",
            name="html/madeUpThing",
            status="supported",
            maps_to="placeholder",
        )
        result = self.adapter.run(e)
        self.assertEqual(result.status, Status.OOS)

    def test_wontfix_is_OOS(self):
        e = CatalogEntry(
            surface="html",
            name="html/Element_classList",
            status="wontfix",
            maps_to="explicitly out of scope",
        )
        result = self.adapter.run(e)
        self.assertEqual(result.status, Status.OOS)

    # ── Feature-category entries (ARIA, StyleSheet_inline, etc.) ─────

    def test_feature_partial_is_DIVERGE(self):
        e = CatalogEntry(
            surface="html",
            name="html/DocumentFragment",
            status="partial",
            maps_to="Minimal shim — appendChild/removeChild only.",
            supported_values=[],
            unsupported_values=["full Range / Selection API"],
        )
        result = self.adapter.run(e)
        self.assertEqual(result.status, Status.DIVERGE, msg=result.detail)

    def test_feature_supported_with_unsupported_values_is_DIVERGE(self):
        """`StyleSheet_inline` is `supported` but lists @media/@import/etc.
        as unsupported — the harness must report DIVERGE."""
        e = CatalogEntry(
            surface="html",
            name="html/StyleSheet_inline",
            status="supported",
            maps_to="Inline <style> elements parsed and applied to the document.",
            supported_values=["tag", ".class", "#id"],
            unsupported_values=["@media", "@keyframes", "calc()"],
        )
        result = self.adapter.run(e)
        self.assertEqual(result.status, Status.DIVERGE, msg=result.detail)

    # ── DOM-mutation methods live in dom-ops.js, not element.js ──────

    def test_dom_op_methods_are_recognized(self):
        """appendChild/removeChild/insertBefore/replaceChild are patched
        onto Element.prototype in web-compat-dom-ops.js, NOT in
        web-compat-element.js. The adapter MUST search both files or the
        DOM-mutation methods all false-positive as NOT-IMPL."""
        for member in ("appendChild", "removeChild", "insertBefore", "replaceChild"):
            e = CatalogEntry(
                surface="html",
                name=f"html/Element_{member}",
                status="supported",
                maps_to="web-compat-dom-ops.js.",
                supported_values=[],
                unsupported_values=[],
            )
            with self.subTest(member=member):
                result = self.adapter.run(e)
                self.assertEqual(
                    result.status,
                    Status.PASS,
                    msg=f"{member}: {result.status} ({result.detail})",
                )


class HtmlVerifierEndToEndTest(unittest.TestCase):
    """Full pipeline — compat.json -> all html entries -> coverage report."""

    def test_collects_all_html_entries(self):
        compat = load_compat(REPO_ROOT)
        entries = collect_entries(compat, "html")
        # 60 keys in compat.json["html"], minus the `_note` metadata string.
        self.assertEqual(len(entries), 59, "compat.json html/ must yield 59 dict entries")

    def test_runs_html_surface_end_to_end(self):
        results = run_surface(REPO_ROOT, "html")
        self.assertEqual(len(results), 59)
        for r in results:
            self.assertIsInstance(r.status, Status)

    def test_no_html_entry_crashes(self):
        compat = load_compat(REPO_ROOT)
        entries = collect_entries(compat, "html")
        adapter = HtmlAdapter(REPO_ROOT)
        for e in entries:
            r = adapter.run(e)
            self.assertIsInstance(r.status, Status, msg=e.name)

    def test_coverage_distribution_is_nonzero(self):
        """Sanity: with the catalog as-is, we have at least some PASS and
        some DIVERGE entries. (We don't require NOT-IMPL to be non-zero —
        the html surface is more mature than yoga, but we DO require ARIA
        to classify as NOT-IMPL.)"""
        results = run_surface(REPO_ROOT, "html")
        statuses = [r.status for r in results]
        counts = StatusCounts.from_results(statuses)
        self.assertGreater(counts.pass_, 0, "expected at least 1 PASS")
        self.assertGreater(counts.diverge, 0, "expected at least 1 DIVERGE")
        # ARIA is documented missing — assert its specific verdict.
        aria = next(r for r in results if r.entry.name == "html/ARIA")
        self.assertEqual(aria.status, Status.NOT_IMPL)

    def test_json_output_contains_all_entries(self):
        from tools.harness.verifier import render_json

        results = run_surface(REPO_ROOT, "html")
        payload = render_json({"html": results}, sha="test")
        self.assertIn("html", payload["surfaces"])
        self.assertEqual(payload["surfaces"]["html"]["total"], 59)
        self.assertEqual(
            payload["surfaces"]["html"]["total"],
            len(payload["surfaces"]["html"]["results"]),
        )


class HtmlVerifierCliTest(unittest.TestCase):
    """Smoke-tests the CLI entrypoint via subprocess for the html surface."""

    def test_json_subcommand_exits_zero(self):
        env = os.environ.copy()
        env["PYTHONPATH"] = str(REPO_ROOT)
        result = subprocess.run(
            [
                sys.executable,
                str(REPO_ROOT / "tools" / "harness" / "verifier.py"),
                "--surface=html",
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
        self.assertEqual(payload["surfaces"]["html"]["total"], 59)


if __name__ == "__main__":
    unittest.main()
