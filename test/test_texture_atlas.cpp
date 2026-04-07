#include <catch2/catch_test_macros.hpp>
#include <pulp/render/texture_atlas.hpp>

using namespace pulp::render;

TEST_CASE("AtlasPacker allocates regions", "[render][atlas]") {
    AtlasPacker p(256, 256);
    AtlasPacker::Region r;
    REQUIRE(p.allocate(32, 32, r));
    REQUIRE(r.x == 0);
    REQUIRE(r.y == 0);
    REQUIRE(r.w == 32);

    REQUIRE(p.allocate(32, 32, r));
    REQUIRE(r.x == 32);  // next to first
}

TEST_CASE("AtlasPacker wraps to new shelf", "[render][atlas]") {
    AtlasPacker p(100, 200);
    AtlasPacker::Region r;
    // Fill first shelf
    REQUIRE(p.allocate(60, 30, r));
    REQUIRE(r.x == 0);
    REQUIRE(p.allocate(60, 30, r));
    // Should wrap to next shelf
    REQUIRE(r.x == 0);
    REQUIRE(r.y == 30);
}

TEST_CASE("AtlasPacker rejects oversized", "[render][atlas]") {
    AtlasPacker p(64, 64);
    AtlasPacker::Region r;
    REQUIRE_FALSE(p.allocate(128, 128, r));
}

TEST_CASE("ImageAtlas allocate and evict", "[render][atlas]") {
    ImageAtlas atlas(256);
    AtlasPacker::Region r;
    REQUIRE(atlas.allocate(42, 32, 32, r));
    REQUIRE(atlas.entry_count() == 1);

    // Same key reuses existing entry
    REQUIRE(atlas.allocate(42, 32, 32, r));
    REQUIRE(atlas.entry_count() == 1);

    atlas.release(42);
    atlas.release(42);  // ref_count reaches 0
    auto evicted = atlas.evict_stale(200, 10);
    REQUIRE(evicted == 1);
    REQUIRE(atlas.entry_count() == 0);
}

TEST_CASE("GradientAtlas allocate and evict", "[render][atlas]") {
    GradientAtlas ga;
    int row = -1;
    REQUIRE(ga.allocate(100, row));
    REQUIRE(row == 0);

    REQUIRE(ga.allocate(200, row));
    REQUIRE(row == 1);

    // Same key returns same row
    REQUIRE(ga.allocate(100, row));
    REQUIRE(row == 0);

    ga.mark_used(100, 150);  // used recently
    ga.mark_used(200, 10);   // used long ago
    auto evicted = ga.evict_stale(200, 100);
    REQUIRE(evicted == 1);  // key 200 evicted (190 frames old > 100 max age)
}

TEST_CASE("BufferPool acquire and release", "[render][atlas]") {
    BufferPool<float> pool;
    auto v1 = pool.acquire();
    REQUIRE(v1.empty());

    v1.resize(100);
    pool.release(std::move(v1));
    REQUIRE(pool.pool_size() == 1);

    auto v2 = pool.acquire();
    REQUIRE(v2.capacity() >= 100);  // reused allocation
    REQUIRE(v2.empty());  // but cleared
    REQUIRE(pool.pool_size() == 0);
}
