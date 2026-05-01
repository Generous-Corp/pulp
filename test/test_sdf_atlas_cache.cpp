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

TEST_CASE("SdfAtlasCache failed initialize clears prior resident glyphs",
          "[canvas][cache][issue-641]") {
    SdfAtlasCache cache;
    REQUIRE(cache.initialize("", {U'A'}, 32, 0, 32));
    REQUIRE(cache.size() == 1);
    REQUIRE(cache.touch(U'A') != nullptr);

    REQUIRE_FALSE(cache.initialize("", {U'A', U'B'}, 32, 0, 32));
    REQUIRE(cache.size() == 0);
    REQUIRE(cache.touch(U'A') == nullptr);
    REQUIRE(cache.atlas().glyph(U'A') == nullptr);
    REQUIRE(cache.atlas().pixels() == nullptr);
}

TEST_CASE("SdfAtlasCache reinitialize stamps new glyphs at current frame",
          "[canvas][cache][issue-641]") {
    SdfAtlasCache cache;
    REQUIRE(cache.initialize("", {U'A'}, 24, 4, 512));
    cache.next_frame();
    cache.next_frame();

    REQUIRE(cache.initialize("", {U'Z'}, 24, 4, 512));
    REQUIRE(cache.size() == 1);
    REQUIRE(cache.touch(U'A') == nullptr);

    const auto* z = cache.touch(U'Z');
    REQUIRE(z != nullptr);
    REQUIRE(z->frame_last_used == cache.current_frame());
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

TEST_CASE("SdfAtlasCache eviction keeps glyphs at the exact age threshold",
          "[canvas][cache][issue-641]") {
    SdfAtlasCache cache;
    REQUIRE(cache.initialize("", {U'A', U'B'}, 24, 4, 512));

    for (int i = 0; i < 5; ++i) cache.next_frame();
    REQUIRE(cache.evict_older_than(5) == 0);
    REQUIRE(cache.size() == 2);

    cache.next_frame();
    REQUIRE(cache.evict_older_than(5) == 2);
    REQUIRE(cache.size() == 0);
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

TEST_CASE("SdfAtlasCache ensure failure preserves existing cache state",
          "[canvas][cache][issue-641]") {
    SdfAtlasCache cache;
    REQUIRE(cache.initialize("", {U'A'}, 32, 0, 32));
    REQUIRE(cache.size() == 1);

    REQUIRE_FALSE(cache.ensure(U'B'));
    REQUIRE(cache.size() == 1);
    REQUIRE(cache.touch(U'A') != nullptr);
    REQUIRE(cache.touch(U'B') == nullptr);
    REQUIRE(cache.atlas().glyph(U'A') != nullptr);
    REQUIRE(cache.atlas().glyph(U'B') == nullptr);
}

TEST_CASE("SdfAtlasCache ensure rebuild preserves glyph recency",
          "[canvas][cache][issue-641]") {
    SdfAtlasCache cache;
    REQUIRE(cache.initialize("", {U'A'}, 24, 4, 512));

    cache.next_frame();
    cache.next_frame();
    const auto* a_before = cache.touch(U'A');
    REQUIRE(a_before != nullptr);
    REQUIRE(a_before->frame_last_used == 2);

    cache.next_frame();
    cache.next_frame();
    cache.next_frame();
    REQUIRE(cache.ensure(U'X'));

    REQUIRE(cache.evict_older_than(2) == 1);
    REQUIRE(cache.touch(U'A') == nullptr);

    const auto* x = cache.touch(U'X');
    REQUIRE(x != nullptr);
    REQUIRE(x->frame_last_used == 5);
}
