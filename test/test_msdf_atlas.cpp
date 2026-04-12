// Tests for the MSDF glyph atlas (Phase 2 scaffold).
//
// Until msdfgen is integrated the atlas writes a placeholder radial
// gradient, so these tests validate structure (packing, metrics,
// bounds) rather than the true multi-channel signal.

#include <pulp/canvas/msdf_atlas.hpp>
#include <pulp/canvas/sdf_text.hpp>

#include <catch2/catch_test_macros.hpp>

using pulp::canvas::MsdfAtlas;
using pulp::canvas::MsdfGlyph;

TEST_CASE("MsdfAtlas packs the requested glyphs", "[canvas][msdf]") {
    MsdfAtlas atlas;
    std::vector<char32_t> chars = {U'A', U'B', U'C', U'D'};
    REQUIRE(atlas.build("ignored-for-stub", chars, 32, 4, 1024));
    REQUIRE(atlas.glyph_count() == 4);
    REQUIRE(atlas.base_size() == 32);
    REQUIRE(atlas.width()  > 0);
    REQUIRE(atlas.height() > 0);
    REQUIRE(atlas.pixels() != nullptr);

    for (auto c : chars) {
        const MsdfGlyph* g = atlas.glyph(c);
        REQUIRE(g != nullptr);
        REQUIRE(g->codepoint == c);
        REQUIRE(g->width  == 32);
        REQUIRE(g->height == 32);
    }
    REQUIRE(atlas.glyph(U'Z') == nullptr);
}

TEST_CASE("MsdfAtlas pixel buffer is RGB8 (3 bytes per texel)",
          "[canvas][msdf]") {
    MsdfAtlas atlas;
    REQUIRE(atlas.build("stub", {U'O'}, 48, 4, 256));

    const int w = atlas.width();
    const int h = atlas.height();
    // The placeholder writes equal R=G=B, so median(r,g,b) == r. A texel
    // near the centre of the glyph tile must be 'inside' (R > 200) and a
    // texel just outside the tile must be 'outside' (R < 60).
    const MsdfGlyph* g = atlas.glyph(U'O');
    REQUIRE(g != nullptr);

    auto at = [&](int x, int y) {
        return atlas.pixels()[(y * w + x) * 3];  // R channel
    };
    const int cx = g->atlas_x + g->width / 2;
    const int cy = g->atlas_y + g->height / 2;
    REQUIRE(at(cx, cy) > 200);
    REQUIRE(at(g->atlas_x - 2, g->atlas_y - 2) < 60);
    (void)h;
}

TEST_CASE("MsdfAtlas hybrid-alpha mode emits RGBA with A-channel SDF",
          "[canvas][msdf][alpha]") {
    MsdfAtlas atlas;
    std::vector<char32_t> chars = {U'A'};
    REQUIRE(atlas.build("stub", chars, 32, 4, 1024, /*include_alpha*/ true));
    REQUIRE(atlas.channels() == 4);

    const MsdfGlyph* g = atlas.glyph(U'A');
    REQUIRE(g != nullptr);

    const int w = atlas.width();
    auto at = [&](int x, int y, int c) {
        return atlas.pixels()[(y * w + x) * 4 + c];
    };
    const int cx = g->atlas_x + g->width / 2;
    const int cy = g->atlas_y + g->height / 2;
    REQUIRE(at(cx, cy, 3) > 200);
    REQUIRE(at(cx, cy, 0) > 200);
    REQUIRE(at(g->atlas_x - 2, g->atlas_y - 2, 3) < 60);
}

TEST_CASE("MsdfAtlas default mode is RGB (no alpha channel)",
          "[canvas][msdf][alpha]") {
    MsdfAtlas atlas;
    REQUIRE(atlas.build("stub", {U'X'}, 24, 4, 1024));
    REQUIRE(atlas.channels() == 3);
}

TEST_CASE("fill_text_msdf produces one quad per glyph and advances the pen",
          "[canvas][msdf][fill_text]") {
    using pulp::canvas::MsdfAtlas;
    using pulp::canvas::SdfTextOptions;
    using pulp::canvas::fill_text_msdf;
    using pulp::canvas::SdfPenSnap;

    MsdfAtlas atlas;
    REQUIRE(atlas.build("stub", {U'A', U'B', U'C'}, 32, 4, 1024));

    SdfTextOptions opts;
    opts.snap = SdfPenSnap::Nearest;
    const auto quads = fill_text_msdf(atlas, U"ABC", 10.5f, 20.0f,
                                      /*render_size*/ 32.0f, opts);
    REQUIRE(quads.size() == 3);
    // Pen x is snapped with Nearest → 11.0f on the first glyph.
    REQUIRE(quads[0].dst_x == 11.0f);
    // Subsequent glyphs advance monotonically.
    REQUIRE(quads[1].dst_x >= quads[0].dst_x);
    REQUIRE(quads[2].dst_x >= quads[1].dst_x);
    // Codepoints round-trip.
    REQUIRE(quads[0].codepoint == U'A');
    REQUIRE(quads[1].codepoint == U'B');
    REQUIRE(quads[2].codepoint == U'C');
}

TEST_CASE("MsdfAtlas refuses to build when exceeding max size",
          "[canvas][msdf]") {
    MsdfAtlas atlas;
    std::vector<char32_t> many;
    for (char32_t c = 0; c < 100; ++c) many.push_back(c);
    REQUIRE_FALSE(atlas.build("stub", many, 128, 8, 256));
    REQUIRE(atlas.glyph_count() == 0);
}
