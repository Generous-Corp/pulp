#include "../core/view/src/design_import_native_common.hpp"
#include "../core/view/src/design_import_internal.hpp"
#include "../core/view/src/design_ir_helpers.hpp"

#include <pulp/view/svg_path_widget.hpp>
#include <pulp/view/widgets/svg_line.hpp>
#include <pulp/view/widgets/svg_rect.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

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

// Channel-wise color check. CSS 8-bit channels round-trip through floats, so
// exact equality is unavailable; naming every expected channel (rather than
// asserting "not the default") is what makes a dropped alpha or a
// wrong-parser result fail instead of passing on a plausible-looking color.
void check_color(const pulp::canvas::Color& actual,
                 float r, float g, float b, float a) {
    CHECK(actual.r == Catch::Approx(r).margin(0.01f));
    CHECK(actual.g == Catch::Approx(g).margin(0.01f));
    CHECK(actual.b == Catch::Approx(b).margin(0.01f));
    CHECK(actual.a == Catch::Approx(a).margin(0.01f));
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
    const auto memory_resolved = resolve_design_ir_native(ir, {});
    auto resolved = resolve_design_ir_native_json(json, {});
    REQUIRE(resolved_snapshot(memory_resolved) == resolved_snapshot(resolved));
    REQUIRE(resolved.kind == NativeWidgetKind::view);
    REQUIRE(resolved.children.size() == 2);
    REQUIRE(child(resolved, 0).kind == NativeWidgetKind::image_view);
    REQUIRE(child(resolved, 0).id == "logo-anchor");
    REQUIRE_FALSE(has_diag(child(resolved, 0), "native-missing-asset"));
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

TEST_CASE("clear_baked_knob_antenna removes the antenna without notching the disc",
          "[view][import][knob][antenna]") {
    // Synthetic 40x50 RGBA8: a thin vertical "antenna" (x 18..21) standing above
    // a solid "disc body" block (x 8..31, y 20..44). This mirrors ELYSIUM's disc:
    // a baked indicator stem above the disc. The clear must erase the antenna and
    // leave the disc body byte-for-byte intact (an earlier version cut a notch).
    const int W = 40, H = 50;
    std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4, 0);
    auto set = [&](int x, int y, uint8_t a) {
        px[(static_cast<size_t>(y) * W + x) * 4 + 0] = 180;
        px[(static_cast<size_t>(y) * W + x) * 4 + 1] = 180;
        px[(static_cast<size_t>(y) * W + x) * 4 + 2] = 180;
        px[(static_cast<size_t>(y) * W + x) * 4 + 3] = a;
    };
    auto alpha = [&](int x, int y) {
        return px[(static_cast<size_t>(y) * W + x) * 4 + 3];
    };
    for (int y = 5; y < 20; ++y)            // antenna: thin column above the disc
        for (int x = 18; x <= 21; ++x) set(x, y, 255);
    for (int y = 20; y < 45; ++y)           // disc body: wide solid block
        for (int x = 8; x <= 31; ++x) set(x, y, 255);

    // Opaque bbox covers antenna + disc: x 8..31 (w=24), y 5..44 (h=40).
    clear_baked_knob_antenna(px, W, H, /*core_x=*/8, /*core_y=*/5,
                             /*core_w=*/24, /*core_h=*/40);

    // Antenna is gone (every antenna pixel cleared)...
    for (int y = 5; y < 20; ++y)
        for (int x = 18; x <= 21; ++x)
            REQUIRE(alpha(x, y) == 0);
    // ...and the disc body is untouched — no notch at its top edge or anywhere.
    for (int y = 20; y < 45; ++y)
        for (int x = 8; x <= 31; ++x)
            REQUIRE(alpha(x, y) == 255);
}

TEST_CASE("clear_baked_knob_antenna is a no-op when there is no antenna",
          "[view][import][knob][antenna]") {
    // A disc with no baked antenna (just the body) must be left fully intact —
    // the scan hits the wide disc row immediately and stops.
    const int W = 40, H = 40;
    std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4, 0);
    auto a = [&](int x, int y) { return px[(static_cast<size_t>(y) * W + x) * 4 + 3]; };
    for (int y = 8; y < 32; ++y)
        for (int x = 8; x <= 31; ++x) px[(static_cast<size_t>(y) * W + x) * 4 + 3] = 255;

    clear_baked_knob_antenna(px, W, H, 8, 8, 24, 24);

    for (int y = 8; y < 32; ++y)
        for (int x = 8; x <= 31; ++x)
            REQUIRE(a(x, y) == 255);
}

TEST_CASE("native resolver recognizes the Ink & Signal design-system vocabulary",
          "[view][import][native-resolver][design-system]") {
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.stable_anchor_id = "root";

    auto add = [&](const char* type) {
        IRNode n;
        n.type = type;
        ir.root.children.push_back(n);
    };
    // Design-system / common-web aliases that must map to native widgets.
    add("toggle");        // 0 → toggle_button
    add("switch");        // 1 → toggle_button
    add("combobox");      // 2 → combo_box (previously unmapped — real gap)
    add("dropdown");      // 3 → combo_box
    add("select");        // 4 → combo_box
    add("pan");           // 5 → fader (1-D control)
    add("badge");         // 6 → label (text pill)
    add("panel");         // 7 → view (container)
    add("channel_strip"); // 8 → view (container)
    add("sidebar");       // 9 → view (container)

    auto resolved = resolve_design_ir_native(ir, {});
    REQUIRE(resolved.children.size() == 10);
    REQUIRE(child(resolved, 0).kind == NativeWidgetKind::toggle_button);
    REQUIRE(child(resolved, 1).kind == NativeWidgetKind::toggle_button);
    REQUIRE(child(resolved, 2).kind == NativeWidgetKind::combo_box);
    REQUIRE(child(resolved, 3).kind == NativeWidgetKind::combo_box);
    REQUIRE(child(resolved, 4).kind == NativeWidgetKind::combo_box);
    REQUIRE(child(resolved, 5).kind == NativeWidgetKind::fader);
    REQUIRE(child(resolved, 6).kind == NativeWidgetKind::label);
    REQUIRE(child(resolved, 7).kind == NativeWidgetKind::view);
    REQUIRE(child(resolved, 8).kind == NativeWidgetKind::view);
    REQUIRE(child(resolved, 9).kind == NativeWidgetKind::view);

    // None of these should be flagged as an unsupported node anymore.
    REQUIRE_FALSE(has_diag(resolved, "native-unsupported-node"));
}

TEST_CASE("native hit-ownership contract is exhaustive across widget kinds",
          "[design-import][hit-policy]") {
    // The runtime materializer and the baked-C++ codegen share one definition of
    // these predicates, so this table is the single canonical contract. Every
    // NativeWidgetKind is listed explicitly; a new kind or a flipped answer must
    // be reconciled here, which is what keeps the two lowerers from drifting
    // (combo_box, in particular, is interactive and owns its children's hits).
    struct HitPolicyRow {
        NativeWidgetKind kind;
        bool interactive;
        bool owns_child_hits;
    };
    const HitPolicyRow rows[] = {
        {NativeWidgetKind::view,          false, false},
        {NativeWidgetKind::label,         false, false},
        {NativeWidgetKind::text_button,   true,  true},
        {NativeWidgetKind::text_editor,   true,  true},
        {NativeWidgetKind::checkbox,      true,  true},
        {NativeWidgetKind::toggle_button, true,  true},
        {NativeWidgetKind::combo_box,     true,  true},
        {NativeWidgetKind::knob,          true,  true},
        {NativeWidgetKind::fader,         true,  true},
        {NativeWidgetKind::meter,         false, true},
        {NativeWidgetKind::xy_pad,        true,  true},
        {NativeWidgetKind::waveform,      false, true},
        {NativeWidgetKind::spectrum,      false, true},
        {NativeWidgetKind::image_view,    false, false},
        {NativeWidgetKind::canvas,        false, false},
        {NativeWidgetKind::svg_path,      false, false},
        {NativeWidgetKind::svg_rect,      false, false},
        {NativeWidgetKind::svg_line,      false, false},
    };

    for (const auto& row : rows) {
        INFO("kind=" << native_widget_kind_name(row.kind));
        CHECK(is_interactive_native_kind(row.kind) == row.interactive);
        CHECK(native_kind_owns_imported_child_hits(row.kind) == row.owns_child_hits);
    }
}

TEST_CASE("every per-side border color accepts non-hex CSS",
          "[view][import][native-common][css-color]") {
    // The four per-side border colors are the paint sites furthest from the one
    // branch that historically owned the rgb()/rgba() fallback, so they are where
    // a call-site-local fix silently stops. Each side carries a DIFFERENT color
    // syntax: a per-side assertion fails loudly if a side reverts to hex-only,
    // where a single shared color would let a copy-paste slip through.
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.stable_anchor_id = "grid";
    ir.root.style.width = 100.0f;
    ir.root.style.height = 40.0f;
    ir.root.style.border_top_width = 1.0f;
    ir.root.style.border_right_width = 1.0f;
    ir.root.style.border_bottom_width = 1.0f;
    ir.root.style.border_left_width = 1.0f;
    // The real Figma shape: a hairline stroke demoted to a 1px frame whose fill
    // carries the alpha. Dropping the alpha renders the grid at full opacity;
    // dropping the color renders it not at all. Assert the alpha, not just a>0.
    ir.root.style.border_top_color = "rgba(171, 171, 171, 0.1)";
    ir.root.style.border_right_color = "rgb(137, 180, 250)";
    ir.root.style.border_bottom_color = "#89b4fa";
    ir.root.style.border_left_color = "transparent";

    auto root = build_native_view_tree(ir, {}, {});
    REQUIRE(root != nullptr);

    check_color(root->border_top_color(), 171.0f / 255.0f, 171.0f / 255.0f,
                171.0f / 255.0f, 0.1f);
    check_color(root->border_right_color(), 137.0f / 255.0f, 180.0f / 255.0f,
                250.0f / 255.0f, 1.0f);
    // The hex fast path shares the helper's first branch — asserted here so a
    // regression that breaks hex while chasing rgb() is caught in the same test.
    check_color(root->border_bottom_color(), 137.0f / 255.0f, 180.0f / 255.0f,
                250.0f / 255.0f, 1.0f);
    check_color(root->border_left_color(), 0.0f, 0.0f, 0.0f, 0.0f);
}

TEST_CASE("an unparseable CSS color leaves the paint site untouched",
          "[view][import][native-common][css-color]") {
    // A color the helper recognizes neither as hex nor as a functional syntax
    // must yield nullopt so the paint site keeps its default. This matters
    // because the shared CSS parser returns opaque WHITE for anything it fails
    // to understand: routing an unknown token into it would repaint the border
    // white rather than leave it alone — a wrong color is worse than no color,
    // since it can't be told apart from a deliberate one downstream.
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.stable_anchor_id = "panel";
    ir.root.style.width = 100.0f;
    ir.root.style.height = 40.0f;
    ir.root.style.border_width = 1.0f;
    ir.root.style.border_color = "chartreuse";

    auto root = build_native_view_tree(ir, {}, {});
    REQUIRE(root != nullptr);

    // View's untouched default — NOT the parser's white fallback.
    check_color(root->border_color(), 0.0f, 0.0f, 0.0f, 1.0f);
}

TEST_CASE("hsl() paints the color it names",
          "[view][import][native-common][css-color]") {
    // This case was first written to characterize a gap: the helper admitted an
    // `hsl(` prefix and handed it to a parser that implemented only #hex, rgb(),
    // rgba() and `transparent`, so hsl() fell off the end and took that parser's
    // opaque-WHITE default. That is worse than refusing it — an unparseable
    // token leaves a paint site untouched, but a wrong color is
    // indistinguishable downstream from a deliberate one, so an hsl() design
    // painted white and looked like someone had chosen white.
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.stable_anchor_id = "panel";
    ir.root.style.width = 100.0f;
    ir.root.style.height = 40.0f;
    ir.root.style.border_width = 1.0f;
    ir.root.style.border_color = "hsl(210, 90%, 60%)";

    auto root = build_native_view_tree(ir, {}, {});
    REQUIRE(root != nullptr);
    // rgb(61, 153, 245), checked against a reference implementation rather than
    // taken on trust — the value this replaced said 138 and was simply wrong.
    check_color(root->border_color(), 61.0f / 255.0f, 153.0f / 255.0f, 245.0f / 255.0f, 1.0f);
}

TEST_CASE("hsl() handles the forms designs actually ship",
          "[view][import][native-common][css-color]") {
    auto border_of = [](const char* css) {
        DesignIR ir;
        ir.root.type = "frame";
        ir.root.stable_anchor_id = "panel";
        ir.root.style.width = 100.0f;
        ir.root.style.height = 40.0f;
        ir.root.style.border_width = 1.0f;
        ir.root.style.border_color = css;
        return build_native_view_tree(ir, {}, {})->border_color();
    };

    // Saturation 0 is grey at every hue — the cheapest check that the maths is
    // not accidentally hue-driven.
    check_color(border_of("hsl(0, 0%, 50%)"), 0.5f, 0.5f, 0.5f, 1.0f);
    // Alpha must survive: a dropped alpha is the silent flattening this branch
    // has hit over and over.
    check_color(border_of("hsla(120, 100%, 50%, 0.5)"), 0.0f, 1.0f, 0.0f, 0.5f);
    // Hue wraps rather than clamps: hsl(370) is hsl(10), and clamping to 360
    // would turn a red into a wrong red rather than an obviously broken one.
    check_color(border_of("hsl(370, 100%, 50%)"), border_of("hsl(10, 100%, 50%)").r,
                border_of("hsl(10, 100%, 50%)").g, border_of("hsl(10, 100%, 50%)").b, 1.0f);
    // The modern space-separated spelling is the one Figma and most token
    // pipelines emit today.
    check_color(border_of("hsl(120 100% 50%)"), 0.0f, 1.0f, 0.0f, 1.0f);
}

TEST_CASE("SVG fill and stroke accept non-hex CSS across every shape widget",
          "[view][import][native-common][css-color]") {
    // The three SVG paint helpers are separate overloads, each with its own
    // fill/stroke pair, so each is its own chance to miss the shared helper.
    // A Figma vector export writes rgba() strokes directly, so a hex-only site
    // here renders the shape in the widget's default black.
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.stable_anchor_id = "vector-root";

    IRNode path;
    path.type = "path";
    path.stable_anchor_id = "curve";
    path.attributes["d"] = "M0 0L10 10";
    path.attributes["fill"] = "rgba(126, 106, 255, 0.25)";
    path.attributes["stroke"] = "rgb(137, 180, 250)";
    ir.root.children.push_back(path);

    IRNode rect;
    rect.type = "rect";
    rect.stable_anchor_id = "pad";
    rect.attributes["width"] = "10";
    rect.attributes["height"] = "10";
    rect.attributes["fill"] = "rgb(255, 0, 128)";
    rect.attributes["stroke"] = "#89b4fa";
    ir.root.children.push_back(rect);

    IRNode line;
    line.type = "line";
    line.stable_anchor_id = "hairline";
    line.attributes["x2"] = "10";
    line.attributes["stroke"] = "rgba(171, 171, 171, 0.1)";
    ir.root.children.push_back(line);

    auto root = build_native_view_tree(ir, {}, {});
    REQUIRE(root != nullptr);
    REQUIRE(root->child_count() == 3);

    auto* svg_path = dynamic_cast<SvgPathWidget*>(root->child_at(0));
    REQUIRE(svg_path != nullptr);
    check_color(svg_path->fill_color(), 126.0f / 255.0f, 106.0f / 255.0f,
                255.0f / 255.0f, 0.25f);
    check_color(svg_path->stroke_color(), 137.0f / 255.0f, 180.0f / 255.0f,
                250.0f / 255.0f, 1.0f);

    auto* svg_rect = dynamic_cast<SvgRectWidget*>(root->child_at(1));
    REQUIRE(svg_rect != nullptr);
    check_color(svg_rect->fill_color(), 1.0f, 0.0f, 128.0f / 255.0f, 1.0f);
    // Hex kept under assertion alongside its rgb() sibling on the same widget.
    check_color(svg_rect->stroke_color(), 137.0f / 255.0f, 180.0f / 255.0f,
                250.0f / 255.0f, 1.0f);

    auto* svg_line = dynamic_cast<SvgLineWidget*>(root->child_at(2));
    REQUIRE(svg_line != nullptr);
    check_color(svg_line->stroke_color(), 171.0f / 255.0f, 171.0f / 255.0f,
                171.0f / 255.0f, 0.1f);
}
// ── Shared design-IR helpers ─────────────────────────────────────────────
// design_ir_helpers.hpp holds the one definition of the accessors and parsers
// every design lane (native materializer, C++ emitter, Swift emitter) reads the
// IR through. These pin the contract each lane now depends on, so a change here
// is a deliberate cross-lane decision rather than a silent per-copy drift.

TEST_CASE("shared attr accessors read design-IR attributes", "[design-ir-helpers]") {
    IRNode node;
    node.attributes["src"] = "logo.png";
    node.attributes["empty"] = "";

    CHECK(attr(node, "src") == std::optional<std::string>("logo.png"));
    CHECK(attr(node, "empty") == std::optional<std::string>(""));
    CHECK_FALSE(attr(node, "absent").has_value());
}

TEST_CASE("shared attr_bool accepts both polarities and falls back", "[design-ir-helpers]") {
    IRNode node;
    for (const char* truthy : {"true", "1", "yes", "on", "TRUE", "On", "YES"}) {
        node.attributes["flag"] = truthy;
        INFO("value=" << truthy);
        CHECK(attr_bool(node, "flag"));
        CHECK(attr_bool(node, "flag", true));
    }
    for (const char* falsy : {"false", "0", "no", "off", "FALSE", "Off"}) {
        node.attributes["flag"] = falsy;
        INFO("value=" << falsy);
        CHECK_FALSE(attr_bool(node, "flag"));
        CHECK_FALSE(attr_bool(node, "flag", true));
    }
    // An unrecognized spelling and an absent attribute both yield the fallback.
    node.attributes["flag"] = "maybe";
    CHECK_FALSE(attr_bool(node, "flag"));
    CHECK(attr_bool(node, "flag", true));
    CHECK_FALSE(attr_bool(node, "absent"));
    CHECK(attr_bool(node, "absent", true));
}

TEST_CASE("shared first_asset_id honors key priority then sorts", "[design-ir-helpers]") {
    IRNode node;
    node.attributes["zAssetId"] = "z";
    node.attributes["hrefAssetId"] = "href";
    node.attributes["backgroundImageAssetId"] = "bg";
    node.attributes["srcAssetId"] = "src";
    CHECK(first_asset_id(node) == std::optional<std::string>("src"));

    node.attributes.erase("srcAssetId");
    CHECK(first_asset_id(node) == std::optional<std::string>("bg"));
    node.attributes.erase("backgroundImageAssetId");
    CHECK(first_asset_id(node) == std::optional<std::string>("href"));
    node.attributes.erase("hrefAssetId");

    // asset_ref is the last explicit key, ahead of the sorted `*AssetId` scan.
    node.attributes["asset_ref"] = "ref";
    CHECK(first_asset_id(node) == std::optional<std::string>("ref"));
    node.attributes.erase("asset_ref");

    // Fallback scan: lowest key wins, deterministically, and an empty value is
    // not a match.
    node.attributes["aAssetId"] = "a";
    CHECK(first_asset_id(node) == std::optional<std::string>("a"));
    node.attributes["aAssetId"] = "";
    CHECK(first_asset_id(node) == std::optional<std::string>("z"));

    IRNode bare;
    bare.attributes["notAnAsset"] = "x";
    CHECK_FALSE(first_asset_id(bare).has_value());
}

TEST_CASE("shared asset_uri prefers a local file and rejects remote", "[design-ir-helpers]") {
    IRAssetRef asset;
    asset.local_path = "/tmp/art.png";
    asset.original_uri = "https://example.com/art.png";
    CHECK(asset_uri(asset) == "file:///tmp/art.png");

    asset.local_path.reset();
    // A remote URI is not loadable by anything downstream — report unresolved.
    CHECK(asset_uri(asset).empty());

    for (const char* self_contained : {"data:image/png;base64,AA==",
                                       "resource://icons/knob.png",
                                       "memory://cache/0"}) {
        asset.original_uri = self_contained;
        INFO("uri=" << self_contained);
        CHECK(asset_uri(asset) == self_contained);
    }

    asset.original_uri.clear();
    CHECK(asset_uri(asset).empty());
}

TEST_CASE("shared lower_copy lowercases ASCII only", "[design-ir-helpers]") {
    CHECK(lower_copy("Flex-Start") == "flex-start");
    CHECK(lower_copy("") == "");
    // Non-ASCII bytes pass through untouched.
    CHECK(lower_copy("Ünicode") == "Ünicode");
}

TEST_CASE("shared hex_digit maps hex characters", "[design-ir-helpers]") {
    CHECK(hex_digit('0') == 0);
    CHECK(hex_digit('9') == 9);
    CHECK(hex_digit('a') == 10);
    CHECK(hex_digit('F') == 15);
    CHECK(hex_digit('g') == -1);
    CHECK(hex_digit('#') == -1);
}

TEST_CASE("shared parse_hex_color_rgba covers every hex shape", "[design-ir-helpers]") {
    using Rgba = std::array<unsigned, 4>;

    // Short forms expand each nibble to a byte; alpha defaults to opaque.
    CHECK(parse_hex_color_rgba("#abc") == std::optional<Rgba>(Rgba{0xaa, 0xbb, 0xcc, 0xff}));
    CHECK(parse_hex_color_rgba("#abcd") == std::optional<Rgba>(Rgba{0xaa, 0xbb, 0xcc, 0xdd}));
    CHECK(parse_hex_color_rgba("#1a2b3c") == std::optional<Rgba>(Rgba{0x1a, 0x2b, 0x3c, 0xff}));
    CHECK(parse_hex_color_rgba("#1a2b3c4d") == std::optional<Rgba>(Rgba{0x1a, 0x2b, 0x3c, 0x4d}));
    CHECK(parse_hex_color_rgba("#ABCDEF") == std::optional<Rgba>(Rgba{0xab, 0xcd, 0xef, 0xff}));

    // Anything that is not a well-formed hex triplet is the caller's problem —
    // notably a CSS rgb()/rgba() token, which only the Swift lane parses.
    CHECK_FALSE(parse_hex_color_rgba("").has_value());
    CHECK_FALSE(parse_hex_color_rgba("abc").has_value());
    CHECK_FALSE(parse_hex_color_rgba("#ab").has_value());
    CHECK_FALSE(parse_hex_color_rgba("#abcde").has_value());
    CHECK_FALSE(parse_hex_color_rgba("#gggggg").has_value());
    CHECK_FALSE(parse_hex_color_rgba("rgb(1,2,3)").has_value());
    CHECK_FALSE(parse_hex_color_rgba("rebeccapurple").has_value());
}
