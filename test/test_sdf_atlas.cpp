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
#include <pulp/canvas/sdf_text.hpp>

#include <catch2/catch_test_macros.hpp>

using pulp::canvas::SdfAtlas;
using pulp::canvas::SdfGlyph;

TEST_CASE("SdfAtlas default state is empty", "[canvas][sdf][issue-641]") {
    SdfAtlas atlas;
    REQUIRE(atlas.glyph_count() == 0);
    REQUIRE(atlas.base_size() == 0);
    REQUIRE(atlas.width() == 0);
    REQUIRE(atlas.height() == 0);
    REQUIRE(atlas.pixels() == nullptr);
    REQUIRE(atlas.glyph(U'A') == nullptr);
}

TEST_CASE("SdfAtlas move operations transfer atlas storage",
          "[canvas][sdf][coverage][issue-650]") {
    SdfAtlas original;
    REQUIRE(original.build("stub", {U'A', U'B'}, 20, 2, 128));
    const auto* before = original.glyph(U'B');
    REQUIRE(before != nullptr);

    SdfAtlas moved(std::move(original));
    REQUIRE(moved.glyph_count() == 2);
    REQUIRE(moved.base_size() == 20);
    REQUIRE(moved.pixels() != nullptr);
    REQUIRE(moved.glyph(U'B') != nullptr);
    REQUIRE(moved.glyph(U'B')->atlas_x == before->atlas_x);

    SdfAtlas assigned;
    assigned = std::move(moved);
    REQUIRE(assigned.glyph_count() == 2);
    REQUIRE(assigned.pixels() != nullptr);
    REQUIRE(assigned.glyph(U'A') != nullptr);
    REQUIRE(assigned.glyph(U'Z') == nullptr);
}

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

TEST_CASE("SdfAtlas rejects invalid build arguments without allocation",
          "[canvas][sdf][issue-641]") {
    SdfAtlas empty_chars;
    REQUIRE_FALSE(empty_chars.build("stub", {}, 32, 4, 256));
    REQUIRE(empty_chars.glyph_count() == 0);
    REQUIRE(empty_chars.pixels() == nullptr);

    SdfAtlas invalid_size;
    REQUIRE_FALSE(invalid_size.build("stub", {U'A'}, 0, 4, 256));
    REQUIRE(invalid_size.glyph_count() == 0);
    REQUIRE(invalid_size.pixels() == nullptr);

    SdfAtlas invalid_padding;
    REQUIRE_FALSE(invalid_padding.build("stub", {U'A'}, 32, -1, 256));
    REQUIRE(invalid_padding.glyph_count() == 0);
    REQUIRE(invalid_padding.pixels() == nullptr);
}

TEST_CASE("SdfAtlas invalid rebuild preserves prior atlas but overflow clears it",
          "[canvas][sdf][coverage][issue-650]") {
    SdfAtlas atlas;
    REQUIRE(atlas.build("stub", {U'A'}, 16, 2, 128));
    REQUIRE(atlas.glyph_count() == 1);
    REQUIRE(atlas.glyph(U'A') != nullptr);

    REQUIRE_FALSE(atlas.build("stub", {}, 16, 2, 128));
    REQUIRE(atlas.glyph_count() == 1);
    REQUIRE(atlas.glyph(U'A') != nullptr);

    std::vector<char32_t> many;
    for (char32_t c = 0; c < 100; ++c) many.push_back(c);
    REQUIRE_FALSE(atlas.build("stub", many, 128, 8, 256));
    REQUIRE(atlas.glyph_count() == 0);
    REQUIRE(atlas.pixels() == nullptr);
}

TEST_CASE("SdfAtlas packs glyphs into deterministic tile coordinates",
          "[canvas][sdf][issue-641]") {
    SdfAtlas atlas;
    REQUIRE(atlas.build("stub", {U'A', U'B', U'C'}, 10, 1, 24));
    REQUIRE(atlas.width() == 24);
    REQUIRE(atlas.height() == 24);

    const SdfGlyph* a = atlas.glyph(U'A');
    const SdfGlyph* b = atlas.glyph(U'B');
    const SdfGlyph* c = atlas.glyph(U'C');
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(c != nullptr);

    REQUIRE(a->atlas_x == 1);
    REQUIRE(a->atlas_y == 1);
    REQUIRE(b->atlas_x == 13);
    REQUIRE(b->atlas_y == 1);
    REQUIRE(c->atlas_x == 1);
    REQUIRE(c->atlas_y == 13);
}

TEST_CASE("SdfAtlas duplicate codepoints share one lookup entry",
          "[canvas][sdf][issue-641]") {
    SdfAtlas atlas;
    REQUIRE(atlas.build("stub", {U'A', U'A'}, 16, 2, 128));
    REQUIRE(atlas.glyph_count() == 1);
    REQUIRE(atlas.glyph(U'A') != nullptr);
}

TEST_CASE("SdfAtlas SDF values: inside high, outside low", "[canvas][sdf][!mayfail]") {
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
          "[canvas][sdf][!mayfail]") {
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

TEST_CASE("SdfAtlas captures real glyph metrics when Skia is available",
          "[canvas][sdf][metrics]") {
    SdfAtlas atlas;
    std::vector<char32_t> chars = {U'i', U'M', U' '};
    REQUIRE(atlas.build("", chars, 48, 6, 1024));

    const SdfGlyph* gi = atlas.glyph(U'i');
    const SdfGlyph* gM = atlas.glyph(U'M');
    const SdfGlyph* gsp = atlas.glyph(U' ');
    REQUIRE(gi != nullptr);
    REQUIRE(gM != nullptr);
    REQUIRE(gsp != nullptr);

    // All advances must be positive in both real and fallback paths.
    REQUIRE(gi->advance > 0.0f);
    REQUIRE(gsp->advance > 0.0f);
    REQUIRE(gM->advance >= gi->advance);

    // When Skia actually resolved a typeface the advances of 'i' and 'M'
    // diverge and bearings are non-trivial. When the test environment has
    // no default font SdfAtlas fills metrics with the base_size fallback
    // (advance == 48, bearing_y == 48) — that case is still covered by
    // the non-regressive assertions above. Only validate the "real" path
    // when we can see it was taken.
    const bool real_metrics = gM->advance != gi->advance;
    if (real_metrics) {
        REQUIRE(gM->advance > gi->advance);
        REQUIRE(gM->bearing_y >  0.0f);
        REQUIRE(gM->bearing_y <= 96.0f);
    }
}

TEST_CASE("SDF pen snapping policies produce expected positions",
          "[canvas][sdf][subpixel]") {
    using pulp::canvas::SdfPenSnap;
    using pulp::canvas::snap_pen_x;
    using pulp::canvas::snap_pen_y;

    // Free: passthrough.
    REQUIRE(snap_pen_x(10.37f, SdfPenSnap::Free) == 10.37f);
    REQUIRE(snap_pen_y(10.37f, SdfPenSnap::Free) == 10.37f);

    // Nearest: integer rounding.
    REQUIRE(snap_pen_x(10.49f, SdfPenSnap::Nearest) == 10.0f);
    REQUIRE(snap_pen_x(10.51f, SdfPenSnap::Nearest) == 11.0f);
    REQUIRE(snap_pen_y(10.51f, SdfPenSnap::Nearest) == 11.0f);

    // SubpixelThird: round to nearest 1/3 on x, integer on y.
    const float t1 = snap_pen_x(10.10f, SdfPenSnap::SubpixelThird);
    const float t2 = snap_pen_x(10.40f, SdfPenSnap::SubpixelThird);
    const float t3 = snap_pen_x(10.80f, SdfPenSnap::SubpixelThird);
    REQUIRE(std::abs(t1 - 10.0f) < 1e-4f);
    REQUIRE(std::abs(t2 - (10.0f + 1.0f / 3.0f)) < 1e-4f);
    REQUIRE(std::abs(t3 - (10.0f + 2.0f / 3.0f)) < 1e-4f);
    REQUIRE(snap_pen_y(10.80f, SdfPenSnap::SubpixelThird) == 11.0f);
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
