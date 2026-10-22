#!/usr/bin/env python3
"""Unit tests for the importer differential lab's stable comparison protocol."""

from __future__ import annotations

from argparse import Namespace
from contextlib import redirect_stderr, redirect_stdout
import io
import json
import re
import sys
import tempfile
import unittest
from types import SimpleNamespace
from pathlib import Path


class StageProvenanceTests(unittest.TestCase):
    def test_command_record_keeps_safe_argv_and_import_timing(self) -> None:
        from importer_differential.core import CommandResult, command_record

        result = CommandResult(
            ["/private/build/pulp-import-design", "--file",
             "/private/source/fixture.html"],
            1234,
            0,
            '✓ imported "fixture" in 9.30s  — decode 9.10s · parse 20ms\n',
            "",
        )
        record = command_record(result)
        self.assertEqual(record["argv"], ["pulp-import-design", "--file", "fixture.html"])
        self.assertEqual(record["import_timing_line"], result.stdout.strip())
        self.assertEqual(len(record["stdout_sha256"]), 64)

from unittest.mock import patch

try:
    from PIL import Image
except ImportError:
    # Only the two visual-lens cases below need Pillow. Guarding the whole
    # module on it stranded every other case in this file, which exercises
    # the comparison protocol on stdlib types alone.
    Image = None

requires_pillow = unittest.skipUnless(Image is not None, "requires Pillow")

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools" / "import-validation"))
from importer_differential import core as LAB  # noqa: E402
from importer_differential import cli as LAB_CLI  # noqa: E402
from importer_differential import form_controls as FORM_CONTROLS  # noqa: E402


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
            "computedStyleNames": ["display"],
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

    def test_backend_node_id_matches_element_to_element_before_text_heuristics(self) -> None:
        strings = ["DIV", "#text", "Gain", "display", "block"]
        snapshot = {
            "strings": strings,
            "computedStyleNames": ["display"],
            "documents": [{
                "nodes": {
                    "parentIndex": [-1, 0],
                    "nodeType": [1, 3],
                    "nodeName": [0, 1],
                    "nodeValue": [-1, 2],
                    "backendNodeId": [35, 36],
                    "attributes": [[], []],
                },
                "layout": {
                    "nodeIndex": [0],
                    "bounds": [[0, 0, 100, 40]],
                    "styles": [[4]],
                },
            }],
        }
        browser = LAB.browser_nodes(snapshot)
        ir = {
            "root": {
                "type": "frame",
                "name": "div",
                "stable_anchor_id": "root",
                "attributes": {"backend_node_id": "35"},
                "children": [{
                    "type": "text",
                    "name": "#text",
                    "content": "Gain",
                    "stable_anchor_id": "text",
                    "attributes": {"backend_node_id": "36"},
                    "children": [],
                }],
            },
        }
        layout = {"views": [
            {"anchor_id": "root", "x": 0, "y": 0, "width": 100, "height": 40},
            {"anchor_id": "text", "x": 0, "y": 0, "width": 30, "height": 20},
        ]}
        native = LAB.native_nodes(ir, layout)
        matches, missing, extra = LAB.match_nodes(browser, native)
        self.assertEqual(len(matches), 1)
        self.assertEqual(matches[0][0].backend_node_id, 35)
        self.assertEqual(matches[0][1].backend_node_id, 35)
        self.assertEqual(len(missing), 0)
        self.assertEqual(len(extra), 1)

    def test_mismatched_backend_ids_fail_closed_despite_same_text_and_tag(self) -> None:
        browser = LAB.BrowserNode(
            1, "span", -1, "Gain", {}, (0, 0, 40, 20), {}, 35)
        native = LAB.NativeNode(
            "wrong", "frame", "span", "Gain", None, {}, (0, 0, 40, 20), 99)
        matches, missing, extra = LAB.match_nodes([browser], [native])
        self.assertEqual(matches, [])
        self.assertEqual(len(missing), 1)
        self.assertEqual(len(extra), 1)

    @staticmethod
    def _panel_with_gain_span():
        browser = [
            LAB.BrowserNode(0, "div", -1, "", {"id": "panel"}, (0, 0, 100, 40), {}, 10),
            LAB.BrowserNode(1, "span", 0, "Gain", {}, (10, 10, 40, 20), {}, 11),
        ]
        return browser

    def test_an_element_lowered_to_bare_text_is_missing_not_matched(self) -> None:
        # The native lost the span: only a text child carries its text (and
        # its backend id). That text node is a representation node excluded
        # from the precision denominator, so it must not count as a match.
        browser = self._panel_with_gain_span()
        native = [
            LAB.NativeNode("panel", "frame", "div", "", None, {}, (0, 0, 100, 40), 10),
            LAB.NativeNode("gain-text", "text", "#text", "Gain", "panel", {},
                           (10, 10, 40, 20), 11),
        ]
        result = LAB.structural_comparison(browser, native)
        self.assertLessEqual(result["precision"], 1.0)
        self.assertEqual([node.tag for node in result["missing"]], ["span"])
        self.assertEqual(result["structurally_valid_matches"], 1)
        self.assertAlmostEqual(result["recall"], 0.5)
        self.assertLess(result["score"], LAB.PROMOTION_THRESHOLDS["structural"])

    def test_control_a_faithful_lowering_scores_one(self) -> None:
        browser = self._panel_with_gain_span()
        native = [
            LAB.NativeNode("panel", "frame", "div", "", None, {}, (0, 0, 100, 40), 10),
            LAB.NativeNode("gain", "frame", "span", "Gain", "panel", {}, (10, 10, 40, 20), 11),
            LAB.NativeNode("gain-text", "text", "#text", "Gain", "gain", {}, (10, 10, 40, 20)),
        ]
        result = LAB.structural_comparison(browser, native)
        self.assertEqual(result["missing"], [])
        self.assertEqual(result["structurally_valid_matches"], 2)
        self.assertAlmostEqual(result["precision"], 1.0)
        self.assertAlmostEqual(result["score"], 1.0)

    @staticmethod
    def _canvas_ir(with_canvas: bool) -> dict:
        plane = {
            "type": "image", "name": "canvas#scope/layer",
            "stable_anchor_id": "panel/canvas/layer[0]",
            "attributes": {"asset_ref": "canvas:14", "captured_layer": "viewport"},
            "children": [],
        }
        canvas = {
            "type": "frame", "name": "canvas#scope", "stable_anchor_id": "panel/canvas",
            "attributes": {"backend_node_id": "14", "captured_element": "canvas"},
            "children": [plane],
        }
        return {"root": {
            "type": "frame", "name": "div", "stable_anchor_id": "panel",
            "attributes": {"backend_node_id": "11"},
            "children": [canvas] if with_canvas else [plane],
        }}

    def test_a_captured_canvas_plane_is_its_canvas_paint(self) -> None:
        browser = [
            LAB.BrowserNode(0, "div", -1, "", {}, (0, 0, 100, 40), {}, 11),
            LAB.BrowserNode(1, "canvas", 0, "", {}, (0, 0, 80, 30), {}, 14),
        ]
        native = LAB.native_nodes(self._canvas_ir(True), {"views": []})
        result = LAB.structural_comparison(browser, native)
        self.assertEqual(result["extra"], [])
        self.assertIn("panel/canvas/layer[0]",
                      [node.anchor for node in result["ignored_native"]])
        self.assertAlmostEqual(result["score"], 1.0)

    def test_control_a_plane_without_its_canvas_is_not_excused(self) -> None:
        browser = [
            LAB.BrowserNode(0, "div", -1, "", {}, (0, 0, 100, 40), {}, 11),
            LAB.BrowserNode(1, "canvas", 0, "", {}, (0, 0, 80, 30), {}, 14),
        ]
        native = LAB.native_nodes(self._canvas_ir(False), {"views": []})
        result = LAB.structural_comparison(browser, native)
        self.assertEqual([node.tag for node in result["missing"]], ["canvas"])
        self.assertEqual([node.anchor for node in result["extra"]],
                         ["panel/canvas/layer[0]"])
        self.assertLess(result["score"], LAB.PROMOTION_THRESHOLDS["structural"])

    def test_representation_nodes_are_not_structural_element_extras(self) -> None:
        native = [
            LAB.NativeNode("browser:root", "frame", "Browser-evaluated HTML", "", None, {}, None),
            LAB.NativeNode("html", "frame", "html", "", None, {}, None),
            LAB.NativeNode("body", "frame", "body", "", None, {}, None),
            LAB.NativeNode("panel", "frame", "main", "", None, {}, None),
            LAB.NativeNode("panel-text", "text", "#text", "Panel", "panel", {}, None),
        ]
        comparable, ignored = LAB.comparable_native_nodes(native)
        self.assertEqual([node.anchor for node in comparable], ["panel"])
        self.assertEqual(
            [node.anchor for node in ignored],
            ["browser:root", "html", "body", "panel-text"],
        )

    @requires_pillow
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

    @requires_pillow
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

    @requires_pillow
    def test_visual_lens_keeps_renderer_floor_advisory_and_kills_gate_controls(self) -> None:
        """The strict visual gate tolerates tiny edge AA variance only.

        This is a deterministic calibration control, not a Chromium/Skia
        claim. A real floor campaign must replace the synthetic pair with
        authenticated same-source captures and retain its receipt.
        """
        from PIL import Image, ImageDraw

        def panel(*, shift=0, fill=(220, 220, 220), border=2, rounded=False):
            image = Image.new("RGBA", (64, 48), (24, 28, 36, 255))
            draw = ImageDraw.Draw(image)
            box = (14 + shift, 10, 49 + shift, 37)
            draw.rectangle(box, fill=fill, outline=(120, 130, 145, 255), width=border)
            if rounded:
                draw.rounded_rectangle(box, radius=12, fill=fill, outline=(120, 130, 145, 255), width=border)
            return image

        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            reference_image = panel()
            reference = root / "reference.png"
            reference_image.save(reference)

            healthy_image = reference_image.copy()
            healthy_pixels = healthy_image.load()
            for x in range(14, 50):
                for y in (10, 37):
                    red, green, blue, alpha = healthy_pixels[x, y]
                    healthy_pixels[x, y] = (min(255, red + 12), min(255, green + 12), min(255, blue + 12), alpha)
            healthy = root / "healthy.png"
            healthy_image.save(healthy)
            healthy_metrics = LAB.visual_metrics(reference, healthy, root / "healthy-artifacts")
            self.assertGreaterEqual(healthy_metrics["score"], LAB.PROMOTION_THRESHOLDS["visual"])
            self.assertGreater(healthy_metrics["differing_pixel_count"], 0)

            controls = {
                "one-pixel-shift": panel(shift=1),
                "color-change": panel(fill=(90, 220, 160)),
                "missing-border": panel(border=0),
                "wrong-weight": panel(border=4),
                "corner-radius": panel(rounded=True),
            }
            for name, image in controls.items():
                with self.subTest(control=name):
                    candidate = root / f"{name}.png"
                    image.save(candidate)
                    metrics = LAB.visual_metrics(reference, candidate, root / f"{name}-artifacts")
                    self.assertLess(metrics["score"], LAB.PROMOTION_THRESHOLDS["visual"])
                    self.assertGreater(metrics["differing_pixel_count"], 0)

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

    def test_css_initial_typography_omissions_are_equivalent(self) -> None:
        browser = LAB.BrowserNode(
            1, "span", -1, "Gain", {}, (0, 0, 40, 20), {
                "font-style": "normal",
                "text-align": "start",
                "letter-spacing": "normal",
                "line-height": "normal",
                "text-transform": "none",
                "text-decoration-line": "none",
                "white-space": "normal",
            })
        native = LAB.NativeNode(
            "gain", "text", "span", "Gain", None, {}, (0, 0, 40, 20))
        metrics = LAB.typography_metrics([(browser, native, 4.0)])
        self.assertEqual(metrics["comparable_properties"], 7)
        self.assertEqual(metrics["equal_properties"], 7)
        self.assertEqual(metrics["score"], 1.0)

    def test_capture_extent_uses_reference_not_responsive_viewport(self) -> None:
        capture = {
            "reference": {"logical_width": 1440, "logical_height": 1200},
            "provenance": {
                "viewport": {"resolved": {"width": 1280, "height": 800}}},
        }
        self.assertEqual(LAB.capture_logical_size(capture), (1440, 1200))

    def test_geometry_metrics_preserves_signed_residuals_and_root(self) -> None:
        browser = [
            LAB.BrowserNode(0, "main", -1, "", {}, (10, 20, 100, 40), {}),
            LAB.BrowserNode(1, "span", 0, "Gain", {}, (20, 30, 40, 10), {}),
        ]
        native = [
            LAB.NativeNode("root", "frame", "main", "", None, {}, (11, 18, 102, 39)),
            LAB.NativeNode("gain", "text", "span", "Gain", "root", {}, (19, 31, 40, 11)),
        ]
        metrics = LAB.geometry_metrics([
            (browser[0], native[0], 1.0), (browser[1], native[1], 1.0)])
        self.assertEqual(metrics["rows"][0]["signed_delta"],
                         {"x": 1, "y": -2, "width": 2, "height": -1})
        self.assertEqual(metrics["root_residual"]["native_anchor"], "root")
        self.assertEqual(metrics["signed_delta_summary"]["x"]["median"], 0.0)

    @staticmethod
    def _pseudo_badge_snapshot() -> dict:
        # A badge whose ::before is absolutely positioned. Chromium emits the
        # pseudo's own box first, then a zero-size row for its empty generated
        # content, both pointing at the same DOM node.
        strings = ["DIV", "::before", "#text", "On", "display", "block"]
        return {
            "strings": strings,
            "computedStyleNames": ["display"],
            "documents": [{
                "nodes": {
                    "parentIndex": [-1, 0, 0],
                    "nodeType": [1, 1, 3],
                    "nodeName": [0, 1, 2],
                    "nodeValue": [-1, -1, 3],
                    "backendNodeId": [11, 12, 13],
                    "attributes": [[], [], []],
                },
                "layout": {
                    "nodeIndex": [0, 1, 1, 2],
                    "bounds": [[28, 28, 131, 42], [42, 44, 10, 10],
                               [0, 0, 0, 0], [64, 40, 77, 17]],
                    "styles": [[5], [5], [5], [5]],
                },
            }],
        }

    def test_a_pseudo_element_keeps_its_own_box_not_its_generated_row(self) -> None:
        browser = LAB.browser_nodes(self._pseudo_badge_snapshot())
        pseudo = [node for node in browser if node.tag == "::before"]
        self.assertEqual(len(pseudo), 1)
        self.assertEqual(pseudo[0].bounds, (42.0, 44.0, 10.0, 10.0))
        native = [
            LAB.NativeNode("badge", "frame", "div.badge", "", None, {},
                           (28, 28, 131, 42), 11),
            LAB.NativeNode("badge/::before", "frame", "::before", "", "badge", {},
                           (42, 44, 10, 10), 12),
            LAB.NativeNode("badge/#text", "text", "#text", "On", "badge", {},
                           (64, 40, 77, 17), 13),
        ]
        result = LAB.structural_comparison(browser, native)
        self.assertEqual(result["missing"], [])
        self.assertEqual(result["extra"], [])
        self.assertAlmostEqual(result["score"], 1.0)

    def test_control_a_native_that_drops_the_pseudo_element_is_missing_it(self) -> None:
        browser = LAB.browser_nodes(self._pseudo_badge_snapshot())
        native = [
            LAB.NativeNode("badge", "frame", "div.badge", "", None, {},
                           (28, 28, 131, 42), 11),
            LAB.NativeNode("badge/#text", "text", "#text", "On", "badge", {},
                           (64, 40, 77, 17), 13),
        ]
        result = LAB.structural_comparison(browser, native)
        self.assertEqual([node.tag for node in result["missing"]], ["::before"])
        self.assertLess(result["score"], LAB.PROMOTION_THRESHOLDS["structural"])

    def test_own_layout_rows_agree_with_the_cpp_reader_fixture(self) -> None:
        # test_browser_capture_ir.cpp asserts CapturedStyleIndex::own_layout_row
        # against the same expected rows, so the two readers share one rule.
        fixture = REPO / "test/fixtures/browser-capture-layout-rows"
        snapshot = json.loads((fixture / "dom-snapshot.json").read_text())
        expected = json.loads((fixture / "expected-own-rows.json").read_text())
        rows = LAB.principal_layout_rows(snapshot["documents"][0]["layout"])
        self.assertEqual({str(node): row for node, row in rows.items()},
                         expected["own_rows"])
        bounds = snapshot["documents"][0]["layout"]["bounds"]
        browser = {node.index: node.bounds for node in LAB.browser_nodes(snapshot)}
        for node in (4, 5):
            self.assertEqual(browser[node],
                             tuple(float(v) for v in bounds[expected["own_rows"][str(node)]]))

    def test_an_all_empty_node_still_reports_its_first_row(self) -> None:
        rows = LAB.principal_layout_rows({
            "nodeIndex": [0, 1, 1],
            "bounds": [[0, 0, 10, 10], [0, 0, 0, 0], [5, 5, 0, 3]],
        })
        self.assertEqual(rows, {0: 0, 1: 1})

    @staticmethod
    def _icon_snapshot() -> dict:
        strings = ["DIV", "svg", "circle", "display", "block"]
        return {
            "strings": strings,
            "computedStyleNames": ["display"],
            "documents": [{
                "nodes": {
                    "parentIndex": [-1, 0, 1],
                    "nodeType": [1, 1, 1],
                    "nodeName": [0, 1, 2],
                    "nodeValue": [-1, -1, -1],
                    "backendNodeId": [11, 12, 13],
                    "attributes": [[], [], []],
                },
                "layout": {
                    "nodeIndex": [0, 1, 2],
                    "bounds": [[24, 24, 212, 124], [40, 40, 64, 64],
                               [45, 45, 54, 54]],
                    "styles": [[4], [4], [4]],
                },
            }],
        }

    def _icon_geometry(self, shape_box):
        browser = LAB.browser_nodes(self._icon_snapshot())
        native = [
            LAB.NativeNode("icon", "frame", "div", "", None, {}, (24, 24, 212, 124), 11),
            LAB.NativeNode("icon/svg", "frame", "svg", "", "icon", {}, (40, 40, 64, 64), 12),
            LAB.NativeNode("icon/svg/circle", "path", "circle", "", "icon/svg", {},
                           shape_box, 13),
        ]
        result = LAB.structural_comparison(browser, native)
        return result, LAB.geometry_metrics(result["matches"])

    def test_a_vector_shape_is_placed_in_its_svg_viewport_frame(self) -> None:
        # The native shape carries its root <svg>'s box (the viewBox frame its
        # path data is drawn in), not the circle's own ink box.
        result, geometry = self._icon_geometry((40, 40, 64, 64))
        self.assertEqual(result["missing"], [])
        self.assertAlmostEqual(result["score"], 1.0)
        shape_row = next(row for row in geometry["rows"]
                         if row["native_anchor"] == "icon/svg/circle")
        self.assertEqual(shape_row["reference_frame"], "svg-viewport")
        self.assertEqual(shape_row["max_abs_delta"], 0)
        element_rows = [row for row in geometry["rows"]
                        if row["native_anchor"] != "icon/svg/circle"]
        self.assertTrue(all(row["reference_frame"] == "element"
                            for row in element_rows))

    def test_control_a_shape_outside_its_svg_frame_fails_geometry(self) -> None:
        _result, geometry = self._icon_geometry((43, 40, 64, 64))
        shape_row = next(row for row in geometry["rows"]
                         if row["native_anchor"] == "icon/svg/circle")
        self.assertEqual(shape_row["max_abs_delta"], 3)
        self.assertGreater(shape_row["max_abs_delta"],
                           LAB.GEOMETRY_MAX_ABS_DELTA_PX)

    def test_svg_viewport_frame_registers_with_the_authored_frame(self) -> None:
        browser = LAB.browser_nodes(self._icon_snapshot())
        capture = {"reference": {"authored_frame": {"x": 24, "y": 24}}}
        registered, _ = LAB.register_browser_nodes_to_native_frame(
            browser, capture, capture)
        shape = next(node for node in registered if node.tag == "circle")
        self.assertEqual(shape.svg_viewport, (16.0, 16.0, 64.0, 64.0))
        self.assertEqual(shape.bounds, (21.0, 21.0, 54.0, 54.0))

    def test_viewport_dpr_audit_rejects_wrong_pixel_extent(self) -> None:
        capture, native_capture, _source_sha256 = self._bound_captures()
        for item in (capture, native_capture):
            item["reference"] = {
                "logical_width": 1280, "logical_height": 800,
                "device_scale_factor": 2,
            }
        audit = LAB.viewport_dpr_audit(
            capture, native_capture, (1280, 800), (2559, 1600))
        self.assertFalse(audit["ok"])
        self.assertFalse(audit["checks"]["render_size_matches_dpr"])

    def test_viewport_dpr_audit_requires_native_dpr_and_allows_overflow_extent(self) -> None:
        capture, native_capture, _source_sha256 = self._bound_captures()
        capture["reference"] = {
            "logical_width": 1440, "logical_height": 1200,
            "device_scale_factor": 2,
        }
        native_capture["reference"] = {
            "logical_width": 1440, "logical_height": 1200,
        }
        with self.assertRaisesRegex(LAB.LabError, "device scale"):
            LAB.viewport_dpr_audit(capture, native_capture, (1280, 800), (2880, 2400))
        native_capture["reference"]["device_scale_factor"] = 2
        audit = LAB.viewport_dpr_audit(
            capture, native_capture, (1280, 800), (2880, 2400))
        self.assertTrue(audit["ok"])
        self.assertTrue(audit["checks"]["logical_extent_covers_request"])

    def test_viewport_dpr_audit_accepts_matching_root_contract(self) -> None:
        capture, native_capture, _source_sha256 = self._bound_captures()
        for item in (capture, native_capture):
            item["reference"] = {
                "logical_width": 1280, "logical_height": 800,
                "device_scale_factor": 2,
            }
        audit = LAB.viewport_dpr_audit(
            capture, native_capture, (1280, 800), (2560, 1600))
        self.assertTrue(audit["ok"])

    def test_viewport_dpr_audit_each_negative_check_fails_closed(self) -> None:
        cases = {
            "resolved_viewport_matches_request": lambda capture, native: (
                capture["provenance"]["viewport"]["resolved"].update(width=1279)),
            "logical_extent_covers_request": lambda capture, native: (
                capture["reference"].update(logical_width=1279)),
            "native_logical_size_matches_browser": lambda capture, native: (
                native["reference"].update(logical_width=1279)),
            "dpr_matches_native": lambda capture, native: (
                native["reference"].update(device_scale_factor=1)),
            "resolved_viewport_matches_native": lambda capture, native: (
                native["provenance"]["viewport"]["resolved"].update(width=1279)),
            "render_size_matches_dpr": lambda capture, native: None,
        }
        for name, mutate in cases.items():
            with self.subTest(check=name):
                capture, native_capture, _source_sha256 = self._bound_captures()
                for item in (capture, native_capture):
                    item["reference"] = {
                        "logical_width": 1280, "logical_height": 800,
                        "device_scale_factor": 2,
                    }
                mutate(capture, native_capture)
                actual_size = (2559, 1600) if name == "render_size_matches_dpr" else (2560, 1600)
                audit = LAB.viewport_dpr_audit(
                    capture, native_capture, (1280, 800), actual_size)
                self.assertFalse(audit["ok"])
                self.assertFalse(audit["checks"][name])

    @staticmethod
    def _bound_captures() -> tuple[dict, dict, str]:
        source_sha256 = "a" * 64
        provenance = {
            "source": {"sha256": source_sha256},
            "viewport": {"requested": {"width": 1280, "height": 800},
                         "resolved": {"width": 1280, "height": 800}},
            "browser": {"product": "Chrome", "version": "140.0"},
        }
        return ({"provenance": json.loads(json.dumps(provenance)),
                 "reference": {"device_scale_factor": 1}},
                {"provenance": json.loads(json.dumps(provenance)),
                 "reference": {"device_scale_factor": 1}}, source_sha256)

    def test_capture_binding_accepts_captures_of_the_same_input(self) -> None:
        capture, native_capture, source_sha256 = self._bound_captures()
        LAB.verify_capture_binding(capture, native_capture, source_sha256)

    def test_interaction_plan_identity_matches_capture_redaction(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            plan = Path(raw) / "plan.json"
            plan.write_text(json.dumps({
                "schema": "pulp-browser-interactions-v1",
                "version": 1,
                "actions": [{"action": "click", "selector": "#go"}],
            }), encoding="utf-8")
            self.assertEqual(
                LAB.interaction_plan_sha256(plan),
                "92231b6cd069d5bd57b91c23334cbe13456cd987664773ca070806f49df1d40c",
            )

    def test_interaction_plan_identity_matches_javascript_utf16_length(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            plan = Path(raw) / "plan.json"
            plan.write_text(json.dumps({
                "schema": "pulp-browser-interactions-v1",
                "version": 1,
                "actions": [{"action": "type", "selector": "#name", "text": "😀"}],
            }), encoding="utf-8")
            expected = {
                "schema": "pulp-browser-interactions-v1", "version": 1,
                "actions": [{"action": "type", "selector": "#name",
                              "timeout_ms": 5000, "text_length": 2}],
            }
            canonical = json.dumps(expected, separators=(",", ":"), ensure_ascii=False).encode()
            import hashlib
            self.assertEqual(LAB.interaction_plan_sha256(plan), hashlib.sha256(canonical).hexdigest())

    def test_capture_binding_rejects_unbound_interaction_plan(self) -> None:
        capture, native_capture, source_sha256 = self._bound_captures()
        interactions = {"plan_sha256": "a" * 64}
        capture["provenance"]["interactions"] = interactions
        native_capture["provenance"]["interactions"] = dict(interactions)
        with self.assertRaisesRegex(LAB.LabError, "not bound"):
            LAB.verify_capture_binding(
                capture, native_capture, source_sha256,
                expected_interaction_plan_sha256="b" * 64,
            )

    def test_capture_binding_fails_closed_on_each_mismatch(self) -> None:
        cases = {
            "browser capture source hash is not bound":
                lambda browser, native: browser["provenance"]["source"].update(sha256="b" * 64),
            "native lowering source hash is not bound":
                lambda browser, native: native["provenance"]["source"].update(sha256="b" * 64),
            "viewport provenance differs":
                lambda browser, native: native["provenance"]["viewport"]["resolved"].update(width=1440),
            "browser provenance differs":
                lambda browser, native: native["provenance"]["browser"].update(version="141.0"),
        }
        for message, mutate in cases.items():
            with self.subTest(message=message):
                capture, native_capture, source_sha256 = self._bound_captures()
                mutate(capture, native_capture)
                with self.assertRaisesRegex(LAB.LabError, message):
                    LAB.verify_capture_binding(capture, native_capture, source_sha256)

    def test_capture_binding_checks_dpr_and_interaction_provenance(self) -> None:
        capture, native_capture, source_sha256 = self._bound_captures()
        capture["reference"] = {"device_scale_factor": 2}
        native_capture["reference"] = {"device_scale_factor": 1}
        with self.assertRaisesRegex(LAB.LabError, "device scale factor"):
            LAB.verify_capture_binding(capture, native_capture, source_sha256)
        native_capture["reference"]["device_scale_factor"] = 2
        capture["provenance"]["interactions"] = {"plan_sha256": "a" * 64}
        native_capture["provenance"]["interactions"] = {"plan_sha256": "b" * 64}
        with self.assertRaisesRegex(LAB.LabError, "interaction provenance"):
            LAB.verify_capture_binding(capture, native_capture, source_sha256)

        for missing in ("browser", "native"):
            with self.subTest(missing=missing):
                browser, native, source = self._bound_captures()
                target = browser if missing == "browser" else native
                target["reference"].pop("device_scale_factor")
                with self.assertRaisesRegex(LAB.LabError, "device scale factor"):
                    LAB.verify_capture_binding(browser, native, source)

    @requires_pillow
    def test_capture_image_binding_rejects_stale_png(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            image_path = root / "browser.png"
            Image.new("RGBA", (2, 1), (0, 0, 0, 255)).save(image_path)
            capture = {
                "reference": {"asset_id": "reference:browser"},
                "assets": [{"id": "reference:browser", "sha256": LAB.sha256_file(image_path),
                            "width_px": 2, "height_px": 1}],
            }
            LAB.verify_capture_image_binding(capture, image_path)
            Image.new("RGBA", (2, 1), (255, 0, 0, 255)).save(image_path)
            with self.assertRaisesRegex(LAB.LabError, "digest"):
                LAB.verify_capture_image_binding(capture, image_path)
            Image.new("RGBA", (3, 1), (0, 0, 0, 255)).save(image_path)
            capture["assets"][0]["sha256"] = LAB.sha256_file(image_path)
            with self.assertRaisesRegex(LAB.LabError, "dimensions"):
                LAB.verify_capture_image_binding(capture, image_path)

    def test_snapshot_binding_preserves_legacy_but_requires_producer_for_experiment(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            evidence = Path(directory)
            snapshot = evidence / "dom-snapshot.json"
            snapshot.write_text('{"background":"black"}\n', encoding="utf-8")
            self.assertEqual(LAB.verify_capture_snapshot_binding({}, evidence)[0],
                             {"status": "legacy-unbound", "sha256": None})
            self.assertEqual(LAB.verify_capture_snapshot_binding(
                {"documents": [{"snapshot_asset": "dom-snapshot.json"}]}, evidence)[0],
                {"status": "legacy-unbound", "sha256": None})
            # Legacy native envelopes may name the snapshot through an asset ID.
            legacy = {"documents": [{"snapshot_asset": "snapshot:main"}]}
            self.assertEqual(LAB.verify_capture_snapshot_binding(legacy, evidence)[0],
                             {"status": "legacy-unbound", "sha256": None})
            with self.assertRaisesRegex(LAB.LabError, "no producer digest"):
                LAB.verify_capture_snapshot_binding(legacy, evidence, required=True)
            with self.assertRaisesRegex(LAB.LabError, "no producer digest"):
                LAB.verify_capture_snapshot_binding({}, evidence, required=True)
            capture = {"documents": [{"snapshot_asset": "dom-snapshot.json",
                                      "snapshot_sha256": LAB.sha256_file(snapshot)}]}
            binding, verified_bytes = LAB.verify_capture_snapshot_binding(capture, evidence, required=True)
            self.assertEqual(verified_bytes, snapshot.read_bytes())
            self.assertEqual(binding,
                             {"status": "producer-bound", "sha256": LAB.sha256_file(snapshot)})
            # A style-only edit and a different valid snapshot are both wrong bytes.
            for replacement in ('{"background":"white"}\n', '{"documents":[]}\n'):
                snapshot.write_text(replacement, encoding="utf-8")
                with self.assertRaisesRegex(LAB.LabError, "does not match"):
                    LAB.verify_capture_snapshot_binding(capture, evidence)
            snapshot.unlink()
            with self.assertRaisesRegex(LAB.LabError, "unavailable"):
                LAB.verify_capture_snapshot_binding(capture, evidence)
            # Windows symlink creation requires an optional OS privilege.
            if sys.platform != "win32":
                outside = evidence.parent / (evidence.name + "-snapshot.json")
                try:
                    outside.write_text('{"background":"black"}\n', encoding="utf-8")
                    snapshot.symlink_to(outside)
                    with self.assertRaisesRegex(LAB.LabError, "escapes evidence"):
                        LAB.verify_capture_snapshot_binding(capture, evidence)
                finally:
                    outside.unlink(missing_ok=True)

    def test_snapshot_binding_refuses_oversize_before_open_for_bound_and_legacy(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            evidence = Path(directory)
            snapshot = evidence / "dom-snapshot.json"
            snapshot.write_bytes(b"123456789")
            capture = {"documents": [{"snapshot_asset": "dom-snapshot.json",
                                      "snapshot_sha256": LAB.sha256_file(snapshot)}]}
            for envelope in ({}, capture):
                with self.subTest(bound=bool(envelope)), \
                        patch.object(LAB, "MAX_SNAPSHOT_BYTES", 8), \
                        patch.object(Path, "open") as opened, \
                        self.assertRaisesRegex(LAB.LabError, "exceeds 256 MiB"):
                    LAB.verify_capture_snapshot_binding(envelope, evidence)
                opened.assert_not_called()

    def test_snapshot_binding_rejects_growth_and_truncation_during_bounded_read(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            evidence = Path(directory)
            snapshot = evidence / "dom-snapshot.json"
            original = b'{"documents":[]}\n'
            fstat = LAB.os.fstat
            for replacement in (original + b"extra", original[:-1]):
                snapshot.write_bytes(original)
                capture = {"documents": [{"snapshot_asset": "dom-snapshot.json",
                                          "snapshot_sha256": LAB.sha256_file(snapshot)}]}

                def mutate_after_measurement(descriptor):
                    info = fstat(descriptor)
                    snapshot.write_bytes(replacement)
                    return info

                with self.subTest(length=len(replacement)), \
                        patch.object(LAB.os, "fstat", side_effect=mutate_after_measurement), \
                        self.assertRaisesRegex(LAB.LabError, "changed during bounded read"):
                    LAB.verify_capture_snapshot_binding(capture, evidence, required=True)

    def test_snapshot_binding_rejects_malformed_declarations(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            evidence = Path(directory)
            for digest in (None, True, 12, "", "a" * 63, "A" * 64):
                with self.subTest(digest=digest), self.assertRaisesRegex(LAB.LabError, "malformed"):
                    LAB.verify_capture_snapshot_binding({"documents": [{
                        "snapshot_asset": "dom-snapshot.json", "snapshot_sha256": digest,
                    }]}, evidence)
            with self.assertRaisesRegex(LAB.LabError, "dom-snapshot.json document"):
                LAB.verify_capture_snapshot_binding({"documents": [{
                    "snapshot_asset": "../dom-snapshot.json", "snapshot_sha256": "a" * 64,
                }]}, evidence)
            with self.assertRaisesRegex(LAB.LabError, "exactly one"):
                LAB.verify_capture_snapshot_binding({"documents": [{
                    "snapshot_asset": "dom-snapshot.json", "snapshot_sha256": "a" * 64,
                }, {}]}, evidence)
            for documents in ([{}], [None], ["x"], {}):
                for required in (False, True):
                    with self.subTest(documents=documents, required=required), \
                            self.assertRaises(LAB.LabError):
                        LAB.verify_capture_snapshot_binding(
                            {"documents": documents}, evidence, required=required)

    def test_capture_binding_refuses_a_capture_without_provenance(self) -> None:
        _, native_capture, source_sha256 = self._bound_captures()
        with self.assertRaisesRegex(LAB.LabError, "browser capture source hash"):
            LAB.verify_capture_binding({}, native_capture, source_sha256)

    def test_native_lane_lowers_the_authored_source_not_the_offline_parser(self) -> None:
        source_args = ["--from", "claude", "--file", "panel.html"]
        viewport_args = ["--render-size", "1280x800"]
        interactions = ["--browser-interactions", "plan.json"]
        native = LAB.import_lane_args(
            Path("importer"), source_args, Path("native.json"), viewport_args, interactions,
            Path("chrome"), native=True)
        browser = LAB.import_lane_args(
            Path("importer"), source_args, Path("browser.json"), viewport_args, interactions,
            Path("chrome"), native=False)
        self.assertIn("--native-panel-lowering", native)
        self.assertNotIn("--native-panel-lowering", browser)
        for args in (native, browser):
            self.assertNotIn("--offline", args)
            self.assertNotIn("--execute-bundle", args)
            for flag in (*source_args, *viewport_args, *interactions, "--browser", "chrome"):
                self.assertIn(flag, args)

    def test_compare_one_checks_the_binding_before_rendering(self) -> None:
        # Both imports "succeed" with captures that carry no provenance; the
        # comparison must refuse there, before any render command runs.
        calls: list[list[str]] = []

        def fake_run(argv: list[str], timeout_seconds: int) -> LAB.CommandResult:
            calls.append(argv)
            out = Path(argv[argv.index("--output") + 1])
            out.write_text("{}", encoding="utf-8")
            return LAB.CommandResult(argv, 1, 0, "", "")

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "panel.html"
            source.write_text("<div></div>", encoding="utf-8")
            evidence = root / "evidence"
            evidence.mkdir()
            (evidence / "capture.json").write_text("{}", encoding="utf-8")
            with patch.object(LAB, "run_command", side_effect=fake_run), \
                    patch.object(LAB, "find_browser_evidence", return_value=evidence):
                with self.assertRaisesRegex(LAB.LabError, "source hash is not bound"):
                    LAB.compare_one(Path("importer"), Path("observer"), source, root / "out",
                                    60, "claude", Path("chrome"))
        self.assertEqual(len(calls), 2)
        native = calls[1]
        self.assertIn("--native-panel-lowering", native)
        self.assertNotIn("--offline", native)

    @requires_pillow
    def test_compare_one_success_records_source_digest(self) -> None:
        """Exercise the success path past capture binding and integrity output."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "panel.html"
            source.write_text("<div></div>", encoding="utf-8")
            plan = root / "plan.json"
            plan.write_text(json.dumps({
                "schema": "pulp-browser-interactions-v1",
                "version": 1,
                "actions": [{"action": "click", "selector": "#go"}],
            }), encoding="utf-8")
            evidence = root / "evidence"
            evidence.mkdir()
            source_sha256 = LAB.sha256_file(source)
            plan_sha256 = LAB.interaction_plan_sha256(plan)
            provenance = {
                "source": {"sha256": source_sha256},
                "viewport": {"requested": {"width": 1, "height": 1},
                             "resolved": {"width": 1, "height": 1}},
                "browser": {"product": "Chrome", "version": "140.0"},
                "interactions": {"plan_sha256": plan_sha256},
            }
            capture = {
                "provenance": provenance,
                "reference": {"asset_id": "reference:browser",
                               "logical_width": 1, "logical_height": 1,
                               "device_scale_factor": 1,
                               "authored_frame": {"x": 0, "y": 0,
                                                   "width": 1, "height": 1}},
            }
            (evidence / "capture.json").write_text(json.dumps(capture), encoding="utf-8")
            Image.new("RGBA", (1, 1), (0, 0, 0, 255)).save(evidence / "browser.png")
            capture["assets"] = [{"id": "reference:browser", "sha256": LAB.sha256_file(evidence / "browser.png"),
                                  "width_px": 1, "height_px": 1}]
            (evidence / "capture.json").write_text(json.dumps(capture), encoding="utf-8")
            (evidence / "dom-snapshot.json").write_text(
                json.dumps({"documents": []}), encoding="utf-8")
            mutate_snapshot_during_render = False

            def fake_run(argv: list[str], timeout_seconds: int) -> LAB.CommandResult:
                if "--render" in argv:
                    if mutate_snapshot_during_render:
                        (evidence / "dom-snapshot.json").write_text(
                            '{"documents":[],"changedAfterVerification":true}', encoding="utf-8")
                        (evidence / "capture.json").write_text(
                            '{"changedAfterVerification":true}', encoding="utf-8")
                    Image.new("RGBA", (1, 1), (0, 0, 0, 255)).save(
                        Path(argv[argv.index("--render") + 1]))
                    Path(argv[argv.index("--layout") + 1]).write_text(
                        json.dumps({"views": [], "nodes": [{
                            "id": "browser:root",
                            "rect": {"x": 0, "y": 0, "w": 1, "h": 1},
                        }]}), encoding="utf-8")
                    Path(argv[argv.index("--render-receipt") + 1]).write_text(
                        json.dumps({"schema": "pulp-design-ir-render-receipt-v1",
                                    "backend_requested": "skia", "backend_used": "skia",
                                    "ok": True}), encoding="utf-8")
                else:
                    output = Path(argv[argv.index("--output") + 1])
                    output.write_text(json.dumps({"root": {
                        "type": "frame", "style": {"width": 1, "height": 1},
                        "children": []}}),
                                      encoding="utf-8")
                return LAB.CommandResult(argv, 1, 0, "", "")

            with patch.object(LAB, "run_command", side_effect=fake_run), \
                    patch.object(LAB, "find_browser_evidence", return_value=evidence), \
                    patch.object(LAB, "verify_capture_binding", wraps=LAB.verify_capture_binding) as binding, \
                    patch.object(LAB, "verify_capture_image_binding", wraps=LAB.verify_capture_image_binding) as image_binding, \
                    patch.object(LAB, "verify_capture_snapshot_binding", wraps=LAB.verify_capture_snapshot_binding) as snapshot_binding:
                report = LAB.compare_one(
                    Path("importer"), Path("observer"), source, root / "out", 60,
                    "claude", Path("chrome"),
                    fixture_metadata={"viewport": {"width": 1, "height": 1},
                                      "browser_interactions": "plan.json"})
            self.assertEqual(report["integrity"]["source_sha256"], source_sha256)
            self.assertTrue(report["integrity"]["source_bound"])
            self.assertTrue(report["comparison"]["viewport_dpr_audit"]["ok"])
            binding.assert_called_once()
            self.assertEqual(binding.call_args.kwargs["expected_interaction_plan_sha256"], plan_sha256)
            self.assertEqual(image_binding.call_count, 2)
            image_binding.assert_any_call(capture, evidence / "browser.png")
            self.assertEqual(snapshot_binding.call_count, 2)
            self.assertEqual(report["integrity"]["browser_snapshot_binding"]["status"], "legacy-unbound")
            # Verify declared bytes on compare_one's real path, before rendering.
            snapshot_path = evidence / "dom-snapshot.json"
            capture["documents"] = [{"snapshot_asset": "dom-snapshot.json",
                                     "snapshot_sha256": LAB.sha256_file(snapshot_path)}]
            (evidence / "capture.json").write_text(json.dumps(capture), encoding="utf-8")
            verified_capture_bytes = (evidence / "capture.json").read_bytes()
            mutate_snapshot_during_render = True
            with patch.object(LAB, "run_command", side_effect=fake_run), \
                    patch.object(LAB, "find_browser_evidence", return_value=evidence):
                bound_report = LAB.compare_one(
                    Path("importer"), Path("observer"), source, root / "out-bound", 60,
                    "claude", Path("chrome"),
                    fixture_metadata={"viewport": {"width": 1, "height": 1},
                                      "browser_interactions": "plan.json"})
            self.assertEqual(bound_report["integrity"]["browser_snapshot_binding"]["status"], "producer-bound")
            self.assertEqual(bound_report["integrity"]["native_snapshot_binding"]["sha256"],
                             capture["documents"][0]["snapshot_sha256"])
            self.assertEqual(LAB.sha256_file(root / "out-bound/browser/dom-snapshot.json"),
                             capture["documents"][0]["snapshot_sha256"])
            self.assertNotEqual(LAB.sha256_file(snapshot_path), capture["documents"][0]["snapshot_sha256"])
            self.assertEqual((root / "out-bound/browser/capture.json").read_bytes(), verified_capture_bytes)
            self.assertEqual(bound_report["integrity"]["browser_capture_sha256"],
                             LAB.sha256_file(root / "out-bound/browser/capture.json"))
            self.assertEqual(bound_report["integrity"]["native_capture_canonical_sha256"],
                             LAB.canonical_json_sha256(verified_capture_bytes))
            mutate_snapshot_during_render = False
            (evidence / "capture.json").write_bytes(verified_capture_bytes)
            snapshot_path.write_text('{"documents":[],"changedStyle":"white"}', encoding="utf-8")
            with patch.object(LAB, "run_command", side_effect=fake_run) as run, \
                    patch.object(LAB, "find_browser_evidence", return_value=evidence), \
                    self.assertRaisesRegex(LAB.LabError, "snapshot digest does not match"):
                LAB.compare_one(
                    Path("importer"), Path("observer"), source, root / "out-tampered", 60,
                    "claude", Path("chrome"),
                    fixture_metadata={"viewport": {"width": 1, "height": 1},
                                      "browser_interactions": "plan.json"})
            self.assertFalse(any("--render" in call.args[0] for call in run.call_args_list))
            snapshot_path.write_text(json.dumps({"documents": []}), encoding="utf-8")
            # Exercise compare_one's wiring, not just the metrics helper: a
            # panel-local match must not replace the raw page-space score.
            capture["reference"]["authored_frame"] = {
                "x": 0.25, "y": 0.25, "width": 1, "height": 1}
            (evidence / "capture.json").write_text(json.dumps(capture), encoding="utf-8")
            browser = LAB.BrowserNode(
                0, "main", -1, "", {}, (0.25, 0.25, 0.5, 0.5), {}, 10)
            native = LAB.NativeNode(
                "panel", "frame", "main", "", None, {}, (0, 0, 0.5, 0.5), 10)
            with patch.object(LAB, "run_command", side_effect=fake_run), \
                    patch.object(LAB, "find_browser_evidence", return_value=evidence), \
                    patch.object(LAB, "browser_nodes", return_value=[browser]), \
                    patch.object(LAB, "native_nodes", return_value=[native]):
                offset_report = LAB.compare_one(
                    Path("importer"), Path("observer"), source, root / "out-offset", 60,
                    "claude", Path("chrome"),
                    fixture_metadata={"viewport": {"width": 1, "height": 1},
                                      "browser_interactions": "plan.json"})
            geometry = offset_report["comparison"]["geometry"]
            self.assertLess(geometry["score"], geometry["registered_geometry"]["score"])
            self.assertEqual(geometry["registered_geometry"]["score"], 1.0)
            self.assertEqual(geometry["registration_applied"]["x"], -0.25)
            # The snapshot has no DOM authored root, so frame placement fails
            # closed and the perfect registered score cannot promote.
            self.assertFalse(
                geometry["gate"]["checks"]["frame_placement"]["checks"]
                ["dom_authored_root_found"]["ok"])
            self.assertIn("geometry gate failed: frame_placement",
                          offset_report["promotion"]["reasons"])
            summary = (root / "out-offset/comparison/summary.md").read_text()
            self.assertIn("Geometry gate: fail (frame_placement)", summary)
            self.assertIn("Geometry raw page-space score (diagnostic, not gated)", summary)
            with patch.object(
                LAB, "viewport_dpr_audit",
                return_value={"ok": False, "checks": {"planted_negative": False}},
            ):
                with patch.object(LAB, "run_command", side_effect=fake_run), \
                        patch.object(LAB, "find_browser_evidence", return_value=evidence):
                    with self.assertRaisesRegex(LAB.LabError, "viewport/DPR audit failed"):
                        LAB.compare_one(
                            Path("importer"), Path("observer"), source, root / "out-refusal", 60,
                            "claude", Path("chrome"),
                            fixture_metadata={"viewport": {"width": 1, "height": 1},
                                              "browser_interactions": "plan.json"})
            with patch.object(
                LAB, "viewport_dpr_audit",
                side_effect=LAB.LabError("viewport/DPR audit failed"),
            ):
                with patch.object(LAB, "run_command", side_effect=fake_run), \
                        patch.object(LAB, "find_browser_evidence", return_value=evidence):
                    with self.assertRaisesRegex(LAB.LabError, "viewport/DPR audit failed"):
                        LAB.compare_one(
                            Path("importer"), Path("observer"), source, root / "out-fail", 60,
                            "claude", Path("chrome"),
                            fixture_metadata={"viewport": {"width": 1, "height": 1},
                                              "browser_interactions": "plan.json"})

    @requires_pillow
    def test_compare_one_scores_structure_through_the_shared_comparison(self) -> None:
        """compare_one reports the score structural_comparison computes."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "panel.html"
            source.write_text("<div></div>", encoding="utf-8")
            evidence = root / "evidence"
            evidence.mkdir()
            source_sha256 = LAB.sha256_file(source)
            provenance = {
                "source": {"sha256": source_sha256},
                "viewport": {"requested": {"width": 1, "height": 1},
                             "resolved": {"width": 1, "height": 1}},
                "browser": {"product": "Chrome", "version": "140.0"},
            }
            capture = {
                "provenance": provenance,
                "reference": {"asset_id": "reference:browser",
                               "logical_width": 1, "logical_height": 1,
                               "device_scale_factor": 1,
                               "authored_frame": {"x": 0, "y": 0,
                                                   "width": 1, "height": 1}},
            }
            (evidence / "capture.json").write_text(json.dumps(capture), encoding="utf-8")
            Image.new("RGBA", (1, 1), (0, 0, 0, 255)).save(evidence / "browser.png")
            capture["assets"] = [{"id": "reference:browser", "sha256": LAB.sha256_file(evidence / "browser.png"),
                                  "width_px": 1, "height_px": 1}]
            (evidence / "capture.json").write_text(json.dumps(capture), encoding="utf-8")
            (evidence / "dom-snapshot.json").write_text(
                json.dumps({"documents": []}), encoding="utf-8")

            def fake_run(argv: list[str], timeout_seconds: int) -> LAB.CommandResult:
                if "--render" in argv:
                    Image.new("RGBA", (1, 1), (0, 0, 0, 255)).save(
                        Path(argv[argv.index("--render") + 1]))
                    Path(argv[argv.index("--layout") + 1]).write_text(
                        json.dumps({"views": [], "nodes": [{
                            "id": "browser:root",
                            "rect": {"x": 0, "y": 0, "w": 1, "h": 1},
                        }]}), encoding="utf-8")
                    Path(argv[argv.index("--render-receipt") + 1]).write_text(
                        json.dumps({"schema": "pulp-design-ir-render-receipt-v1",
                                    "backend_requested": "skia", "backend_used": "skia",
                                    "ok": True}), encoding="utf-8")
                else:
                    output = Path(argv[argv.index("--output") + 1])
                    output.write_text(json.dumps({"root": {
                        "type": "frame", "style": {"width": 1, "height": 1},
                        "children": []}}),
                                      encoding="utf-8")
                return LAB.CommandResult(argv, 1, 0, "", "")

            sentinel = {
                "matches": [], "missing": [], "extra": [], "comparable_native": [],
                "ignored_native": [], "parentage_errors": [],
                "structurally_valid_matches": 0, "precision": 0.25, "recall": 0.25,
                "score": 0.123456,
            }
            with patch.object(LAB, "run_command", side_effect=fake_run), \
                    patch.object(LAB, "find_browser_evidence", return_value=evidence), \
                    patch.object(LAB, "structural_comparison", return_value=sentinel):
                report = LAB.compare_one(
                    Path("importer"), Path("observer"), source, root / "out", 60,
                    "claude", Path("chrome"),
                    fixture_metadata={"viewport": {"width": 1, "height": 1}})
            self.assertEqual(report["comparison"]["structural"]["score"], 0.123456)

    def test_canonical_json_digest_ignores_run_clock_fields(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            first = Path(directory) / "first.json"
            second = Path(directory) / "second.json"
            first.write_text(json.dumps({
                "imported_at": "2026-10-15T00:00:00Z",
                "capture": {"elapsed_ms": 12, "source": "same"},
            }), encoding="utf-8")
            second.write_text(json.dumps({
                "imported_at": "2026-10-15T00:01:00Z",
                "capture": {"elapsed_ms": 99, "source": "same"},
            }), encoding="utf-8")
            self.assertEqual(
                LAB.canonical_json_sha256(first),
                LAB.canonical_json_sha256(second))
            second.write_text(json.dumps({
                "imported_at": "2026-10-15T00:01:00Z",
                "capture": {"elapsed_ms": 99, "source": "changed"},
            }), encoding="utf-8")
            self.assertNotEqual(
                LAB.canonical_json_sha256(first),
                LAB.canonical_json_sha256(second))

    def test_canonical_capture_digest_ignores_settle_poll_counters(self) -> None:
        def capture(rounds: int, stable: int, frame_x: float = 20.0,
                    asset: str = "dom-snapshot.json") -> bytes:
            return json.dumps({
                "schema": "pulp-browser-capture-v1",
                "provenance": {"settle": {"rounds": rounds, "stable_rounds": stable,
                                          "elapsed_ms": rounds * 90}},
                "reference": {"authored_frame": {"x": frame_x, "y": 20.0,
                                                 "width": 420.0, "height": 220.0}},
                "documents": [{"snapshot_asset": asset}],
            }).encode()

        baseline = LAB.canonical_json_sha256(capture(14, 12))
        self.assertEqual(baseline, LAB.canonical_json_sha256(capture(15, 13)))
        # Negative controls: content fields still enter the digest.
        self.assertNotEqual(baseline, LAB.canonical_json_sha256(capture(15, 13, frame_x=21.0)))
        self.assertNotEqual(baseline, LAB.canonical_json_sha256(
            capture(15, 13, asset="other-snapshot.json")))
        # The counters are scrubbed only in capture envelopes, and only there.
        other = json.loads(capture(14, 12))
        other["schema"] = "pulp-design-ir-v1"
        changed = json.loads(capture(15, 13))
        changed["schema"] = "pulp-design-ir-v1"
        self.assertNotEqual(LAB.canonical_json_sha256(json.dumps(other).encode()),
                            LAB.canonical_json_sha256(json.dumps(changed).encode()))
        nested = json.loads(capture(14, 12))
        nested["reference"]["rounds"] = 1
        nested_changed = json.loads(capture(14, 12))
        nested_changed["reference"]["rounds"] = 2
        self.assertNotEqual(LAB.canonical_json_sha256(json.dumps(nested).encode()),
                            LAB.canonical_json_sha256(json.dumps(nested_changed).encode()))

    def test_canonical_capture_digest_excludes_only_raw_document_bindings(self) -> None:
        first = {"schema": "pulp-browser-capture-v1", "documents": [
            {"snapshot_asset": "dom-snapshot.json", "snapshot_sha256": "a" * 64}]}
        raw = json.dumps(first).encode()
        first["documents"][0]["snapshot_sha256"] = "b" * 64
        changed = json.dumps(first).encode()
        self.assertNotEqual(raw, changed)
        self.assertNotEqual(LAB.hashlib.sha256(raw).hexdigest(),
                            LAB.hashlib.sha256(changed).hexdigest())
        self.assertEqual(LAB.canonical_json_sha256(raw), LAB.canonical_json_sha256(changed))
        first["documents"][0]["snapshot_asset"] = "changed.json"
        self.assertNotEqual(LAB.canonical_json_sha256(raw),
                            LAB.canonical_json_sha256(json.dumps(first).encode()))

    def test_semantic_dom_digest_sees_content_and_ignores_session_ids(self) -> None:
        def snapshot(frame: str, theme: str, loader: str = "L1") -> bytes:
            return json.dumps({
                "strings": ["about:blank", frame, "INPUT"],
                "documents": [{"documentURL": 0, "frameId": 1,
                               "nodes": {"nodeName": [2]},
                               "loaderId": loader}],
                "formControlTheme": {"controls": [
                    {"node_index": 0, "theme": theme}]},
            }).encode()

        baseline = LAB.semantic_dom_sha256(snapshot("F" * 32, "native"))
        # Per-session CDP identifiers do not move it.
        self.assertEqual(baseline, LAB.semantic_dom_sha256(
            snapshot("A" * 32, "native", loader="L2")))
        # Content inside the snapshot does, where the canonical capture digest
        # (which drops the snapshot binding) cannot.
        self.assertNotEqual(baseline, LAB.semantic_dom_sha256(
            snapshot("F" * 32, "author")))
        changed = json.loads(snapshot("F" * 32, "native"))
        changed["strings"][2] = "SELECT"
        self.assertNotEqual(baseline, LAB.semantic_dom_sha256(
            json.dumps(changed).encode()))
        # Key order and whitespace are not content.
        self.assertEqual(baseline, LAB.semantic_dom_sha256(json.dumps(
            json.loads(snapshot("F" * 32, "native")), indent=2,
            sort_keys=True).encode()))

    def test_semantic_dom_digests_are_determinism_fields(self) -> None:
        self.assertIn("browser_dom_semantic_sha256", LAB_CLI.DETERMINISM_FIELDS)
        self.assertIn("native_dom_semantic_sha256", LAB_CLI.DETERMINISM_FIELDS)

    def test_canonical_digest_preserves_same_named_fields_outside_capture_document(self) -> None:
        for document in (
            {"documents": [{"snapshot_sha256": "a" * 64}]},
            {"schema": "different", "documents": [{"snapshot_sha256": "a" * 64}]},
            {"schema": "pulp-browser-capture-v1", "snapshot_sha256": "a" * 64},
            {"schema": "pulp-browser-capture-v1", "nested": {
                "documents": [{"snapshot_sha256": "a" * 64}]}},
            {"schema": "pulp-browser-capture-v1", "documents": [{
                "nested": {"snapshot_sha256": "a" * 64}}]},
        ):
            with self.subTest(document=document):
                raw = json.dumps(document).encode()
                changed = raw.replace(("a" * 64).encode(), ("b" * 64).encode())
                self.assertNotEqual(LAB.canonical_json_sha256(raw),
                                    LAB.canonical_json_sha256(changed))

    def test_canonical_equivalence_does_not_bypass_snapshot_verification(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            evidence = Path(directory)
            snapshot = evidence / "dom-snapshot.json"
            snapshot.write_bytes(b'{"documents":[]}\n')
            capture = {"schema": "pulp-browser-capture-v1", "documents": [{
                "snapshot_asset": "dom-snapshot.json",
                "snapshot_sha256": LAB.sha256_file(snapshot)}]}
            raw = json.dumps(capture).encode()
            binding, verified = LAB.verify_capture_snapshot_binding(capture, evidence, required=True)
            self.assertEqual(verified, snapshot.read_bytes())
            self.assertEqual(binding["status"], "producer-bound")
            capture["documents"][0]["snapshot_sha256"] = "0" * 64
            self.assertEqual(LAB.canonical_json_sha256(raw),
                             LAB.canonical_json_sha256(json.dumps(capture).encode()))
            with self.assertRaises(LAB.LabError):
                LAB.verify_capture_snapshot_binding(capture, evidence, required=True)

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
        geometry = {"score": 1.0, "gate": {"ok": True, "failed": []}}
        result = LAB.promotion_recommendation(
            ["canvas"], perfect, geometry, perfect, perfect)
        self.assertEqual(result["classification"], "browser-required")
        self.assertTrue(result["advisory_only"])
        result = LAB.promotion_recommendation(
            [], perfect, geometry, perfect, perfect)
        self.assertEqual(
            result["classification"], "native-with-browser-validation")
        self.assertTrue(result["threshold_eligible"])
        self.assertFalse(result["production_promotion_enabled"])
        # A geometry result without an evaluated gate is a failure.
        result = LAB.promotion_recommendation(
            [], perfect, perfect, perfect, perfect)
        self.assertFalse(result["threshold_eligible"])
        self.assertIn("geometry gate failed: gate not evaluated", result["reasons"])

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

    def test_observability_is_fail_closed_and_cache_state_is_declared(self) -> None:
        report = {
            "observability": {
                "ttfp": {"value_ms": None, "status": "unverified"},
                "ttni": {"value_ms": None, "status": "unverified"},
                "ttni_proxy": {"value_ms": 12, "status": "measured"},
                "ifnf": {"value_ms": None, "status": "readback-only"},
                "cache_state": {
                    "identity": "cold",
                    "dimensions": {"dawn_device": "not-applicable"},
                },
            },
            "promotion": {"classification": "browser-required"},
            "comparison": {name: {"score": 1.0} for name in
                            ("structural", "geometry", "typography", "visual")},
            "timings": {"browser_import_ms": 20, "native_import_ms": 12,
                        "native_render_ms": 4, "native_total_ms": 16,
                        "browser_to_native_import_speedup": 1.667},
            "classifications": [],
        }
        aggregate = LAB.aggregate_reports([report])
        self.assertEqual(aggregate["observability"]["ttfp"]["status"], "unverified")
        self.assertEqual(aggregate["observability"]["ttni"]["status"], "unverified")
        self.assertEqual(aggregate["observability"]["ttni_proxy"]["p50_ms"], 12.0)
        self.assertEqual(aggregate["observability"]["ifnf"]["status"], "readback-only")
        self.assertEqual(aggregate["observability"]["cache_states"], ["cold"])

    def test_timing_budget_is_opt_in_and_fail_closed(self) -> None:
        timings = {
            "browser_import_ms": 120,
            "native_import_ms": 20,
            "native_render_ms": 8,
            "native_total_ms": 28,
            "browser_to_native_import_speedup": 6.0,
        }
        self.assertEqual(
            LAB.timing_budget_result(timings, None)["status"], "not-requested")
        passed = LAB.timing_budget_result(
            timings, {"native_total_ms": 30})
        self.assertEqual(passed["status"], "pass")
        failed = LAB.timing_budget_result(
            timings, {"native_total_ms": 10, "browser_import_ms": 100})
        self.assertEqual(failed["status"], "fail")
        self.assertEqual(
            {row["metric"] for row in failed["violations"]},
            {"native_total_ms", "browser_import_ms"})
        report = {
            "timing_budget": failed,
            "promotion": {"classification": "browser-required"},
            "comparison": {name: {"score": 1.0} for name in
                            ("structural", "geometry", "typography", "visual")},
            "timings": timings,
            "classifications": [],
        }
        aggregate = LAB.aggregate_reports([report])
        self.assertEqual(aggregate["timing_budget"]["status"], "fail")
        self.assertEqual(aggregate["timing_budget"]["budgets_ms"], failed["budgets_ms"])

    def test_timing_budget_file_parser_rejects_unknown_or_empty_metrics(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            unknown = root / "unknown.json"
            unknown.write_text(json.dumps({"ttfp_ms": 10}))
            with self.assertRaisesRegex(ValueError, "unknown metric"):
                LAB_CLI.read_timing_budgets(unknown)
            empty = root / "empty.json"
            empty.write_text("{}")
            with self.assertRaisesRegex(ValueError, "at least one"):
                LAB_CLI.read_timing_budgets(empty)

    def test_benchmark_cache_buckets_require_an_explicit_campaign_state(self) -> None:
        reports = [
            {"timings": {"browser_import_ms": 100, "native_total_ms": 20}},
            {"timings": {"browser_import_ms": 80, "native_total_ms": 10}},
        ]
        unknown_cold = LAB_CLI.cache_bucket(reports, "unknown", "cold")
        self.assertEqual(unknown_cold["status"], "unverified")
        self.assertIn("not measured", unknown_cold["reason"])
        declared_warm = LAB_CLI.cache_bucket(reports, "warm", "warm")
        self.assertEqual(declared_warm["status"], "measured")
        self.assertEqual(declared_warm["cache_state"], "warm")
        self.assertEqual(declared_warm["browser_import_p50_ms"], 90.0)

    def test_benchmark_sequence_stats_are_named_by_position(self) -> None:
        reports = [
            {"timings": {"browser_import_ms": 100, "native_total_ms": 20}},
            {"timings": {"browser_import_ms": 80, "native_total_ms": 10}},
        ]
        self.assertEqual(
            LAB_CLI.timing_summary(reports),
            {
                "browser_import_p50_ms": 90.0,
                "browser_import_p95_ms": 100,
                "native_total_p50_ms": 15.0,
                "native_total_p95_ms": 20,
                "p50_ms_saved": 75.0,
            },
        )

    def test_determinism_receipt_requires_three_runs(self) -> None:
        reports = [{"integrity": {field: "a" for field in LAB_CLI.DETERMINISM_FIELDS}}]
        receipt = LAB_CLI.determinism_receipt(reports)
        self.assertEqual(receipt["status"], "unverified")
        self.assertEqual(receipt["required_runs"], 3)

    def test_determinism_receipt_passes_only_when_all_authenticated_digests_agree(self) -> None:
        def report(seed: str) -> dict:
            return {"integrity": {field: seed for field in LAB_CLI.DETERMINISM_FIELDS}}

        self.assertEqual(
            LAB_CLI.determinism_receipt([report("a"), report("a"), report("a")])["status"],
            "pass",
        )
        self.assertEqual(
            LAB_CLI.determinism_receipt([report("a"), report("a"), report("b")])["status"],
            "fail",
        )

    def test_benchmark_receipt_does_not_infer_cache_state_from_run_order(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            importer = root / "importer"
            observer = root / "observer"
            source = root / "fixture.html"
            importer.write_text("importer", encoding="utf-8")
            observer.write_text("observer", encoding="utf-8")
            source.write_text("<main>fixture</main>", encoding="utf-8")
            output = root / "benchmark"

            def fake_compare(*_args, **kwargs):
                return {
                    "fixture": {"id": "fixture"},
                    "source": {"path": source.name, "sha256": "source"},
                    "oracle": {"authority": "chromium"},
                    "timings": {
                        "browser_import_ms": 100,
                        "native_import_ms": 10,
                        "native_render_ms": 5,
                        "native_total_ms": 15,
                        "browser_to_native_import_speedup": 10.0,
                    },
                    "observability": {
                        "cache_state": {"identity": kwargs["cache_state"]},
                        "ttni_proxy": {"value_ms": 10},
                    },
                    "timing_budget": {"status": "not-requested"},
                    "promotion": {"classification": "browser-required"},
                    "comparison": {},
                }

            args = Namespace(
                importer=importer,
                observer=observer,
                file=source,
                output=output,
                runs=2,
                timeout_seconds=1,
                from_source="html",
                browser=None,
                cache_state="unknown",
                timing_budget=None,
            )
            with patch.object(LAB_CLI, "compare_one", side_effect=fake_compare):
                with redirect_stdout(io.StringIO()):
                    result = LAB_CLI.command_benchmark(args)
            self.assertEqual(result, 0)
            benchmark = json.loads(
                (output / "benchmark.json").read_text(encoding="utf-8"))
            self.assertEqual(benchmark["cold"]["status"], "unverified")
            self.assertEqual(benchmark["warm"]["status"], "unverified")
            self.assertEqual(benchmark["first_invocation"]["browser_import_ms"], 100)
            self.assertEqual(benchmark["repeat_runs"]["browser_import_p50_ms"], 100.0)

            try:
                import jsonschema
            except ImportError:
                self.skipTest("requires jsonschema")
            schema = json.loads(
                (REPO / "tools/import-validation/schemas/"
                 "importer-differential-benchmark-v1.schema.json")
                .read_text(encoding="utf-8"))
            validator = jsonschema.Draft202012Validator(schema)
            validator.validate(benchmark)

            legacy = dict(benchmark)
            legacy["cold"] = {
                "browser_import_ms": 100,
                "native_import_ms": 10,
                "native_render_ms": 5,
                "native_total_ms": 15,
                "browser_to_native_import_speedup": 10.0,
            }
            legacy["warm"] = {
                "browser_import_p50_ms": 100.0,
                "browser_import_p95_ms": 100,
                "native_total_p50_ms": 15.0,
                "native_total_p95_ms": 15,
                "p50_ms_saved": 85.0,
            }
            validator.validate(legacy)

    def test_command_benchmark_fails_when_authenticated_runs_disagree(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            source = root / "fixture.html"
            importer = root / "importer"
            observer = root / "observer"
            source.write_text("<main>fixture</main>", encoding="utf-8")
            importer.write_text("importer", encoding="utf-8")
            observer.write_text("observer", encoding="utf-8")
            reports = iter((
                {"integrity": {field: "a" for field in LAB_CLI.DETERMINISM_FIELDS},
                 "observability": {"cache_state": {"identity": "unknown"},
                                    "ttni_proxy": {"value_ms": 1}},
                 "timings": {"browser_import_ms": 1, "native_total_ms": 1},
                 "timing_budget": {"status": "not-requested"},
                 "fixture": {}, "source": {}, "oracle": {}, "promotion": {},
                 "comparison": {}},
                {"integrity": {field: "a" for field in LAB_CLI.DETERMINISM_FIELDS},
                 "observability": {"cache_state": {"identity": "unknown"},
                                    "ttni_proxy": {"value_ms": 1}},
                 "timings": {"browser_import_ms": 1, "native_total_ms": 1},
                 "timing_budget": {"status": "not-requested"},
                 "fixture": {}, "source": {}, "oracle": {}, "promotion": {},
                 "comparison": {}},
                {"integrity": {field: "b" for field in LAB_CLI.DETERMINISM_FIELDS},
                 "observability": {"cache_state": {"identity": "unknown"},
                                    "ttni_proxy": {"value_ms": 1}},
                 "timings": {"browser_import_ms": 1, "native_total_ms": 1},
                 "timing_budget": {"status": "not-requested"},
                 "fixture": {}, "source": {}, "oracle": {}, "promotion": {},
                 "comparison": {}},
            ))
            args = Namespace(importer=importer, observer=observer, file=source,
                             output=root / "benchmark", runs=3, timeout_seconds=1,
                             from_source="html", browser=None, cache_state="unknown",
                             timing_budget=None)
            with patch.object(LAB_CLI, "compare_one", side_effect=lambda *args, **kwargs: next(reports)), \
                    redirect_stdout(io.StringIO()):
                self.assertEqual(LAB_CLI.command_benchmark(args), 1)

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


def surface_snapshot(surface: tuple[float, float, float, float],
                     children: list[tuple[float, float, float, float]]) -> dict:
    """Minimal DOMSnapshot: html > body > authored root > children."""
    strings = ["#document", "HTML", "BODY", "MAIN", "SPAN"]
    names = [0, 1, 2, 3] + [4] * len(children)
    parents = [-1, 0, 1, 2] + [3] * len(children)
    types = [9, 1, 1, 1] + [1] * len(children)
    bounds = [[0, 0, 1280, 800], [0, 0, 1280, 800], list(surface)] + [
        list(child) for child in children]
    return {
        "strings": strings,
        "documents": [{
            "nodes": {"parentIndex": parents, "nodeType": types,
                      "nodeName": names},
            "layout": {"nodeIndex": list(range(1, 4 + len(children))),
                       "bounds": bounds},
        }],
    }


class GeometryGateTests(unittest.TestCase):
    # Shapes taken from the absolute-overlay fixture: a 420x220 stage at
    # (20, 20) and a far-right peak label.
    FRAME = (20.0, 20.0, 420.0, 220.0)
    CHILD = (368.4, 38.0, 51.6, 18.0)
    ROOT_ANCHOR = "capture:html[0]/body[0]/main[0]"
    CHILD_ANCHOR = "capture:html[0]/body[0]/main[0]/span[0]"

    def local_child(self, dx=0.0, dw_scale=1.0):
        return (self.CHILD[0] - self.FRAME[0] + dx, self.CHILD[1] - self.FRAME[1],
                self.CHILD[2] * dw_scale, self.CHILD[3])

    def build(self, *, native_child=None, recorded_shift=0.0, html_shift=0.0,
              root_shift=0.0, provide_placement=True):
        snapshot = surface_snapshot(self.FRAME, [self.CHILD])
        capture = {"reference": {"authored_frame": dict(
            zip(("x", "y", "width", "height"), self.FRAME))}}
        ir = {"root": {"attributes": {
            "browser_authored_frame_x": f"{self.FRAME[0] + recorded_shift:.6f}",
            "browser_authored_frame_y": f"{self.FRAME[1]:.6f}",
        }}}
        child = native_child or self.local_child()
        root_box = (0.0, 0.0, self.FRAME[2], self.FRAME[3])
        layout = {"nodes": [
            {"id": "browser:root", "rect": {"x": 0.0, "y": 0.0, "w": 1280, "h": 800}},
            {"id": "capture:html[0]", "rect": {
                "x": -self.FRAME[0] + html_shift, "y": -self.FRAME[1],
                "w": 1280, "h": 260}},
            {"id": self.ROOT_ANCHOR, "rect": dict(zip("xywh", (
                root_shift, 0.0, root_box[2], root_box[3])))},
            {"id": self.CHILD_ANCHOR, "rect": dict(zip("xywh", child))},
        ]}
        browser_raw = [
            LAB.BrowserNode(3, "main", 2, "", {}, self.FRAME, {}, 3),
            LAB.BrowserNode(4, "span", 3, "", {}, self.CHILD, {}, 4),
        ]
        registered_browser, registration = LAB.register_browser_nodes_to_native_frame(
            browser_raw, capture, capture)
        native = [
            LAB.NativeNode(self.ROOT_ANCHOR, "frame", "main", "", None, {}, root_box, 3),
            LAB.NativeNode(self.CHILD_ANCHOR, "frame", "span", "", None, {}, child, 4),
        ]
        registered = list(zip(registered_browser, native, [100.0, 100.0]))
        raw = list(zip(browser_raw, native, [100.0, 100.0]))
        placement = (LAB.frame_placement_check(snapshot, ir, layout, registered, capture)
                     if provide_placement else None)
        return LAB.geometry_metrics_with_registration(
            raw, registered, registration, placement)

    def test_known_good_panel_local_import_passes_with_raw_reported(self) -> None:
        geometry = self.build()
        self.assertTrue(geometry["gate"]["ok"], geometry["gate"])
        self.assertEqual(geometry["gate"]["matched_bounds"], 2)
        # The panel translation keeps raw page-space below the threshold; it
        # is reported and ungated.
        self.assertLess(geometry["score"], 0.98)
        self.assertEqual(geometry["role"], "diagnostic-raw-page-space")

    def test_registration_without_placement_proof_fails(self) -> None:
        geometry = self.build(provide_placement=False)
        self.assertEqual(geometry["gate"]["failed"], ["frame_placement"])

    def test_design_ir_origin_shift_fails_with_dom_and_layout_untouched(self) -> None:
        geometry = self.build(recorded_shift=3.0)
        self.assertEqual(geometry["gate"]["failed"], ["frame_placement"])
        checks = geometry["gate"]["checks"]["frame_placement"]["checks"]
        self.assertFalse(checks["design_ir_origin_matches_dom"]["ok"])
        self.assertTrue(checks["native_html_layout_at_negative_dom_origin"]["ok"])

    def test_native_html_rect_shift_fails(self) -> None:
        geometry = self.build(html_shift=3.0)
        self.assertEqual(geometry["gate"]["failed"], ["frame_placement"])
        checks = geometry["gate"]["checks"]["frame_placement"]["checks"]
        self.assertFalse(checks["native_html_layout_at_negative_dom_origin"]["ok"])

    def test_native_root_layout_off_panel_origin_fails(self) -> None:
        geometry = self.build(root_shift=3.0)
        self.assertEqual(geometry["gate"]["failed"], ["frame_placement"])
        checks = geometry["gate"]["checks"]["frame_placement"]["checks"]
        self.assertFalse(checks["native_root_layout_at_panel_origin"]["ok"])

    def test_placement_within_half_pixel_passes(self) -> None:
        self.assertTrue(self.build(recorded_shift=0.4, html_shift=-0.4)["gate"]["ok"])

    def test_split_inline_root_reads_its_own_row_not_a_continuation(self) -> None:
        # An inline authored root broken across lines carries one layout row
        # per line box. The placement proof must read the same row as the
        # browser node it is matched to; a continuation row starts at the
        # next line's left edge and would fail a correctly placed panel.
        snapshot = surface_snapshot(self.FRAME, [self.CHILD])
        layout = snapshot["documents"][0]["layout"]
        root_row = layout["nodeIndex"].index(3)
        layout["nodeIndex"].insert(root_row + 1, 3)
        layout["bounds"].insert(root_row + 1, [0.0, 240.0, 64.0, 18.0])
        surface = LAB.dom_authored_surface(snapshot)
        self.assertEqual(surface["bounds"], self.FRAME)
        browser = {node.index: node for node in LAB.browser_nodes(snapshot)}
        self.assertEqual(surface["bounds"], browser[surface["index"]].bounds)

    def test_missing_dom_authored_root_fails_closed(self) -> None:
        placement = LAB.frame_placement_check({"documents": []}, {}, {}, [])
        self.assertFalse(placement["ok"])
        self.assertFalse(placement["checks"]["dom_authored_root_found"]["ok"])

    def test_three_pixel_shift_on_farthest_node_fails_both_directions(self) -> None:
        for dx in (3.0, -3.0):
            with self.subTest(dx=dx):
                geometry = self.build(native_child=self.local_child(dx=dx))
                # The relative mean cannot see a few pixels this far from the
                # origin; the per-node maximum must.
                self.assertGreaterEqual(geometry["gate"]["score"], 0.98)
                self.assertEqual(geometry["gate"]["failed"],
                                 ["registered_max_abs_delta"])
                violation = (geometry["gate"]["checks"]["registered_max_abs_delta"]
                             ["violations"][0])
                self.assertEqual(violation["native_anchor"], self.CHILD_ANCHOR)
                self.assertAlmostEqual(violation["signed_delta"]["x"], dx)
        # Raw page-space rewards a shift toward the page origin, which is why
        # it is a diagnostic and not the gate.
        self.assertGreater(self.build(native_child=self.local_child(dx=3.0))["score"],
                           self.build()["score"])

    def test_ten_percent_narrow_box_fails(self) -> None:
        geometry = self.build(native_child=self.local_child(dw_scale=0.9))
        self.assertIn("registered_max_abs_delta", geometry["gate"]["failed"])

    def test_sub_pixel_delta_passes(self) -> None:
        geometry = self.build(native_child=self.local_child(dx=0.9))
        self.assertTrue(geometry["gate"]["ok"], geometry["gate"])

    def test_large_intra_panel_defect_fails_registered_mean(self) -> None:
        geometry = self.build(native_child=(4.0, 120.0, 60.0, 4.0))
        self.assertIn("registered_mean", geometry["gate"]["failed"])

    def test_aggregate_counts_geometry_gate_not_score(self) -> None:
        def report(geometry):
            return {
                "promotion": {"classification": "native-with-browser-validation"},
                "comparison": {
                    "structural": {"score": 1.0}, "geometry": geometry,
                    "typography": {"score": 1.0}, "visual": {"score": 1.0}},
                "timings": {"browser_import_ms": 1, "native_import_ms": 1,
                            "native_render_ms": 1, "native_total_ms": 2,
                            "browser_to_native_import_speedup": 1.0},
                "classifications": [],
            }

        aggregate = LAB.aggregate_reports([
            report(self.build()),
            report(self.build(native_child=self.local_child(dx=3.0)))])
        self.assertEqual(aggregate["gate_summary"]["geometry"]["pass_count"], 1)
        raw = aggregate["diagnostics"]["raw_page_space_geometry"]
        self.assertEqual(raw["role"], "diagnostic")
        self.assertEqual(raw["fixture_count"], 2)


class InputTypeTableTests(unittest.TestCase):
    """The three input-type tables, one per language, must not drift apart."""

    CPP_CONTROLS = REPO / "tools/import-design/browser_capture_controls.cpp"
    JS_THEME = REPO / "tools/import-design/browser_capture/form_control_theme.mjs"

    @staticmethod
    def string_set(text: str, opening: str, closing: str) -> set[str]:
        start = text.index(opening) + len(opening)
        return set(re.findall(r'"([^"]*)"', text[start:text.index(closing, start)]))

    def tables(self) -> tuple[set[str], set[str], set[str], frozenset[str]]:
        cpp = self.CPP_CONTROLS.read_text(encoding="utf-8")
        js = self.JS_THEME.read_text(encoding="utf-8")
        value_types = self.string_set(cpp, "kValueTypes = {", "};")
        text_types = self.string_set(js, "const TEXT_INPUT_TYPES = new Set([", "]);")
        button_types = self.string_set(js, "const BUTTON_INPUT_TYPES = new Set([", "]);")
        return value_types, text_types, button_types, FORM_CONTROLS.SHADOW_PART_INPUT_TYPES

    def test_value_text_types_are_the_theme_families_without_password(self) -> None:
        value_types, text_types, button_types, _ = self.tables()
        self.assertIn("password", text_types)
        self.assertEqual(value_types, (text_types | button_types) - {"password"})

    def test_known_input_type_tables_agree_across_languages(self) -> None:
        cpp = self.CPP_CONTROLS.read_text(encoding="utf-8")
        js = self.JS_THEME.read_text(encoding="utf-8")
        cpp_known = self.string_set(cpp, "kKnownInputTypes = {", "};")
        js_known = self.string_set(js, "const KNOWN_INPUT_TYPES = new Set([", "]);")
        self.assertEqual(len(cpp_known), 22)
        self.assertEqual(cpp_known, js_known)
        self.assertEqual(cpp_known, set(FORM_CONTROLS.KNOWN_INPUT_TYPES))
        value_types, text_types, button_types, shadow = self.tables()
        # Every family is drawn from the known states, so an unknown type can
        # only ever reach them as `text`.
        for table in (value_types, text_types, button_types, set(shadow)):
            self.assertLessEqual(table, cpp_known)

    def test_type_normalisation_follows_html(self) -> None:
        normalize = FORM_CONTROLS.normalize_input_type
        self.assertEqual(normalize(None), "text")
        self.assertEqual(normalize(""), "text")
        self.assertEqual(normalize("bogus"), "text")
        self.assertEqual(normalize("PASSWORD"), "password")
        self.assertEqual(normalize("Range"), "range")
        # Not trimmed: Chromium paints type=" password " as a text field.
        self.assertEqual(normalize(" password "), "text")
        # ASCII case-insensitive only: U+0130 does not fold to "i".
        self.assertEqual(normalize("\u0130mage"), "text")

    def test_unknown_type_is_a_text_field_for_the_boundary(self) -> None:
        node = SimpleNamespace(
            tag="input", index=1, bounds=(0.0, 0.0, 10.0, 10.0),
            attributes={"type": "bogus"}, styles={"appearance": "auto"})
        snapshot = {"documents": [], "strings": [], "formControlTheme": {
            "controls": [{"node_index": 1, "theme": "author"}]}}
        rows = FORM_CONTROLS.form_controls([node], snapshot)
        self.assertEqual(rows[0]["type"], "text")
        self.assertEqual(rows[0]["status"], FORM_CONTROLS.AUTHOR_STYLED)

    def test_shadow_part_types_are_outside_both_theme_families(self) -> None:
        _, text_types, button_types, shadow = self.tables()
        self.assertTrue(shadow)
        self.assertEqual(set(shadow) & (text_types | button_types), set())


class FormControlBoundaryTests(unittest.TestCase):
    """Themed form controls are a stated boundary, never a hidden score."""

    @staticmethod
    def form_snapshot(with_theme: bool = True) -> dict:
        # 0 DIV, 1 INPUT[type=range] (themed), 2 INPUT[type=checkbox]
        # (appearance:none), 3 INPUT[type=range] (appearance:none), 4 SELECT,
        # 5 OPTION (no layout), 6 its text, 7 SPAN (no layout, outside a select),
        # 8 BUTTON (appearance:auto, author background), 9 BUTTON
        # (appearance:auto, unstyled).
        strings = [
            "DIV", "INPUT", "SELECT", "OPTION", "#text", "SPAN", "type",
            "range", "checkbox", "block", "auto", "none", "inline-block",
            "A", "BUTTON",
        ]
        snapshot = {
            "strings": strings,
            "computedStyleNames": ["display", "appearance"],
            "documents": [{
                "nodes": {
                    "parentIndex": [-1, 0, 0, 0, 0, 4, 5, 0, 0, 0],
                    "nodeType": [1, 1, 1, 1, 1, 1, 3, 1, 1, 1],
                    "nodeName": [0, 1, 1, 1, 2, 3, 4, 5, 14, 14],
                    "nodeValue": [-1, -1, -1, -1, -1, -1, 13, -1, -1, -1],
                    "attributes": [[], [6, 7], [6, 8], [6, 7], [], [], [], [],
                                   [], []],
                },
                "layout": {
                    "nodeIndex": [0, 1, 2, 3, 4, 8, 9],
                    "bounds": [[0, 0, 100, 140], [10, 10, 40, 10],
                               [10, 30, 10, 10], [10, 50, 40, 10],
                               [10, 70, 40, 20], [10, 95, 40, 20],
                               [10, 118, 40, 20]],
                    "styles": [[9, 11], [12, 10], [12, 11], [12, 11], [12, 11],
                               [12, 10], [12, 10]],
                },
            }],
        }
        if with_theme:
            # The capture's verdicts, as `form_control_theme.mjs` writes them.
            snapshot["formControlTheme"] = {
                "schema": "pulp-form-control-theme-v1",
                "controls": [
                    {"node_index": 1, "theme": "native"},
                    {"node_index": 2, "theme": "none"},
                    {"node_index": 3, "theme": "none"},
                    {"node_index": 4, "theme": "none"},
                    {"node_index": 8, "theme": "author"},
                    {"node_index": 9, "theme": "native"},
                ],
            }
        return snapshot

    def test_unlaid_out_options_are_not_expected_boxes(self) -> None:
        tags = [node.tag for node in LAB.browser_nodes(self.form_snapshot())]
        # The select draws its option; the option has no box to lower.
        self.assertNotIn("option", tags)
        # Control: an element without layout OUTSIDE a select is still counted,
        # so the exclusion is the select's ownership and not missing layout.
        self.assertIn("span", tags)
        self.assertIn("select", tags)

    def test_control_status_separates_theme_from_author_styling(self) -> None:
        from importer_differential.form_controls import form_controls

        snapshot = self.form_snapshot()
        rows = form_controls(LAB.browser_nodes(snapshot), snapshot)
        status = {row["browser_node"]: row["status"] for row in rows}
        self.assertEqual(status[1], "ua-default-form-chrome")
        self.assertEqual(status[2], "author-styled")
        # appearance:none does not reach a range's thumb.
        self.assertEqual(status[3], "ua-shadow-form-part")
        self.assertEqual(status[4], "author-styled")
        # Both buttons compute `appearance: auto`. Author box styling drops
        # Chromium's theme, so only the unstyled one is the theme's.
        self.assertEqual(status[8], "author-styled")
        self.assertEqual(status[9], "ua-default-form-chrome")

        # A capture without verdicts cannot say who painted an `auto`
        # control, and does not guess either way.
        legacy = self.form_snapshot(with_theme=False)
        rows = form_controls(LAB.browser_nodes(legacy), legacy)
        status = {row["browser_node"]: row["status"] for row in rows}
        self.assertEqual(status[8], "theme-unresolved")
        self.assertEqual(status[9], "theme-unresolved")
        self.assertEqual(status[2], "author-styled")
        from importer_differential.form_controls import control_status
        self.assertEqual(control_status("button", "", {}),
                         "appearance-unrecorded")

    @requires_pillow
    def test_residual_is_attributed_to_the_control_that_owns_it(self) -> None:
        from importer_differential.form_controls import (
            attribute_residual, support_boundaries)

        controls = [
            {"tag": "input", "type": "range", "appearance": "auto",
             "status": "ua-default-form-chrome", "bounds": [10, 10, 20, 10]},
            {"tag": "input", "type": "checkbox", "appearance": "none",
             "status": "author-styled", "bounds": [60, 10, 10, 10]},
        ]
        reference = Image.new("RGBA", (100, 40), (240, 240, 240, 255))
        candidate = reference.copy()
        # Error only inside the themed control's box (page origin (5, 5)).
        for x in range(6, 24):
            for y in range(6, 14):
                reference.putpixel((x, y), (0, 0, 255, 255))
        attribution = attribute_residual(controls, reference, candidate,
                                         (5.0, 5.0), 1.0)
        shares = [row["foreground_error_share"]
                  for row in attribution["controls"]]
        self.assertEqual(shares, [1.0, 0.0])
        self.assertEqual(attribution["boundary_foreground_error_share"], 1.0)
        boundaries = support_boundaries(attribution)
        self.assertEqual([b["boundary"] for b in boundaries],
                         ["ua-default-form-chrome"])
        # Negative control: the same error under the author-styled control is
        # not the theme's, and no boundary takes credit for it.
        candidate = reference.copy()
        reference = Image.new("RGBA", (100, 40), (240, 240, 240, 255))
        for x in range(56, 64):
            for y in range(6, 14):
                reference.putpixel((x, y), (0, 0, 255, 255))
        attribution = attribute_residual(controls, reference,
                                         Image.new("RGBA", (100, 40),
                                                   (240, 240, 240, 255)),
                                         (5.0, 5.0), 1.0)
        self.assertEqual(attribution["boundary_foreground_error_share"], 0.0)
        self.assertEqual(attribution["controls"][1]["foreground_error_share"],
                         1.0)

    def test_boundary_makes_fixture_browser_required_without_moving_scores(
            self) -> None:
        perfect = {"score": 1.0}
        visual = {"score": 0.7, "registered_visual": {"score": 0.71}}
        boundary = [{"boundary": "ua-default-form-chrome", "control_count": 2,
                     "controls": ["button", "input[type=range]"],
                     "foreground_error_share": 0.9}]
        result = LAB.promotion_recommendation(
            [], perfect, perfect, perfect, visual, boundary)
        self.assertEqual(result["classification"], "browser-required")
        self.assertFalse(result["threshold_eligible"])
        self.assertEqual(result["visual_gate_score"], 0.71)
        self.assertEqual(result["visual_full_frame_score"], 0.7)
        self.assertEqual(result["support_boundaries"], [
            {"boundary": "ua-default-form-chrome", "control_count": 2,
             "foreground_error_share": 0.9}])
        self.assertFalse(result["boundary_overrides_passing_gates"])
        self.assertTrue(any("visual score below" in reason
                            for reason in result["reasons"]))
        # A fixture whose every numeric gate passes stays ineligible across the
        # boundary, and the report says the boundary, not a score, decided it.
        gated = {"score": 1.0, "gate": {"ok": True, "failed": []}}
        result = LAB.promotion_recommendation(
            [], perfect, gated, perfect, perfect, boundary)
        self.assertTrue(result["numeric_gates_pass"])
        self.assertFalse(result["threshold_eligible"])
        self.assertTrue(result["boundary_overrides_passing_gates"])
        self.assertIn("every numeric gate passes", result["reasons"][0])
        self.assertEqual(result["support_boundaries"][0]
                         ["foreground_error_share"], 0.9)
        # Without the boundary the same scores are eligible: the override is
        # the boundary's alone.
        result = LAB.promotion_recommendation(
            [], perfect, gated, perfect, perfect, [])
        self.assertTrue(result["threshold_eligible"])
        self.assertFalse(result["boundary_overrides_passing_gates"])
        recognition = LAB.source_recognition("<!doctype html><html></html>")
        causes = [finding["cause"] for finding in LAB.classify_gaps(
            recognition, perfect, perfect, perfect, visual,
            {"support_boundaries": boundary, "controls": []})]
        self.assertIn("ua-default-form-chrome", causes)

    def test_corpus_lists_every_boundary_reclassification(self) -> None:
        def report(path, promotion):
            return {
                "source": {"path": path},
                "promotion": {"classification": "browser-required",
                              **promotion},
                "comparison": {
                    "structural": {"score": 1.0},
                    "geometry": {"score": 1.0, "gate": {"ok": True}},
                    "typography": {"score": 1.0}, "visual": {"score": 1.0}},
                "timings": {"browser_import_ms": 1, "native_import_ms": 1,
                            "native_render_ms": 1, "native_total_ms": 2,
                            "browser_to_native_import_speedup": 1.0},
                "classifications": [],
            }

        boundary = [{"boundary": "ua-default-form-chrome", "control_count": 1,
                     "foreground_error_share": 0.4}]
        aggregate = LAB.aggregate_reports([
            report("passing.html", {"support_boundaries": boundary,
                                    "boundary_overrides_passing_gates": True}),
            report("plain.html", {})])
        rows = aggregate["support_boundary_fixtures"]
        self.assertEqual([row["fixture"] for row in rows], ["passing.html"])
        self.assertTrue(rows[0]["boundary_overrides_passing_gates"])
        from importer_differential.reports import format_corpus_summary
        self.assertIn("every numeric gate passes",
                      format_corpus_summary(aggregate))


@requires_pillow
class FullFrameClassificationTests(unittest.TestCase):
    PAGE = (16, 21, 34, 255)
    PANEL = (40, 60, 90, 255)
    INK = (230, 230, 230, 255)
    FRAME = {"x": 6, "y": 4, "width": 12, "height": 10}

    def panel(self, image, left, top):
        for y in range(top, top + 10):
            for x in range(left, left + 12):
                image.putpixel((x, y), self.PANEL)
        for x in range(left + 2, left + 8):
            image.putpixel((x, top + 4), self.INK)

    def classify(self, native_hook=None):
        browser = Image.new("RGBA", (40, 30), self.PAGE)
        self.panel(browser, 6, 4)
        native = Image.new("RGBA", (40, 30), self.PAGE)
        self.panel(native, 0, 0)
        if native_hook:
            native_hook(native)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            browser.save(root / "browser.png")
            native.save(root / "native.png")
            return LAB.visual_metrics(
                root / "browser.png", root / "native.png", root / "overlays",
                (40, 30), self.FRAME, 1.0)

    def promote(self, visual):
        perfect = {"score": 1.0}
        geometry = {"score": 1.0, "gate": {"ok": True, "failed": []}}
        return LAB.promotion_recommendation([], perfect, geometry, perfect, visual)

    def test_panel_translation_is_measured_and_promotes(self) -> None:
        visual = self.classify()
        self.assertLess(visual["score"], 0.995)
        self.assertEqual(visual["registered_visual"]["score"], 1.0)
        classification = visual["full_frame_classification"]
        self.assertEqual(classification["classification"], "panel-translation")
        self.assertEqual(classification["offset_px"], [6, 4])
        self.assertEqual(classification["shifted_score"], 1.0)
        self.assertEqual(
            classification["shifted_outside_frame"]["differing_pixel_count"], 0)
        self.assertTrue(LAB.full_frame_ok(visual))
        self.assertTrue(self.promote(visual)["threshold_eligible"])

    def test_planted_outside_frame_defect_stays_unexplained_and_blocks(self) -> None:
        def blob(native):
            for y in range(20, 24):
                for x in range(30, 34):
                    native.putpixel((x, y), self.INK)
        visual = self.classify(blob)
        # The panel itself still registers perfectly; only the full-frame
        # classification can see the defect.
        self.assertEqual(visual["registered_visual"]["score"], 1.0)
        classification = visual["full_frame_classification"]
        self.assertEqual(classification["classification"], "unexplained")
        self.assertIn("outside-frame", classification["residual_locus"])
        self.assertGreater(
            classification["shifted_outside_frame"]["differing_pixel_count"], 0)
        self.assertFalse(LAB.full_frame_ok(visual))
        result = self.promote(visual)
        self.assertFalse(result["threshold_eligible"])
        self.assertTrue(any(reason.startswith("full-frame visual")
                            and reason.endswith("classified unexplained")
                            for reason in result["reasons"]), result["reasons"])

    def test_page_background_mismatch_stays_unexplained(self) -> None:
        def host_clear(native):
            for y in range(native.height):
                for x in range(native.width):
                    if not (x < 12 and y < 10):
                        native.putpixel((x, y), (30, 30, 46, 255))
        classification = self.classify(host_clear)["full_frame_classification"]
        self.assertEqual(classification["classification"], "unexplained")
        self.assertIn("outside-frame", classification["residual_locus"])

    def test_subtle_outside_frame_defect_blocks_despite_high_shifted_score(self) -> None:
        def faint(native):
            native.putpixel((20, 15), (22, 21, 34, 255))
        classification = self.classify(faint)["full_frame_classification"]
        # One faint pixel barely moves the similarity score, but any differing
        # outside-frame pixel leaves the residual unexplained.
        self.assertGreaterEqual(classification["shifted_score"], 0.995)
        self.assertEqual(
            classification["shifted_outside_frame"]["differing_pixel_count"], 1)
        self.assertEqual(classification["classification"], "unexplained")

    def test_vacated_margin_uses_native_background_not_browser(self) -> None:
        # A panel that nearly fills the native canvas leaves only margin that
        # shifts out of view. The vacated page margin must be filled from the
        # native render; filling it from the browser would hide a native page
        # background that differs from Chromium's.
        browser = Image.new("RGBA", (40, 30), self.PAGE)
        native = Image.new("RGBA", (40, 30), (30, 30, 46, 255))
        for image, left, top in ((browser, 6, 4), (native, 0, 0)):
            for y in range(top, top + 26):
                for x in range(left, left + 34):
                    image.putpixel((x, y), self.PANEL)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            browser.save(root / "browser.png")
            native.save(root / "native.png")
            visual = LAB.visual_metrics(
                root / "browser.png", root / "native.png", root / "overlays",
                (40, 30), {"x": 6, "y": 4, "width": 34, "height": 26}, 1.0)
        classification = visual["full_frame_classification"]
        self.assertEqual(classification["fill"], [30, 30, 46, 255])
        self.assertEqual(classification["classification"], "unexplained")

    def test_unoffset_residual_is_unexplained(self) -> None:
        frame = Image.new("RGBA", (40, 30), self.PAGE)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            frame.save(root / "browser.png")
            Image.new("RGBA", (40, 30), self.INK).save(root / "native.png")
            visual = LAB.visual_metrics(
                root / "browser.png", root / "native.png", root / "overlays",
                (40, 30), {"x": 0, "y": 0, "width": 40, "height": 30}, 1.0)
        self.assertEqual(visual["full_frame_classification"]["classification"],
                         "unexplained")
        self.assertFalse(LAB.full_frame_ok(visual))

    def test_low_full_frame_without_classification_fails_closed(self) -> None:
        self.assertFalse(LAB.full_frame_ok({"score": 0.9}))
        self.assertFalse(LAB.full_frame_ok({"score": 0.9, "full_frame_classification": {
            "classification": "panel-translation", "shifted_score": 0.9,
            "shifted_outside_frame": {"differing_pixel_count": 0}}}))
        self.assertFalse(LAB.full_frame_ok({"score": 0.9, "full_frame_classification": {
            "classification": "panel-translation", "shifted_score": 1.0,
            "shifted_outside_frame": {"differing_pixel_count": 3}}}))
        self.assertTrue(LAB.full_frame_ok({"score": 0.999}))

    def test_boundary_and_full_frame_reasons_are_both_reported(self) -> None:
        def blob(native):
            for y in range(20, 24):
                for x in range(30, 34):
                    native.putpixel((x, y), self.INK)
        perfect = {"score": 1.0}
        geometry = {"score": 1.0, "gate": {"ok": True, "failed": []}}
        boundary = [{"boundary": "ua-default-form-chrome", "control_count": 1,
                     "controls": ["input[type=range]"],
                     "foreground_error_share": 0.5}]
        result = LAB.promotion_recommendation(
            [], perfect, geometry, perfect, self.classify(blob), boundary)
        self.assertEqual(result["classification"], "browser-required")
        self.assertFalse(result["threshold_eligible"])
        self.assertTrue(any(reason.startswith("support boundary ua-default-form-chrome")
                            for reason in result["reasons"]), result["reasons"])
        self.assertTrue(any(reason.startswith("full-frame visual")
                            and reason.endswith("classified unexplained")
                            for reason in result["reasons"]), result["reasons"])


if __name__ == "__main__":
    unittest.main()
