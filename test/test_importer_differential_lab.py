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
                               "device_scale_factor": 1},
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
                        json.dumps({"views": []}), encoding="utf-8")
                    Path(argv[argv.index("--render-receipt") + 1]).write_text(
                        json.dumps({"schema": "pulp-design-ir-render-receipt-v1",
                                    "backend_requested": "skia", "backend_used": "skia",
                                    "ok": True}), encoding="utf-8")
                else:
                    output = Path(argv[argv.index("--output") + 1])
                    output.write_text(json.dumps({"root": {"type": "frame",
                                                              "children": []}}),
                                      encoding="utf-8")
                return LAB.CommandResult(argv, 1, 0, "", "")

            with patch.object(LAB, "run_command", side_effect=fake_run), \
                    patch.object(LAB, "find_browser_evidence", return_value=evidence), \
                    patch.object(LAB, "verify_capture_binding", wraps=LAB.verify_capture_binding) as binding, \
                    patch.object(LAB, "verify_capture_image_binding", wraps=LAB.verify_capture_image_binding) as image_binding:
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
                               "device_scale_factor": 1},
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
                        json.dumps({"views": []}), encoding="utf-8")
                    Path(argv[argv.index("--render-receipt") + 1]).write_text(
                        json.dumps({"schema": "pulp-design-ir-render-receipt-v1",
                                    "backend_requested": "skia", "backend_used": "skia",
                                    "ok": True}), encoding="utf-8")
                else:
                    output = Path(argv[argv.index("--output") + 1])
                    output.write_text(json.dumps({"root": {"type": "frame",
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


if __name__ == "__main__":
    unittest.main()
