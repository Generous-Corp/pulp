"""Tests for harness coverage report rendering.

Invocation::

    python3 -m unittest tools.harness.tests.test_verify_report
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parent.parent.parent
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools.harness.adapters.base import CatalogEntry, Result  # noqa: E402
from tools.harness.status import Status  # noqa: E402
from tools.harness.verify_report import render_json, render_markdown  # noqa: E402


def _entry(surface: str, name: str, status: str) -> CatalogEntry:
    return CatalogEntry(surface=surface, name=f"{surface}/{name}", status=status)


def _result(
    surface: str,
    name: str,
    catalog_status: str,
    status: Status,
    detail: str = "",
) -> Result:
    return Result(
        entry=_entry(surface, name, catalog_status),
        status=status,
        detail=detail,
    )


class VerifyReportMarkdownTests(unittest.TestCase):
    def test_markdown_renders_summary_buckets_totals_and_drift_list(self) -> None:
        long_detail = "uses | separator " + ("x" * 160)
        results = {
            "css": [
                _result("css", "opacity", "supported", Status.PASS, "ok"),
                _result(
                    "css",
                    "color",
                    "supported",
                    Status.SUPPORTED_NO_EVIDENCE,
                    long_detail,
                ),
            ],
            "yoga": [
                _result("yoga", "gap", "partial", Status.DIVERGE, "partial route"),
                _result("yoga", "float", "wontfix", Status.OOS, "out of scope"),
            ],
        }

        markdown = render_markdown(
            results,
            "abc123",
            visual_counts={"css": {"label": "1/2"}, "yoga": {"label": "0/1"}},
            validation_counts={"css": {"label": "2/2"}, "yoga": {"label": "1/1"}},
        )

        self.assertIn("# Harness coverage", markdown)
        self.assertIn("sha `abc123`", markdown)
        self.assertIn("| `css/` | 2 | 1 | 1 | 0 | 0 | 0 | 0 | 50.0%", markdown)
        self.assertIn("| **TOTAL** | 4 | 1 | 1 | 1 | 0 | 0 | 1 | 25.0%", markdown)
        self.assertIn("### SUPPORTED-NO-EVIDENCE (1)", markdown)
        self.assertIn("uses \\| separator", markdown)
        self.assertIn("### Drift list — `css/` (1 entries)", markdown)
        self.assertIn("| `css/color` | supported (PASS) | SUPPORTED-NO-EVIDENCE |", markdown)

    def test_markdown_single_surface_omits_total_row_and_uses_default_counts(self) -> None:
        markdown = render_markdown(
            {"css": [_result("css", "opacity", "supported", Status.PASS)]},
            "solo",
        )

        self.assertNotIn("| **TOTAL** |", markdown)
        self.assertIn("| `css/` | 1 | 1 | 0 | 0 | 0 | 0 | 0 | 100.0%", markdown)
        self.assertIn("| 0/0 | 0/0 |", markdown)


class VerifyReportJsonTests(unittest.TestCase):
    def test_json_renders_surface_results_totals_and_coverage_aliases(self) -> None:
        results = {
            "css": [
                _result("css", "opacity", "supported", Status.PASS, "ok"),
                _result("css", "color", "supported", Status.SUPPORTED_NO_EVIDENCE),
            ],
            "yoga": [
                _result("yoga", "gap", "partial", Status.DIVERGE),
            ],
        }

        payload = render_json(
            results,
            "abc123",
            visual_counts={"css": {"pass": 1, "total": 2, "label": "1/2"}},
            validation_counts={"css": {"pass": 2, "total": 2, "label": "2/2"}},
        )

        self.assertEqual(payload["schema_version"], "0.2")
        self.assertEqual(payload["sha"], "abc123")
        self.assertEqual(payload["totals"]["total"], 3)
        self.assertEqual(payload["totals"]["pass"], 1)
        self.assertEqual(payload["totals"]["supported_no_evidence"], 1)
        self.assertEqual(payload["totals"]["diverge"], 1)
        self.assertEqual(payload["totals"]["drift_count"], 1)
        self.assertEqual(payload["totals"]["visual_coverage"]["label"], "1/2")
        self.assertIs(payload["totals"]["visual_pass"], payload["totals"]["visual_coverage"])
        self.assertEqual(payload["surfaces"]["css"]["pass_pct"], 50.0)
        self.assertEqual(payload["surfaces"]["css"]["progress_pct"], 100.0)
        self.assertIs(
            payload["surfaces"]["css"]["visual_pass"],
            payload["surfaces"]["css"]["visual_coverage"],
        )
        self.assertTrue(payload["surfaces"]["css"]["results"][1]["drift"])

    def test_json_defaults_missing_visual_and_validation_counts(self) -> None:
        payload = render_json(
            {"css": [_result("css", "opacity", "supported", Status.PASS)]},
            "abc123",
        )

        self.assertEqual(payload["surfaces"]["css"]["visual_coverage"]["label"], "0/0")
        self.assertEqual(payload["surfaces"]["css"]["validation_routes"]["label"], "0/0")
        self.assertEqual(payload["totals"]["visual_coverage"]["label"], "0/0")
        self.assertEqual(payload["totals"]["validation_routes"]["label"], "0/0")


if __name__ == "__main__":
    unittest.main()
