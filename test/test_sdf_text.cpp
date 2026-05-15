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
using pulp::canvas::fill_text_msdf;
using pulp::canvas::fill_text_psdf;
using pulp::canvas::fill_text_sdf;
using pulp::canvas::snap_pen_x;
using pulp::canvas::snap_pen_y;

namespace {
struct FakeGlyph {
    int atlas_x = 0;
    int atlas_y = 0;
    int width = 0;
    int height = 0;
    float bearing_x = 0.0f;
    float bearing_y = 0.0f;
    float advance = 0.0f;
};

struct FakeAtlas {
    int base = 10;
    FakeGlyph glyph_a{3, 5, 5, 6, 0.25f, 3.0f, 4.0f};

    int base_size() const { return base; }
    const FakeGlyph* glyph(char32_t c) const { return c == U'A' ? &glyph_a : nullptr; }
};
}  // namespace

TEST_CASE("SdfTextOptions default to shader contract values",
          "[canvas][sdf][layout][coverage][issue-650]") {
    SdfTextOptions opts;
    REQUIRE(opts.edge == Catch::Approx(0.5f));
    REQUIRE(opts.softness == 0.0f);
    REQUIRE(opts.mip_bias == 0.0f);
    REQUIRE(opts.gamma == Catch::Approx(2.2f));
    REQUIRE(opts.snap == SdfPenSnap::Free);
}

TEST_CASE("snap_pen_x honours the pen-snap policy", "[canvas][sdf][snap]") {
    REQUIRE(snap_pen_x(3.7f, SdfPenSnap::Free)    == 3.7f);
    REQUIRE(snap_pen_x(3.7f, SdfPenSnap::Nearest) == 4.0f);
    // SubpixelThird rounds to nearest 1/3 px. std::round(10.5) == 11 →
    // 11/3 = 3.6667; std::round(10.3) == 10 → 10/3 = 3.3333.
    REQUIRE(snap_pen_x(3.5f, SdfPenSnap::SubpixelThird)
            == Catch::Approx(3.6667f).margin(0.01f));
    REQUIRE(snap_pen_x(3.43f, SdfPenSnap::SubpixelThird)
            == Catch::Approx(3.3333f).margin(0.01f));
    REQUIRE(snap_pen_x(-1.6f, SdfPenSnap::Nearest) == -2.0f);
    REQUIRE(snap_pen_x(-1.4f, SdfPenSnap::SubpixelThird)
            == Catch::Approx(-1.3333f).margin(0.01f));
}

TEST_CASE("snap_pen_y always snaps to integers when not Free",
          "[canvas][sdf][snap]") {
    REQUIRE(snap_pen_y(2.3f, SdfPenSnap::Free)    == 2.3f);
    REQUIRE(snap_pen_y(2.3f, SdfPenSnap::Nearest) == 2.0f);
    REQUIRE(snap_pen_y(2.7f, SdfPenSnap::SubpixelThird) == 3.0f);
    REQUIRE(snap_pen_y(-2.7f, SdfPenSnap::Nearest) == -3.0f);
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

TEST_CASE("build_text_quads handles empty text and zero render size",
          "[canvas][sdf][layout][coverage][issue-650]") {
    FakeAtlas atlas;
    REQUIRE(build_text_quads(atlas, std::u32string(), 10.0f, 20.0f, 12.0f).empty());

    auto zero = build_text_quads(atlas, std::u32string(U"A"),
                                 10.0f, 20.0f, 0.0f);
    REQUIRE(zero.size() == 1);
    REQUIRE(zero[0].dst_x == 10.0f);
    REQUIRE(zero[0].dst_y == 20.0f);
    REQUIRE(zero[0].dst_w == 0.0f);
    REQUIRE(zero[0].dst_h == 0.0f);
}

TEST_CASE("build_text_quads returns empty when atlas base size is invalid",
          "[canvas][sdf][layout][issue-641]") {
    FakeAtlas atlas;
    atlas.base = 0;
    REQUIRE(build_text_quads(atlas, std::u32string(U"A"), 1.0f, 2.0f, 10.0f).empty());
}

TEST_CASE("build_text_quads applies snapping and scaled glyph metrics",
          "[canvas][sdf][layout][issue-641]") {
    FakeAtlas atlas;
    SdfTextOptions opts;
    opts.snap = SdfPenSnap::Nearest;

    auto quads = build_text_quads(atlas, std::u32string(U"AZ"),
                                  1.6f, 9.4f, 20.0f, opts);
    REQUIRE(quads.size() == 1);

    const auto& q = quads[0];
    REQUIRE(q.codepoint == U'A');
    REQUIRE(q.dst_x == 3.0f);
    REQUIRE(q.dst_y == 3.0f);
    REQUIRE(q.dst_w == 10.0f);
    REQUIRE(q.dst_h == 12.0f);
    REQUIRE(q.src_x == 3.0f);
    REQUIRE(q.src_y == 5.0f);
    REQUIRE(q.src_w == 5.0f);
    REQUIRE(q.src_h == 6.0f);
}

TEST_CASE("named SDF text wrappers forward to shared quad builder",
          "[canvas][sdf][layout][issue-641]") {
    FakeAtlas atlas;

    auto sdf = fill_text_sdf(atlas, std::u32string(U"A"), 0.0f, 0.0f, 10.0f);
    auto msdf = fill_text_msdf(atlas, std::u32string(U"A"), 0.0f, 0.0f, 10.0f);
    auto psdf = fill_text_psdf(atlas, std::u32string(U"A"), 0.0f, 0.0f, 10.0f);

    REQUIRE(sdf.size() == 1);
    REQUIRE(msdf.size() == 1);
    REQUIRE(psdf.size() == 1);
    REQUIRE(sdf[0].codepoint == U'A');
    REQUIRE(msdf[0].codepoint == U'A');
    REQUIRE(psdf[0].codepoint == U'A');
}

TEST_CASE("named SDF text wrappers forward options consistently",
          "[canvas][sdf][layout][coverage][issue-650]") {
    FakeAtlas atlas;
    SdfTextOptions opts;
    opts.snap = SdfPenSnap::Nearest;

    const auto sdf = fill_text_sdf(atlas, std::u32string(U"A"),
                                   1.6f, 2.4f, 10.0f, opts);
    const auto msdf = fill_text_msdf(atlas, std::u32string(U"A"),
                                     1.6f, 2.4f, 10.0f, opts);
    const auto psdf = fill_text_psdf(atlas, std::u32string(U"A"),
                                     1.6f, 2.4f, 10.0f, opts);

    REQUIRE(sdf.size() == 1);
    REQUIRE(msdf.size() == 1);
    REQUIRE(psdf.size() == 1);
    REQUIRE(sdf[0].dst_x == msdf[0].dst_x);
    REQUIRE(msdf[0].dst_x == psdf[0].dst_x);
    REQUIRE(sdf[0].dst_y == psdf[0].dst_y);
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
