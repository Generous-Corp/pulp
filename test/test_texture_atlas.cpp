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

// ── Additional coverage — issue-646 ─────────────────────────────────────

TEST_CASE("AtlasPacker starts new shelf when current is full - issue-646",
          "[render][atlas][issue-646]") {
    // 100-wide packer: first 60 fit on shelf 0, second 60 won't fit next to them,
    // so the allocator rolls to a new shelf and returns x=0 at y=shelf_h.
    AtlasPacker p(100, 200);
    AtlasPacker::Region a{};
    AtlasPacker::Region b{};
    REQUIRE(p.allocate(60, 40, a));
    REQUIRE(a.x == 0);
    REQUIRE(a.y == 0);

    // Does not fit next to `a` (60 + 60 > 100); should roll to next shelf.
    REQUIRE(p.allocate(60, 30, b));
    REQUIRE(b.x == 0);
    REQUIRE(b.y == 40);   // shelf advanced by previous shelf height
    REQUIRE(p.width() == 100);
    REQUIRE(p.height() == 200);
}

TEST_CASE("AtlasPacker returns false when atlas is full - issue-646",
          "[render][atlas][issue-646]") {
    AtlasPacker p(64, 64);
    AtlasPacker::Region r{};
    REQUIRE(p.allocate(64, 50, r));   // occupies most of the height
    // Next allocation needs more height than remains and cannot fit on the
    // current shelf horizontally (64 wide), so the packer rejects it.
    REQUIRE_FALSE(p.allocate(64, 20, r));
}

TEST_CASE("AtlasPacker reset lets us pack again from origin - issue-646",
          "[render][atlas][issue-646]") {
    AtlasPacker p(64, 64);
    AtlasPacker::Region r{};
    REQUIRE(p.allocate(64, 64, r));
    REQUIRE_FALSE(p.allocate(8, 8, r));  // full

    p.reset();
    REQUIRE(p.allocate(8, 8, r));
    REQUIRE(r.x == 0);
    REQUIRE(r.y == 0);
}

TEST_CASE("AtlasPacker rejects non-positive dimensions - issue-646",
          "[render][atlas][issue-646]") {
    AtlasPacker p(64, 64);
    AtlasPacker::Region r{};

    REQUIRE_FALSE(p.allocate(0, 8, r));
    REQUIRE_FALSE(p.allocate(8, 0, r));
    REQUIRE_FALSE(p.allocate(-1, 8, r));
    REQUIRE_FALSE(p.allocate(8, -1, r));

    REQUIRE(p.allocate(8, 8, r));
    REQUIRE(r.x == 0);
    REQUIRE(r.y == 0);
}

TEST_CASE("ImageAtlas mark_used keeps live entry from eviction - issue-646",
          "[render][atlas][issue-646]") {
    ImageAtlas atlas(256);
    AtlasPacker::Region r{};
    REQUIRE(atlas.allocate(7, 16, 16, r));
    atlas.release(7);                   // ref_count -> 0, eligible for stale eviction
    atlas.mark_used(7, /*frame=*/100);

    // current_frame - last_used = 10 <= max_age (50) → NOT evicted.
    REQUIRE(atlas.evict_stale(110, /*max_age=*/50) == 0);
    REQUIRE(atlas.entry_count() == 1);

    // Now push age past the threshold; entry goes away.
    REQUIRE(atlas.evict_stale(200, /*max_age=*/50) == 1);
    REQUIRE(atlas.entry_count() == 0);
}

TEST_CASE("ImageAtlas release of unknown key is a no-op - issue-646",
          "[render][atlas][issue-646]") {
    ImageAtlas atlas(128);
    atlas.release(999);                 // no entry, must not crash or mutate
    REQUIRE(atlas.entry_count() == 0);
    atlas.mark_used(999, 1);            // mark_used on missing key also no-op
    REQUIRE(atlas.entry_count() == 0);
}

TEST_CASE("ImageAtlas skips eviction while ref_count is non-zero - issue-646",
          "[render][atlas][issue-646]") {
    ImageAtlas atlas(128);
    AtlasPacker::Region r{};
    REQUIRE(atlas.allocate(5, 8, 8, r));
    // No release() → ref_count stays at 1; even very old entries survive.
    REQUIRE(atlas.evict_stale(/*current=*/10'000, /*max_age=*/1) == 0);
    REQUIRE(atlas.entry_count() == 1);
}

TEST_CASE("ImageAtlas rejects full atlas allocations and clamps release - issue-646",
          "[render][atlas][issue-646]") {
    ImageAtlas atlas(16);
    AtlasPacker::Region r{};
    REQUIRE(atlas.allocate(1, 16, 16, r));
    REQUIRE_FALSE(atlas.allocate(2, 1, 1, r));
    REQUIRE(atlas.entry_count() == 1);

    atlas.release(1);
    atlas.release(1);  // extra release must not underflow ref_count
    REQUIRE(atlas.evict_stale(100, /*max_age=*/1) == 1);
    REQUIRE(atlas.entry_count() == 0);
}

TEST_CASE("GradientAtlas has() reflects allocation and mark_used no-ops - issue-646",
          "[render][atlas][issue-646]") {
    GradientAtlas ga;
    REQUIRE_FALSE(ga.has(42));
    int row = -1;
    REQUIRE(ga.allocate(42, row));
    REQUIRE(ga.has(42));
    REQUIRE(ga.entry_count() == 1);

    // mark_used on unknown key should silently do nothing.
    ga.mark_used(999, 100);
    REQUIRE(ga.entry_count() == 1);
}

TEST_CASE("GradientAtlas reports capacity exhaustion - issue-646",
          "[render][atlas][issue-646]") {
    GradientAtlas ga;
    int row = -1;
    for (int i = 0; i < 512; ++i) {
        REQUIRE(ga.allocate(static_cast<uint64_t>(i), row));
        REQUIRE(row == i);
    }

    REQUIRE_FALSE(ga.allocate(9999, row));
    REQUIRE(ga.entry_count() == 512);
}

TEST_CASE("GlyphAtlas allocate reuses same key, evicts by age - issue-646",
          "[render][atlas][issue-646]") {
    GlyphAtlas atlas(256);
    AtlasPacker::Region r1{};
    AtlasPacker::Region r2{};

    REQUIRE(atlas.allocate(/*glyph=*/11, 12, 16, r1));
    REQUIRE(atlas.entry_count() == 1);

    // Same key returns the same region without re-packing.
    REQUIRE(atlas.allocate(11, 12, 16, r2));
    REQUIRE(r2.x == r1.x);
    REQUIRE(r2.y == r1.y);
    REQUIRE(atlas.entry_count() == 1);

    atlas.mark_used(11, 50);
    // Age still within max_age: not evicted.
    REQUIRE(atlas.evict_stale(100, /*max_age=*/100) == 0);
    // Age exceeds max_age: evicted.
    REQUIRE(atlas.evict_stale(1000, /*max_age=*/100) == 1);
    REQUIRE(atlas.entry_count() == 0);

    // mark_used after eviction is a no-op.
    atlas.mark_used(11, 2000);
    REQUIRE(atlas.entry_count() == 0);
}

TEST_CASE("GlyphAtlas rejects oversized glyph - issue-646",
          "[render][atlas][issue-646]") {
    GlyphAtlas atlas(32);
    AtlasPacker::Region r{};
    REQUIRE_FALSE(atlas.allocate(1, /*w=*/64, /*h=*/64, r));
    REQUIRE(atlas.entry_count() == 0);
}

TEST_CASE("PathAtlas allocate, cache hit, and eviction - issue-646",
          "[render][atlas][issue-646]") {
    PathAtlas atlas(256);
    AtlasPacker::Region r1{};
    AtlasPacker::Region r2{};

    REQUIRE(atlas.allocate(/*hash=*/0xABCD, 40, 40, r1));
    REQUIRE(atlas.entry_count() == 1);

    // Cache hit on same hash returns same region without advancing packer.
    REQUIRE(atlas.allocate(0xABCD, 40, 40, r2));
    REQUIRE(r2.x == r1.x);
    REQUIRE(r2.y == r1.y);
    REQUIRE(atlas.entry_count() == 1);

    atlas.mark_used(0xABCD, 500);
    REQUIRE(atlas.evict_stale(600, /*max_age=*/200) == 0);   // still fresh
    REQUIRE(atlas.evict_stale(2000, /*max_age=*/200) == 1);  // stale
    REQUIRE(atlas.entry_count() == 0);
}

TEST_CASE("PathAtlas rejects oversized path bitmap - issue-646",
          "[render][atlas][issue-646]") {
    PathAtlas atlas(64);
    AtlasPacker::Region r{};
    REQUIRE_FALSE(atlas.allocate(1, /*w=*/128, /*h=*/128, r));
    REQUIRE(atlas.entry_count() == 0);
}

TEST_CASE("Atlas eviction treats future last-used frames as fresh - issue-646",
          "[render][atlas][issue-646]") {
    AtlasPacker::Region r{};

    ImageAtlas image(64);
    REQUIRE(image.allocate(1, 8, 8, r));
    image.release(1);
    image.mark_used(1, 100);
    REQUIRE(image.evict_stale(50, /*max_age=*/0) == 0);
    REQUIRE(image.entry_count() == 1);
    REQUIRE(image.evict_stale(101, /*max_age=*/0) == 1);

    GradientAtlas gradient;
    int row = -1;
    REQUIRE(gradient.allocate(1, row));
    gradient.mark_used(1, 100);
    REQUIRE(gradient.evict_stale(50, /*max_age=*/0) == 0);
    REQUIRE(gradient.entry_count() == 1);
    REQUIRE(gradient.evict_stale(101, /*max_age=*/0) == 1);

    GlyphAtlas glyph(64);
    REQUIRE(glyph.allocate(1, 8, 8, r));
    glyph.mark_used(1, 100);
    REQUIRE(glyph.evict_stale(50, /*max_age=*/0) == 0);
    REQUIRE(glyph.entry_count() == 1);
    REQUIRE(glyph.evict_stale(101, /*max_age=*/0) == 1);

    PathAtlas path(64);
    REQUIRE(path.allocate(1, 8, 8, r));
    path.mark_used(1, 100);
    REQUIRE(path.evict_stale(50, /*max_age=*/0) == 0);
    REQUIRE(path.entry_count() == 1);
    REQUIRE(path.evict_stale(101, /*max_age=*/0) == 1);
}

TEST_CASE("BufferPool caps retained buffers at max_pool_size - issue-646",
          "[render][atlas][issue-646]") {
    BufferPool<int> pool;
    // max_pool_size_ defaults to 32 (see texture_atlas.hpp); release many more
    // and verify the pool does not grow unbounded.
    for (int i = 0; i < 100; ++i) {
        std::vector<int> v;
        v.reserve(4);
        pool.release(std::move(v));
    }
    REQUIRE(pool.pool_size() == 32);

    // Draining returns cached buffers, not more than we stored.
    std::size_t drained = 0;
    while (pool.pool_size() > 0) {
        (void)pool.acquire();
        ++drained;
    }
    REQUIRE(drained == 32);
}
