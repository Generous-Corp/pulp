// Tests for the SDF glyph atlas exploration prototype (#76).
//
// These tests exercise the API and the distance transform without
// depending on any particular font being installed. The placeholder
// rasterizer in sdf_atlas.cpp draws a circle for each codepoint, so
// the tests can validate that:
//   - the atlas builds at the expected size
//   - every requested glyph is found
//   - the SDF data is monotonic away from the edge
//   - the value at the centre of an "inside" glyph is high (close to 255)
//   - the value at a far "outside" texel is low (close to 0)

#include <pulp/canvas/sdf_atlas.hpp>

#include <catch2/catch_test_macros.hpp>

using pulp::canvas::SdfAtlas;
using pulp::canvas::SdfGlyph;

TEST_CASE("SdfAtlas builds with the requested glyphs", "[canvas][sdf]") {
    SdfAtlas atlas;
    std::vector<char32_t> chars = {U'A', U'B', U'C', U'D'};
    REQUIRE(atlas.build("ignored-for-stub", chars, 32, 4, 1024));
    REQUIRE(atlas.glyph_count() == 4);
    REQUIRE(atlas.base_size() == 32);
    REQUIRE(atlas.width()  > 0);
    REQUIRE(atlas.height() > 0);
    REQUIRE(atlas.pixels() != nullptr);

    for (auto c : chars) {
        const SdfGlyph* g = atlas.glyph(c);
        REQUIRE(g != nullptr);
        REQUIRE(g->codepoint == c);
        REQUIRE(g->width  == 32);
        REQUIRE(g->height == 32);
    }
    REQUIRE(atlas.glyph(U'Z') == nullptr);
}

TEST_CASE("SdfAtlas SDF values: inside high, outside low", "[canvas][sdf]") {
    SdfAtlas atlas;
    REQUIRE(atlas.build("stub", {U'O'}, 64, 8, 256));

    const SdfGlyph* g = atlas.glyph(U'O');
    REQUIRE(g != nullptr);

    // The placeholder rasterizer draws a circle centred in the tile,
    // so the centre of the glyph tile should be inside (high SDF
    // value, > 200), and a corner texel near the tile edge should be
    // outside (low SDF value, < 60).
    int cx = g->atlas_x + g->width / 2;
    int cy = g->atlas_y + g->height / 2;
    auto sample = [&](int x, int y) {
        return atlas.pixels()[y * atlas.width() + x];
    };
    int center = sample(cx, cy);
    int corner = sample(g->atlas_x - 4, g->atlas_y - 4);  // outside the glyph

    REQUIRE(center > 200);
    REQUIRE(corner < 60);
}

TEST_CASE("SdfAtlas SDF gradient is monotonic from centre outward",
          "[canvas][sdf]") {
    SdfAtlas atlas;
    REQUIRE(atlas.build("stub", {U'M'}, 48, 6, 256));

    const SdfGlyph* g = atlas.glyph(U'M');
    REQUIRE(g != nullptr);

    int cx = g->atlas_x + g->width / 2;
    int cy = g->atlas_y + g->height / 2;
    auto sample = [&](int x, int y) {
        return atlas.pixels()[y * atlas.width() + x];
    };

    // Walk a horizontal ray from the centre toward the right edge of
    // the tile and verify the SDF value never increases (it should
    // decrease as we leave the inside of the circle and head outward).
    int prev = sample(cx, cy);
    for (int dx = 1; dx < g->width / 2; ++dx) {
        int v = sample(cx + dx, cy);
        // Allow tiny aliasing wiggles of +/- 2 lsb between adjacent
        // texels (the placeholder shape edge is rasterized binarily).
        REQUIRE(v <= prev + 2);
        prev = v;
    }
}

TEST_CASE("SdfAtlas refuses to build when atlas would exceed max size",
          "[canvas][sdf]") {
    SdfAtlas atlas;
    std::vector<char32_t> many;
    for (char32_t c = 0; c < 100; ++c) many.push_back(c);
    // 100 glyphs at base_size=128 + padding=8 = 144 px tile.
    // 100 tiles needs ~12 columns × 9 rows = 1728 × 1296 — exceeds 256.
    REQUIRE_FALSE(atlas.build("stub", many, 128, 8, 256));
    REQUIRE(atlas.glyph_count() == 0);
}
