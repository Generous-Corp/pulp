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

TEST_CASE("DrawBatcher merges chained same-key draws into one bounding box",
          "[render][batcher][issue-646]") {
    DrawBatcher b;

    b.begin();
    b.record({10, 5, 20, 10}, 7);
    b.record({-5, 1, 3, 3}, 7);
    b.record({40, 30, 5, 5}, 7);
    const auto stats = b.end();

    REQUIRE(stats.draws_before == 3);
    REQUIRE(stats.draws_after == 1);
    REQUIRE(stats.batches_merged == 2);

    REQUIRE(b.entries().size() == 1);
    const auto& bounds = b.entries().front().bounds;
    REQUIRE(bounds.x == -5.0f);
    REQUIRE(bounds.y == 1.0f);
    REQUIRE(bounds.w == 50.0f);
    REQUIRE(bounds.h == 34.0f);
}

TEST_CASE("DrawBatcher ignores records after end until the next begin",
          "[render][batcher][issue-646]") {
    DrawBatcher b;

    b.begin();
    b.record({0, 0, 10, 10}, 11);
    REQUIRE(b.end().draws_after == 1);

    b.record({20, 0, 10, 10}, 11);
    REQUIRE(b.entries().size() == 1);

    b.begin();
    REQUIRE(b.entries().empty());
    b.record({20, 0, 10, 10}, 11);
    REQUIRE(b.end().draws_after == 1);
    REQUIRE(b.entries().front().bounds.x == 20.0f);
}

TEST_CASE("DrawBatcher edge-touching blockers do not prevent same-key merge",
          "[render][batcher][issue-646]") {
    DrawBatcher b;

    b.begin();
    b.record({0, 0, 10, 10}, 3);
    b.record({12, 10, 5, 5}, 99);
    b.record({20, 0, 10, 10}, 3);
    const auto stats = b.end();

    REQUIRE(stats.draws_before == 3);
    REQUIRE(stats.draws_after == 2);
    REQUIRE(stats.batches_merged == 1);

    REQUIRE(b.entries().size() == 2);
    REQUIRE(b.entries()[0].state_key == 3);
    REQUIRE(b.entries()[0].bounds.x == 0.0f);
    REQUIRE(b.entries()[0].bounds.y == 0.0f);
    REQUIRE(b.entries()[0].bounds.w == 30.0f);
    REQUIRE(b.entries()[0].bounds.h == 10.0f);
    REQUIRE(b.entries()[1].state_key == 99);
}

TEST_CASE("DrawBatcher merges across incompatible draws outside combined bounds",
          "[render][batcher][issue-646]") {
    DrawBatcher b;

    b.begin();
    b.record({0, 0, 10, 10}, 4);
    b.record({12, 14, 4, 4}, 99);
    b.record({20, 0, 10, 10}, 4);
    const auto stats = b.end();

    REQUIRE(stats.draws_before == 3);
    REQUIRE(stats.draws_after == 2);
    REQUIRE(stats.batches_merged == 1);

    REQUIRE(b.entries().size() == 2);
    REQUIRE(b.entries()[0].state_key == 4);
    REQUIRE(b.entries()[0].bounds.x == 0.0f);
    REQUIRE(b.entries()[0].bounds.y == 0.0f);
    REQUIRE(b.entries()[0].bounds.w == 30.0f);
    REQUIRE(b.entries()[0].bounds.h == 10.0f);
    REQUIRE(b.entries()[1].state_key == 99);
}

TEST_CASE("DrawBatcher blocker prevents one merge without stopping later compatible merges",
          "[render][batcher][issue-646]") {
    DrawBatcher b;

    b.begin();
    b.record({0, 0, 10, 10}, 5);
    b.record({5, 0, 20, 10}, 99);
    b.record({20, 0, 10, 10}, 5);
    b.record({40, 0, 10, 10}, 5);
    const auto stats = b.end();

    REQUIRE(stats.draws_before == 4);
    REQUIRE(stats.draws_after == 3);
    REQUIRE(stats.batches_merged == 1);

    REQUIRE(b.entries().size() == 3);
    REQUIRE(b.entries()[0].state_key == 5);
    REQUIRE(b.entries()[0].bounds.x == 0.0f);
    REQUIRE(b.entries()[0].bounds.w == 10.0f);
    REQUIRE(b.entries()[1].state_key == 99);
    REQUIRE(b.entries()[2].state_key == 5);
    REQUIRE(b.entries()[2].bounds.x == 20.0f);
    REQUIRE(b.entries()[2].bounds.w == 30.0f);
}

TEST_CASE("DrawBatcher begin resets entries and stats after a blocked pass",
          "[render][batcher][issue-646]") {
    DrawBatcher b;

    b.begin();
    b.record({0, 0, 10, 10}, 6);
    b.record({5, 0, 20, 10}, 99);
    b.record({20, 0, 10, 10}, 6);
    const auto blocked_stats = b.end();

    REQUIRE(blocked_stats.draws_before == 3);
    REQUIRE(blocked_stats.draws_after == 3);
    REQUIRE(blocked_stats.batches_merged == 0);
    REQUIRE(b.entries().size() == 3);

    b.begin();
    REQUIRE(b.entries().empty());
    b.record({100, 20, 5, 5}, 6);
    b.record({110, 20, 5, 5}, 6);
    const auto reset_stats = b.end();

    REQUIRE(reset_stats.draws_before == 2);
    REQUIRE(reset_stats.draws_after == 1);
    REQUIRE(reset_stats.batches_merged == 1);
    REQUIRE(b.entries().size() == 1);
    REQUIRE(b.entries().front().bounds.x == 100.0f);
    REQUIRE(b.entries().front().bounds.w == 15.0f);
}
