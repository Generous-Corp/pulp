#include <catch2/catch_test_macros.hpp>
#include <pulp/render/draw_batcher.hpp>

using namespace pulp::render;

TEST_CASE("DrawBatcher basic batching", "[render][batcher]") {
    DrawBatcher b;
    b.begin();
    // Two non-overlapping draws with same key
    b.record({0, 0, 10, 10}, 1);
    b.record({20, 0, 10, 10}, 1);
    auto stats = b.end();

    REQUIRE(stats.draws_before == 2);
    REQUIRE(stats.draws_after == 1);  // merged
    REQUIRE(stats.batches_merged == 1);
}

TEST_CASE("DrawBatcher does not merge different keys", "[render][batcher]") {
    DrawBatcher b;
    b.begin();
    b.record({0, 0, 10, 10}, 1);
    b.record({20, 0, 10, 10}, 2);
    auto stats = b.end();

    REQUIRE(stats.draws_before == 2);
    REQUIRE(stats.draws_after == 2);  // different keys, no merge
}

TEST_CASE("DrawBatcher prevents merge when overlap blocks it", "[render][batcher]") {
    DrawBatcher b;
    b.begin();
    // A(key=1) at left, B(key=2) in middle overlapping combined bounds, C(key=1) at right
    b.record({0, 0, 10, 10}, 1);   // A
    b.record({5, 0, 20, 10}, 2);   // B overlaps combined bounds of A+C
    b.record({20, 0, 10, 10}, 1);  // C
    auto stats = b.end();

    // A and C can't merge because B (different key) overlaps the combined bounds
    REQUIRE(stats.draws_after == 3);
}

TEST_CASE("DrawBatcher empty batch", "[render][batcher]") {
    DrawBatcher b;
    b.begin();
    auto stats = b.end();
    REQUIRE(stats.draws_before == 0);
    REQUIRE(stats.draws_after == 0);
}

TEST_CASE("DrawBatcher single draw", "[render][batcher]") {
    DrawBatcher b;
    b.begin();
    b.record({0, 0, 100, 100}, 42);
    auto stats = b.end();
    REQUIRE(stats.draws_before == 1);
    REQUIRE(stats.draws_after == 1);
}

TEST_CASE("DrawBatcher manual batch hints", "[render][batcher]") {
    DrawBatcher b;
    REQUIRE_FALSE(b.in_manual_batch());
    b.begin_batch();
    REQUIRE(b.in_manual_batch());
    b.end_batch();
    REQUIRE_FALSE(b.in_manual_batch());
}

TEST_CASE("DrawBatcher not active before begin", "[render][batcher]") {
    DrawBatcher b;
    REQUIRE_FALSE(b.is_active());
    b.record({0, 0, 10, 10}, 1);  // ignored
    b.begin();
    REQUIRE(b.is_active());
    b.record({0, 0, 10, 10}, 1);
    b.end();
    REQUIRE_FALSE(b.is_active());
    REQUIRE(b.entries().size() == 1);
}
