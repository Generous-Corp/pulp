#include "../core/view/src/design_import_native_common.hpp"

#include <catch2/catch_test_macros.hpp>

#include <sstream>
#include <string>
#include <string_view>

using namespace pulp::view;

namespace {

bool has_diag(const ResolvedNativeNode& node, std::string_view code) {
    for (const auto& diagnostic : node.diagnostics) {
        if (diagnostic.code == code) return true;
    }
    for (const auto& child : node.children) {
        if (has_diag(child, code)) return true;
    }
    return false;
}

const ResolvedNativeNode& child(const ResolvedNativeNode& node, std::size_t index) {
    REQUIRE(index < node.children.size());
    return node.children[index];
}

std::string resolved_snapshot(const ResolvedNativeNode& node, int depth = 0) {
    std::ostringstream out;
    const std::string indent(static_cast<std::size_t>(depth * 2), ' ');
    out << indent << native_widget_kind_name(node.kind) << "#" << node.id;
    if (node.text) out << " text=" << *node.text;
    out << "\n";
    for (const auto& diagnostic : node.diagnostics) {
        out << indent << "  diag:" << diagnostic.code << ":" << diagnostic.path;
        if (diagnostic.property) out << ":" << *diagnostic.property;
        out << "\n";
    }
    for (const auto& resolved_child : node.children)
        out << resolved_snapshot(resolved_child, depth + 1);
    return out.str();
}

} // namespace

TEST_CASE("native resolver applies mapping precedence and HTML subtype rules",
          "[view][import][native-resolver]") {
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.name = "Panel";
    ir.root.stable_anchor_id = "root-anchor";

    IRNode audio;
    audio.type = "video";
    audio.name = "Gain";
    audio.audio_widget = AudioWidgetType::knob;
    audio.audio_label = "Gain";
    ir.root.children.push_back(audio);

    IRNode range;
    range.type = "input";
    range.name = "MixRange";
    range.attributes["type"] = "range";
    ir.root.children.push_back(range);

    IRNode checkbox;
    checkbox.type = "input";
    checkbox.name = "Bypass";
    checkbox.attributes["type"] = "checkbox";
    checkbox.attributes["label"] = "Bypass";
    ir.root.children.push_back(checkbox);

    IRNode button;
    button.type = "button";
    button.text_content = "Save";
    button.attributes["id"] = "save-button";
    ir.root.children.push_back(button);

    auto resolved = resolve_design_ir_native(ir, {});
    REQUIRE(resolved.kind == NativeWidgetKind::view);
    REQUIRE(resolved.id == "root-anchor");
    REQUIRE(resolved.children.size() == 4);

    REQUIRE(child(resolved, 0).kind == NativeWidgetKind::knob);
    REQUIRE(child(resolved, 0).text == "Gain");
    REQUIRE_FALSE(has_diag(child(resolved, 0), "native-unsupported-node"));

    REQUIRE(child(resolved, 1).kind == NativeWidgetKind::fader);
    REQUIRE(child(resolved, 2).kind == NativeWidgetKind::checkbox);
    REQUIRE(child(resolved, 2).text == "Bypass");
    REQUIRE(child(resolved, 3).kind == NativeWidgetKind::text_button);
    REQUIRE(child(resolved, 3).id == "save-button");
    REQUIRE(child(resolved, 3).text == "Save");
}

TEST_CASE("native resolver consumes frozen DesignIR JSON plus manifest diagnostics",
          "[view][import][native-resolver]") {
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.name = "Root";

    ImportDiagnostic top_level;
    top_level.severity = ImportDiagnosticSeverity::warning;
    top_level.kind = ImportDiagnosticKind::snapshot_semantics_warning;
    top_level.code = "snapshot-dynamic-api";
    top_level.path = "<source>";
    top_level.message = "Date.now";
    ir.diagnostics.push_back(top_level);

    IRNode image;
    image.type = "image";
    image.name = "Logo";
    image.stable_anchor_id = "logo-anchor";
    image.attributes["srcAssetId"] = "asset-logo";
    image.style.background_gradient = "linear-gradient(#000,#fff)";
    ir.root.children.push_back(image);

    IRNode missing;
    missing.type = "image";
    missing.name = "Missing";
    missing.attributes["srcAssetId"] = "asset-missing";
    ir.root.children.push_back(missing);

    IRAssetRef asset;
    asset.asset_id = "asset-logo";
    asset.original_uri = "logo.png";
    ImportDiagnostic asset_diagnostic;
    asset_diagnostic.severity = ImportDiagnosticSeverity::error;
    asset_diagnostic.kind = ImportDiagnosticKind::unresolved_asset;
    asset_diagnostic.code = "asset-unresolved";
    asset_diagnostic.message = "could not resolve logo.png";
    asset.diagnostics.push_back(asset_diagnostic);

    IRAssetManifest manifest;
    manifest.assets.push_back(asset);
    ir.asset_manifest = manifest;

    const auto json = serialize_design_ir(ir);
    auto resolved = resolve_design_ir_native_json(json, {});
    REQUIRE(resolved.kind == NativeWidgetKind::view);
    REQUIRE(resolved.children.size() == 2);
    REQUIRE(child(resolved, 0).kind == NativeWidgetKind::image_view);
    REQUIRE(child(resolved, 0).id == "logo-anchor");
    REQUIRE(has_diag(resolved, "snapshot-dynamic-api"));
    REQUIRE(has_diag(resolved, "asset-unresolved"));
    REQUIRE(has_diag(resolved, "native-missing-asset"));
    REQUIRE(has_diag(resolved, "native-unsupported-property"));
}

TEST_CASE("native resolver falls back gracefully and is bit-stable",
          "[view][import][native-resolver]") {
    DesignIR ir;
    ir.root.type = "frame";

    IRNode unknown;
    unknown.type = "video";
    unknown.style.filter = "blur(2px)";
    unknown.children.push_back(IRNode{});
    unknown.children.back().type = "text";
    unknown.children.back().text_content = "Caption";
    ir.root.children.push_back(unknown);

    IRNode canvas;
    canvas.type = "canvas";
    ir.root.children.push_back(canvas);

    IRNode svg_path;
    svg_path.type = "svg_path";
    ir.root.children.push_back(svg_path);

    IRNode svg_rect;
    svg_rect.type = "rect";
    ir.root.children.push_back(svg_rect);

    IRNode svg_line;
    svg_line.type = "line";
    ir.root.children.push_back(svg_line);

    const auto first = resolve_design_ir_native(ir, {});
    const auto second = resolve_design_ir_native(ir, {});

    REQUIRE(first.kind == NativeWidgetKind::view);
    REQUIRE(child(first, 0).kind == NativeWidgetKind::view);
    REQUIRE(child(first, 0).id == "$/children[0]");
    REQUIRE(child(child(first, 0), 0).kind == NativeWidgetKind::label);
    REQUIRE(child(child(first, 0), 0).text == "Caption");
    REQUIRE(child(first, 1).kind == NativeWidgetKind::canvas);
    REQUIRE(child(first, 2).kind == NativeWidgetKind::svg_path);
    REQUIRE(child(first, 3).kind == NativeWidgetKind::svg_rect);
    REQUIRE(child(first, 4).kind == NativeWidgetKind::svg_line);
    REQUIRE(has_diag(first, "native-unsupported-node"));
    REQUIRE(has_diag(first, "native-unsupported-property"));
    REQUIRE(resolved_snapshot(first) == resolved_snapshot(second));
}

TEST_CASE("native resolver keeps asset diagnostics deterministic across JSON order",
          "[view][import][native-resolver]") {
    DesignIR ir;
    ir.root.type = "frame";

    IRNode image;
    image.type = "image";
    image.attributes["srcAssetId"] = "missing-src";
    image.attributes["backgroundImageAssetId"] = "missing-background";
    ir.root.children.push_back(image);

    const auto memory_resolved = resolve_design_ir_native(ir, {});
    const auto json_resolved = resolve_design_ir_native_json(serialize_design_ir(ir), {});
    const auto memory_snapshot = resolved_snapshot(memory_resolved);

    REQUIRE(memory_snapshot == resolved_snapshot(json_resolved));

    const auto background_pos =
        memory_snapshot.find("diag:native-missing-asset:$/children[0]:backgroundImageAssetId");
    const auto src_pos =
        memory_snapshot.find("diag:native-missing-asset:$/children[0]:srcAssetId");
    REQUIRE(background_pos != std::string::npos);
    REQUIRE(src_pos != std::string::npos);
    REQUIRE(background_pos < src_pos);
}
