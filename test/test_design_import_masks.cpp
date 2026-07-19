// Sibling-mask lowering (audit item 9) — consumer-side end-to-end tests for
// the envelope contract every Figma producer (.fig decoder, plugin, REST)
// emits for Figma sibling masks. Producer-side coverage lives with each lane:
// tools/import-design/fig/fig.test.mjs, tools/figma-plugin/test/masks.test.ts,
// and test/test_figma_rest_export.py.

#include "test_design_import_shared.hpp"
// ──────────────────────────────────────────────────────────────────────────
// Sibling-mask lowering (audit item 9) — the envelope contract every Figma
// producer (.fig decoder, plugin, REST) now emits: a mask child paints
// NOWHERE; the siblings painted after it arrive under a synthetic
// `(mask scope)` wrapper whose style.clip_path carries the mask outline in
// the parent's space. These tests pin the consumer half end-to-end: the
// wrapper's clip parses into IRStyle::clip_path, the masked siblings are its
// children, the explicit audio_widget "none" opt-out keeps a widgety mask
// name from being guessed into a control, and the semantic-loss diagnostics
// (mask-luminance-approximated / complex-mask-flattened) land on the IR.

TEST_CASE("figma-plugin envelope: mask-scope wrapper carries the clip and the masked siblings",
          "[view][import][figma-plugin][masks]") {
    // Shape mirrors tools/figma-plugin/src/serialize.ts for a panel whose
    // children were [Below, Knob (isMask), ContentA, ContentB]: the mask is
    // gone, Below stays outside the wrapper, ContentA/ContentB are inside.
    const std::string envelope = R"JSON({
        "format_version": "v1",
        "parser_version": "0.1.0",
        "compat_schema_version": "v1",
        "provenance": {
            "adapter": "figma-plugin",
            "version": "0.1.0",
            "source_uri": "figma://design/abc/Masked-Panel"
        },
        "root": {
            "type": "frame",
            "name": "Panel",
            "figma_node_id": "1:1",
            "style": { "width": 400, "height": 300 },
            "children": [
                {
                    "type": "frame",
                    "name": "Below",
                    "figma_node_id": "1:2",
                    "style": { "width": 400, "height": 300,
                               "position": "absolute", "left": 0, "top": 0,
                               "background_color": "#222222" },
                    "children": []
                },
                {
                    "type": "frame",
                    "name": "Knob (mask scope)",
                    "figma_node_id": "1:3/mask-scope",
                    "audio_widget": "none",
                    "style": {
                        "width": 400, "height": 300,
                        "position": "absolute", "left": 0, "top": 0,
                        "clip_path": "path(\"M40 30 L240 30 L240 130 L40 130 L40 30 Z\")"
                    },
                    "children": [
                        {
                            "type": "frame",
                            "name": "ContentA",
                            "figma_node_id": "1:4",
                            "style": { "width": 400, "height": 300,
                                       "position": "absolute", "left": 0, "top": 0,
                                       "background_color": "#ff3355" },
                            "children": []
                        },
                        {
                            "type": "frame",
                            "name": "ContentB",
                            "figma_node_id": "1:5",
                            "style": { "width": 100, "height": 100,
                                       "position": "absolute", "left": 10, "top": 10,
                                       "background_color": "#3355ff" },
                            "children": []
                        }
                    ]
                }
            ]
        }
    })JSON";

    auto ir = parse_figma_plugin_json(envelope);
    REQUIRE(ir.root.children.size() == 2);
    REQUIRE(ir.root.children[0].name == "Below");

    const auto& scope = ir.root.children[1];
    REQUIRE(scope.name == "Knob (mask scope)");
    // The outline clip survives into the field codegen lowers to
    // setClipPath → SkPath::FromSVGString.
    REQUIRE(scope.style.clip_path.has_value());
    REQUIRE(*scope.style.clip_path
            == "path(\"M40 30 L240 30 L240 130 L40 130 L40 30 Z\")");
    // The wrapper paints nothing of the mask's own fill…
    REQUIRE_FALSE(scope.style.background_color.has_value());
    // …and the explicit "none" opt-out beats the name heuristic ("Knob …"
    // would otherwise be guessed into a knob widget).
    REQUIRE(scope.audio_widget == AudioWidgetType::none);
    // The masked siblings are the wrapper's children, coordinates preserved.
    REQUIRE(scope.children.size() == 2);
    REQUIRE(scope.children[0].name == "ContentA");
    REQUIRE(scope.children[1].name == "ContentB");
    // The mask node itself was never emitted.
    std::function<bool(const IRNode&)> has_mask_node = [&](const IRNode& n) {
        if (n.name == "Knob") return true;
        for (const auto& c : n.children)
            if (has_mask_node(c)) return true;
        return false;
    };
    REQUIRE_FALSE(has_mask_node(ir.root));
}

TEST_CASE("REST-shaped envelope: luminance mask keeps the best-effort clip and is diagnosed",
          "[view][import][figma-plugin][masks][diagnostics]") {
    // Shape mirrors tools/import-design/figma_rest_export.py for a luminance
    // mask: the wrapper still carries the outline clip (best effort), and the
    // structured semantic-loss diagnostic rides the envelope's diagnostics
    // array in the plugin diagnostic shape.
    const std::string envelope = R"JSON({
        "format_version": "v1",
        "parser_version": "0.1.0",
        "compat_schema_version": "v1",
        "provenance": { "adapter": "figma-rest", "version": "0.1.0" },
        "root": {
            "type": "frame",
            "name": "Panel",
            "figma_node_id": "1:1",
            "style": { "width": 400, "height": 300 },
            "children": [
                {
                    "type": "frame",
                    "name": "Fade (mask scope)",
                    "figma_node_id": "1:3/mask-scope",
                    "audio_widget": "none",
                    "style": {
                        "width": 400, "height": 300,
                        "position": "absolute", "left": 0, "top": 0,
                        "clip_path": "path(\"M40 30 L240 30 L240 130 L40 130 L40 30 Z\")"
                    },
                    "children": [
                        {
                            "type": "frame",
                            "name": "Content",
                            "figma_node_id": "1:4",
                            "style": { "width": 400, "height": 300 },
                            "children": []
                        }
                    ]
                }
            ]
        },
        "diagnostics": [
            {
                "severity": "warning",
                "code": "mask-luminance-approximated",
                "kind": "unsupported_property",
                "message": "Luminance mask \"Fade\" flattened to its outline clip; pixel-brightness alpha is not reproduced.",
                "path": "1:3"
            }
        ]
    })JSON";

    auto ir = parse_figma_plugin_json(envelope);
    REQUIRE(ir.root.children.size() == 1);
    const auto& scope = ir.root.children[0];
    REQUIRE(scope.style.clip_path.has_value());
    REQUIRE(scope.children.size() == 1);

    REQUIRE(ir.diagnostics.size() == 1);
    const auto& diag = ir.diagnostics[0];
    REQUIRE(diag.code == "mask-luminance-approximated");
    REQUIRE(diag.severity == ImportDiagnosticSeverity::warning);
    REQUIRE(diag.kind == ImportDiagnosticKind::unsupported_property);
    REQUIRE(diag.path == "1:3");
}
