// Tests for PSDF atlas + vector-fallback policy (Phase 3 scaffold).
//
// Today PsdfAtlas inherits SdfAtlas' generator; these tests verify the
// surface is correct and the fallback threshold behaves as documented.

#include <pulp/canvas/psdf_atlas.hpp>

#include <catch2/catch_test_macros.hpp>

using pulp::canvas::PsdfAtlas;
using pulp::canvas::should_use_vector_fallback;

TEST_CASE("PsdfAtlas builds with the requested glyphs", "[canvas][psdf]") {
    PsdfAtlas atlas;
    std::vector<char32_t> chars = {U'A', U'B', U'C'};
    REQUIRE(atlas.build("", chars, 32, 4, 512));
    REQUIRE(atlas.glyph_count() == 3);
    REQUIRE(atlas.pixels() != nullptr);
}

TEST_CASE("PsdfAtlas keeps the SdfAtlas lookup and move surface",
          "[canvas][psdf][coverage][issue-650]") {
    PsdfAtlas atlas;
    REQUIRE(atlas.build("stub", {U'P', U'S'}, 24, 3, 256));
    REQUIRE(atlas.base_size() == 24);
    REQUIRE(atlas.glyph(U'P') != nullptr);
    REQUIRE(atlas.glyph(U'X') == nullptr);

    PsdfAtlas moved(std::move(atlas));
    REQUIRE(moved.glyph_count() == 2);
    REQUIRE(moved.pixels() != nullptr);
    REQUIRE(moved.glyph(U'S') != nullptr);
}

TEST_CASE("PsdfAtlas default and invalid builds leave atlas empty",
          "[canvas][psdf][coverage][phase3]") {
    PsdfAtlas atlas;
    REQUIRE(atlas.glyph_count() == 0);
    REQUIRE(atlas.base_size() == 0);
    REQUIRE(atlas.width() == 0);
    REQUIRE(atlas.height() == 0);
    REQUIRE(atlas.pixels() == nullptr);
    REQUIRE(atlas.glyph(U'P') == nullptr);

    REQUIRE_FALSE(atlas.build("stub", {}, 32, 4, 256));
    REQUIRE_FALSE(atlas.build("stub", {U'P'}, 0, 4, 256));
    REQUIRE_FALSE(atlas.build("stub", {U'P'}, 32, -1, 256));
    REQUIRE(atlas.glyph_count() == 0);
    REQUIRE(atlas.pixels() == nullptr);
}

TEST_CASE("PsdfAtlas packs glyph tiles and preserves metrics",
          "[canvas][psdf][coverage][phase3]") {
    PsdfAtlas atlas;
    REQUIRE(atlas.build("stub", {U'P', U'S', U'D'}, 12, 2, 32));
    REQUIRE(atlas.width() == 32);
    REQUIRE(atlas.height() == 32);

    const auto* p = atlas.glyph(U'P');
    const auto* s = atlas.glyph(U'S');
    const auto* d = atlas.glyph(U'D');
    REQUIRE(p != nullptr);
    REQUIRE(s != nullptr);
    REQUIRE(d != nullptr);
    REQUIRE(p->atlas_x == 2);
    REQUIRE(s->atlas_x == 18);
    REQUIRE(d->atlas_x == 2);
    REQUIRE(d->atlas_y == 18);
    REQUIRE(p->width == 12);
    REQUIRE(s->height == 12);
    REQUIRE(d->advance > 0.0f);
}

TEST_CASE("PsdfAtlas de-duplicates and can be rebuilt",
          "[canvas][psdf][coverage][phase3]") {
    PsdfAtlas atlas;
    REQUIRE(atlas.build("stub", {U'P', U'P', U'S'}, 16, 2, 128));
    REQUIRE(atlas.glyph_count() == 2);
    REQUIRE(atlas.glyph(U'P') != nullptr);
    REQUIRE(atlas.glyph(U'S') != nullptr);

    REQUIRE(atlas.build("stub", {U'D'}, 20, 1, 64));
    REQUIRE(atlas.glyph_count() == 1);
    REQUIRE(atlas.base_size() == 20);
    REQUIRE(atlas.glyph(U'D') != nullptr);
    REQUIRE(atlas.glyph(U'P') == nullptr);
    REQUIRE(atlas.pixels() != nullptr);
}

TEST_CASE("PsdfAtlas move assignment transfers the built atlas",
          "[canvas][psdf][coverage][phase3]") {
    PsdfAtlas source;
    PsdfAtlas destination;
    REQUIRE(source.build("stub", {U'A', U'B'}, 18, 2, 128));
    REQUIRE(destination.build("stub", {U'Z'}, 10, 1, 64));

    destination = std::move(source);
    REQUIRE(destination.glyph_count() == 2);
    REQUIRE(destination.base_size() == 18);
    REQUIRE(destination.glyph(U'A') != nullptr);
    REQUIRE(destination.glyph(U'B') != nullptr);
    REQUIRE(destination.glyph(U'Z') == nullptr);
    REQUIRE(destination.pixels() != nullptr);
}

TEST_CASE("vector_fallback threshold picks SDF vs path rendering",
          "[canvas][psdf][fallback]") {
    // base_size 48 → 1x at 48px, 4x at 192px, 10x at 480px.
    REQUIRE_FALSE(should_use_vector_fallback(48.0f,  48.0f));
    REQUIRE_FALSE(should_use_vector_fallback(192.0f, 48.0f));
    REQUIRE      (should_use_vector_fallback(480.0f, 48.0f));
    REQUIRE_FALSE(should_use_vector_fallback(384.0f, 48.0f));

    // Custom threshold.
    REQUIRE      (should_use_vector_fallback(200.0f, 48.0f, 2.0f));
    REQUIRE_FALSE(should_use_vector_fallback(96.0f, 48.0f, 2.0f));
    REQUIRE_FALSE(should_use_vector_fallback(50.0f,  48.0f, 2.0f));

    // Degenerate base_size is never a fallback trigger.
    REQUIRE_FALSE(should_use_vector_fallback(100.0f, 0.0f));
    REQUIRE_FALSE(should_use_vector_fallback(-100.0f, 48.0f));
}

TEST_CASE("vector_fallback equality and negative thresholds are strict",
          "[canvas][psdf][fallback][coverage][issue-650]") {
    REQUIRE_FALSE(should_use_vector_fallback(96.0f, 48.0f, 2.0f));
    REQUIRE(should_use_vector_fallback(96.1f, 48.0f, 2.0f));
    REQUIRE(should_use_vector_fallback(1.0f, 48.0f, -1.0f));
    REQUIRE_FALSE(should_use_vector_fallback(1.0f, -48.0f, -1.0f));
}

TEST_CASE("vector_fallback scales render size against the selected base",
          "[canvas][psdf][fallback][coverage][phase3]") {
    REQUIRE_FALSE(should_use_vector_fallback(63.9f, 16.0f, 4.0f));
    REQUIRE_FALSE(should_use_vector_fallback(64.0f, 16.0f, 4.0f));
    REQUIRE(should_use_vector_fallback(64.1f, 16.0f, 4.0f));
    REQUIRE_FALSE(should_use_vector_fallback(7.9f, 1.0f));
    REQUIRE_FALSE(should_use_vector_fallback(8.0f, 1.0f));
    REQUIRE(should_use_vector_fallback(8.1f, 1.0f));
}
