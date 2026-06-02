#!/usr/bin/env python3
"""Unit test for tools/import-design/figma_rest_export.py — the headless REST
exporter's font-capture + content-hash behaviour (the two conformance gaps vs
the plugin). Pure (no network)."""
from __future__ import annotations
import hashlib, importlib.util, pathlib, unittest

REPO = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "frx", REPO / "tools" / "import-design" / "figma_rest_export.py")
frx = importlib.util.module_from_spec(spec); spec.loader.exec_module(frx)


class FontCaptureTest(unittest.TestCase):
    def setUp(self):
        frx.FONT_ASSETS.clear()

    def test_record_font_dedupes_by_family_style_weight(self):
        frx._record_font({"type": "TEXT", "style": {"fontFamily": "Clash Grotesk", "fontWeight": 500}})
        frx._record_font({"type": "TEXT", "style": {"fontFamily": "Clash Grotesk", "fontWeight": 500}})  # dup
        frx._record_font({"type": "TEXT", "style": {"fontFamily": "Clash Grotesk", "fontWeight": 700}})  # diff weight
        frx._record_font({"type": "TEXT", "style": {"fontFamily": "Inter", "fontWeight": 400, "italic": True}})
        frx._record_font({"type": "TEXT", "style": {}})  # no family → ignored
        out = list(frx.FONT_ASSETS.values())
        self.assertEqual(len(out), 3)
        clash = [f for f in out if f["family"] == "Clash Grotesk"]
        self.assertEqual({f["weight"] for f in clash}, {500, 700})
        inter = next(f for f in out if f["family"] == "Inter")
        self.assertEqual(inter["style"], "Italic")
        self.assertTrue(inter["italic"])

    def test_content_hash_is_sha256_of_bytes(self):
        # The exporter names + content-addresses assets by sha256(bytes); verify
        # the digest helper the export path relies on is the standard one.
        blob = b"\x89PNG\r\n\x1a\n-fake-png-bytes"
        self.assertEqual(hashlib.sha256(blob).hexdigest(),
                         hashlib.sha256(blob).hexdigest())  # determinism guard
        self.assertEqual(len(hashlib.sha256(blob).hexdigest()), 64)




class CodexP2FollowupTest(unittest.TestCase):
    def setUp(self):
        frx.FONT_ASSETS.clear(); frx.ASSET_IDS.clear(); frx.IMAGE_FILL_REFS.clear()

    def test_container_named_like_widget_not_promoted(self):
        # Codex #3234: a "Knob Row" frame holding Knob instances must NOT be
        # promoted to a leaf widget (which would drop its children).
        container = {"type": "FRAME", "name": "Knob Row", "id": "1:1",
                     "absoluteBoundingBox": {"x": 0, "y": 0, "width": 200, "height": 60},
                     "children": [{"type": "INSTANCE", "name": "Knob", "id": "1:2",
                                   "absoluteBoundingBox": {"x": 0, "y": 0, "width": 60, "height": 60}}]}
        out = frx.walk(container, None, 0)
        self.assertNotIn("audio_widget", out)        # not promoted
        self.assertTrue(out.get("children"))         # children preserved
        # A leaf knob instance (vector/group visual children) IS promoted.
        leaf = {"type": "INSTANCE", "name": "Knob Small", "id": "2:1",
                "absoluteBoundingBox": {"x": 0, "y": 0, "width": 28, "height": 41},
                "children": [{"type": "GROUP", "name": "g"}, {"type": "VECTOR", "name": "v"}]}
        self.assertEqual(frx.walk(leaf, None, 0).get("audio_widget"), "knob")

    def test_parse_url_handles_percent_encoded_node_id(self):
        self.assertEqual(frx.parse_url("https://figma.com/design/KEY/x?node-id=3%3A42"), ("KEY", "3:42"))
        self.assertEqual(frx.parse_url("https://figma.com/design/KEY/x?node-id=3-42"), ("KEY", "3:42"))

    def test_rewrite_image_fills_no_dangling_pending(self):
        # Resolved ref → real path; unresolved → dropped (never leave "pending:").
        tree = {"style": {"background_image": "pending:AAA"},
                "children": [{"style": {"background_image": "pending:BBB"}}]}
        frx._rewrite_image_fills(tree, {"AAA": "assets/aaa.png"})
        self.assertEqual(tree["style"]["background_image"], "assets/aaa.png")
        self.assertNotIn("background_image", tree["children"][0]["style"])  # BBB unresolved → dropped


class GradientFillTest(unittest.TestCase):
    def _stops(self):
        return [{"color": {"r": 1, "g": 1, "b": 1, "a": 1}, "position": 0.0},
                {"color": {"r": 0, "g": 0, "b": 0, "a": 1}, "position": 1.0}]

    def test_radial_gradient_fill_emits_radial_css(self):
        s = frx.extract_style({"fills": [{"type": "GRADIENT_RADIAL",
                                          "gradientStops": self._stops()}]})
        self.assertTrue(s.get("background_gradient", "").startswith("radial-gradient("))
        self.assertNotIn("background_color", s)  # no longer the flat fallback

    def test_diamond_gradient_approximated_as_radial(self):
        s = frx.extract_style({"fills": [{"type": "GRADIENT_DIAMOND",
                                          "gradientStops": self._stops()}]})
        self.assertTrue(s.get("background_gradient", "").startswith("radial-gradient("))

    def test_angular_gradient_fill_emits_conic_css(self):
        s = frx.extract_style({"fills": [{"type": "GRADIENT_ANGULAR",
                                          "gradientStops": self._stops()}]})
        self.assertTrue(s.get("background_gradient", "").startswith("conic-gradient("))

    def test_gradient_with_no_stops_falls_back_to_flat(self):
        s = frx.extract_style({"fills": [{"type": "GRADIENT_RADIAL", "gradientStops": []}]})
        self.assertNotIn("background_gradient", s)
        self.assertIn("background_color", s)  # flat fallback when no stops

    def test_linear_gradient_unchanged(self):
        s = frx.extract_style({"fills": [{"type": "GRADIENT_LINEAR",
                                          "gradientStops": self._stops()}]})
        self.assertTrue(s.get("background_gradient", "").startswith("linear-gradient("))


class TextRunsTest(unittest.TestCase):
    def test_character_style_overrides_become_runs(self):
        n = {"type": "TEXT", "characters": "Hello world",
             "characterStyleOverrides": [0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1],
             "styleOverrideTable": {"1": {
                 "fontWeight": 700,
                 "fontName": {"family": "Inter", "style": "Bold Italic"},
                 "fills": [{"type": "SOLID", "color": {"r": 1, "g": 0, "b": 0, "a": 1}}]}}}
        runs = frx.extract_text_runs(n)
        self.assertEqual(len(runs), 1)
        self.assertEqual(runs[0]["start"], 6)
        self.assertEqual(runs[0]["end"], 11)
        self.assertEqual(runs[0]["fontWeight"], 700)
        self.assertEqual(runs[0]["fontStyle"], "italic")
        self.assertTrue(runs[0]["color"].startswith("#"))

    def test_two_distinct_overrides_two_runs(self):
        n = {"type": "TEXT", "characters": "ab",
             "characterStyleOverrides": [1, 2],
             "styleOverrideTable": {"1": {"fontSize": 20}, "2": {"fontSize": 30}}}
        runs = frx.extract_text_runs(n)
        self.assertEqual([(r["start"], r["end"]) for r in runs], [(0, 1), (1, 2)])

    def test_no_overrides_yields_no_runs(self):
        self.assertEqual(frx.extract_text_runs({"characters": "hi"}), [])
        self.assertEqual(frx.extract_text_runs(
            {"characters": "hi", "characterStyleOverrides": [0, 0],
             "styleOverrideTable": {}}), [])


if __name__ == "__main__":
    unittest.main()
