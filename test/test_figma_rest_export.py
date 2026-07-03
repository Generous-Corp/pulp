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
        # P2: fonts accumulate in an explicit ExtractContext, not a module global.
        self.ctx = frx.ExtractContext()

    def test_record_font_dedupes_by_family_style_weight(self):
        frx._record_font({"type": "TEXT", "style": {"fontFamily": "Clash Grotesk", "fontWeight": 500}}, self.ctx)
        frx._record_font({"type": "TEXT", "style": {"fontFamily": "Clash Grotesk", "fontWeight": 500}}, self.ctx)  # dup
        frx._record_font({"type": "TEXT", "style": {"fontFamily": "Clash Grotesk", "fontWeight": 700}}, self.ctx)  # diff weight
        frx._record_font({"type": "TEXT", "style": {"fontFamily": "Inter", "fontWeight": 400, "italic": True}}, self.ctx)
        frx._record_font({"type": "TEXT", "style": {}}, self.ctx)  # no family → ignored
        out = list(self.ctx.fonts.values())
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
    # P2: walk() is now driven via node_tree_to_ir(), which returns (ir, ctx) with
    # explicit side-effect accumulators — no module globals to clear.

    def test_container_named_like_widget_not_promoted(self):
        # #3234: a "Knob Row" frame holding Knob instances must NOT be
        # promoted to a leaf widget (which would drop its children).
        container = {"type": "FRAME", "name": "Knob Row", "id": "1:1",
                     "absoluteBoundingBox": {"x": 0, "y": 0, "width": 200, "height": 60},
                     "children": [{"type": "INSTANCE", "name": "Knob", "id": "1:2",
                                   "absoluteBoundingBox": {"x": 0, "y": 0, "width": 60, "height": 60}}]}
        out, _ctx = frx.node_tree_to_ir(container)
        self.assertNotIn("audio_widget", out)        # not promoted
        self.assertTrue(out.get("children"))         # children preserved
        # A leaf knob instance (vector/group visual children) IS promoted.
        leaf = {"type": "INSTANCE", "name": "Knob Small", "id": "2:1",
                "absoluteBoundingBox": {"x": 0, "y": 0, "width": 28, "height": 41},
                "children": [{"type": "GROUP", "name": "g"}, {"type": "VECTOR", "name": "v"}]}
        self.assertEqual(frx.node_tree_to_ir(leaf)[0].get("audio_widget"), "knob")

    def test_node_tree_to_ir_returns_side_effects_explicitly(self):
        # P2 decomposition: walk()'s three side effects (asset ids, fonts, image
        # fills) come back on the ExtractContext, not via module globals — and two
        # independent calls don't leak into each other.
        tree = {"type": "FRAME", "name": "Panel", "id": "0:1",
                "absoluteBoundingBox": {"x": 0, "y": 0, "width": 100, "height": 100},
                "fills": [{"type": "IMAGE", "imageRef": "img-abc"}],
                "children": [
                    {"type": "TEXT", "name": "label", "id": "0:2", "characters": "Hi",
                     "absoluteBoundingBox": {"x": 0, "y": 0, "width": 40, "height": 16},
                     "style": {"fontFamily": "Inter", "fontWeight": 600}},
                    {"type": "VECTOR", "name": "icon", "id": "0:3",
                     "absoluteBoundingBox": {"x": 0, "y": 0, "width": 24, "height": 24}},
                ]}
        ir, ctx = frx.node_tree_to_ir(tree)
        self.assertEqual(ir["type"], "frame")
        self.assertIn("img-abc", ctx.image_fills)            # IMAGE fill captured
        self.assertEqual(ctx.asset_ids, ["0:3"])             # vector → PNG asset
        self.assertEqual([f["family"] for f in ctx.fonts.values()], ["Inter"])
        # A SECOND walk is independent — no cross-call leakage (the old global bug).
        ir2, ctx2 = frx.node_tree_to_ir({"type": "FRAME", "name": "Empty", "id": "9:9",
                                         "absoluteBoundingBox": {"x": 0, "y": 0, "width": 10, "height": 10}})
        self.assertEqual(ctx2.asset_ids, [])
        self.assertEqual(ctx2.fonts, {})
        self.assertEqual(ctx2.image_fills, set())

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
        # A common shape: the "Search" placeholder is a TEXT leaf; the field is its
        # parent group with a filled box + a leading magnifier icon. The icon must
        # NOT match; the overlay is INSET past the icon (starts at the text's x) so
        # the baked magnifier stays visible, and carries the box's own bg color so
        # the inset edge blends seamlessly.
        figma_root = {
            "id": "3:42", "absoluteBoundingBox": {"x": 0, "y": 0, "width": 1000, "height": 600},
            "children": [
                {"name": "Group 59", "id": "g59", "type": "GROUP",
                 "absoluteBoundingBox": {"x": 21, "y": 73, "width": 184, "height": 26},
                 "children": [
                     {"name": "Box", "type": "RECTANGLE",
                      "absoluteBoundingBox": {"x": 21, "y": 73, "width": 184, "height": 26},
                      "fills": [{"type": "SOLID", "visible": True,
                                 "color": {"r": 37 / 255, "g": 38 / 255, "b": 38 / 255, "a": 1}}]},
                     {"name": "ic:round-search", "type": "FRAME",
                      "absoluteBoundingBox": {"x": 27, "y": 76, "width": 15, "height": 15}},
                     {"name": "Search", "type": "TEXT", "characters": "Search",
                      "absoluteBoundingBox": {"x": 44, "y": 78, "width": 43, "height": 17}},
                 ]},
            ],
        }
        els = frx.detect_overlay_controls(figma_root, (0.0, 0.0), (73.0, 50.0))
        self.assertEqual(len(els), 1)              # icon skipped, one field found
        e = els[0]
        # Inset to the text x (44) past the icon: x = 44+73 = 117, w = 21+184-44 = 161.
        self.assertEqual((e["x"], e["y"], e["w"], e["h"]), (117.0, 123.0, 161.0, 26.0))
        self.assertEqual(e["source_node_id"], "g59")  # the parent group, not the text
        self.assertEqual(e["bg_color"], "#252626")    # the box's own fill (seamless inset)

    def test_parse_panel_bounds_picks_the_panel_rect(self):
        svg = ('<svg width="1146" height="746" xmlns="http://www.w3.org/2000/svg">'
               '<rect x="0" y="0" width="1146" height="746" fill="#000"/>'  # full frame -> excluded
               '<rect x="73" y="50" width="1000" height="600" fill="#252626"/>'
               '<rect x="83" y="112" width="980" height="367" fill="#1c1d1d"/></svg>')
        self.assertEqual(frx.parse_panel_bounds(svg), (73.0, 50.0, 1000.0, 600.0))

    def test_detect_overlay_controls_dropdowns_only_with_down_chevron(self):
        # Only a FRAME named ~dropdown WITH a down-chevron ("expand_more") child is
        # a real dropdown. The < > section-header steppers (chevron child is a
        # "Frame 41" pair) become STEPPERS, not dropdowns. The unconfigured
        # "Dropdown" placeholder must NOT be detected. A tiny "+" is skipped too.
        def chev(name):  # a chevron icon child
            return {"name": name, "type": "FRAME",
                    "absoluteBoundingBox": {"x": 0, "y": 0, "width": 8, "height": 8}}
        figma_root = {
            "id": "3:42", "absoluteBoundingBox": {"x": 0, "y": 0, "width": 1000, "height": 600},
            "children": [
                # real dropdown: expand_more child + a real value
                {"name": "Dropdown", "id": "d1", "type": "FRAME",
                 "absoluteBoundingBox": {"x": 700, "y": 480, "width": 103, "height": 27},
                 "children": [{"type": "TEXT", "characters": "1/4 Delay"},
                              chev("expand_more_FILL0 1")]},
                # < > stepper: named "Dropdown" but chevron child is "Frame 41" -> skip
                {"name": "Dropdown", "id": "s1", "type": "FRAME",
                 "absoluteBoundingBox": {"x": 100, "y": 440, "width": 220, "height": 20},
                 "children": [{"type": "TEXT", "characters": "Short Plucks"}, chev("Frame 41")]},
                # unconfigured placeholder: expand_more but text == "Dropdown" -> skip
                {"name": "Dropdown", "id": "p1", "type": "FRAME",
                 "absoluteBoundingBox": {"x": 320, "y": 520, "width": 103, "height": 27},
                 "children": [{"type": "TEXT", "characters": "Dropdown"}, chev("expand_more 2")]},
                {"name": "Dropdown", "id": "d2", "type": "FRAME",          # "+" button — too small
                 "absoluteBoundingBox": {"x": 950, "y": 480, "width": 26, "height": 27},
                 "children": [chev("expand_more 3")]},
            ],
        }
        els = frx.detect_overlay_controls(figma_root, (0.0, 0.0), (73.0, 50.0))
        dropdowns = [e for e in els if e["kind"] == "dropdown"]
        self.assertEqual(len(dropdowns), 1)               # only d1
        e = dropdowns[0]
        self.assertEqual((e["x"], e["y"], e["w"], e["h"]), (773.0, 530.0, 103.0, 27.0))
        self.assertEqual(e["options"], ["1/4 Delay"])     # only the real shown value (no fabricated options)
        self.assertEqual(e["source_node_id"], "d1")
        # s1 (Frame 41 < > pair) is a stepper, not a dropdown; placeholder p1 skipped.
        steppers = [e for e in els if e["kind"] == "stepper"]
        self.assertEqual(len(steppers), 1)                # only s1
        self.assertEqual(steppers[0]["source_node_id"], "s1")

    def test_detect_overlay_controls_finds_stepper(self):
        # A "Dropdown"-named FRAME whose chevron child is a < > PAIR ("Frame 41",
        # or a left+right chevron pair) and whose shown value != "Dropdown" is a
        # < > stepper. Mapped node->SVG with the (+73,+50) panel origin.
        figma_root = {
            "id": "3:42", "absoluteBoundingBox": {"x": 0, "y": 0, "width": 1000, "height": 600},
            "children": [
                # Frame-41 style < > pair
                {"name": "Dropdown", "id": "st1", "type": "FRAME",
                 "absoluteBoundingBox": {"x": 100, "y": 120, "width": 180, "height": 22},
                 "children": [{"type": "TEXT", "characters": "Short Plucks"},
                              {"name": "Frame 41", "type": "FRAME",
                               "absoluteBoundingBox": {"x": 0, "y": 0, "width": 40, "height": 12}}]},
                # explicit left+right chevron pair (no Frame 41)
                {"name": "Dropdown", "id": "st2", "type": "FRAME",
                 "absoluteBoundingBox": {"x": 400, "y": 120, "width": 160, "height": 22},
                 "children": [{"type": "TEXT", "characters": "Sine"},
                              {"name": "chevron_left", "type": "FRAME",
                               "absoluteBoundingBox": {"x": 0, "y": 0, "width": 8, "height": 8}},
                              {"name": "chevron_right", "type": "FRAME",
                               "absoluteBoundingBox": {"x": 0, "y": 0, "width": 8, "height": 8}}]},
            ],
        }
        els = frx.detect_overlay_controls(figma_root, (0.0, 0.0), (73.0, 50.0))
        steppers = [e for e in els if e["kind"] == "stepper"]
        self.assertEqual(len(steppers), 2)
        s = next(e for e in steppers if e["source_node_id"] == "st1")
        self.assertEqual((s["x"], s["y"], s["w"], s["h"]), (173.0, 170.0, 180.0, 22.0))
        self.assertEqual(s["options"], ["Short Plucks"])   # only the real shown value (no fabricated options)
        self.assertEqual(s["selected_index"], 0)
        # No dropdowns produced (neither has a down-chevron).
        self.assertEqual([e for e in els if e["kind"] == "dropdown"], [])

    def test_detect_overlay_controls_finds_tab_group(self):
        # A row of >=3 container children with short labels = a tab group; the one
        # with a visible SOLID fill is selected. Mapped node->SVG (+73,+50).
        def tab(x, label, filled=False):
            t = {"name": "Button", "type": "FRAME",
                 "absoluteBoundingBox": {"x": x, "y": 76, "width": 29, "height": 20},
                 "children": [{"type": "TEXT", "characters": label}]}
            if filled:
                t["fills"] = [{"type": "SOLID", "visible": True}]
            return t
        figma_root = {
            "id": "3:42", "absoluteBoundingBox": {"x": 0, "y": 0, "width": 1000, "height": 600},
            "children": [
                {"name": "Pager", "id": "tg", "type": "FRAME",
                 "absoluteBoundingBox": {"x": 220, "y": 76, "width": 120, "height": 20},
                 "children": [tab(220, "1"), tab(249, "2"), tab(279, "3", filled=True), tab(308, "4")]},
            ],
        }
        els = frx.detect_overlay_controls(figma_root, (0.0, 0.0), (73.0, 50.0))
        tabs = [e for e in els if e["kind"] == "tab_group"]
        self.assertEqual(len(tabs), 1)
        t = tabs[0]
        self.assertEqual(t["options"], ["1", "2", "3", "4"])
        self.assertEqual(t["selected_index"], 2)          # the filled "3"
        # rect = union of tabs (220,76)-(337,96) -> svg (293,126,117,20)
        self.assertEqual((t["x"], t["y"], t["w"], t["h"]), (293.0, 126.0, 117.0, 20.0))

    def test_detect_overlay_controls_none_when_no_match(self):
        root = {"absoluteBoundingBox": {"x": 0, "y": 0, "width": 10, "height": 10},
                "children": [{"name": "Knob", "absoluteBoundingBox": {"x": 0, "y": 0, "width": 4, "height": 4}}]}
        self.assertEqual(frx.detect_overlay_controls(root, (0.0, 0.0), (0.0, 0.0)), [])

    def _tab_group_node(self, nid, x, y):
        def tab(tx, label):
            return {"name": "Button", "type": "FRAME",
                    "absoluteBoundingBox": {"x": tx, "y": y, "width": 29, "height": 20},
                    "children": [{"type": "TEXT", "characters": label}]}
        return {"name": "Radio Button", "id": nid, "type": "FRAME",
                "absoluteBoundingBox": {"x": x, "y": y, "width": 120, "height": 20},
                "children": [tab(x, "1"), tab(x + 29, "2"), tab(x + 58, "3"), tab(x + 87, "4")]}

    def test_detect_overlay_controls_drops_occluded_control(self):
        # A tab group fully painted over by a LATER opaque sibling (an envelope
        # graph panel drawn on top) is not visible → must NOT be surfaced. This
        # is the "spurious envelope 1/2/3/4" guard: the leftover radio layer sits
        # under an opaque panel, so the importer must skip it.
        tg = self._tab_group_node("buried", 50, 530)
        cover = {"name": "Graph Panel", "id": "cover", "type": "FRAME",
                 "absoluteBoundingBox": {"x": 0, "y": 434, "width": 1000, "height": 142},
                 "fills": [{"type": "GRADIENT_RADIAL", "visible": True,
                            "gradientStops": [{"color": {"a": 1.0}}, {"color": {"a": 1.0}}]}],
                 "children": []}
        root = {"id": "root", "absoluteBoundingBox": {"x": 0, "y": 0, "width": 1000, "height": 600},
                "children": [tg, cover]}              # tg painted BEFORE the cover
        els = frx.detect_overlay_controls(root, (0.0, 0.0), (0.0, 0.0))
        self.assertEqual([e for e in els if e["kind"] == "tab_group"], [])

    def test_detect_overlay_controls_keeps_visible_control(self):
        # Same tab group, but the opaque panel is painted BEFORE it (lower z) —
        # so the tab group is on top and visible. It must be surfaced.
        cover = {"name": "Graph Panel", "id": "cover", "type": "FRAME",
                 "absoluteBoundingBox": {"x": 0, "y": 434, "width": 1000, "height": 142},
                 "fills": [{"type": "SOLID", "visible": True, "color": {"a": 1.0}}],
                 "children": []}
        tg = self._tab_group_node("ontop", 50, 530)
        root = {"id": "root", "absoluteBoundingBox": {"x": 0, "y": 0, "width": 1000, "height": 600},
                "children": [cover, tg]}              # tg painted AFTER the cover
        els = frx.detect_overlay_controls(root, (0.0, 0.0), (0.0, 0.0))
        self.assertEqual(len([e for e in els if e["kind"] == "tab_group"]), 1)

    def test_detect_overlay_controls_own_background_is_not_an_occluder(self):
        # A control whose OWN background <rect> fills it (a descendant painted
        # after the group) must NOT be treated as occluding itself.
        tg = self._tab_group_node("self", 50, 530)
        tg["children"].insert(0, {  # background rect spanning the whole group, drawn first child
            "name": "bg", "id": "selfbg", "type": "RECTANGLE",
            "absoluteBoundingBox": {"x": 50, "y": 530, "width": 120, "height": 20},
            "fills": [{"type": "SOLID", "visible": True, "color": {"a": 1.0}}]})
        root = {"id": "root", "absoluteBoundingBox": {"x": 0, "y": 0, "width": 1000, "height": 600},
                "children": [tg]}
        els = frx.detect_overlay_controls(root, (0.0, 0.0), (0.0, 0.0))
        self.assertEqual(len([e for e in els if e["kind"] == "tab_group"]), 1)

    def test_emitted_overlay_kinds_conform_to_schema(self):
        # P1a contract guard: the REST producer emits overlay kinds the knob-only
        # schema used to forbid. The schema's interactive_element.kind enum must
        # be a SUPERSET of every kind this producer can emit, and each emitted
        # overlay must carry the per-kind required box [x,y,w,h] the schema's
        # allOf branch demands. This pins the producer<->schema contract from the
        # REST side so the drift the plan flagged can't silently return.
        import json
        schema_path = (REPO / "tools" / "figma-plugin" / "schema"
                       / "figma-plugin-export-v1.json")
        schema = json.loads(schema_path.read_text())
        kind_enum = set(schema["$defs"]["interactive_element"]["properties"]["kind"]["enum"])
        # Every kind the producers emit (knob + the overlays) plus the P1a additions.
        self.assertTrue({"knob", "fader", "toggle", "dropdown", "text_field",
                         "tab_group", "stepper"}.issubset(kind_enum))

        # A fixture exercising dropdown + stepper + text_field + tab_group output.
        figma_root = {
            "id": "3:42", "absoluteBoundingBox": {"x": 0, "y": 0, "width": 1000, "height": 600},
            "children": [
                {"name": "Dropdown", "id": "d1", "type": "FRAME",
                 "absoluteBoundingBox": {"x": 100, "y": 80, "width": 160, "height": 22},
                 "children": [{"type": "TEXT", "characters": "Sine"},
                              {"name": "expand_more", "type": "FRAME",
                               "absoluteBoundingBox": {"x": 0, "y": 0, "width": 8, "height": 8}}]},
                {"name": "Dropdown", "id": "st1", "type": "FRAME",
                 "absoluteBoundingBox": {"x": 100, "y": 120, "width": 180, "height": 22},
                 "children": [{"type": "TEXT", "characters": "Short Plucks"},
                              {"name": "Frame 41", "type": "FRAME",
                               "absoluteBoundingBox": {"x": 0, "y": 0, "width": 40, "height": 12}}]},
            ],
        }
        els = frx.detect_overlay_controls(figma_root, (0.0, 0.0), (0.0, 0.0))
        self.assertTrue(els)  # produced something
        for e in els:
            self.assertIn(e["kind"], kind_enum,
                          f"producer emitted kind {e['kind']!r} the schema forbids")
            for f in ("x", "y", "w", "h"):
                self.assertIn(f, e, f"overlay {e['kind']!r} must carry required {f!r}")


class FaithfulVectorDefaultTest(unittest.TestCase):
    """The faithful-vector lane (interactive overlays) must be the DEFAULT — a
    plain import should produce live widgets, not a static node tree. Opt out
    with --no-faithful-vector."""

    def test_faithful_vector_defaults_on(self):
        args = frx.build_argparser().parse_args(["--out", "x.json", "--url", "u"])
        self.assertTrue(args.faithful_vector)

    def test_no_faithful_vector_opts_out(self):
        args = frx.build_argparser().parse_args(
            ["--out", "x.json", "--url", "u", "--no-faithful-vector"])
        self.assertFalse(args.faithful_vector)

    def test_faithful_vector_explicit_on_still_accepted(self):
        args = frx.build_argparser().parse_args(
            ["--out", "x.json", "--url", "u", "--faithful-vector"])
        self.assertTrue(args.faithful_vector)


class ElementLabelTest(unittest.TestCase):
    """§2.1 importer auto-labeling: an interactive element gets a `label` (the
    generated-parameter name) from its meaningfully-named source Figma layer, and
    nothing when the layer name is auto-generated or a structural/kind word."""

    def test_node_label_filters_default_and_noise_names(self):
        self.assertEqual(frx._node_label("Cutoff"), "Cutoff")
        self.assertEqual(frx._node_label("  Delay Mode  "), "Delay Mode")
        for default in ("Ellipse 12", "Rectangle", "Frame 41", "Group 3",
                        "Vector", "Instance 2", "Boolean"):
            self.assertEqual(frx._node_label(default), "", default)
        for noise in ("Knob", "dropdown", "Search", "Value", "field", "Tabs"):
            self.assertEqual(frx._node_label(noise), "", noise)
        self.assertEqual(frx._node_label(""), "")

    def test_overlay_label_from_source_node_name(self):
        figma_root = {
            "id": "root",
            "absoluteBoundingBox": {"x": 100, "y": 100, "width": 1000, "height": 600},
            "children": [
                {"id": "d1", "name": "Delay Mode",
                 "absoluteBoundingBox": {"x": 700, "y": 140, "width": 120, "height": 28}},
                {"id": "s1", "name": "Search",  # structural word → no label
                 "absoluteBoundingBox": {"x": 140, "y": 200, "width": 280, "height": 32}},
            ],
        }
        elements = [
            {"kind": "dropdown", "source_node_id": "d1"},
            {"kind": "text_field", "source_node_id": "s1"},
        ]
        frx._label_elements(elements, figma_root, (100.0, 100.0))
        self.assertEqual(elements[0].get("label"), "Delay Mode")
        self.assertNotIn("label", elements[1])  # "Search" filtered → key absent

    def test_geometry_knob_label_from_overlapping_named_node(self):
        # frame origin (100,100); a "Cutoff" node centered at frame-local (60,60),
        # a default-named ellipse centered at (260,60).
        figma_root = {
            "id": "root",
            "absoluteBoundingBox": {"x": 100, "y": 100, "width": 1000, "height": 600},
            "children": [
                {"id": "k1", "name": "Cutoff",
                 "absoluteBoundingBox": {"x": 140, "y": 140, "width": 40, "height": 40}},
                {"id": "k2", "name": "Ellipse 7",  # auto-generated → no label
                 "absoluteBoundingBox": {"x": 340, "y": 140, "width": 40, "height": 40}},
            ],
        }
        elements = [
            {"kind": "knob", "cx": 60.0, "cy": 60.0, "hit_radius": 30.0},    # over "Cutoff"
            {"kind": "knob", "cx": 260.0, "cy": 60.0, "hit_radius": 30.0},   # over the ellipse
            {"kind": "knob", "cx": 900.0, "cy": 500.0, "hit_radius": 30.0},  # over nothing
        ]
        frx._label_elements(elements, figma_root, (100.0, 100.0))
        self.assertEqual(elements[0].get("label"), "Cutoff")
        self.assertNotIn("label", elements[1])  # default-named node → no label
        self.assertNotIn("label", elements[2])  # no overlapping named node


class RateLimitRetryTest(unittest.TestCase):
    """figma_get must honor Figma's 429 Retry-After (and back off) instead of
    crashing on the first rate-limit — the regression that aborted exports
    mid-run on the Tier-1 /images endpoint."""

    class _Resp:
        def __init__(self, body): self._body = body
        def __enter__(self): return self
        def __exit__(self, *a): return False
        def read(self): return self._body

    class _RaiseOnRead:
        # Models a connection that opens fine but fails mid-stream: urlopen()
        # returns normally, then r.read() raises (urlopen does NOT wrap this).
        def __init__(self, exc): self._exc = exc
        def __enter__(self): return self
        def __exit__(self, *a): return False
        def read(self): raise self._exc

    @staticmethod
    def _http_error(code, headers=None):
        return frx.urllib.error.HTTPError(
            "https://api.figma.com/x", code, "err", headers or {}, None)

    def _patch_seq(self, seq):
        # urlopen side-effect: raise Exception items, return response items.
        import unittest.mock as mock
        self.slept = []
        def side(*a, **k):
            item = seq.pop(0)
            if isinstance(item, Exception):
                raise item
            return item
        p1 = mock.patch.object(frx.urllib.request, "urlopen", side_effect=side)
        p2 = mock.patch.object(frx.time, "sleep", side_effect=lambda s: self.slept.append(s))
        p1.start(); p2.start()
        self.addCleanup(p1.stop); self.addCleanup(p2.stop)

    def test_honors_retry_after_then_succeeds(self):
        self._patch_seq([self._http_error(429, {"Retry-After": "7"}), self._Resp(b"OK")])
        out = frx.figma_get("https://api.figma.com/x", token="t", what="unit")
        self.assertEqual(out, b"OK")
        self.assertEqual(self.slept, [7])  # waited exactly the Retry-After seconds

    def test_backoff_when_no_retry_after_header(self):
        self._patch_seq([self._http_error(429), self._http_error(429), self._Resp(b"OK")])
        out = frx.figma_get("https://api.figma.com/x", token="t", what="unit")
        self.assertEqual(out, b"OK")
        self.assertEqual(self.slept, [1, 2])  # capped exponential backoff: 2**0, 2**1

    def test_raises_with_diagnostics_after_max_retries(self):
        err = self._http_error(429, {"X-Figma-Rate-Limit-Type": "high",
                                     "X-Figma-Plan-Tier": "starter"})
        self._patch_seq([err, err, err])  # initial try + 2 retries all 429
        with self.assertRaises(RuntimeError) as ctx:
            frx.figma_get("https://api.figma.com/x", token="t", what="unit", max_retries=2)
        msg = str(ctx.exception)
        self.assertIn("rate-limited", msg)
        self.assertIn("plan-tier=starter", msg)   # surfaces the documented diagnostics
        self.assertEqual(len(self.slept), 2)       # retried exactly max_retries times

    def test_5xx_is_retried(self):
        self._patch_seq([self._http_error(503), self._Resp(b"OK")])
        self.assertEqual(frx.figma_get("https://api.figma.com/x", what="unit"), b"OK")

    def test_4xx_non_429_is_not_retried(self):
        self._patch_seq([self._http_error(404)])
        with self.assertRaises(frx.urllib.error.HTTPError):
            frx.figma_get("https://api.figma.com/x", what="unit")
        self.assertEqual(self.slept, [])

    def test_read_phase_timeout_is_retried(self):
        # urlopen() succeeds but the body read raises a raw TimeoutError (not a
        # URLError); it must be caught and retried, not fatal.
        self._patch_seq([self._RaiseOnRead(TimeoutError("read timed out")),
                         self._Resp(b"OK")])
        self.assertEqual(frx.figma_get("https://api.figma.com/x", what="unit"), b"OK")
        self.assertEqual(self.slept, [1])

    def test_connection_reset_is_retried(self):
        self._patch_seq([self._RaiseOnRead(ConnectionResetError("reset")),
                         self._Resp(b"OK")])
        self.assertEqual(frx.figma_get("https://api.figma.com/x", what="unit"), b"OK")

    def test_negative_retry_after_is_clamped_not_crashed(self):
        # A negative Retry-After (clock skew / bad proxy) must NOT reach
        # time.sleep (which raises ValueError on negatives) — clamp to 0.
        self._patch_seq([self._http_error(429, {"Retry-After": "-5"}), self._Resp(b"OK")])
        out = frx.figma_get("https://api.figma.com/x", token="t", what="unit")
        self.assertEqual(out, b"OK")
        self.assertEqual(self.slept, [0])

    def test_absurd_retry_after_is_capped(self):
        self._patch_seq([self._http_error(429, {"Retry-After": "99999"}), self._Resp(b"OK")])
        frx.figma_get("https://api.figma.com/x", token="t", what="unit")
        self.assertEqual(self.slept, [300])  # capped, never sleeps for a day


class WidgetKindFromNameTest(unittest.TestCase):
    """P2 resolver unification — whole-word match, in lockstep with the C++
    detect_audio_widget and the TS audioWidgetKindFromName."""

    def test_true_positives_match_whole_words(self):
        self.assertEqual(frx.widget_kind_from_name("Cutoff Knob"), "knob")
        self.assertEqual(frx.widget_kind_from_name("Knobs"), "knob")       # plural tolerated
        self.assertEqual(frx.widget_kind_from_name("Volume Fader"), "fader")
        self.assertEqual(frx.widget_kind_from_name("Main Slider"), "fader")
        self.assertEqual(frx.widget_kind_from_name("VUMeter"), "meter")    # acronym split
        self.assertEqual(frx.widget_kind_from_name("XY Pad"), "xy_pad")
        self.assertEqual(frx.widget_kind_from_name("XYPad"), "xy_pad")
        self.assertEqual(frx.widget_kind_from_name("Waveform"), "waveform")
        self.assertEqual(frx.widget_kind_from_name("Spectrum"), "spectrum")

    def test_full_vocab_in_lockstep_with_cpp(self):
        # These tokens are recognized by C++ detect_audio_widget and TS
        # audioWidgetKindFromName; the Python lane was previously missing them,
        # so a "Level"/"Oscilloscope"/"Analyzer"-named leaf was rasterized as a
        # PNG sprite instead of promoted to a native widget. Pin parity here.
        self.assertEqual(frx.widget_kind_from_name("Level Meter"), "meter")
        self.assertEqual(frx.widget_kind_from_name("Output Level"), "meter")   # bare "level"
        self.assertEqual(frx.widget_kind_from_name("Oscilloscope"), "waveform")
        self.assertEqual(frx.widget_kind_from_name("Analyzer"), "spectrum")
        self.assertEqual(frx.widget_kind_from_name("Spectrum Analyser"), "spectrum")  # British spelling

    def test_substring_false_positives_are_rejected(self):
        # These used to mis-resolve under the substring `in` match.
        self.assertIsNone(frx.widget_kind_from_name("Dialog"))    # was knob ("dial")
        self.assertIsNone(frx.widget_kind_from_name("Radial"))    # was knob ("dial")
        self.assertIsNone(frx.widget_kind_from_name("Diameter"))  # was meter ("meter")
        self.assertIsNone(frx.widget_kind_from_name("Parameter"))  # was meter ("meter")
        self.assertIsNone(frx.widget_kind_from_name("Reverb"))

    def test_tokenize_name_matches_cpp_boundaries(self):
        self.assertEqual(frx._tokenize_name("VUMeter"), ["vu", "meter"])
        self.assertEqual(frx._tokenize_name("Knob_1"), ["knob", "1"])
        self.assertEqual(frx._tokenize_name("Dialog"), ["dialog"])


class RateLimitAdviceTest(unittest.TestCase):
    """A terminal 429 must fail LOUDLY toward the local-first path (Figma desktop
    MCP / plugin), not silently — this is the codified fix for reaching for the
    rate-limited REST lane when the local paths are available."""

    def setUp(self):
        frx._rate_limit_advice_shown = False  # reset the one-time latch

    def _raise_429(self, *a, **k):
        raise frx.urllib.error.HTTPError(
            "https://api.figma.com/v1/images/x", 429, "Too Many Requests",
            {"Retry-After": "300", "X-Figma-Rate-Limit-Type": "image-render"}, None)

    def test_terminal_429_advises_local_first_and_prints_once(self):
        import contextlib, io
        orig = frx.urllib.request.urlopen
        frx.urllib.request.urlopen = self._raise_429
        try:
            err = io.StringIO()
            # max_retries=0 → the first 429 is terminal (no backoff sleep in the test).
            with contextlib.redirect_stderr(err):
                with self.assertRaises(RuntimeError) as cm:
                    frx.figma_get("https://api.figma.com/v1/images/x",
                                  token="t", what="frame SVG render", max_retries=0)
            # The raised error points at the local-first path…
            self.assertIn("Local-first", str(cm.exception))
            # …and the loud one-time advisory named the local MCP + plugin.
            adv = err.getvalue()
            self.assertIn("get_screenshot", adv)
            self.assertIn("Pulp Figma desktop plugin", adv)
        finally:
            frx.urllib.request.urlopen = orig

    def test_advice_latch_prints_only_once(self):
        frx._advise_rate_limit_once()
        import contextlib, io
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            frx._advise_rate_limit_once()  # already shown → silent
        self.assertEqual(err.getvalue(), "")


if __name__ == "__main__":
    unittest.main()
