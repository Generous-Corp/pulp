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
