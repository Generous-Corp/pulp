#include <pulp/canvas/sdf_atlas_cache.hpp>

#include <catch2/catch_test_macros.hpp>

using pulp::canvas::SdfAtlasCache;

TEST_CASE("SdfAtlasCache initializes with a seed glyph set", "[canvas][cache]") {
    SdfAtlasCache cache;
    REQUIRE(cache.initialize("", {U'A', U'B', U'C'}, 32, 4, 1024));
    REQUIRE(cache.size() == 3);
    REQUIRE(cache.touch(U'A') != nullptr);
    REQUIRE(cache.touch(U'Z') == nullptr);
}

TEST_CASE("SdfAtlasCache advances frame counter and records recency",
          "[canvas][cache]") {
    SdfAtlasCache cache;
    REQUIRE(cache.initialize("", {U'A', U'B'}, 24, 4, 512));
    REQUIRE(cache.current_frame() == 0);

    cache.next_frame();                    // frame 1
    cache.next_frame();                    // frame 2
    REQUIRE(cache.current_frame() == 2);

    const auto* a = cache.touch(U'A');     // used at frame 2
    REQUIRE(a != nullptr);
    REQUIRE(a->frame_last_used == 2);
}

TEST_CASE("SdfAtlasCache evicts glyphs unused beyond max_age",
          "[canvas][cache]") {
    SdfAtlasCache cache;
    REQUIRE(cache.initialize("", {U'A', U'B', U'C'}, 24, 4, 512));

    // Advance 10 frames, then touch only B.
    for (int i = 0; i < 10; ++i) cache.next_frame();
    cache.touch(U'B');

    const auto removed = cache.evict_older_than(/*max_age_frames*/ 5);
    // A and C haven't been touched since frame 0, so both evict at
    // frame 10 (age = 10 > 5). B was touched at frame 10, age = 0.
    REQUIRE(removed == 2);
    REQUIRE(cache.size() == 1);
    REQUIRE(cache.touch(U'B') != nullptr);
    REQUIRE(cache.touch(U'A') == nullptr);
}

TEST_CASE("SdfAtlasCache ensure grows the atlas on demand",
          "[canvas][cache][phase5]") {
    SdfAtlasCache cache;
    REQUIRE(cache.initialize("", {U'A', U'B'}, 24, 4, 512));
    REQUIRE(cache.size() == 2);
    REQUIRE(cache.touch(U'X') == nullptr);  // miss before ensure

    REQUIRE(cache.ensure(U'X'));
    REQUIRE(cache.size() == 3);
    const auto* x = cache.touch(U'X');
    REQUIRE(x != nullptr);
    REQUIRE(x->dirty);  // newly-added glyph needs GPU upload

    // Calling ensure on an already-resident glyph is a no-op and still
    // reports success.
    REQUIRE(cache.ensure(U'A'));
    REQUIRE(cache.size() == 3);
}
