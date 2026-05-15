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
