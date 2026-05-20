#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <pulp/render/texture_atlas.hpp>
#include <pulp/render/atlas_inventory.hpp>

#include <string>

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

TEST_CASE("AtlasPacker shelf height tracks the tallest item - issue-646",
          "[render][atlas][issue-646]") {
    AtlasPacker p(80, 80);
    AtlasPacker::Region r{};

    REQUIRE(p.allocate(20, 10, r));
    REQUIRE(r.y == 0);

    REQUIRE(p.allocate(20, 30, r));
    REQUIRE(r.x == 20);
    REQUIRE(r.y == 0);

    REQUIRE(p.allocate(60, 10, r));
    REQUIRE(r.x == 0);
    REQUIRE(r.y == 30);
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

TEST_CASE("ImageAtlas cache hit keeps original region and refcount - issue-646",
          "[render][atlas][issue-646]") {
    ImageAtlas atlas(64);
    AtlasPacker::Region first{};
    AtlasPacker::Region second{};

    REQUIRE(atlas.allocate(9, 16, 12, first));
    REQUIRE(atlas.allocate(9, 48, 48, second));
    REQUIRE(second.x == first.x);
    REQUIRE(second.y == first.y);
    REQUIRE(second.w == first.w);
    REQUIRE(second.h == first.h);
    REQUIRE(atlas.entry_count() == 1);

    atlas.release(9);
    REQUIRE(atlas.evict_stale(1000, /*max_age=*/1) == 0);
    REQUIRE(atlas.entry_count() == 1);

    atlas.release(9);
    REQUIRE(atlas.evict_stale(1000, /*max_age=*/1) == 1);
    REQUIRE(atlas.entry_count() == 0);
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

TEST_CASE("GradientAtlas cache hit still succeeds after capacity is full - issue-646",
          "[render][atlas][issue-646]") {
    GradientAtlas ga;
    int row = -1;
    for (int i = 0; i < 512; ++i) {
        REQUIRE(ga.allocate(static_cast<uint64_t>(i), row));
    }

    row = -1;
    REQUIRE(ga.allocate(17, row));
    REQUIRE(row == 17);
    REQUIRE(ga.entry_count() == 512);

    REQUIRE_FALSE(ga.allocate(512, row));
    REQUIRE(ga.entry_count() == 512);
}

TEST_CASE("GradientAtlas allocates monotonically after eviction - issue-646",
          "[render][atlas][issue-646]") {
    GradientAtlas ga;
    int row = -1;
    REQUIRE(ga.allocate(1, row));
    REQUIRE(row == 0);
    REQUIRE(ga.evict_stale(10, /*max_age=*/1) == 1);
    REQUIRE(ga.entry_count() == 0);

    REQUIRE(ga.allocate(2, row));
    REQUIRE(row == 1);
    REQUIRE(ga.entry_count() == 1);
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

TEST_CASE("GlyphAtlas rejects full atlas allocations without dropping entries - issue-646",
          "[render][atlas][issue-646]") {
    GlyphAtlas atlas(16);
    AtlasPacker::Region r{};
    REQUIRE(atlas.allocate(1, 16, 16, r));
    REQUIRE_FALSE(atlas.allocate(2, 1, 1, r));
    REQUIRE(atlas.entry_count() == 1);
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

TEST_CASE("PathAtlas mark_used missing key is a no-op and full atlas rejects - issue-646",
          "[render][atlas][issue-646]") {
    PathAtlas atlas(16);
    AtlasPacker::Region r{};
    atlas.mark_used(999, 1);
    REQUIRE(atlas.entry_count() == 0);

    REQUIRE(atlas.allocate(1, 16, 16, r));
    REQUIRE_FALSE(atlas.allocate(2, 1, 1, r));
    REQUIRE(atlas.entry_count() == 1);
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

// ── Phase 6.2 — atlas introspection accessors ───────────────────────────────
//
// Spec: planning/2026-05-19-inspector-phase6-gpu-perf-spike.md § Phase 6.2.
// The inspector's texture-atlas viewer reads per-atlas dimensions and a
// shelf-packer occupancy estimate. These tests pin the read-only accessors
// added to AtlasPacker / ImageAtlas / GlyphAtlas / GradientAtlas / PathAtlas.

TEST_CASE("AtlasPacker reports capacity and occupancy - phase6.2",
          "[render][atlas][phase6.2]") {
    AtlasPacker p(64, 64);
    REQUIRE(p.capacity() == 64u * 64u);
    REQUIRE(p.used_area() == 0u);
    REQUIRE(p.occupancy() == Catch::Approx(0.0f));

    AtlasPacker::Region r{};
    // Pack a 32×64 column on the first shelf: the in-progress shelf is
    // 32 wide × 64 tall → 2048 texels of a 4096-texel atlas.
    REQUIRE(p.allocate(32, 64, r));
    REQUIRE(p.used_area() == 32u * 64u);
    REQUIRE(p.occupancy() == Catch::Approx(0.5f));

    // Fill the rest of the shelf — atlas is now fully occupied.
    REQUIRE(p.allocate(32, 64, r));
    REQUIRE(p.occupancy() == Catch::Approx(1.0f));
}

TEST_CASE("AtlasPacker occupancy is zero for a degenerate atlas - phase6.2",
          "[render][atlas][phase6.2]") {
    AtlasPacker p(0, 0);
    REQUIRE(p.capacity() == 0u);
    REQUIRE(p.occupancy() == Catch::Approx(0.0f));  // no divide-by-zero.
}

TEST_CASE("ImageAtlas exposes dimensions and occupancy - phase6.2",
          "[render][atlas][phase6.2]") {
    ImageAtlas atlas(128);
    REQUIRE(atlas.width() == 128);
    REQUIRE(atlas.height() == 128);
    REQUIRE(atlas.occupancy() == Catch::Approx(0.0f));

    AtlasPacker::Region r{};
    REQUIRE(atlas.allocate(1, 64, 128, r));  // half the page width.
    REQUIRE(atlas.occupancy() == Catch::Approx(0.5f));
    REQUIRE(atlas.entry_count() == 1);
}

TEST_CASE("GlyphAtlas and PathAtlas expose dimensions - phase6.2",
          "[render][atlas][phase6.2]") {
    GlyphAtlas glyphs(256);
    REQUIRE(glyphs.width() == 256);
    REQUIRE(glyphs.height() == 256);
    REQUIRE(glyphs.occupancy() == Catch::Approx(0.0f));

    PathAtlas paths(512);
    REQUIRE(paths.width() == 512);
    REQUIRE(paths.height() == 512);
    REQUIRE(paths.occupancy() == Catch::Approx(0.0f));
}

TEST_CASE("GradientAtlas exposes row capacity and occupancy - phase6.2",
          "[render][atlas][phase6.2]") {
    GradientAtlas ga;
    REQUIRE(ga.row_capacity() == 512);
    REQUIRE(ga.rows_used() == 0);
    REQUIRE(ga.occupancy() == Catch::Approx(0.0f));

    int row = -1;
    for (int i = 0; i < 256; ++i)
        REQUIRE(ga.allocate(static_cast<uint64_t>(i), row));
    REQUIRE(ga.rows_used() == 256);
    REQUIRE(ga.occupancy() == Catch::Approx(0.5f));  // 256 / 512.
}

// ── Phase 6.2 — AtlasInventory aggregator ───────────────────────────────────

TEST_CASE("AtlasInventory starts empty - phase6.2",
          "[render][atlas][phase6.2]") {
    AtlasInventory inv;
    REQUIRE(inv.empty());
    REQUIRE(inv.size() == 0);
    REQUIRE(inv.total_pages() == 0);
    REQUIRE(inv.total_entries() == 0u);
    REQUIRE(inv.average_occupancy() == Catch::Approx(0.0f));
}

TEST_CASE("AtlasInventory snapshots a packed atlas - phase6.2",
          "[render][atlas][phase6.2]") {
    GlyphAtlas glyphs(256);
    AtlasPacker::Region r{};
    REQUIRE(glyphs.allocate(7, 128, 256, r));  // half-page → 50% occupancy.

    AtlasInventory inv;
    inv.add_atlas(glyphs, AtlasKind::glyph);
    REQUIRE_FALSE(inv.empty());
    REQUIRE(inv.size() == 1);

    const AtlasInfo& info = inv.atlases().front();
    REQUIRE(info.kind == AtlasKind::glyph);
    REQUIRE(info.label == "glyph");          // defaults to the kind name.
    REQUIRE(info.width == 256);
    REQUIRE(info.height == 256);
    REQUIRE(info.pages == 1);
    REQUIRE(info.entries == 1u);
    REQUIRE(info.occupancy == Catch::Approx(0.5f));
    REQUIRE(info.occupancy_percent() == 50);
    REQUIRE(info.texel_capacity() == 256u * 256u);
}

TEST_CASE("AtlasInventory snapshot_gradient uses row capacity as height - phase6.2",
          "[render][atlas][phase6.2]") {
    GradientAtlas ga;
    int row = -1;
    for (int i = 0; i < 128; ++i)
        REQUIRE(ga.allocate(static_cast<uint64_t>(i), row));

    AtlasInventory inv;
    inv.add_gradient(ga);
    const AtlasInfo& info = inv.atlases().front();
    REQUIRE(info.kind == AtlasKind::gradient);
    REQUIRE(info.width == 256);             // default ramp width.
    REQUIRE(info.height == 512);            // GradientAtlas row budget.
    REQUIRE(info.entries == 128u);
    REQUIRE(info.occupancy == Catch::Approx(0.25f));  // 128 / 512.
}

TEST_CASE("AtlasInventory aggregates across multiple atlases - phase6.2",
          "[render][atlas][phase6.2]") {
    ImageAtlas images(128);
    GlyphAtlas glyphs(256);
    AtlasPacker::Region r{};
    REQUIRE(images.allocate(1, 64, 128, r));   // 50% of the image atlas.
    REQUIRE(glyphs.allocate(2, 256, 256, r));  // 100% of the glyph atlas.

    AtlasInventory inv;
    inv.add_atlas(images, AtlasKind::image, "images", /*pages=*/2);
    inv.add_atlas(glyphs, AtlasKind::glyph);

    REQUIRE(inv.size() == 2);
    REQUIRE(inv.total_pages() == 3);          // 2 + 1.
    REQUIRE(inv.total_entries() == 2u);       // one entry per atlas.
    // average occupancy = (0.5 + 1.0) / 2 = 0.75.
    REQUIRE(inv.average_occupancy() == Catch::Approx(0.75f));
    // The custom label survives; the default falls back to the kind name.
    REQUIRE(inv.atlases()[0].label == "images");
    REQUIRE(inv.atlases()[1].label == "glyph");
}

TEST_CASE("AtlasInventory clear empties the collection - phase6.2",
          "[render][atlas][phase6.2]") {
    ImageAtlas images(64);
    AtlasInventory inv;
    inv.add_atlas(images, AtlasKind::image);
    REQUIRE(inv.size() == 1);
    inv.clear();
    REQUIRE(inv.empty());
    REQUIRE(inv.total_pages() == 0);
}

TEST_CASE("AtlasInfo occupancy_percent clamps out-of-range values - phase6.2",
          "[render][atlas][phase6.2]") {
    AtlasInfo over;
    over.occupancy = 1.7f;
    REQUIRE(over.occupancy_percent() == 100);  // clamped high.

    AtlasInfo under;
    under.occupancy = -0.3f;
    REQUIRE(under.occupancy_percent() == 0);   // clamped low.

    AtlasInfo mid;
    mid.occupancy = 0.333f;
    REQUIRE(mid.occupancy_percent() == 33);    // rounds to nearest.
}

TEST_CASE("atlas_kind_name covers every AtlasKind - phase6.2",
          "[render][atlas][phase6.2]") {
    REQUIRE(std::string(atlas_kind_name(AtlasKind::glyph))    == "glyph");
    REQUIRE(std::string(atlas_kind_name(AtlasKind::image))    == "image");
    REQUIRE(std::string(atlas_kind_name(AtlasKind::gradient)) == "gradient");
    REQUIRE(std::string(atlas_kind_name(AtlasKind::path))     == "path");
}
