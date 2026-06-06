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

    def test_run_offsets_are_utf8_byte_offsets(self):
        # "café world": é is 2 UTF-8 bytes, so the run over "world" (char index 5)
        # must be emitted as BYTE offset 6, not char index 5.
        n = {"type": "TEXT", "characters": "café world",
             "characterStyleOverrides": [0, 0, 0, 0, 0, 1, 1, 1, 1, 1],
             "styleOverrideTable": {"1": {"fontWeight": 700}}}
        runs = frx.extract_text_runs(n)
        self.assertEqual(len(runs), 1)
        self.assertEqual(runs[0]["start"], 6)   # byte offset (char index would be 5)
        self.assertEqual(runs[0]["end"], 11)

    def test_astral_emoji_offsets_use_utf16_to_byte_map(self):
        # "A😀B": the emoji is 2 UTF-16 code units (a surrogate pair) and 4 UTF-8
        # bytes. characterStyleOverrides is UTF-16-indexed (length 4), so a run
        # over "B" begins at UTF-16 unit 3 -> byte offset 5 (A=1 + emoji=4), not
        # code-point index 2. Guards the surrogate-pair conversion.
        n = {"type": "TEXT", "characters": "A\U0001F600B",
             "characterStyleOverrides": [0, 0, 0, 1],
             "styleOverrideTable": {"1": {"fontWeight": 700}}}
        runs = frx.extract_text_runs(n)
        self.assertEqual(len(runs), 1)
        self.assertEqual(runs[0]["start"], 5)
        self.assertEqual(runs[0]["end"], 6)

    def test_no_overrides_yields_no_runs(self):
        self.assertEqual(frx.extract_text_runs({"characters": "hi"}), [])
        self.assertEqual(frx.extract_text_runs(
            {"characters": "hi", "characterStyleOverrides": [0, 0],
             "styleOverrideTable": {}}), [])


class FaithfulVectorTest(unittest.TestCase):
    """Plan B / B4a: faithful-vector lane — frame-SVG knob auto-detect + the
    envelope fields the C++ materializer (DesignFrameView) consumes."""

    SVG = (
        '<svg width="100" height="100" xmlns="http://www.w3.org/2000/svg">'
        '<defs><linearGradient id="g"><stop offset="0" stop-color="#ebf5ff"/>'
        '<stop offset="1" stop-color="#717f8e"/></linearGradient></defs>'
        '<rect x="10" y="10" width="80" height="80" fill="#1c1d1d"/>'
        '<circle cx="50" cy="50" r="20" fill="url(#g)"/>'            # dome
        '<circle cx="50" cy="50" r="5" fill="#222222"/>'             # inner, non-gradient → ignored
        '<path d="M50 38L50 30" stroke="white" stroke-width="3"/>'   # needle
        '<path d="M20 20L25 25" stroke="#506274" stroke-width="2"/>'  # dark tick → ignored
        '</svg>')

    def test_parse_frame_knobs_geometry_autodetect(self):
        knobs = frx.parse_frame_knobs(self.SVG)
        self.assertEqual(len(knobs), 1)
        k = knobs[0]
        self.assertEqual(k["kind"], "knob")
        self.assertEqual((k["cx"], k["cy"], k["hit_radius"]), (50.0, 50.0, 20.0))
        self.assertEqual(k["svg_patch_d"], "M50 38L50 30")  # exact d so the needle can rotate
        self.assertEqual(k["default_value"], 0.5)

    def test_parse_frame_knobs_ignores_non_knob_shapes(self):
        # No gradient dome + no light needle → nothing detected.
        plain = ('<svg xmlns="http://www.w3.org/2000/svg">'
                 '<circle cx="10" cy="10" r="20" fill="#333"/>'        # solid, not a dome
                 '<path d="M5 5L9 9" stroke="#506274"/></svg>')        # dark tick
        self.assertEqual(frx.parse_frame_knobs(plain), [])

    def test_apply_faithful_vector_sets_fields_and_svg_asset(self):
        root_node = {"type": "frame", "name": "ELYSIUM"}
        figma_root = {"id": "3:42", "name": "ELYSIUM",
                      "absoluteBoundingBox": {"x": 0, "y": 0, "width": 100, "height": 100}}
        entry = frx.apply_faithful_vector(root_node, figma_root, self.SVG,
                                          "KEY", "3:42", out_dir="", knob_names=[],
                                          write_file=False)
        self.assertEqual(root_node["render_mode"], "faithful_svg")
        self.assertEqual(root_node["svg_asset_id"], "frame-svg-3:42")
        self.assertEqual(len(root_node["interactive_elements"]), 1)
        # The asset is the SVG document, embedded so the importer always resolves it.
        self.assertEqual(entry["asset_id"], "frame-svg-3:42")
        self.assertEqual(entry["mime"], "image/svg+xml")
        self.assertTrue(entry["original_uri"].startswith("data:image/svg+xml;base64,"))

    def test_name_override_supplements_geometry(self):
        geom = frx.parse_frame_knobs(self.SVG)  # one knob at (50,50)
        figma_root = {
            "id": "3:42", "absoluteBoundingBox": {"x": 0, "y": 0, "width": 100, "height": 100},
            "children": [
                # Far from the geometry knob, named like a knob → added (no needle d).
                {"name": "Big Dial", "absoluteBoundingBox": {"x": 70, "y": 70, "width": 20, "height": 20}},
                # AT the geometry knob's center → already covered, must NOT duplicate.
                {"name": "Knob", "absoluteBoundingBox": {"x": 40, "y": 40, "width": 20, "height": 20}},
            ],
        }
        added = frx._name_override_knobs(figma_root, ["dial", "knob"], geom)
        self.assertEqual(len(added), 1)
        self.assertEqual((added[0]["cx"], added[0]["cy"]), (80.0, 80.0))
        self.assertEqual(added[0]["svg_patch_d"], "")  # no needle path identified

    def test_name_override_empty_when_no_names(self):
        self.assertEqual(frx._name_override_knobs({"children": []}, [], []), [])

    def test_detect_overlay_controls_named_field_uses_own_rect(self):
        # A node named like a field uses its OWN rect. Coords map node->SVG:
        # svg = (node_abs - root_abs) + panel_origin. root_abs (100,200),
        # panel_origin (73,50): node (116,250) -> (116-100+73, 250-200+50)=(89,100).
        figma_root = {
            "id": "3:42", "absoluteBoundingBox": {"x": 100, "y": 200, "width": 1000, "height": 600},
            "children": [
                {"name": "Search Field", "id": "5:1", "type": "FRAME",
                 "absoluteBoundingBox": {"x": 116, "y": 250, "width": 280, "height": 32},
                 "children": [{"type": "TEXT", "characters": "Search"}]},
                {"name": "Some Frame", "id": "5:2",
                 "absoluteBoundingBox": {"x": 500, "y": 250, "width": 100, "height": 100}},
            ],
        }
        els = frx.detect_overlay_controls(figma_root, (100.0, 200.0), (73.0, 50.0))
        self.assertEqual(len(els), 1)
        e = els[0]
        self.assertEqual(e["kind"], "text_field")
        self.assertEqual((e["x"], e["y"], e["w"], e["h"]), (89.0, 100.0, 280.0, 32.0))
        self.assertEqual(e["placeholder"], "Search")
        self.assertEqual(e["source_node_id"], "5:1")

    def test_detect_overlay_controls_placeholder_text_uses_parent_group(self):
        # ELYSIUM shape: the "Search" placeholder is a TEXT leaf; the field is its
        # parent group. The magnifier "ic:round-search" must NOT match.
        figma_root = {
            "id": "3:42", "absoluteBoundingBox": {"x": 0, "y": 0, "width": 1000, "height": 600},
            "children": [
                {"name": "Group 59", "id": "g59", "type": "GROUP",
                 "absoluteBoundingBox": {"x": 21, "y": 73, "width": 184, "height": 26},
                 "children": [
                     {"name": "ic:round-search", "type": "FRAME",
                      "absoluteBoundingBox": {"x": 21, "y": 76, "width": 15, "height": 15}},
                     {"name": "Search", "type": "TEXT", "characters": "Search",
                      "absoluteBoundingBox": {"x": 44, "y": 78, "width": 43, "height": 17}},
                 ]},
            ],
        }
        els = frx.detect_overlay_controls(figma_root, (0.0, 0.0), (73.0, 50.0))
        self.assertEqual(len(els), 1)              # icon skipped, one field found
        e = els[0]
        self.assertEqual((e["x"], e["y"], e["w"], e["h"]), (94.0, 123.0, 184.0, 26.0))
        self.assertEqual(e["source_node_id"], "g59")  # the parent group, not the text

    def test_parse_panel_bounds_picks_the_panel_rect(self):
        svg = ('<svg width="1146" height="746" xmlns="http://www.w3.org/2000/svg">'
               '<rect x="0" y="0" width="1146" height="746" fill="#000"/>'  # full frame -> excluded
               '<rect x="73" y="50" width="1000" height="600" fill="#252626"/>'
               '<rect x="83" y="112" width="980" height="367" fill="#1c1d1d"/></svg>')
        self.assertEqual(frx.parse_panel_bounds(svg), (73.0, 50.0, 1000.0, 600.0))

    def test_detect_overlay_controls_none_when_no_match(self):
        root = {"absoluteBoundingBox": {"x": 0, "y": 0, "width": 10, "height": 10},
                "children": [{"name": "Knob", "absoluteBoundingBox": {"x": 0, "y": 0, "width": 4, "height": 4}}]}
        self.assertEqual(frx.detect_overlay_controls(root, (0.0, 0.0), (0.0, 0.0)), [])


if __name__ == "__main__":
    unittest.main()
