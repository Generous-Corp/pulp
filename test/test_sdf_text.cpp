// Tests for shared SDF/MSDF text helpers in <pulp/canvas/sdf_text.hpp>.
//
// These cover the pen-snap policy and the atlas-agnostic quad builder
// that underpins both `fill_text_sdf` and `fill_text_msdf` paths.

#include <pulp/canvas/msdf_atlas.hpp>
#include <pulp/canvas/sdf_atlas.hpp>
#include <pulp/canvas/sdf_text.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using pulp::canvas::MsdfAtlas;
using pulp::canvas::SdfAtlas;
using pulp::canvas::SdfPenSnap;
using pulp::canvas::SdfTextOptions;
using pulp::canvas::build_text_quads;
using pulp::canvas::snap_pen_x;
using pulp::canvas::snap_pen_y;

TEST_CASE("snap_pen_x honours the pen-snap policy", "[canvas][sdf][snap]") {
    REQUIRE(snap_pen_x(3.7f, SdfPenSnap::Free)    == 3.7f);
    REQUIRE(snap_pen_x(3.7f, SdfPenSnap::Nearest) == 4.0f);
    // SubpixelThird rounds to nearest 1/3 px. std::round(10.5) == 11 →
    // 11/3 = 3.6667; std::round(10.3) == 10 → 10/3 = 3.3333.
    REQUIRE(snap_pen_x(3.5f, SdfPenSnap::SubpixelThird)
            == Catch::Approx(3.6667f).margin(0.01f));
    REQUIRE(snap_pen_x(3.43f, SdfPenSnap::SubpixelThird)
            == Catch::Approx(3.3333f).margin(0.01f));
}

TEST_CASE("snap_pen_y always snaps to integers when not Free",
          "[canvas][sdf][snap]") {
    REQUIRE(snap_pen_y(2.3f, SdfPenSnap::Free)    == 2.3f);
    REQUIRE(snap_pen_y(2.3f, SdfPenSnap::Nearest) == 2.0f);
    REQUIRE(snap_pen_y(2.7f, SdfPenSnap::SubpixelThird) == 3.0f);
}

TEST_CASE("build_text_quads emits one quad per resolvable glyph",
          "[canvas][sdf][layout]") {
    SdfAtlas atlas;
    REQUIRE(atlas.build("", {U'A', U'B', U'C'}, 32, 4, 1024));

    auto quads = build_text_quads(atlas, std::u32string(U"ABC"),
                                  /*x*/ 10.0f, /*y*/ 50.0f,
                                  /*render_size*/ 32.0f);
    REQUIRE(quads.size() == 3);

    // Each quad's dst_x strictly advances.
    REQUIRE(quads[0].dst_x <= quads[1].dst_x);
    REQUIRE(quads[1].dst_x <= quads[2].dst_x);

    // Codepoints round-trip.
    REQUIRE(quads[0].codepoint == U'A');
    REQUIRE(quads[2].codepoint == U'C');

    // Source rects reference the atlas bounds.
    for (const auto& q : quads) {
        REQUIRE(q.src_w == 32);
        REQUIRE(q.src_h == 32);
    }
}

TEST_CASE("build_text_quads skips missing glyphs", "[canvas][sdf][layout]") {
    SdfAtlas atlas;
    REQUIRE(atlas.build("", {U'A'}, 32, 4, 1024));
    auto quads = build_text_quads(atlas, std::u32string(U"AXA"),
                                  0.0f, 0.0f, 32.0f);
    REQUIRE(quads.size() == 2);  // X is skipped.
}

TEST_CASE("build_text_quads works against MsdfAtlas (shared surface)",
          "[canvas][msdf][layout]") {
    MsdfAtlas atlas;
    REQUIRE(atlas.build("", {U'A', U'B'}, 32, 4, 1024));

    auto quads = build_text_quads(atlas, std::u32string(U"BA"),
                                  5.0f, 40.0f, 24.0f);
    REQUIRE(quads.size() == 2);
    REQUIRE(quads[0].codepoint == U'B');
    REQUIRE(quads[1].codepoint == U'A');
}
