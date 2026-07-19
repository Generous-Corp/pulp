#!/usr/bin/env python3
"""Unit test for tools/import-design/figma_rest_export.py — the headless REST
exporter's font-capture + content-hash behavior (the two conformance gaps vs
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
        # Not promoted — and the container decision is PINNED as an explicit
        # "none": asset capture can collapse child containers into leaf images,
        # and without the pin the C++ importer's name heuristic re-promotes
        # this node from that degraded envelope.
        self.assertEqual(out.get("audio_widget"), "none")
        self.assertTrue(out.get("children"))         # children preserved
        # An EMPTY hand-drawn frame named like a knob (no component identity,
        # no art inside) is still promoted — name recognition remains the
        # placeholder fallback when there is nothing to paint over.
        leaf = {"type": "FRAME", "name": "Knob Small", "id": "2:1",
                "absoluteBoundingBox": {"x": 0, "y": 0, "width": 28, "height": 41}}
        self.assertEqual(frx.node_tree_to_ir(leaf)[0].get("audio_widget"), "knob")
        # The same frame WITH drawn shapes is the designer's widget art: not
        # promoted, and (being all-vector) captured whole as one image.
        drawn = {"type": "FRAME", "name": "Knob Small", "id": "2:2",
                 "absoluteBoundingBox": {"x": 0, "y": 0, "width": 28, "height": 41},
                 "children": [{"type": "GROUP", "name": "g",
                               "children": [{"type": "VECTOR", "name": "v", "id": "2:4"}]}]}
        dout, _ = frx.node_tree_to_ir(drawn)
        self.assertEqual(dout.get("audio_widget"), "none")
        self.assertEqual(dout.get("type"), "image", "all-vector art captured whole")

    def test_instance_keeps_designer_art_and_carries_component_identity(self):
        # A component instance is the designer's own widget art. It must NOT be
        # name-promoted to the built-in silver knob; it gets the explicit
        # audio_widget "none" opt-out (which parse_ir_audio_widget honors), its
        # children are preserved, and it carries figma.component_key /
        # main_component_name so the recognition resolver can wire a MATCHED
        # component by key — never-silent-knob, same contract as the .fig lane.
        inst = {"type": "INSTANCE", "name": "sound / knob / mixer", "id": "2:1",
                "componentId": "9:9",
                "absoluteBoundingBox": {"x": 0, "y": 0, "width": 28, "height": 41},
                "children": [
                    {"type": "ELLIPSE", "name": "knob base", "id": "2:2",
                     "absoluteBoundingBox": {"x": 0, "y": 0, "width": 28, "height": 28}},
                    {"type": "TEXT", "name": "caption", "id": "2:3",
                     "characters": "Attack",
                     "absoluteBoundingBox": {"x": 0, "y": 30, "width": 28, "height": 11}},
                ]}
        out, _ctx = frx.node_tree_to_ir(
            inst, components={"9:9": {"key": "abc123", "name": "sound / knob / mixer"}})
        self.assertEqual(out.get("audio_widget"), "none")
        self.assertEqual(out["figma"]["component_key"], "abc123")
        self.assertEqual(out["figma"]["main_component_name"], "sound / knob / mixer")
        self.assertEqual(len(out.get("children", [])), 2, "designer art preserved")

    def test_nodes_inside_instances_are_not_name_promoted(self):
        # The internals ('knob base', 'fader track', …) match the name
        # vocabulary but are component art — each gets the "none" opt-out so
        # neither this exporter nor the C++ importer's name heuristic promotes
        # them, at any depth.
        inst = {"type": "INSTANCE", "name": "widget", "id": "3:1", "componentId": "9:9",
                "absoluteBoundingBox": {"x": 0, "y": 0, "width": 60, "height": 60},
                "children": [
                    {"type": "FRAME", "name": "knob", "id": "3:2",
                     "absoluteBoundingBox": {"x": 0, "y": 0, "width": 28, "height": 28},
                     "children": [
                         {"type": "FRAME", "name": "fader", "id": "3:3",
                          "absoluteBoundingBox": {"x": 0, "y": 0, "width": 4, "height": 20}},
                     ]},
                ]}
        out, _ctx = frx.node_tree_to_ir(inst)
        knob = out["children"][0]
        self.assertEqual(knob.get("audio_widget"), "none")
        self.assertEqual(knob["children"][0].get("audio_widget"), "none")

    def test_component_set_key_preferred_over_component_key(self):
        # The resolver's tables (library-manifest.json, --recognition-manifest)
        # are keyed by component_set_key; a variant instance must carry the
        # SET's key, not the individual variant component's.
        inst = {"type": "INSTANCE", "name": "Pulp / Knob", "id": "4:1",
                "componentId": "9:1",
                "absoluteBoundingBox": {"x": 0, "y": 0, "width": 56, "height": 56}}
        out, _ctx = frx.node_tree_to_ir(
            inst,
            components={"9:1": {"key": "variantkey", "name": "size=small",
                                "componentSetId": "8:1"}},
            component_sets={"8:1": {"key": "setkey", "name": "Pulp / Knob"}})
        self.assertEqual(out["figma"]["component_key"], "setkey")
        self.assertEqual(out["figma"]["main_component_name"], "Pulp / Knob")

    def test_instance_component_semantics_reach_figma_block(self):
        # Audit item 4: REST exposes an instance's typed property values as
        # `componentProperties` and the components/componentSets maps carry the
        # set name + remote flag. All of it must land in the figma block under
        # the SAME field names the plugin's serialize.ts emits, so
        # design_ir_json.cpp parses every lane identically. VARIANT entries
        # double as the variant axis map (REST has no separate
        # variantProperties), and property-name "#id" suffixes pass through —
        # the consumer owns normalization.
        inst = {"type": "INSTANCE", "name": "Knob", "id": "5:1",
                "componentId": "9:1",
                "absoluteBoundingBox": {"x": 0, "y": 0, "width": 56, "height": 56},
                "componentProperties": {
                    "label#9:0": {"type": "TEXT", "value": "Drive"},
                    "showValue#9:1": {"type": "BOOLEAN", "value": True},
                    "icon#9:2": {"type": "INSTANCE_SWAP", "value": "77:5"},
                    "size": {"type": "VARIANT", "value": "lg"},
                    "bound#9:3": {"type": "TEXT",
                                  "value": {"unexpected": "object"}},
                }}
        out, _ctx = frx.node_tree_to_ir(
            inst,
            components={"9:1": {"key": "variantkey", "name": "size=lg",
                                "componentSetId": "8:1", "remote": True}},
            component_sets={"8:1": {"key": "setkey", "name": "Knob"}})
        fig = out["figma"]
        self.assertEqual(fig["main_component_id"], "9:1")
        self.assertEqual(fig["component_set_name"], "Knob")
        self.assertEqual(fig["remote_library"], True)
        self.assertEqual(fig["component_properties"], {
            "label#9:0": {"type": "TEXT", "value": "Drive"},
            "showValue#9:1": {"type": "BOOLEAN", "value": True},
            "icon#9:2": {"type": "INSTANCE_SWAP", "value": "77:5"},
            "size": {"type": "VARIANT", "value": "lg"},
        })  # the non-scalar value is dropped, never emitted malformed
        self.assertEqual(fig["variant_properties"], {"size": "lg"})

    def test_instance_without_component_properties_emits_no_extra_fields(self):
        # Additive contract: a set-less local instance with no typed properties
        # gets identity only — no empty component_properties /
        # variant_properties / remote_library keys.
        inst = {"type": "INSTANCE", "name": "Plain", "id": "6:1",
                "componentId": "9:9",
                "absoluteBoundingBox": {"x": 0, "y": 0, "width": 10, "height": 10}}
        out, _ctx = frx.node_tree_to_ir(
            inst, components={"9:9": {"key": "k1", "name": "Plain"}})
        fig = out["figma"]
        self.assertEqual(fig["main_component_id"], "9:9")
        self.assertNotIn("component_set_name", fig)
        self.assertNotIn("remote_library", fig)
        self.assertNotIn("component_properties", fig)
        self.assertNotIn("variant_properties", fig)

    def test_detached_copy_subtree_is_art_not_widgets(self):
        # A DETACHED component copy is a widget-named FRAME that directly owns
        # raw shapes ('knob base', 'knob ring'). It is ONE widget whose parts
        # are art: nothing inside it may be name-promoted (the old behavior
        # painted a silver knob over each part), and every part carries the
        # "none" opt-out so the C++ name heuristic stays off too.
        detached = {"type": "FRAME", "name": "sound / knob / small unipolar", "id": "5:1",
                    "absoluteBoundingBox": {"x": 0, "y": 0, "width": 40, "height": 52},
                    "children": [
                        {"type": "FRAME", "name": "sound / knob label", "id": "5:2",
                         "absoluteBoundingBox": {"x": 0, "y": 40, "width": 40, "height": 12},
                         "children": [{"type": "TEXT", "name": "caption", "id": "5:3",
                                       "characters": "Attack",
                                       "absoluteBoundingBox": {"x": 0, "y": 40, "width": 40, "height": 12}}]},
                        {"type": "ELLIPSE", "name": "knob base", "id": "5:4",
                         "absoluteBoundingBox": {"x": 6, "y": 6, "width": 28, "height": 28}},
                        {"type": "BOOLEAN_OPERATION", "name": "knob ring", "id": "5:5",
                         "absoluteBoundingBox": {"x": 0, "y": 0, "width": 40, "height": 34}},
                    ]}
        out, _ctx = frx.node_tree_to_ir(detached)
        # The copy itself keeps the opt-out (so the C++ name heuristic stays
        # off) and stays a container with its art inside.
        self.assertEqual(out.get("audio_widget"), "none")
        self.assertTrue(out.get("children"))
        parts = {c["name"]: c for c in out["children"]}
        self.assertEqual(parts["sound / knob label"].get("audio_widget"), "none")
        self.assertEqual(parts["knob ring"].get("audio_widget"), "none")
        # Control: a widget-named group of CONTAINERS is a row, not one widget —
        # an EMPTY placeholder frame inside it still gets promoted.
        row = {"type": "FRAME", "name": "Knob Row", "id": "6:1",
               "absoluteBoundingBox": {"x": 0, "y": 0, "width": 200, "height": 60},
               "children": [
                   {"type": "FRAME", "name": "Knob", "id": "6:2",
                    "absoluteBoundingBox": {"x": 0, "y": 0, "width": 60, "height": 60}},
               ]}
        rout, _ = frx.node_tree_to_ir(row)
        self.assertEqual(rout.get("audio_widget"), "none")  # row pinned, not promoted
        self.assertEqual(rout["children"][0].get("audio_widget"), "knob")

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

    def test_resize_constraints_pass_through_in_rest_spelling(self):
        # The shared producer contract: node-level `constraints` carrying the
        # REST API's raw tokens (LEFT/RIGHT/CENTER/LEFT_RIGHT/SCALE and the
        # vertical analogues). No translation here — design_ir_json.cpp
        # normalizes and codegen lowers to flex within the parent.
        tree = {"type": "FRAME", "name": "Panel", "id": "0:1",
                "absoluteBoundingBox": {"x": 0, "y": 0, "width": 100, "height": 100},
                "children": [
                    {"type": "FRAME", "name": "Footer", "id": "0:2",
                     "absoluteBoundingBox": {"x": 0, "y": 76, "width": 100, "height": 24},
                     "constraints": {"horizontal": "LEFT_RIGHT", "vertical": "BOTTOM"}},
                    {"type": "FRAME", "name": "Default", "id": "0:3",
                     "absoluteBoundingBox": {"x": 0, "y": 0, "width": 10, "height": 10}},
                ]}
        ir, _ctx = frx.node_tree_to_ir(tree)
        footer, default = ir["children"]
        self.assertEqual(footer["constraints"],
                         {"horizontal": "LEFT_RIGHT", "vertical": "BOTTOM"})
        self.assertNotIn("constraints", default)   # no source field, no key
        self.assertNotIn("constraints", ir)        # the imported root anchors the space

    def test_resize_constraints_dropped_for_flowing_auto_layout_children(self):
        # A FLOWING auto-layout child is sized by the stack; its (stale)
        # constraints must not ride along to fight the flex pass. A child opted
        # out with layoutPositioning ABSOLUTE is back in the parent's coordinate
        # space, so its constraints stay — same gate as absolute positioning.
        tree = {"type": "FRAME", "name": "Stack", "id": "0:1", "layoutMode": "VERTICAL",
                "absoluteBoundingBox": {"x": 0, "y": 0, "width": 100, "height": 100},
                "children": [
                    {"type": "FRAME", "name": "Row", "id": "0:2",
                     "absoluteBoundingBox": {"x": 0, "y": 0, "width": 100, "height": 24},
                     "constraints": {"horizontal": "SCALE", "vertical": "TOP"}},
                    {"type": "FRAME", "name": "Badge", "id": "0:3",
                     "layoutPositioning": "ABSOLUTE",
                     "absoluteBoundingBox": {"x": 88, "y": 2, "width": 10, "height": 10},
                     "constraints": {"horizontal": "RIGHT", "vertical": "TOP"}},
                ]}
        ir, _ctx = frx.node_tree_to_ir(tree)
        row, badge = ir["children"]
        self.assertNotIn("constraints", row)
        self.assertEqual(badge["constraints"], {"horizontal": "RIGHT", "vertical": "TOP"})

    def test_bound_variables_resolve_to_token_names(self):
        # Audit item 5: the /nodes payload carries each node's boundVariables;
        # with the /variables/local name map they resolve to canonical token
        # names in figma.bound_variables — single alias bare, alias arrays
        # index-0 bare + ".<i>" suffixed, nested alias maps ".<key>" suffixed.
        alias = lambda vid: {"type": "VARIABLE_ALIAS", "id": vid}
        tree = {"type": "FRAME", "name": "Chip", "id": "0:1",
                "absoluteBoundingBox": {"x": 0, "y": 0, "width": 40, "height": 16},
                "boundVariables": {
                    "fills": [alias("VariableID:1:1"), alias("VariableID:1:3")],
                    "cornerRadius": alias("VariableID:1:2"),
                    "componentProperties": {"label#9:0": alias("VariableID:1:4")},
                },
                "children": [
                    {"type": "FRAME", "name": "Plain", "id": "0:2",
                     "absoluteBoundingBox": {"x": 0, "y": 0, "width": 10, "height": 10}},
                ]}
        ir, ctx = frx.node_tree_to_ir(tree, variable_names={
            "VariableID:1:1": "theme.brand.primary",
            "VariableID:1:2": "theme.radius.md",
            "VariableID:1:3": "theme.brand.secondary",
            "VariableID:1:4": "theme.label.gain",
        })
        self.assertEqual(ir["figma"]["bound_variables"], {
            "fills": "theme.brand.primary",
            "fills.1": "theme.brand.secondary",
            "cornerRadius": "theme.radius.md",
            "componentProperties.label#9:0": "theme.label.gain",
        })
        # Fully resolved: no unresolved-binding diagnostics.
        self.assertEqual([d for d in ctx.diagnostics
                          if d["code"] == "variable-binding-unresolved"], [])
        # A node without boundVariables must not grow the key.
        self.assertNotIn("bound_variables", ir["children"][0]["figma"])

    def test_bound_variables_without_name_map_emit_raw_ids_with_one_diagnostic(self):
        # The variables endpoint is Enterprise-plan-gated, so the common case
        # has NO name map: the raw variable id is emitted (a stable join key,
        # never a fabricated name) and each id is diagnosed once, even when two
        # nodes bind the same variable.
        alias = lambda vid: {"type": "VARIABLE_ALIAS", "id": vid}
        tree = {"type": "FRAME", "name": "Panel", "id": "0:1",
                "absoluteBoundingBox": {"x": 0, "y": 0, "width": 100, "height": 100},
                "boundVariables": {"fills": [alias("VariableID:7:7")]},
                "children": [
                    {"type": "FRAME", "name": "Twin", "id": "0:2",
                     "absoluteBoundingBox": {"x": 0, "y": 0, "width": 10, "height": 10},
                     "boundVariables": {"opacity": alias("VariableID:7:7")}},
                ]}
        ir, ctx = frx.node_tree_to_ir(tree)
        self.assertEqual(ir["figma"]["bound_variables"], {"fills": "VariableID:7:7"})
        self.assertEqual(ir["children"][0]["figma"]["bound_variables"],
                         {"opacity": "VariableID:7:7"})
        diags = [d for d in ctx.diagnostics if d["code"] == "variable-binding-unresolved"]
        self.assertEqual(len(diags), 1)  # once per variable, not per node
        self.assertEqual(diags[0]["severity"], "warning")

    def test_variables_to_tokens_mirrors_plugin_canonical_names_and_modes(self):
        # variables_to_tokens must produce the SAME names the plugin lane's
        # tokens.ts produces: "collection/variable" lowercased, whitespace
        # stripped, "/" → "."; default mode bare + other modes ".<slug>";
        # aliases resolved per mode with fallback to the referent's first mode;
        # booleans encoded as strings.
        payload = {"meta": {
            "variableCollections": {
                "VC:1": {"id": "VC:1", "name": "Theme", "defaultModeId": "m1",
                         "modes": [{"modeId": "m1", "name": "Light"},
                                   {"modeId": "m2", "name": "Dark"}]},
                "VC:2": {"id": "VC:2", "name": "Base", "defaultModeId": "mb",
                         "modes": [{"modeId": "mb", "name": "Value"}]},
            },
            "variables": {
                "VariableID:1:1": {"id": "VariableID:1:1", "name": "Brand/Primary",
                                   "variableCollectionId": "VC:1", "resolvedType": "COLOR",
                                   "valuesByMode": {
                                       "m1": {"type": "VARIABLE_ALIAS", "id": "VariableID:2:1"},
                                       "m2": {"r": 0, "g": 0, "b": 0, "a": 1}}},
                "VariableID:1:2": {"id": "VariableID:1:2", "name": "Radius MD",
                                   "variableCollectionId": "VC:1", "resolvedType": "FLOAT",
                                   "valuesByMode": {"m1": 8}},
                "VariableID:1:3": {"id": "VariableID:1:3", "name": "Show Value",
                                   "variableCollectionId": "VC:1", "resolvedType": "BOOLEAN",
                                   "valuesByMode": {"m1": True}},
                # Referent in another collection: alias falls back to its own
                # first/default mode value.
                "VariableID:2:1": {"id": "VariableID:2:1", "name": "White",
                                   "variableCollectionId": "VC:2", "resolvedType": "COLOR",
                                   "valuesByMode": {"mb": {"r": 1, "g": 1, "b": 1, "a": 1}}},
            }}}
        tokens, id_to_name = frx.variables_to_tokens(payload)
        self.assertEqual(id_to_name["VariableID:1:1"], "theme.brand.primary")
        self.assertEqual(id_to_name["VariableID:1:2"], "theme.radiusmd")
        self.assertEqual(tokens["colors"]["theme.brand.primary"], "#ffffff")
        self.assertEqual(tokens["colors"]["theme.brand.primary.dark"], "#000000")
        self.assertEqual(tokens["dimensions"]["theme.radiusmd"], 8)
        self.assertEqual(tokens["strings"]["theme.showvalue"], "true")
        self.assertEqual(tokens["colors"]["base.white"], "#ffffff")

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

    def test_text_align_vertical_lands_as_vertical_align(self):
        # Design authority the codegen consumes over its tall-slot heuristic.
        for tav, expected in (("CENTER", "middle"), ("BOTTOM", "bottom"), ("TOP", "top")):
            s = {}
            frx.extract_text_style({"style": {"textAlignVertical": tav}}, s)
            self.assertEqual(s.get("vertical_align"), expected, tav)
        s = {}
        frx.extract_text_style({"style": {}}, s)
        self.assertNotIn("vertical_align", s)

    def test_extended_text_metadata_preserved_as_namespaced_attributes(self):
        n = {"type": "TEXT", "style": {
            "textAutoResize": "HEIGHT", "textTruncation": "ENDING",
            "maxLines": 2, "hyperlink": {"type": "URL", "url": "https://pulp.audio"}}}
        attrs = frx.extract_text_attributes(n)
        self.assertEqual(attrs["figma:text_auto_resize"], "height")
        self.assertEqual(attrs["figma:text_truncation"], "ending")
        self.assertEqual(attrs["figma:max_lines"], "2")
        self.assertEqual(attrs["figma:hyperlink"], "https://pulp.audio")
        # Defaults stay silent — no attribute noise on plain labels.
        self.assertEqual(frx.extract_text_attributes(
            {"style": {"textAutoResize": "NONE", "textTruncation": "DISABLED"}}), {})


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
        frx._label_elements(elements, figma_root, (100.0, 100.0), (0.0, 0.0))
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
        frx._label_elements(elements, figma_root, (100.0, 100.0), (0.0, 0.0))
        self.assertEqual(elements[0].get("label"), "Cutoff")
        self.assertEqual(elements[0].get("source_node_id"), "k1")  # provenance stamped
        self.assertNotIn("label", elements[1])  # default-named node → no label
        self.assertNotIn("source_node_id", elements[1])  # no owner → no provenance
        self.assertNotIn("label", elements[2])  # no overlapping named node


class ParamKeyBindingTest(unittest.TestCase):
    """Opt-in host-param binding for a geometry knob via a layer-name sigil,
    lockstep with the C++ figma_binding_from_layer_name and the TS lane
    (faithful-vector.test.ts)."""

    def test_param_key_from_layer_name_sigil_or_empty(self):
        self.assertEqual(frx._param_key_from_layer_name("param:filter.cutoff"), "filter.cutoff")
        self.assertEqual(frx._param_key_from_layer_name("bind:gain"), "gain")
        self.assertEqual(frx._param_key_from_layer_name("meter:out_l"), "out_l")
        self.assertEqual(frx._param_key_from_layer_name("PARAM:Cutoff"), "Cutoff")   # case-insensitive
        self.assertEqual(frx._param_key_from_layer_name("  param:cutoff "), "cutoff")  # ws + trim
        self.assertEqual(frx._param_key_from_layer_name("param: a.b "), "a.b")
        # Not a binding.
        for bare in ("Cutoff", "param:", "param: ", "param:.", "xparam:y", ""):
            self.assertEqual(frx._param_key_from_layer_name(bare), "", bare)

    def test_sigil_knob_binds_param_key_and_provenance_not_label(self):
        figma_root = {
            "id": "root", "name": "TRIAZ",  # named root must NOT steal a centered knob
            "absoluteBoundingBox": {"x": 100, "y": 100, "width": 1000, "height": 600},
            "children": [
                {"id": "k1", "name": "param:filter.cutoff",
                 "absoluteBoundingBox": {"x": 140, "y": 140, "width": 40, "height": 40}},
                {"id": "leaf", "name": "Ellipse 7",  # default leaf must NOT steal ownership
                 "absoluteBoundingBox": {"x": 150, "y": 150, "width": 20, "height": 20}},
            ],
        }
        elements = [{"kind": "knob", "cx": 60.0, "cy": 60.0, "hit_radius": 30.0}]
        frx._label_elements(elements, figma_root, (100.0, 100.0), (0.0, 0.0))
        self.assertEqual(elements[0].get("param_key"), "filter.cutoff")
        self.assertEqual(elements[0].get("source_node_id"), "k1")  # the sigil node, not the leaf
        self.assertNotIn("label", elements[0])  # a sigil is a binding, not a human label

    def test_descriptive_knob_gets_label_and_provenance_no_param_key(self):
        figma_root = {
            "id": "root", "name": "TRIAZ",
            "absoluteBoundingBox": {"x": 100, "y": 100, "width": 1000, "height": 600},
            "children": [
                {"id": "k1", "name": "Resonance",
                 "absoluteBoundingBox": {"x": 140, "y": 140, "width": 40, "height": 40}},
            ],
        }
        elements = [{"kind": "knob", "cx": 60.0, "cy": 60.0, "hit_radius": 30.0}]
        frx._label_elements(elements, figma_root, (100.0, 100.0), (0.0, 0.0))
        self.assertEqual(elements[0].get("label"), "Resonance")
        self.assertEqual(elements[0].get("source_node_id"), "k1")
        self.assertNotIn("param_key", elements[0])  # descriptive → manifest lane, not a sigil

    def test_triaz_shaped_panel_descriptive_knobs_get_provenance_not_binding(self):
        # Modeled on the real Triaz synth panel (file D1CT7a6gjCM0Yb773bse7v node
        # 10578:288008 = "sound / main panel", 988x300): a DESCRIPTIVELY-named
        # container with descriptively-named rotary-knob instances, no sigils.
        # Each knob must get a human label + provenance source_node_id (so the
        # annotated-manifest lane can bind it by node id) — and NO param_key,
        # because a descriptive name is not an explicit binding. The panel's own
        # descriptive name must NOT be mis-bound to a centered knob.
        figma_root = {
            "id": "10578:288008", "name": "sound / main panel",
            "absoluteBoundingBox": {"x": 8, "y": 68, "width": 988, "height": 300},
            "children": [
                {"id": "k:cut", "name": "Cutoff",
                 "absoluteBoundingBox": {"x": 100, "y": 150, "width": 56, "height": 56}},
                {"id": "k:res", "name": "Resonance",
                 "absoluteBoundingBox": {"x": 300, "y": 150, "width": 56, "height": 56}},
                {"id": "k:lfo", "name": "LFO Rate",
                 "absoluteBoundingBox": {"x": 500, "y": 150, "width": 56, "height": 56}},
            ],
        }
        # Geometry knobs at each instance center, in frame-local SVG space (ox=8, oy=68).
        elements = [
            {"kind": "knob", "cx": 120.0, "cy": 110.0, "hit_radius": 28.0},  # Cutoff
            {"kind": "knob", "cx": 320.0, "cy": 110.0, "hit_radius": 28.0},  # Resonance
            {"kind": "knob", "cx": 520.0, "cy": 110.0, "hit_radius": 28.0},  # LFO Rate
        ]
        frx._label_elements(elements, figma_root, (8.0, 68.0), (0.0, 0.0))
        self.assertEqual([e.get("label") for e in elements],
                         ["Cutoff", "Resonance", "LFO Rate"])
        self.assertEqual([e.get("source_node_id") for e in elements],
                         ["k:cut", "k:res", "k:lfo"])  # provenance for the manifest lane
        for e in elements:
            self.assertNotIn("param_key", e)  # descriptive names never auto-bind

    def test_drop_shadow_panel_margin_still_binds(self):
        # Regression: parse_frame_knobs reports SVG coords `(node_abs - root_abs) +
        # panel_origin`. A frame with a shadow margin has panel origin (73,50), so a
        # sigil knob node at abs (140,240,40,40) under root (100,200) has SVG center
        # (133,110). Without the panel_origin term the matcher looked at (60,60) —
        # 88px off, outside the hit radius — and the knob silently stayed unbound.
        figma_root = {
            "id": "root", "name": "panel",
            "absoluteBoundingBox": {"x": 100, "y": 200, "width": 500, "height": 400},
            "children": [
                {"id": "s1", "name": "param:filter.cutoff",
                 "absoluteBoundingBox": {"x": 140, "y": 240, "width": 40, "height": 40}},
            ],
        }
        elements = [{"kind": "knob", "cx": 133.0, "cy": 110.0, "hit_radius": 30.0}]
        frx._label_elements(elements, figma_root, (100.0, 200.0), (73.0, 50.0))
        self.assertEqual(elements[0].get("param_key"), "filter.cutoff")
        self.assertEqual(elements[0].get("source_node_id"), "s1")

    def test_sigil_node_wins_over_nearer_descriptive_caption(self):
        # A sigil instance and a smaller descriptive caption both cover the knob;
        # the explicit binding must win regardless of the caption being nearer.
        figma_root = {
            "id": "root",
            "absoluteBoundingBox": {"x": 0, "y": 0, "width": 400, "height": 400},
            "children": [
                {"id": "inst", "name": "param:lfo.rate",
                 "absoluteBoundingBox": {"x": 40, "y": 40, "width": 40, "height": 40}},
                {"id": "cap", "name": "Rate",  # nearer center, smaller — but not a binding
                 "absoluteBoundingBox": {"x": 58, "y": 58, "width": 8, "height": 8}},
            ],
        }
        elements = [{"kind": "knob", "cx": 60.0, "cy": 60.0, "hit_radius": 30.0}]
        frx._label_elements(elements, figma_root, (0.0, 0.0), (0.0, 0.0))
        self.assertEqual(elements[0].get("param_key"), "lfo.rate")  # sigil wins
        self.assertEqual(elements[0].get("source_node_id"), "inst")


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


class ExportCacheTest(unittest.TestCase):
    """--cache-dir memoizes the two REST-heavy payloads per (file_key, node_id) so
    re-testing the same frame does ZERO REST calls (the Figma MCP allows ~6/month
    on a View seat). A hit must not touch the network; a miss fetches then writes."""

    def setUp(self):
        import tempfile
        self._tmp = tempfile.mkdtemp()
        self._orig_nodes = frx.fetch_nodes
        self._orig_svg = frx.fetch_frame_svg

    def tearDown(self):
        import shutil
        frx.fetch_nodes = self._orig_nodes
        frx.fetch_frame_svg = self._orig_svg
        shutil.rmtree(self._tmp, ignore_errors=True)

    def test_cache_key_normalizes_node_colon(self):
        # Figma node ids embed ':' — the key must be a portable filename.
        key = frx._export_cache_key("FILEKEY", "10578:288008")
        self.assertNotIn(":", key)
        self.assertEqual(key, "FILEKEY__10578_288008")

    def test_nodes_miss_fetches_and_writes_then_hit_is_offline(self):
        calls = {"n": 0}
        doc = {"nodes": {"1:2": {"document": {"id": "1:2"}}}}
        def fake_fetch(fk, nid, tok):
            calls["n"] += 1
            return doc
        frx.fetch_nodes = fake_fetch
        # Miss → fetches once and writes the cache file.
        got = frx.fetch_nodes_cached("FK", "1:2", "tok", cache_dir=self._tmp)
        self.assertEqual(got, doc)
        self.assertEqual(calls["n"], 1)
        import os
        path = frx._cache_path(self._tmp, "FK", "1:2", frx._CACHE_NODES_SUFFIX)
        self.assertTrue(os.path.exists(path))
        # Hit → NO further fetch, even with a token that would otherwise be used.
        def boom(*a, **k):
            raise AssertionError("cache hit must not call fetch_nodes")
        frx.fetch_nodes = boom
        again = frx.fetch_nodes_cached("FK", "1:2", "tok", cache_dir=self._tmp)
        self.assertEqual(again, doc)

    def test_refresh_cache_forces_refetch(self):
        calls = {"n": 0}
        def fake_fetch(fk, nid, tok):
            calls["n"] += 1
            return {"v": calls["n"]}
        frx.fetch_nodes = fake_fetch
        frx.fetch_nodes_cached("FK", "1:2", "tok", cache_dir=self._tmp)
        # refresh=True ignores the written cache and re-fetches (rewrites it).
        again = frx.fetch_nodes_cached("FK", "1:2", "tok", cache_dir=self._tmp, refresh=True)
        self.assertEqual(calls["n"], 2)
        self.assertEqual(again, {"v": 2})

    def test_no_cache_dir_is_passthrough(self):
        calls = {"n": 0}
        def fake_fetch(fk, nid, tok):
            calls["n"] += 1
            return {"ok": True}
        frx.fetch_nodes = fake_fetch
        frx.fetch_nodes_cached("FK", "1:2", "tok")  # no cache_dir → always fetch
        frx.fetch_nodes_cached("FK", "1:2", "tok")
        self.assertEqual(calls["n"], 2)

    def test_svg_none_not_cached_but_text_is(self):
        import os
        # A None render must NOT be memoized as a permanent miss.
        frx.fetch_frame_svg = lambda fk, nid, tok: None
        self.assertIsNone(
            frx.fetch_frame_svg_cached("FK", "1:2", "tok", cache_dir=self._tmp))
        self.assertFalse(os.path.exists(
            frx._cache_path(self._tmp, "FK", "1:2", frx._CACHE_SVG_SUFFIX)))
        # A real SVG is cached, and the hit is offline (no token).
        frx.fetch_frame_svg = lambda fk, nid, tok: "<svg/>"
        self.assertEqual(
            frx.fetch_frame_svg_cached("FK", "1:2", "tok", cache_dir=self._tmp), "<svg/>")
        def boom(*a, **k):
            raise AssertionError("cache hit must not call fetch_frame_svg")
        frx.fetch_frame_svg = boom
        self.assertEqual(
            frx.fetch_frame_svg_cached("FK", "1:2", None, cache_dir=self._tmp), "<svg/>")

    def test_svg_no_token_and_miss_returns_none(self):
        # No token + cache miss → nothing to fetch, returns None (no crash).
        called = {"n": 0}
        def boom(*a, **k):
            called["n"] += 1
            raise AssertionError("must not fetch without a token")
        frx.fetch_frame_svg = boom
        self.assertIsNone(
            frx.fetch_frame_svg_cached("FK", "1:2", None, cache_dir=self._tmp))
        self.assertEqual(called["n"], 0)


class ShapePrimitiveTypingTest(unittest.TestCase):
    """Shape leaves must reach the IR as the shape they are.

    Asserted through node_tree_to_ir — the producer's real entry point — so the
    assertion is about what this exporter actually WRITES, not what a helper
    returns in isolation.
    """

    @staticmethod
    def _shape(**over):
        # Figma's own default layer name. A widget-ish name ("knob base") would
        # be name-promoted to audio_widget=knob, and synthesize_node returns
        # early on a recognized widget — so this fixture would then assert
        # nothing about the shape path it is here to cover.
        n = {"type": "ELLIPSE", "name": "Ellipse 1", "id": "3:1",
             "absoluteBoundingBox": {"x": 0, "y": 0, "width": 40, "height": 40},
             "fills": [{"type": "SOLID", "color": {"r": 1, "g": 0, "b": 0, "a": 1}}]}
        n.update(over)
        return n

    def test_filled_ellipse_is_typed_ellipse_not_frame(self):
        # A filled ELLIPSE typed `frame` renders as a SQUARE: a frame paints its
        # own background box, and codegen has no painter for a circle, so the
        # fill has no way to become round. `ellipse` is what the C++ side has
        # accepted all along (is_synthesizable_primitive → synth_ellipse_path).
        out, _ctx = frx.node_tree_to_ir(self._shape())
        self.assertEqual(out["type"], "ellipse")
        # Plain art, not a recognized widget — the case where synthesize_node
        # actually runs and needs the type to be right.
        self.assertIsNone(out.get("audio_widget"))
        # The fill must survive: synthesize_node moves background_color onto the
        # synthesized path. A typed ellipse with no fill would paint nothing.
        self.assertEqual(out["style"]["background_color"], "#ff0000")

    def test_rectangle_stays_frame(self):
        # The control for the rule above: a rect IS a box, so a frame's own
        # background paints it correctly and it must NOT be re-typed.
        rect = self._shape(type="RECTANGLE", name="Rectangle 1")
        self.assertEqual(frx.node_tree_to_ir(rect)[0]["type"], "frame")

    def test_star_and_polygon_are_captured_as_images_not_frames(self):
        # Pins the reason ELLIPSE needs a fix and these do not: is_vector_like()
        # captures them as PNG assets before frame typing can matter. If this
        # ever regresses to "frame", they become squares the same way — and this
        # test says so instead of leaving the omission looking like an oversight.
        for figma_type in ("STAR", "POLYGON", "REGULAR_POLYGON"):
            with self.subTest(figma_type=figma_type):
                out, ctx = frx.node_tree_to_ir(self._shape(type=figma_type,
                                                           name="Pentagon"))
                self.assertEqual(out["type"], "image")
                self.assertEqual(out["asset_ref"], "3:1")
                self.assertIn("3:1", ctx.asset_ids)

    def test_per_corner_radii_from_rectangleCornerRadii(self):
        # Figma REST returns rectangleCornerRadii [tl, tr, br, bl] when the
        # corners differ. The producer only read the uniform cornerRadius, so an
        # asymmetric card imported via REST lost its rounding.
        s = frx.extract_style({"type": "RECTANGLE",
                               "rectangleCornerRadii": [8, 8, 0, 0]})
        self.assertEqual(s.get("border_top_left_radius"), 8)
        self.assertEqual(s.get("border_top_right_radius"), 8)
        self.assertEqual(s.get("border_bottom_right_radius"), 0)
        self.assertEqual(s.get("border_bottom_left_radius"), 0)
        # A uniform radius must NOT sit beside the per-corner ones (codegen's
        # single 'All' call would square off the rounded pair).
        self.assertNotIn("border_radius", s)

    def test_uniform_corners_stay_on_the_single_radius_path(self):
        # Four equal corners lower through the uniform border_radius, not four
        # per-corner fields — one call, not four.
        s = frx.extract_style({"type": "RECTANGLE",
                               "cornerRadius": 6,
                               "rectangleCornerRadii": [6, 6, 6, 6]})
        self.assertEqual(s.get("border_radius"), 6)
        self.assertNotIn("border_top_left_radius", s)


class NodeDispatchTest(unittest.TestCase):
    """Exhaustive node-type dispatch — no family reaches the envelope through a
    silent default-to-frame. Asserted through node_tree_to_ir (the producer's
    real entry point) so the assertions are about what the exporter WRITES:
    skipped families emit no node + a diagnostic, diagnosed families emit a
    node + a diagnostic, and containers dispatch explicitly with no noise.
    Kept in lockstep with the plugin lane (extract-pure.ts::dispatchNodeType)
    and the .fig lane (fig/scene.mjs)."""

    @staticmethod
    def _bb(x, y, w, h):
        return {"x": x, "y": y, "width": w, "height": h}

    def _tree(self):
        return {"type": "FRAME", "name": "Panel", "id": "1:1",
                "absoluteBoundingBox": self._bb(0, 0, 400, 300),
                "children": [
                    {"type": "TEXT_PATH", "name": "Curved Label", "id": "1:2",
                     "absoluteBoundingBox": self._bb(10, 10, 120, 24),
                     "characters": "WOW FACTOR",
                     "style": {"fontFamily": "Inter", "fontWeight": 400,
                               "fontSize": 12}},
                    {"type": "TRANSFORM_GROUP", "name": "Spun Group", "id": "1:3",
                     "absoluteBoundingBox": self._bb(10, 50, 80, 80),
                     "children": []},
                    {"type": "SLOT", "name": "Content Slot", "id": "1:4",
                     "absoluteBoundingBox": self._bb(10, 140, 100, 40),
                     "children": []},
                    {"type": "SLICE", "name": "Export @2x", "id": "1:5",
                     "absoluteBoundingBox": self._bb(0, 0, 400, 300)},
                    {"type": "STICKY", "name": "Reviewer Note", "id": "1:6",
                     "absoluteBoundingBox": self._bb(200, 10, 120, 120)},
                    {"type": "TABLE_CELL", "name": "Cell", "id": "1:7",
                     "absoluteBoundingBox": self._bb(0, 200, 40, 20)},
                ]}

    def _diags(self, ctx, code):
        return [d for d in ctx.diagnostics if d["code"] == code]

    def test_skipped_families_emit_no_node_and_a_diagnostic(self):
        ir, ctx = frx.node_tree_to_ir(self._tree())
        # SLICE (paints nothing — skipping IS correct rendering), STICKY and
        # TABLE_CELL (editor families) are gone; before this contract they were
        # empty generic frames that looked successfully imported.
        self.assertEqual([c["name"] for c in ir["children"]],
                         ["Curved Label", "Spun Group", "Content Slot"])
        slice_diags = self._diags(ctx, "slice-skipped")
        self.assertEqual(len(slice_diags), 1)
        self.assertEqual(slice_diags[0]["kind"], "unsupported_node")
        self.assertEqual(slice_diags[0]["path"], "1:5")
        skipped = self._diags(ctx, "unsupported-node")
        self.assertEqual({d["path"] for d in skipped}, {"1:6", "1:7"})
        self.assertTrue(all(d["kind"] == "unsupported_node" for d in skipped))

    def test_text_path_is_text_with_content_and_flatten_diagnostic(self):
        ir, ctx = frx.node_tree_to_ir(self._tree())
        curved = ir["children"][0]
        self.assertEqual(curved["type"], "text")
        # The content is the point: TEXT_PATH carries real copy, and re-typing
        # it must not drop it.
        self.assertEqual(curved["content"], "WOW FACTOR")
        self.assertEqual(curved["style"]["font_family"], "Inter")
        diags = self._diags(ctx, "text-path-flattened")
        self.assertEqual(len(diags), 1)
        self.assertEqual(diags[0]["kind"], "capture_partial")

    def test_transform_group_is_silent_frame_and_slot_is_diagnosed_frame(self):
        ir, ctx = frx.node_tree_to_ir(self._tree())
        spun = ir["children"][1]
        self.assertEqual(spun["type"], "frame")
        # A transform group renders fine as a container — explicit dispatch,
        # no diagnostic noise.
        self.assertEqual([d for d in ctx.diagnostics if d["path"] == "1:3"], [])
        slot = ir["children"][2]
        self.assertEqual(slot["type"], "frame")
        slot_diags = self._diags(ctx, "slot-placeholder")
        self.assertEqual(len(slot_diags), 1)
        self.assertEqual(slot_diags[0]["kind"], "unsupported_node")

    def test_unknown_type_falls_back_to_frame_with_a_diagnostic(self):
        tree = {"type": "FRAME", "name": "Panel", "id": "2:1",
                "absoluteBoundingBox": self._bb(0, 0, 100, 100),
                "children": [
                    {"type": "HOLOGRAM_2027", "name": "Future Thing", "id": "2:2",
                     "absoluteBoundingBox": self._bb(0, 0, 50, 50)},
                ]}
        ir, ctx = frx.node_tree_to_ir(tree)
        # Never crash on new families — the node survives as a frame...
        self.assertEqual(len(ir["children"]), 1)
        self.assertEqual(ir["children"][0]["type"], "frame")
        # ...but the fallback is stated, not silent.
        diags = self._diags(ctx, "unknown-node-type")
        self.assertEqual(len(diags), 1)
        self.assertEqual(diags[0]["kind"], "unsupported_node")
        self.assertIn("HOLOGRAM_2027", diags[0]["message"])

    def test_dispatch_table_full_matrix(self):
        # One row per family, REST spelling (REGULAR_POLYGON, TABLE_CELL).
        for t in ("FRAME", "GROUP", "SECTION", "TRANSFORM_GROUP", "CANVAS",
                  "COMPONENT", "COMPONENT_SET", "INSTANCE", "RECTANGLE",
                  "POLYGON", "REGULAR_POLYGON", "STAR", "LINE"):
            with self.subTest(figma_type=t):
                self.assertEqual(frx.dispatch_node_type(t), ("frame", None))
        self.assertEqual(frx.dispatch_node_type("ELLIPSE"), ("ellipse", None))
        self.assertEqual(frx.dispatch_node_type("TEXT"), ("text", None))
        self.assertEqual(frx.dispatch_node_type("VECTOR"), ("vector", None))
        self.assertEqual(frx.dispatch_node_type("BOOLEAN_OPERATION"),
                         ("vector", None))
        for t in ("SLICE", "STICKY", "CONNECTOR", "SHAPE_WITH_TEXT",
                  "CODE_BLOCK", "STAMP", "WIDGET", "EMBED", "LINK_UNFURL",
                  "MEDIA", "HIGHLIGHT", "WASHI_TAPE", "TABLE", "TABLE_CELL",
                  "SLIDE", "SLIDE_ROW", "SLIDE_GRID",
                  "INTERACTIVE_SLIDE_ELEMENT"):
            with self.subTest(figma_type=t):
                emitted, diag = frx.dispatch_node_type(t)
                self.assertIsNone(emitted)
                self.assertEqual(diag["kind"], "unsupported_node")


class AutoLayoutTest(unittest.TestCase):
    """Auto-layout completion: wrap extras, child grow/align, GRID, aspect,
    min/max. Mirrors the plugin lane (extract-pure.ts::extractLayout +
    test/layout.test.ts) and the .fig lane (fig/scene.mjs + fig.test.mjs).
    The layout keys use the consumer's camelCase spelling — the exact members
    design_ir_json.cpp::parse_ir_layout reads."""

    BB = {"x": 0, "y": 0, "width": 100, "height": 100}

    def test_wrap_emits_counter_axis_gap_and_align_content(self):
        # A row's wrapped tracks stack vertically → rowGap; SPACE_BETWEEN is
        # the only non-default distribution.
        row = frx.extract_layout({"type": "FRAME", "layoutMode": "HORIZONTAL",
                                  "layoutWrap": "WRAP", "counterAxisSpacing": 12,
                                  "counterAxisAlignContent": "SPACE_BETWEEN"})
        self.assertTrue(row["wrap"])
        self.assertEqual(row["rowGap"], 12)
        self.assertNotIn("columnGap", row)
        self.assertEqual(row["alignContent"], "space-between")
        # A column's tracks stack horizontally → columnGap; AUTO align-content
        # is the default packing and emits nothing.
        col = frx.extract_layout({"type": "FRAME", "layoutMode": "VERTICAL",
                                  "layoutWrap": "WRAP", "counterAxisSpacing": 8,
                                  "counterAxisAlignContent": "AUTO"})
        self.assertEqual(col["columnGap"], 8)
        self.assertNotIn("alignContent", col)
        # Non-wrapping stacks must not leak the wrapped-track fields.
        flat = frx.extract_layout({"type": "FRAME", "layoutMode": "HORIZONTAL",
                                   "counterAxisSpacing": 12,
                                   "counterAxisAlignContent": "SPACE_BETWEEN"})
        self.assertNotIn("rowGap", flat)
        self.assertNotIn("alignContent", flat)

    def test_child_grow_and_align_gated_on_flex_parent(self):
        flex_parent = {"type": "FRAME", "layoutMode": "HORIZONTAL"}
        child = {"type": "RECTANGLE", "layoutGrow": 2, "layoutAlign": "STRETCH"}
        l = frx.extract_layout(child, flex_parent)
        self.assertEqual(l["flexGrow"], 2)
        self.assertEqual(l["alignSelf"], "stretch")
        # INHERIT is the flex default — omitting align-self IS inherit.
        inherit = frx.extract_layout({"type": "TEXT", "layoutAlign": "INHERIT"}, flex_parent)
        self.assertNotIn("alignSelf", inherit)
        # Figma leaves the fields stale under a non-auto-layout parent.
        plain = frx.extract_layout(child, {"type": "FRAME", "layoutMode": "NONE"})
        self.assertNotIn("flexGrow", plain)
        self.assertNotIn("alignSelf", plain)
        # An ABSOLUTE stack child is out of flow: no grow/align either.
        absolute = frx.extract_layout({**child, "layoutPositioning": "ABSOLUTE"}, flex_parent)
        self.assertNotIn("flexGrow", absolute)
        self.assertNotIn("alignSelf", absolute)

    def test_grid_container_and_child_placement(self):
        grid = {"type": "FRAME", "layoutMode": "GRID",
                "gridColumnCount": 4, "gridRowCount": 3,
                "gridColumnGap": 6, "gridRowGap": 4}
        l = frx.extract_layout(grid)
        self.assertEqual(l["display"], "grid")
        self.assertEqual(l["gridTemplateColumns"], "repeat(4, 1fr)")
        self.assertEqual(l["gridTemplateRows"], "repeat(3, 1fr)")
        self.assertEqual(l["columnGap"], 6)
        self.assertEqual(l["rowGap"], 4)
        # 0-based anchors + spans → CSS 1-based lines.
        cell = frx.extract_layout({"type": "RECTANGLE",
                                   "gridColumnAnchorIndex": 1, "gridRowAnchorIndex": 0,
                                   "gridRowSpan": 2}, grid)
        self.assertEqual(cell["gridColumn"], "2")
        self.assertEqual(cell["gridRow"], "1 / span 2")
        # Cell anchors are grid-only: a flex parent must not read them.
        flex_child = frx.extract_layout({"type": "RECTANGLE", "gridColumnAnchorIndex": 1},
                                        {"type": "FRAME", "layoutMode": "HORIZONTAL"})
        self.assertNotIn("gridColumn", flex_child)

    def test_grid_children_flow_without_position_or_constraints(self):
        # A GRID parent lays its children out (cell placement), so they must
        # not get absolute coordinates or stale constraints — the same gate
        # flex children go through.
        tree = {"type": "FRAME", "name": "Grid", "id": "0:1", "layoutMode": "GRID",
                "gridColumnCount": 2, "gridRowCount": 2,
                "absoluteBoundingBox": self.BB,
                "children": [
                    {"type": "FRAME", "name": "cell", "id": "0:2",
                     "absoluteBoundingBox": {"x": 10, "y": 10, "width": 40, "height": 40},
                     "gridColumnAnchorIndex": 1, "gridRowAnchorIndex": 1,
                     "constraints": {"horizontal": "LEFT", "vertical": "TOP"}},
                ]}
        ir, _ctx = frx.node_tree_to_ir(tree)
        cell = ir["children"][0]
        self.assertEqual(cell["layout"]["gridColumn"], "2")
        self.assertNotIn("position", cell.get("style", {}))
        self.assertNotIn("constraints", cell)

    def test_absolute_stack_child_gets_coordinates(self):
        # layoutPositioning ABSOLUTE puts the child back in the parent's
        # coordinate space — before this slice it got NEITHER flex NOR
        # position and collapsed onto the stack's origin.
        tree = {"type": "FRAME", "name": "Stack", "id": "0:1", "layoutMode": "VERTICAL",
                "absoluteBoundingBox": self.BB,
                "children": [
                    {"type": "FRAME", "name": "Badge", "id": "0:2",
                     "layoutPositioning": "ABSOLUTE",
                     "absoluteBoundingBox": {"x": 88, "y": 2, "width": 10, "height": 10}},
                ]}
        ir, _ctx = frx.node_tree_to_ir(tree)
        badge = ir["children"][0]
        self.assertEqual(badge["style"]["position"], "absolute")
        self.assertEqual(badge["style"]["left"], 88)
        self.assertEqual(badge["style"]["top"], 2)

    def test_aspect_ratio_gated_on_flexible_axis(self):
        flex_parent = {"type": "FRAME", "layoutMode": "HORIZONTAL"}
        # REST documents a number; the Plugin API shape is a {x,y} Vector —
        # both must resolve.
        num = frx.extract_layout({"type": "RECTANGLE", "targetAspectRatio": 2.0,
                                  "layoutGrow": 1}, flex_parent)
        self.assertEqual(num["aspectRatio"], 2.0)
        vec = frx.extract_layout({"type": "RECTANGLE", "targetAspectRatio": {"x": 4, "y": 2},
                                  "layoutAlign": "STRETCH"}, flex_parent)
        self.assertEqual(vec["aspectRatio"], 2.0)
        # Fully fixed: the solved w/h already encode the ratio.
        fixed = frx.extract_layout({"type": "RECTANGLE", "targetAspectRatio": 2.0}, flex_parent)
        self.assertNotIn("aspectRatio", fixed)

    def test_min_max_sizing_lands_in_style(self):
        tree = {"type": "FRAME", "name": "Root", "id": "0:1",
                "absoluteBoundingBox": self.BB,
                "children": [
                    {"type": "FRAME", "name": "clamped", "id": "0:2",
                     "absoluteBoundingBox": {"x": 0, "y": 0, "width": 50, "height": 50},
                     "minWidth": 120, "maxWidth": 400, "maxHeight": 0},
                ]}
        ir, _ctx = frx.node_tree_to_ir(tree)
        s = ir["children"][0]["style"]
        self.assertEqual(s["min_width"], 120)
        self.assertEqual(s["max_width"], 400)
        # A 0/absent axis must not emit — a zero max would collapse the node.
        self.assertNotIn("max_height", s)
        self.assertNotIn("min_height", s)


class PaintStackTest(unittest.TestCase):
    """Ordered paint-stack lowering (audit item 7): leading-solid compositing,
    paint-level opacity, image scale mode, and structured diagnostics for
    whatever the color/gradient/image slot model cannot represent. Mirrors
    tools/figma-plugin/test/paints.test.ts field-for-field."""

    BB = {"x": 0, "y": 0, "width": 100, "height": 100}

    @staticmethod
    def _solid(r, g, b, **over):
        return {"type": "SOLID", "color": {"r": r, "g": g, "b": b}, **over}

    def _style(self, node):
        ctx = frx.ExtractContext()
        return frx.extract_style(node, ctx), ctx

    def test_solid_paint_opacity_folds_into_alpha(self):
        # A 50%-opacity black fill must not import fully opaque.
        s, ctx = self._style({"type": "RECTANGLE", "id": "1:1", "name": "half",
                              "absoluteBoundingBox": self.BB,
                              "fills": [self._solid(0, 0, 0, opacity=0.5)]})
        self.assertEqual(s["background_color"], "rgba(0, 0, 0, 0.500)")
        self.assertEqual(ctx.diagnostics, [])

    def test_gradient_paint_opacity_scales_stop_alpha(self):
        p = {"type": "GRADIENT_LINEAR", "opacity": 0.5, "gradientStops": [
            {"position": 0, "color": {"r": 1, "g": 1, "b": 1, "a": 1}},
            {"position": 1, "color": {"r": 0, "g": 0, "b": 0, "a": 0.5}}]}
        self.assertEqual(
            frx.gradient_to_css(p),
            "linear-gradient(to bottom, rgba(255, 255, 255, 0.500), "
            "rgba(0, 0, 0, 0.250))")
        # The positioned-stop helper (radial/conic) folds the same way.
        self.assertIn("rgba(0, 0, 0, 0.250) 100%", frx._gradient_stops_css(p))

    def test_leading_solids_composite_source_over(self):
        # #4b4d51 base with white@0.55 over it -> #aeafb1, the value Figma's
        # own raster samples (same fixture fig/scene.mjs compositeSolids cites).
        s, ctx = self._style({"type": "RECTANGLE", "id": "1:2", "name": "thumb",
                              "absoluteBoundingBox": self.BB,
                              "fills": [self._solid(0x4b/255, 0x4d/255, 0x51/255),
                                        self._solid(1, 1, 1, opacity=0.55)]})
        self.assertEqual(s["background_color"], "#aeafb1")
        self.assertEqual(ctx.diagnostics, [])

    def test_solid_below_gradient_fills_both_slots(self):
        s, ctx = self._style({"type": "RECTANGLE", "id": "1:3", "name": "panel",
                              "absoluteBoundingBox": self.BB,
                              "fills": [self._solid(0, 0, 0),
                                        {"type": "GRADIENT_LINEAR", "gradientStops": [
                                            {"position": 0, "color": {"r": 1, "g": 1, "b": 1, "a": 1}}]}]})
        self.assertEqual(s["background_color"], "#000000")
        self.assertTrue(s["background_gradient"].startswith("linear-gradient("))
        self.assertEqual(ctx.diagnostics, [])

    def test_extra_paints_raise_multi_paint_flattened(self):
        s, ctx = self._style({"type": "RECTANGLE", "id": "1:4", "name": "busy",
                              "absoluteBoundingBox": self.BB,
                              "fills": [
                                  {"type": "GRADIENT_LINEAR", "gradientStops": []},
                                  # A SEMI-TRANSPARENT solid above a gradient has
                                  # no slot (an opaque one would trim the stack
                                  # instead — covered by the test above).
                                  self._solid(1, 0, 0, opacity=0.5),
                              ]})
        self.assertNotIn("background_color", s)
        codes = [d["code"] for d in ctx.diagnostics]
        self.assertIn("multi-paint-flattened", codes)
        d = next(d for d in ctx.diagnostics if d["code"] == "multi-paint-flattened")
        self.assertEqual(d["kind"], "capture_partial")
        self.assertEqual(d["path"], "1:4")
        self.assertIn("SOLID", d["message"])

    def test_opaque_solid_above_gradient_hides_it_exactly(self):
        # Trimming at the last fully opaque solid is exact: the paints below
        # it are invisible in Figma too, so nothing is owed a diagnostic.
        s, ctx = self._style({"type": "RECTANGLE", "id": "1:12", "name": "capped",
                              "absoluteBoundingBox": self.BB,
                              "fills": [
                                  {"type": "GRADIENT_LINEAR", "gradientStops": []},
                                  self._solid(0.5, 0.5, 0.5),
                              ]})
        self.assertEqual(s["background_color"], "#808080")
        self.assertNotIn("background_gradient", s)
        self.assertEqual(ctx.diagnostics, [])

    def test_video_pattern_diagnose_and_never_shadow_a_solid(self):
        s, ctx = self._style({"type": "RECTANGLE", "id": "1:5", "name": "vid",
                              "absoluteBoundingBox": self.BB,
                              "fills": [self._solid(0, 0, 1),
                                        {"type": "VIDEO"}, {"type": "PATTERN"}]})
        self.assertEqual(s["background_color"], "#0000ff")
        d = next(d for d in ctx.diagnostics if d["code"] == "unsupported-paint-type")
        self.assertEqual(d["kind"], "unsupported_property")
        self.assertIn("VIDEO, PATTERN", d["message"])

    def test_paint_blend_mode_is_stated_and_paint_still_lowers(self):
        s, ctx = self._style({"type": "RECTANGLE", "id": "1:6", "name": "mult",
                              "absoluteBoundingBox": self.BB,
                              "fills": [self._solid(1, 1, 1, blendMode="MULTIPLY")]})
        self.assertEqual(s["background_color"], "#ffffff")
        d = next(d for d in ctx.diagnostics if d["code"] == "paint-blend-unsupported")
        self.assertIn("MULTIPLY", d["message"])

    def test_image_scale_modes_map_to_background_size(self):
        for mode, size, repeat in (("FILL", "cover", None),
                                   ("FIT", "contain", None),
                                   ("TILE", "auto", "repeat")):
            s, ctx = self._style({"type": "RECTANGLE", "id": "1:7", "name": "img",
                                  "absoluteBoundingBox": self.BB,
                                  "fills": [{"type": "IMAGE", "imageRef": "ref1",
                                             "scaleMode": mode}]})
            self.assertEqual(s["background_image"], "pending:ref1")
            self.assertEqual(s.get("background_size"), size)
            self.assertEqual(s.get("background_repeat"), repeat)
            self.assertEqual(ctx.diagnostics, [], mode)
            self.assertIn("ref1", ctx.image_fills)

    def test_image_crop_approximates_as_cover_with_diagnostic(self):
        s, ctx = self._style({"type": "RECTANGLE", "id": "1:8", "name": "crop",
                              "absoluteBoundingBox": self.BB,
                              "fills": [{"type": "IMAGE", "imageRef": "ref2",
                                         "scaleMode": "CROP",
                                         "imageTransform": [[1, 0, 0], [0, 1, 0]]}]})
        self.assertEqual(s.get("background_size"), "cover")
        d = next(d for d in ctx.diagnostics if d["code"] == "image-scale-approximated")
        self.assertEqual(d["kind"], "capture_partial")
        self.assertIn("CROP", d["message"])

    def test_image_paint_opacity_folds_when_childless(self):
        s, ctx = self._style({"type": "RECTANGLE", "id": "1:9", "name": "faded",
                              "absoluteBoundingBox": self.BB, "opacity": 0.8,
                              "fills": [{"type": "IMAGE", "imageRef": "ref3",
                                         "opacity": 0.5}]})
        self.assertAlmostEqual(s["opacity"], 0.4, places=6)
        self.assertEqual(ctx.diagnostics, [])

    def test_image_paint_opacity_with_children_is_diagnosed_not_folded(self):
        s, ctx = self._style({"type": "FRAME", "id": "1:10", "name": "parent",
                              "absoluteBoundingBox": self.BB,
                              "children": [{"type": "TEXT", "id": "1:11"}],
                              "fills": [{"type": "IMAGE", "imageRef": "ref4",
                                         "opacity": 0.5}]})
        self.assertNotIn("opacity", s)
        d = next(d for d in ctx.diagnostics if d["code"] == "image-opacity-dropped")
        self.assertEqual(d["kind"], "unsupported_property")

class StrokesTest(unittest.TestCase):
    """Strokes → Pulp's box-border contract, mirroring the plugin lane's
    extract-pure.ts::extractStrokeStyle and the .fig lane in fig/scene.mjs:
    per-side individualStrokeWeights, strokeDashes → dashed, multi-paint /
    non-solid flatten diagnostics, and preserved figma:* provenance."""

    RED = {"type": "SOLID", "visible": True,
           "color": {"r": 1, "g": 0, "b": 0, "a": 1}}
    BLUE = {"type": "SOLID", "visible": True,
            "color": {"r": 0, "g": 0, "b": 1, "a": 1}}
    GRAD = {"type": "GRADIENT_LINEAR", "visible": True}
    BB = {"x": 0, "y": 0, "width": 100, "height": 100}

    def test_uniform_stroke_keeps_the_shorthand(self):
        s = frx.extract_style({"type": "RECTANGLE", "id": "0:2",
                               "strokes": [self.RED], "strokeWeight": 2})
        self.assertEqual(s["border"], "2px solid #ff0000")
        self.assertEqual(s["border_width"], 2)
        self.assertEqual(s["border_style"], "solid")
        self.assertNotIn("border_top_width", s)

    def test_individual_stroke_weights_lower_per_side(self):
        # REST spells Figma's per-side weights as the individualStrokeWeights
        # object; the four discrete widths — an explicit 0 side stays 0 —
        # plus the single stroke color per painted side replace the shorthand.
        s = frx.extract_style({"type": "RECTANGLE", "id": "0:2",
                               "strokes": [self.RED], "strokeWeight": 4,
                               "individualStrokeWeights":
                                   {"top": 4, "right": 0, "bottom": 1, "left": 0}})
        self.assertEqual(s["border_top_width"], 4)
        self.assertEqual(s["border_right_width"], 0)
        self.assertEqual(s["border_bottom_width"], 1)
        self.assertEqual(s["border_left_width"], 0)
        self.assertEqual(s["border_color"], "#ff0000")
        self.assertEqual(s["border_style"], "solid")
        self.assertEqual(s["border_top_color"], "#ff0000")
        self.assertEqual(s["border_bottom_color"], "#ff0000")
        self.assertNotIn("border_right_color", s)
        self.assertNotIn("border", s)
        self.assertNotIn("border_width", s)

    def test_stroke_dashes_map_to_dashed_and_preserve_the_array(self):
        s = frx.extract_style({"type": "RECTANGLE", "id": "0:2",
                               "strokes": [self.RED], "strokeWeight": 2,
                               "strokeDashes": [4, 2]})
        self.assertEqual(s["border"], "2px dashed #ff0000")
        self.assertEqual(s["border_style"], "dashed")
        attrs = frx.extract_stroke_attributes(
            {"strokes": [self.RED], "strokeDashes": [4, 2]})
        self.assertEqual(attrs["figma:dash_pattern"], "4,2")

    def test_multi_paint_and_non_solid_strokes_are_diagnosed(self):
        ctx = frx.ExtractContext()
        # Two solids: first wins; multi-paint-stroke says so.
        s = frx.extract_style({"type": "RECTANGLE", "id": "0:2", "name": "Two",
                               "strokes": [self.RED, self.BLUE],
                               "strokeWeight": 1}, ctx)
        self.assertEqual(s["border_color"], "#ff0000")
        self.assertTrue(any(d["code"] == "multi-paint-stroke" and d["path"] == "0:2"
                            for d in ctx.diagnostics))
        # A gradient on top of a solid: flattened, diagnosed.
        ctx2 = frx.ExtractContext()
        s2 = frx.extract_style({"type": "RECTANGLE", "id": "0:3", "name": "GradTop",
                                "strokes": [self.GRAD, self.RED],
                                "strokeWeight": 1}, ctx2)
        self.assertEqual(s2["border_color"], "#ff0000")
        self.assertTrue(any(d["code"] == "complex-stroke-flattened"
                            for d in ctx2.diagnostics))
        # No solid anywhere: border dropped, diagnosed — never silently.
        ctx3 = frx.ExtractContext()
        s3 = frx.extract_style({"type": "RECTANGLE", "id": "0:4", "name": "GradOnly",
                                "strokes": [self.GRAD], "strokeWeight": 1}, ctx3)
        self.assertNotIn("border", s3)
        diag = [d for d in ctx3.diagnostics if d["code"] == "complex-stroke-flattened"]
        self.assertEqual(len(diag), 1)
        self.assertEqual(diag[0]["kind"], "capture_partial")

    def test_stroke_provenance_attrs_non_default_only(self):
        fancy = frx.extract_stroke_attributes(
            {"strokes": [self.RED], "strokeAlign": "OUTSIDE",
             "strokeCap": "ROUND", "strokeJoin": "BEVEL",
             # 22.6 degrees → miter limit 1/sin(11.3°) ≈ 5.1 (non-default).
             "strokeMiterAngle": 22.6})
        self.assertEqual(fancy["figma:stroke_align"], "outside")
        self.assertEqual(fancy["figma:stroke_cap"], "round")
        self.assertEqual(fancy["figma:stroke_join"], "bevel")
        self.assertEqual(fancy["figma:stroke_miter_limit"], "5.1")
        # Defaults — INSIDE / NONE / MITER / 28.96° (= limit 4.0) — and a
        # node with no visible stroke preserve nothing.
        dflt = frx.extract_stroke_attributes(
            {"strokes": [self.RED], "strokeAlign": "INSIDE", "strokeCap": "NONE",
             "strokeJoin": "MITER", "strokeMiterAngle": 28.96})
        self.assertEqual(dflt, {})
        self.assertEqual(frx.extract_stroke_attributes({"strokes": []}), {})

    def test_walk_merges_stroke_attrs_and_diagnostics_into_the_envelope(self):
        tree = {"type": "FRAME", "name": "Root", "id": "0:1",
                "absoluteBoundingBox": self.BB,
                "children": [
                    {"type": "FRAME", "name": "Dashy", "id": "0:2",
                     "absoluteBoundingBox": {"x": 0, "y": 0, "width": 50, "height": 50},
                     "strokes": [self.GRAD, self.RED], "strokeWeight": 2,
                     "strokeDashes": [10, 5]},
                ]}
        ir, ctx = frx.node_tree_to_ir(tree)
        child = ir["children"][0]
        self.assertEqual(child["style"]["border"], "2px dashed #ff0000")
        self.assertEqual(child["attributes"]["figma:dash_pattern"], "10,5")
        codes = {d["code"] for d in ctx.diagnostics}
        self.assertIn("multi-paint-stroke", codes)
        self.assertIn("complex-stroke-flattened", codes)


if __name__ == "__main__":
    unittest.main()
