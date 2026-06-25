// Unit tests for the reference-free import-fidelity self-checks
// (core/view/src/design_fidelity.cpp). These exercise each pure check in
// isolation via a FidelityContext; the codegen-routing / end-to-end cases live
// in test_design_import.cpp (they drive generate_pulp_js).
#include <catch2/catch_test_macros.hpp>

#include <pulp/view/design_ir.hpp>
#include <pulp/view/design_fidelity.hpp>

#include <optional>
#include <string>

using namespace pulp::view;

// ── Image/sprite no-skew invariant ──────────────────────────────────────────
namespace {
IRNode make_image_node(bool bleed_via_render_bounds, bool asset_bleed,
                       float png_w, float png_h) {
    IRNode n;
    n.type = "image";
    n.name = "Sprite";
    n.style.width = 100.0f;
    n.style.height = 100.0f;
    if (bleed_via_render_bounds)
        n.style.render_bounds = IRStyle::RenderBounds{200.0f, 200.0f, 0.0f, 0.0f};
    if (asset_bleed) n.attributes["asset_bleed"] = "1";
    if (png_w > 0.0f) n.attributes["png_natural_w"] = std::to_string((int)png_w);
    if (png_h > 0.0f) n.attributes["png_natural_h"] = std::to_string((int)png_h);
    return n;
}
}  // namespace

TEST_CASE("fidelity self-check passes when a bleed sprite preserves its aspect",
          "[view][import][fidelity][harness]") {
    // png 200x100 (aspect 2.0); emitted 120x60 (aspect 2.0) → no finding.
    const auto n = make_image_node(/*render_bounds*/true, /*asset_bleed*/false, 200, 100);
    CHECK_FALSE(check_image_sizing_fidelity({n, "Sprite0", 120.0f, 60.0f}).has_value());
}

TEST_CASE("fidelity self-check flags a skewed bleed sprite",
          "[view][import][fidelity][harness]") {
    // png 200x100 (aspect 2.0); emitted 100x100 (aspect 1.0) → skew.
    const auto n = make_image_node(true, false, 200, 100);
    const auto issue = check_image_sizing_fidelity({n, "Sprite0", 100.0f, 100.0f});
    REQUIRE(issue.has_value());
    CHECK(issue->kind == "skew");
    CHECK(issue->node_id == "Sprite0");
}

TEST_CASE("fidelity self-check flags an asset_bleed sprite missing PNG dims",
          "[view][import][fidelity][harness]") {
    const auto n = make_image_node(/*render_bounds*/false, /*asset_bleed*/true, 0, 0);
    const auto issue = check_image_sizing_fidelity({n, "Sprite0", 100.0f, 100.0f});
    REQUIRE(issue.has_value());
    CHECK(issue->kind == "aspect-unverified");
}

TEST_CASE("fidelity self-check ignores ordinary (non-bleed) images",
          "[view][import][fidelity][harness]") {
    // No render_bounds, no asset_bleed: filling the box is intentional, never
    // a finding even when the emitted aspect differs from the PNG.
    const auto n = make_image_node(false, false, 200, 100);
    CHECK_FALSE(check_image_sizing_fidelity({n, "Sprite0", 100.0f, 100.0f}).has_value());
}

TEST_CASE("fidelity self-check flags an asset_bleed sprite that skews",
          "[view][import][fidelity][harness]") {
    const auto n = make_image_node(false, true, 200, 100);  // aspect 2.0
    const auto issue = check_image_sizing_fidelity({n, "Sprite0", 100.0f, 100.0f});  // aspect 1.0
    REQUIRE(issue.has_value());
    CHECK(issue->kind == "skew");
}

// ── Gross-size divergence invariant (any node, any source) ──────────────────
namespace {
IRNode make_sized_node(float src_w, float src_h,
                       SizingMode wmode = SizingMode::fixed,
                       SizingMode hmode = SizingMode::fixed) {
    IRNode n;
    n.type = "frame";
    n.name = "Box";
    n.style.width = src_w;
    n.style.height = src_h;
    n.layout.width_mode = wmode;
    n.layout.height_mode = hmode;
    return n;
}
}  // namespace

TEST_CASE("gross-size check passes within tolerance",
          "[view][import][fidelity][harness]") {
    const auto n = make_sized_node(100.0f, 100.0f);
    CHECK_FALSE(check_gross_size_divergence({n, "Box0", 120.0f, 120.0f}).has_value());
}

TEST_CASE("gross-size check flags a width blow-out on a fixed node",
          "[view][import][fidelity][harness]") {
    const auto n = make_sized_node(100.0f, 100.0f);
    const auto issue = check_gross_size_divergence({n, "Box0", 400.0f, 100.0f});
    REQUIRE(issue.has_value());
    CHECK(issue->kind == "gross-size");
    CHECK(issue->node_id == "Box0");
    CHECK(issue->detail.find("400") != std::string::npos);
}

TEST_CASE("gross-size check skips hug/fill axes (flex intent, never flagged)",
          "[view][import][fidelity][harness]") {
    const auto hug = make_sized_node(100.0f, 100.0f,
                                     SizingMode::fixed, SizingMode::hug);
    CHECK_FALSE(check_gross_size_divergence({hug, "Box0", 100.0f, 900.0f}).has_value());
    const auto fill = make_sized_node(100.0f, 100.0f,
                                      SizingMode::fill, SizingMode::fixed);
    CHECK_FALSE(check_gross_size_divergence({fill, "Box0", 900.0f, 100.0f}).has_value());
}

TEST_CASE("gross-size check skips absolute and display:none nodes",
          "[view][import][fidelity][harness]") {
    auto abs_node = make_sized_node(100.0f, 100.0f);
    abs_node.style.position = "absolute";
    CHECK_FALSE(check_gross_size_divergence({abs_node, "Box0", 900.0f, 900.0f}).has_value());

    auto hidden = make_sized_node(100.0f, 100.0f);
    hidden.layout.display = "none";
    CHECK_FALSE(check_gross_size_divergence({hidden, "Box0", 900.0f, 900.0f}).has_value());
}

TEST_CASE("gross-size check ignores the exact-3x boundary and is source-agnostic",
          "[view][import][fidelity][harness]") {
    const auto edge = make_sized_node(100.0f, 100.0f);
    CHECK_FALSE(check_gross_size_divergence({edge, "Box0", 300.0f, 100.0f}).has_value());
    const auto bare = make_sized_node(100.0f, 100.0f);
    CHECK(check_gross_size_divergence({bare, "Box0", 301.0f, 100.0f}).has_value());
}

// ── Registry dispatch: each check runs ONLY for its element kind ─────────────
TEST_CASE("run_fidelity_checks dispatches by element kind",
          "[view][import][fidelity][harness]") {
    // image context → image-sizing check fires (skew); gross-size must NOT see
    // an image (its emitted box legitimately differs from its style box).
    const auto img = make_image_node(/*render_bounds*/true, false, 200, 100);  // aspect 2.0
    std::vector<FidelityIssue> s_img;
    run_fidelity_checks({img, "Img0", 100.0f, 100.0f, FidelityElement::image}, s_img);  // aspect 1.0
    REQUIRE(s_img.size() == 1);
    CHECK(s_img[0].kind == "skew");

    // container context → gross-size fires; the image check must NOT fire.
    const auto box = make_sized_node(100.0f, 100.0f);
    std::vector<FidelityIssue> s_box;
    run_fidelity_checks({box, "Box0", 400.0f, 100.0f, FidelityElement::container}, s_box);  // 4x
    REQUIRE(s_box.size() == 1);
    CHECK(s_box[0].kind == "gross-size");
}

// ── Widget intrinsic-size invariant ─────────────────────────────────────────
namespace {
IRNode make_widget_node(AudioWidgetType t, float src_w, float src_h,
                        bool via_shape_attr = false) {
    IRNode n;
    n.type = "frame";
    n.name = "W";
    n.audio_widget = t;
    if (via_shape_attr) {
        n.attributes["shape_width"]  = std::to_string((int)src_w);
        n.attributes["shape_height"] = std::to_string((int)src_h);
    } else {
        n.style.width = src_w;
        n.style.height = src_h;
    }
    return n;
}
}  // namespace

TEST_CASE("widget-size: emitted matching source intrinsic passes",
          "[view][import][fidelity][harness]") {
    const auto n = make_widget_node(AudioWidgetType::knob, 62, 62);
    CHECK_FALSE(check_widget_intrinsic_size(
        {n, "K0", 62.0f, 62.0f, FidelityElement::widget}).has_value());
}

TEST_CASE("widget-size: >1.5x divergence is flagged",
          "[view][import][fidelity][harness]") {
    const auto n = make_widget_node(AudioWidgetType::knob, 62, 62);
    const auto i = check_widget_intrinsic_size(
        {n, "K0", 130.0f, 62.0f, FidelityElement::widget});  // ~2.1x on W
    REQUIRE(i.has_value());
    CHECK(i->kind == "widget-size");
}

TEST_CASE("widget-size: below-native-minimum clamp-up is informational",
          "[view][import][fidelity][harness]") {
    // 20x20 knob source clamped up to the 56x56 native minimum → not a hard fail.
    const auto n = make_widget_node(AudioWidgetType::knob, 20, 20);
    const auto i = check_widget_intrinsic_size(
        {n, "K0", 56.0f, 56.0f, FidelityElement::widget});
    REQUIRE(i.has_value());
    CHECK(i->kind == "widget-undersized");
    // #3274: the clamp-up is advisory — it must carry the informational
    // flag so --strict-fidelity (which fails on hard findings only) ignores it.
    CHECK(i->informational);
}

TEST_CASE("widget-size: a real divergence is a hard (non-informational) finding",
          "[view][import][fidelity][harness]") {
    const auto n = make_widget_node(AudioWidgetType::knob, 62, 62);
    const auto i = check_widget_intrinsic_size(
        {n, "K0", 130.0f, 62.0f, FidelityElement::widget});  // ~2.1x, above native min
    REQUIRE(i.has_value());
    CHECK(i->kind == "widget-size");
    CHECK_FALSE(i->informational);
}

TEST_CASE("widget-size: reads the shape_* attribute as the source",
          "[view][import][fidelity][harness]") {
    const auto n = make_widget_node(AudioWidgetType::fader, 44, 120, /*shape*/true);
    CHECK_FALSE(check_widget_intrinsic_size(
        {n, "F0", 44.0f, 120.0f, FidelityElement::widget}).has_value());
}

TEST_CASE("widget-size: a non-widget node self-skips",
          "[view][import][fidelity][harness]") {
    IRNode n; n.type = "frame"; n.style.width = 100.0f; n.style.height = 100.0f;
    CHECK_FALSE(check_widget_intrinsic_size(
        {n, "X0", 400.0f, 100.0f, FidelityElement::widget}).has_value());
}

// ── Text vertical-centering invariant ───────────────────────────────────────
namespace {
IRNode make_text_node(float font, bool emitted_center) {
    IRNode n;
    n.type = "text";
    n.name = "T";
    n.text_content = "Search";
    n.style.font_size = font;   // line_height defaults to font*1.2
    n.attributes["_emitted_vertical_align"] = emitted_center ? "center" : "top";
    return n;
}
}  // namespace

TEST_CASE("text-vcenter: centered single-line in a tall slot passes",
          "[view][import][fidelity][harness]") {
    const auto n = make_text_node(14.0f, /*center*/true);  // line ~16.8; slot 26 has slack, single-line
    CHECK_FALSE(check_text_vertical_centering(
        {n, "T0", 0.0f, 26.0f, FidelityElement::text}).has_value());
}

TEST_CASE("text-vcenter: top-aligned single-line in a tall slot is flagged",
          "[view][import][fidelity][harness]") {
    const auto n = make_text_node(14.0f, /*center*/false);
    const auto i = check_text_vertical_centering(
        {n, "T0", 0.0f, 26.0f, FidelityElement::text});
    REQUIRE(i.has_value());
    CHECK(i->kind == "text-vcenter");
}

TEST_CASE("text-vcenter: a multi-line box self-skips",
          "[view][import][fidelity][harness]") {
    const auto n = make_text_node(14.0f, false);  // line ~16.8; 40 > 1.8*line → multi-line
    CHECK_FALSE(check_text_vertical_centering(
        {n, "T0", 0.0f, 40.0f, FidelityElement::text}).has_value());
}

TEST_CASE("text-vcenter: a box with no vertical slack self-skips",
          "[view][import][fidelity][harness]") {
    const auto n = make_text_node(14.0f, false);  // line ~16.8; 18 < 1.15*line → no slack
    CHECK_FALSE(check_text_vertical_centering(
        {n, "T0", 0.0f, 18.0f, FidelityElement::text}).has_value());
}

// ── Vector-renderability invariant (tree pass) ──────────────────────────────
namespace {
IRNode make_vector_node(const std::string& type, float w, float h) {
    IRNode n;
    n.type = type;
    n.name = "Shape";
    n.style.width = w;
    n.style.height = h;
    return n;
}
// Identity-ish id mapper for tests (codegen passes its real node→id map).
std::string vec_id_of(const IRNode& n) { return n.name.empty() ? n.type : n.name; }
const std::vector<ImportDiagnostic> kNoDiagnostics{};
}  // namespace

TEST_CASE("vector-renderability flags a bare path that produced no primitive",
          "[view][import][fidelity][harness][vector]") {
    IRNode n = make_vector_node("path", 64.0f, 48.0f);  // no asset_path, no fill, no children
    std::vector<FidelityIssue> sink;
    check_vector_renderability(n, kNoDiagnostics, vec_id_of, sink);
    REQUIRE(sink.size() == 1);
    CHECK(sink[0].kind == "dropped-vector");
    CHECK(sink[0].node_id == "Shape");
}

TEST_CASE("vector-renderability flags a stroke-only rect (border dropped by frame branch)",
          "[view][import][fidelity][harness][vector]") {
    // A bare rect carrying only a border falls to codegen's generic-frame
    // branch, which paints background_color only — the stroke silently vanishes.
    IRNode n = make_vector_node("rect", 80.0f, 40.0f);
    n.style.border_color = "#333333";
    n.style.border_width = 1.0f;
    std::vector<FidelityIssue> sink;
    check_vector_renderability(n, kNoDiagnostics, vec_id_of, sink);
    REQUIRE(sink.size() == 1);
    CHECK(sink[0].kind == "dropped-vector");
}

TEST_CASE("vector-renderability passes a rasterized vector (has asset_path)",
          "[view][import][fidelity][harness][vector]") {
    IRNode n = make_vector_node("vector", 100.0f, 100.0f);
    n.attributes["asset_path"] = "/tmp/shape.png";  // importer rasterized it → image branch
    std::vector<FidelityIssue> sink;
    check_vector_renderability(n, kNoDiagnostics, vec_id_of, sink);
    CHECK(sink.empty());
}

TEST_CASE("vector-renderability passes a filled rect (renders via frame branch)",
          "[view][import][fidelity][harness][vector]") {
    IRNode n = make_vector_node("rect", 80.0f, 40.0f);
    n.style.background_color = "#ff0000";  // solid fill renders
    std::vector<FidelityIssue> sink;
    check_vector_renderability(n, kNoDiagnostics, vec_id_of, sink);
    CHECK(sink.empty());
}

TEST_CASE("vector-renderability skips invisible / zero-area / sub-threshold nodes",
          "[view][import][fidelity][harness][vector]") {
    std::vector<FidelityIssue> sink;

    IRNode hidden = make_vector_node("path", 64.0f, 64.0f);
    hidden.style.opacity = 0.0f;                       // invisible
    check_vector_renderability(hidden, kNoDiagnostics, vec_id_of, sink);
    CHECK(sink.empty());

    IRNode none_disp = make_vector_node("ellipse", 64.0f, 64.0f);
    none_disp.layout.display = "none";                 // display:none
    check_vector_renderability(none_disp, kNoDiagnostics, vec_id_of, sink);
    CHECK(sink.empty());

    IRNode hairline = make_vector_node("line", 200.0f, 1.0f);  // area 200 < 256
    check_vector_renderability(hairline, kNoDiagnostics, vec_id_of, sink);
    CHECK(sink.empty());

    IRNode zero = make_vector_node("rect", 0.0f, 0.0f);
    check_vector_renderability(zero, kNoDiagnostics, vec_id_of, sink);
    CHECK(sink.empty());
}

TEST_CASE("vector-renderability does not double-report an already-diagnosed vector",
          "[view][import][fidelity][harness][vector]") {
    SECTION("confidence == not_impl") {
        IRNode n = make_vector_node("path", 64.0f, 64.0f);
        n.confidence = IRConfidence::not_impl;         // importer already told the user
        std::vector<FidelityIssue> sink;
        check_vector_renderability(n, kNoDiagnostics, vec_id_of, sink);
        CHECK(sink.empty());
    }
    SECTION("a render-affecting diagnostic anchors the node by structural path") {
        IRNode n = make_vector_node("path", 64.0f, 64.0f);
        ImportDiagnostic d;
        d.kind = ImportDiagnosticKind::unsupported_property;
        d.path = "$";                                  // root path of the node passed below
        std::vector<ImportDiagnostic> diags{d};
        std::vector<FidelityIssue> sink;
        check_vector_renderability(n, diags, vec_id_of, sink);
        CHECK(sink.empty());
    }
    SECTION("a render-affecting diagnostic anchors the node by stable_anchor_id") {
        IRNode n = make_vector_node("path", 64.0f, 64.0f);
        n.stable_anchor_id = "anchor-7";
        ImportDiagnostic d;
        d.kind = ImportDiagnosticKind::unresolved_asset;
        d.anchor_id = "anchor-7";
        std::vector<ImportDiagnostic> diags{d};
        std::vector<FidelityIssue> sink;
        check_vector_renderability(n, diags, vec_id_of, sink);
        CHECK(sink.empty());
    }
}

TEST_CASE("vector-renderability is a tree pass: group with renderable child passes, "
          "lone bare child is flagged",
          "[view][import][fidelity][harness][vector]") {
    IRNode group = make_vector_node("vector", 200.0f, 200.0f);
    group.name = "Group";
    IRNode raster = make_vector_node("rect", 100.0f, 100.0f);
    raster.name = "Raster";
    raster.attributes["asset_path"] = "/tmp/icon.png";  // renderable child
    group.children.push_back(raster);
    IRNode dropped = make_vector_node("path", 50.0f, 50.0f);
    dropped.name = "DroppedPath";
    group.children.push_back(dropped);

    std::vector<FidelityIssue> sink;
    check_vector_renderability(group, kNoDiagnostics, vec_id_of, sink);
    // Group passes (has children); raster child passes (asset_path); only the
    // bare path is flagged.
    REQUIRE(sink.size() == 1);
    CHECK(sink[0].node_id == "DroppedPath");
    CHECK(sink[0].kind == "dropped-vector");
}

TEST_CASE("vector-renderability does not descend into a widget subtree "
          "(consumed stroke children are not flagged)",
          "[view][import][fidelity][harness][vector]") {
    // A knob consumes its child ellipses into its native paint; codegen's widget
    // branch is terminal and never emits them, so they must not be flagged.
    IRNode knob = make_vector_node("ellipse", 56.0f, 56.0f);
    knob.name = "Knob";
    knob.audio_widget = AudioWidgetType::knob;
    IRNode stroke = make_vector_node("ellipse", 40.0f, 40.0f);  // bare, would trip in isolation
    stroke.name = "Stroke";
    knob.children.push_back(stroke);

    std::vector<FidelityIssue> sink;
    check_vector_renderability(knob, kNoDiagnostics, vec_id_of, sink);
    CHECK(sink.empty());
}

TEST_CASE("count_strict_fidelity_failures ignores informational findings",
          "[view][import][fidelity][harness]") {
    // The CLI derives its --strict-fidelity exit code from this count, so an
    // informational finding (clamp-up) must not contribute a failure.
    std::vector<FidelityIssue> issues = {
        {"a", "A", "skew", "hard", /*informational=*/false},
        {"b", "B", "widget-undersized", "advisory", /*informational=*/true},
        {"c", "C", "gross-size", "hard", /*informational=*/false},
    };
    CHECK(count_strict_fidelity_failures(issues) == 2);

    std::vector<FidelityIssue> all_info = {
        {"b", "B", "widget-undersized", "advisory", /*informational=*/true},
    };
    CHECK(count_strict_fidelity_failures(all_info) == 0);
    CHECK(count_strict_fidelity_failures({}) == 0);
}

TEST_CASE("text-vcenter: an auto-height label (n-a) in a tall slot self-skips",
          "[view][import][fidelity][harness]") {
    // #3274: when the IR carries no explicit height, codegen synthesizes
    // label_h = font*1.4 (19.6 for 14px), which lands in the tall-slot range and
    // is stamped top-aligned. Without the "n-a" guard this fires a FALSE
    // text-vcenter on every ordinary auto-height label and breaks
    // --strict-fidelity. The stamp must make it self-skip.
    IRNode n;
    n.type = "text";
    n.name = "T";
    n.text_content = "Search";
    n.style.font_size = 14.0f;
    n.attributes["_emitted_vertical_align"] = "n-a";
    CHECK_FALSE(check_text_vertical_centering(
        {n, "T0", 0.0f, 19.6f, FidelityElement::text}).has_value());
}
