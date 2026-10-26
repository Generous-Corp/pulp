// A DesignIR's captured @font-face faces reach the text renderer with the
// weight range each rule declared, in the native lane and the generated JS.
#include <pulp/canvas/bundled_fonts.hpp>
#include <pulp/view/design_codegen.hpp>
#include <pulp/view/design_import.hpp>
#include <pulp/view/view.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace pulp::view;

#ifndef PULP_REPO_ROOT
#error "PULP_REPO_ROOT must point at the checkout so the test can read external/fonts"
#endif

namespace {

DesignIR ir_with_face(std::string family, std::string path, int weight, int weight_max) {
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.stable_anchor_id = "root";
    ir.root.style.width = 100.0f;
    ir.root.style.height = 40.0f;
    IRFontAsset face;
    face.family = std::move(family);
    face.resolved_path = std::move(path);
    face.weight = weight;
    face.weight_max = weight_max;
    ir.font_family_assets.push_back(face);
    return ir;
}

} // namespace

TEST_CASE("the native lane registers captured @font-face faces with their declared range",
          "[view][import][native-materializer][fonts][synthetic-bold]") {
    const std::string jost = std::string(PULP_REPO_ROOT) + "/external/fonts/Jost-Regular.ttf";
    REQUIRE_FALSE(pulp::canvas::is_font_registered("NativeFaceRange"));
    auto root = build_native_view_tree(ir_with_face("NativeFaceRange", jost, 400, 700), {}, {});
    REQUIRE(root != nullptr);
    CHECK(pulp::canvas::is_font_registered("NativeFaceRange"));
    // Declared 400..700, so Chromium draws 700 from the Regular file unemboldened.
    CHECK(pulp::canvas::declared_face_synthesizes_bold("NativeFaceRange", 700) == false);

    // A single declared weight keeps Chromium's synthesis.
    build_native_view_tree(ir_with_face("NativeFaceSingle", jost, 400, 0), {}, {});
    CHECK(pulp::canvas::declared_face_synthesizes_bold("NativeFaceSingle", 700) == true);

    // A face whose file did not resolve registers nothing, and declares nothing.
    build_native_view_tree(ir_with_face("NativeFaceMissing", "/nonexistent/face.ttf", 400, 700), {},
                           {});
    CHECK_FALSE(pulp::canvas::is_font_registered("NativeFaceMissing"));
    CHECK_FALSE(pulp::canvas::declared_face_synthesizes_bold("NativeFaceMissing", 700).has_value());
}

TEST_CASE("generated JS registers a captured face with its declared weight range",
          "[view][import][codegen][fonts][synthetic-bold]") {
    CodeGenOptions opts;
    opts.mode = CodeGenMode::bridge_native_js;
    const auto range = generate_pulp_js(ir_with_face("Probe", "/fonts/Jost.ttf", 400, 700), opts);
    CHECK(range.find("registerFont('Probe', '/fonts/Jost.ttf', 400, 700);") != std::string::npos);
    const auto single = generate_pulp_js(ir_with_face("Probe", "/fonts/Jost.ttf", 400, 0), opts);
    CHECK(single.find("registerFont('Probe', '/fonts/Jost.ttf', 400, 400);") != std::string::npos);
}
