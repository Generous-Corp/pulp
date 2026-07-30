#!/usr/bin/env python3
"""Unit tests for the importer differential lab's stable comparison protocol."""

from __future__ import annotations

from argparse import Namespace
from contextlib import redirect_stderr, redirect_stdout
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    print("SKIP: importer differential lab tests require Pillow")
    raise SystemExit(77)

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools" / "import-validation"))
from importer_differential import core as LAB  # noqa: E402
from importer_differential import cli as LAB_CLI  # noqa: E402


class DifferentialLabTests(unittest.TestCase):
    def test_source_recognition_records_dynamic_blockers(self) -> None:
        recognition = LAB.source_recognition("""
            import {Dialog} from "@radix-ui/react-dialog";
            import {motion} from "framer-motion";
            export default function App() {
              return <canvas className="flex gap-4" />;
            }
        """)
        self.assertEqual(recognition["framework"], "react")
        self.assertTrue(recognition["features"]["radix"])
        self.assertTrue(recognition["features"]["framer_motion"])
        self.assertTrue(recognition["features"]["canvas"])
        self.assertTrue(recognition["features"]["tailwind"])
        self.assertIn(
            "javascript_evaluation", LAB.unsupported_features(recognition))

    def test_generic_type_and_geometry_cannot_force_a_node_match(self) -> None:
        browser = [LAB.BrowserNode(
            1, "div", -1, "", {}, (0, 0, 100, 100), {})]
        native = [LAB.NativeNode(
            "candidate", "frame", "div", "", None, {},
            (0, 0, 100, 100))]
        matches, missing, extra = LAB.match_nodes(browser, native)
        self.assertFalse(matches)
        self.assertEqual(len(missing), 1)
        self.assertEqual(len(extra), 1)

    def test_browser_snapshot_and_native_ir_match_by_text_and_tag(self) -> None:
        strings = [
            "DIV", "H1", "#text", "Hello", "display", "block", "32px",
        ]
        snapshot = {
            "strings": strings,
            "documents": [{
                "nodes": {
                    "parentIndex": [-1, 0, 1],
                    "nodeType": [1, 1, 3],
                    "nodeName": [0, 1, 2],
                    "nodeValue": [-1, -1, 3],
                    "attributes": [[], [], []],
                },
                "layout": {
                    "nodeIndex": [0, 1],
                    "bounds": [[0, 0, 100, 40], [0, 0, 100, 40]],
                    "styles": [[5], [5]],
                },
            }],
        }
        browser = LAB.browser_nodes(snapshot)
        ir = {
            "root": {
                "type": "frame",
                "name": "div",
                "stable_anchor_id": "root",
                "children": [{
                    "type": "text",
                    "name": "h1",
                    "content": "Hello",
                    "stable_anchor_id": "title",
                    "style": {"fontSize": 32},
                    "children": [],
                }],
            },
        }
        layout = {"views": [
            {"anchor_id": "root", "x": 0, "y": 0, "width": 100, "height": 40},
            {"anchor_id": "title", "x": 0, "y": 0, "width": 100, "height": 40},
        ]}
        native = LAB.native_nodes(ir, layout)
        matches, missing, _extra = LAB.match_nodes(browser, native)
        self.assertEqual(len(matches), 1)
        self.assertEqual(len(missing), 1)
        self.assertIn("title", {match[1].anchor for match in matches})

    def test_visual_similarity_is_exact_for_equal_images(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            reference = root / "reference.png"
            candidate = root / "candidate.png"
            Image.new("RGBA", (8, 8), (10, 20, 30, 255)).save(reference)
            Image.new("RGBA", (8, 8), (10, 20, 30, 255)).save(candidate)
            metrics = LAB.visual_metrics(reference, candidate, root / "artifacts")
            self.assertEqual(metrics["score"], 1.0)
            self.assertEqual(metrics["differing_pixel_count"], 0)
            self.assertTrue((root / "artifacts" / "visual-diff.png").is_file())

    def test_visual_lens_rejects_dimensions_and_detects_color_change(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            reference = root / "reference.png"
            candidate = root / "candidate.png"
            Image.new("RGBA", (8, 8), (255, 0, 0, 255)).save(reference)
            Image.new("RGBA", (8, 8), (0, 0, 255, 255)).save(candidate)
            metrics = LAB.visual_metrics(reference, candidate, root / "artifacts")
            self.assertLess(metrics["score"], 0.5)
            Image.new("RGBA", (7, 8), (0, 0, 255, 255)).save(candidate)
            with self.assertRaises(LAB.LabError):
                LAB.visual_metrics(reference, candidate, root / "mismatch")

    def test_missing_typography_counts_as_missing_coverage(self) -> None:
        browser = LAB.BrowserNode(
            1, "span", -1, "Gain", {}, (0, 0, 40, 20),
            {"font-family": "Inter", "font-size": "14px"})
        native = LAB.NativeNode(
            "gain", "text", "span", "Gain", None, {}, (0, 0, 40, 20))
        metrics = LAB.typography_metrics([(browser, native, 4.0)])
        self.assertEqual(metrics["comparable_properties"], 2)
        self.assertEqual(metrics["score"], 0.0)
        self.assertEqual(
            metrics["rows"][0]["differences"][0]["candidate"], "<missing>")

    def test_capture_extent_uses_reference_not_responsive_viewport(self) -> None:
        capture = {
            "reference": {"logical_width": 1440, "logical_height": 1200},
            "provenance": {
                "viewport": {"resolved": {"width": 1280, "height": 800}}},
        }
        self.assertEqual(LAB.capture_logical_size(capture), (1440, 1200))

    def test_intrinsic_sizing_gap_is_emitted(self) -> None:
        recognition = LAB.source_recognition(
            "<!doctype html><html><body><div>Panel</div></body></html>")
        low = {"score": 0.0, "missing_count": 1}
        visual = {"score": 0.0}
        causes = {
            finding["cause"] for finding in LAB.classify_gaps(
                recognition, low, low, low, visual)}
        self.assertIn("intrinsic-sizing", causes)

    def test_promotion_is_conservative_and_advisory(self) -> None:
        perfect = {"score": 1.0}
        result = LAB.promotion_recommendation(
            ["canvas"], perfect, perfect, perfect, perfect)
        self.assertEqual(result["classification"], "browser-required")
        self.assertTrue(result["advisory_only"])
        result = LAB.promotion_recommendation(
            [], perfect, perfect, perfect, perfect)
        self.assertEqual(
            result["classification"], "native-with-browser-validation")
        self.assertTrue(result["threshold_eligible"])
        self.assertFalse(result["production_promotion_enabled"])

    def test_corpus_aggregation_ranks_repeated_impact(self) -> None:
        report = {
            "promotion": {"classification": "browser-required"},
            "comparison": {
                "structural": {"score": 0.5},
                "geometry": {"score": 0.6},
                "typography": {"score": 0.4},
                "visual": {"score": 0.2},
            },
            "timings": {
                "browser_import_ms": 1000,
                "native_import_ms": 10,
                "native_render_ms": 20,
                "native_total_ms": 30,
                "browser_to_native_import_speedup": 100.0,
            },
            "classifications": [{
                "cause": "flex-layout",
                "confidence": 0.9,
                "suggested_subsystem": "layout",
            }],
        }
        aggregate = LAB.aggregate_reports([report, report])
        self.assertEqual(aggregate["fixture_count"], 2)
        self.assertEqual(aggregate["false_promotions"], 0)
        self.assertEqual(aggregate["ranked_gaps"][0]["cause"], "flex-layout")

    def test_missing_corpus_fixture_is_counted_in_report(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            manifest = root / "manifest.json"
            manifest.write_text(json.dumps({
                "schema": "pulp-importer-differential-manifest-v1",
                "fixtures": [{
                    "id": "missing",
                    "source": "missing.html",
                    "from": "claude",
                }],
            }))
            output = root / "output"
            with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
                result = LAB_CLI.command_corpus(Namespace(
                    manifest=manifest,
                    input=None,
                    output=output,
                    importer=root / "importer",
                    observer=root / "observer",
                    timeout_seconds=1,
                    from_source="claude",
                    browser=None,
                    fail_fast=False,
                ))
            self.assertEqual(result, 1)
            report = json.loads((output / "report.json").read_text())
            self.assertEqual(report["fixture_count"], 1)
            self.assertEqual(report["completed_fixture_count"], 0)
            self.assertEqual(report["failed_fixture_count"], 1)


if __name__ == "__main__":
    unittest.main()
