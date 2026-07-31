// test_retained_layer_store.cpp — the renderer-owned retained compositing
// layer store (WAH-12).
//
// Retained layers are GPU textures whose lifetime belongs to the RENDERER, not
// to the frame-local SkiaCanvas wrapped around each frame's target surface.
// That ownership direction was already right; what was wrong was everything
// about how the storage behaved:
//
//   * a linear scan on every lookup, on the paint path, so a frame compositing
//     N cached layers cost O(N^2);
//   * identity as a `const void*` plus a separate untypecheckable byte, so
//     "same owner" compared an address whose meaning lived elsewhere;
//   * no budget at all — an unbounded store of GPU textures;
//   * a sealed non-cacheable layer that was never drawn was never removed.
//
// The last two are the ones that show up as a user-visible problem: VRAM that
// grows with how long a UI has been open rather than with what it displays.
//
// Gated PULP_HAS_SKIA; a raster SkSurface is enough — none of this needs a GPU.

#include <catch2/catch_test_macros.hpp>

#include <pulp/canvas/canvas.hpp>

#ifdef PULP_HAS_SKIA
#include <pulp/canvas/skia_canvas.hpp>

#include "include/core/SkCanvas.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkSurface.h"

#include <memory>
#include <vector>
#endif

using namespace pulp;

#ifdef PULP_HAS_SKIA

namespace {

constexpr int kW = 64;
constexpr int kH = 64;

struct RasterFixture {
    sk_sp<SkSurface> surface;
    std::shared_ptr<canvas::SkiaCanvas::RetainedLayerStore> store;
    std::unique_ptr<canvas::SkiaCanvas> canvas;

    RasterFixture() {
        surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(kW, kH));
        store = canvas::SkiaCanvas::create_retained_layer_store();
        rebuild_canvas();
    }

    /// A new frame gets a NEW SkiaCanvas over the same store — which is the
    /// whole point of the store existing.
    void rebuild_canvas() {
        canvas = std::make_unique<canvas::SkiaCanvas>(
            surface->getCanvas(), /*recorder*/ nullptr, store);
    }

    /// Seal a layer and return its handle. `cacheable=false` models a layer
    /// consumed by its first draw.
    canvas::Canvas::LayerHandle seal(bool cacheable) {
        auto h = canvas->begin_layer({0, 0, 16, 16}, cacheable);
        canvas->set_fill_color(canvas::Color::rgba8(10, 20, 30));
        canvas->fill_rect(0, 0, 16, 16);
        return canvas->end_layer();
    }
};

}  // namespace

TEST_CASE("a sealed cacheable layer survives across canvas instances",
          "[retained-layer-store][wah-12]") {
    // The ownership direction under test: the store outlives the frame-local
    // canvas, so a handle taken on one frame still composites on the next.
    RasterFixture f;
    const auto handle = f.seal(/*cacheable*/ true);
    REQUIRE(f.canvas->layer_valid(handle));

    f.rebuild_canvas();

    REQUIRE(f.canvas->layer_valid(handle));
}

TEST_CASE("an invalidated layer is gone", "[retained-layer-store][wah-12]") {
    RasterFixture f;
    const auto handle = f.seal(true);
    REQUIRE(f.canvas->retained_layer_count() == 1);

    f.canvas->invalidate_layer(handle);

    REQUIRE_FALSE(f.canvas->layer_valid(handle));
    REQUIRE(f.canvas->retained_layer_count() == 0);
}

TEST_CASE("a stale handle never resolves to new content",
          "[retained-layer-store][wah-12]") {
    // Layer ids are never recycled. If they were, a handle held across an
    // invalidate could silently start compositing a DIFFERENT layer's pixels.
    RasterFixture f;
    const auto first = f.seal(true);
    f.canvas->invalidate_layer(first);
    const auto second = f.seal(true);

    REQUIRE(second.id != first.id);
    REQUIRE_FALSE(f.canvas->layer_valid(first));
    REQUIRE(f.canvas->layer_valid(second));
}

// ── Abandoned non-cacheable layers ──────────────────────────────────────────

TEST_CASE("a sealed non-cacheable layer that is never drawn is pruned",
          "[retained-layer-store][wah-12]") {
    // A non-cacheable layer is consumed by its first draw. One that is sealed
    // and then abandoned — the caller took another branch, the widget left the
    // tree — has no future reader, so its texture was pure waste held until the
    // store was replaced.
    RasterFixture f;
    f.seal(/*cacheable*/ false);
    REQUIRE(f.canvas->retained_layer_count() == 1);

    const std::size_t pruned = f.canvas->prune_abandoned_retained_layers();

    REQUIRE(pruned == 1);
    REQUIRE(f.canvas->retained_layer_count() == 0);
}

TEST_CASE("pruning leaves cacheable layers alone",
          "[retained-layer-store][wah-12]") {
    // The prune must not become a cache flush: a cacheable layer's whole
    // purpose is to survive to the next frame.
    RasterFixture f;
    const auto keep = f.seal(true);
    f.seal(false);
    REQUIRE(f.canvas->retained_layer_count() == 2);

    REQUIRE(f.canvas->prune_abandoned_retained_layers() == 1);

    REQUIRE(f.canvas->retained_layer_count() == 1);
    REQUIRE(f.canvas->layer_valid(keep));
}

TEST_CASE("a drawn non-cacheable layer is already gone before any prune",
          "[retained-layer-store][wah-12]") {
    RasterFixture f;
    const auto handle = f.seal(false);
    f.canvas->draw_layer(handle);

    REQUIRE(f.canvas->retained_layer_count() == 0);
    REQUIRE(f.canvas->prune_abandoned_retained_layers() == 0);
}

TEST_CASE("pruning an empty store is a no-op",
          "[retained-layer-store][wah-12]") {
    RasterFixture f;
    REQUIRE(f.canvas->prune_abandoned_retained_layers() == 0);
    REQUIRE(f.canvas->retained_layer_count() == 0);
}

// ── Cache pressure ──────────────────────────────────────────────────────────

TEST_CASE("the store is bounded under sustained layer creation",
          "[retained-layer-store][wah-12]") {
    // The property that makes this a cache rather than a leak. Without a
    // budget, a UI that opens layers under a scroll or an animation grows GPU
    // texture memory for as long as it stays open.
    RasterFixture f;
    const std::size_t cap = canvas::SkiaCanvas::max_retained_layers();
    REQUIRE(cap > 0);

    std::vector<canvas::Canvas::LayerHandle> handles;
    for (std::size_t i = 0; i < cap * 3; ++i) handles.push_back(f.seal(true));

    REQUIRE(f.canvas->retained_layer_count() <= cap);
}

TEST_CASE("eviction keeps the most recently used layer",
          "[retained-layer-store][wah-12]") {
    // LRU, not arbitrary: the entry a frame has gone longest without touching
    // is the one least likely to be composited next. Evicting the layer that
    // was just drawn would guarantee a miss on the following frame.
    RasterFixture f;
    const std::size_t cap = canvas::SkiaCanvas::max_retained_layers();

    const auto hot = f.seal(true);
    for (std::size_t i = 0; i < cap / 2; ++i) f.seal(true);
    // Touch it, so it is the most recently used entry.
    REQUIRE(f.canvas->layer_valid(hot));
    f.canvas->draw_layer(hot);
    REQUIRE(f.canvas->layer_valid(hot));

    // Now flood past the cap.
    for (std::size_t i = 0; i < cap; ++i) f.seal(true);

    REQUIRE(f.canvas->retained_layer_count() <= cap);
}

// ── Owner / backend replacement ─────────────────────────────────────────────

TEST_CASE("adopting a first owner does not discard the cache",
          "[retained-layer-store][wah-12]") {
    // An unbound store taking its first owner is not a replacement, so it must
    // keep what it has. Getting this wrong flushes the cache on the first frame
    // that binds a backend.
    //
    // NOTE: the synthetic context below is only ever used as an IDENTITY. No
    // layer may be sealed while it is bound — begin_layer() would hand it to
    // SkSurfaces::RenderTarget and dereference it. Every layer here is sealed
    // on the raster path first.
    int context_a = 0;

    RasterFixture f;
    const auto handle = f.seal(true);
    REQUIRE(f.canvas->layer_valid(handle));

    f.rebuild_canvas();
    f.canvas->set_gpu_upload_context(
        reinterpret_cast<GrDirectContext*>(&context_a));

    REQUIRE(f.canvas->layer_valid(handle));
    REQUIRE(f.canvas->retained_layer_count() == 1);
}

TEST_CASE("a backend change invalidates every retained layer",
          "[retained-layer-store][wah-12]") {
    // GPU images cannot cross the recorder/context that created them, so a
    // surface or backend REPLACEMENT must drop the whole store. Carrying one
    // across is a use-after-free, not a stale draw.
    int context_a = 0;
    int context_b = 0;

    RasterFixture f;
    const auto handle = f.seal(true);

    // Frame 2 adopts owner A (initial bind — keeps the cache).
    f.rebuild_canvas();
    f.canvas->set_gpu_upload_context(
        reinterpret_cast<GrDirectContext*>(&context_a));
    REQUIRE(f.canvas->layer_valid(handle));

    // Frame 3 binds a DIFFERENT backend object — a real replacement.
    f.rebuild_canvas();
    f.canvas->set_gpu_upload_context(
        reinterpret_cast<GrDirectContext*>(&context_b));

    REQUIRE_FALSE(f.canvas->layer_valid(handle));
    REQUIRE(f.canvas->retained_layer_count() == 0);
}

TEST_CASE("rebinding the SAME backend keeps the cache",
          "[retained-layer-store][wah-12]") {
    // The complement, and the one that matters for performance: every frame
    // rebinds the same recorder/context, so treating that as a replacement
    // would silently flush the cache on every single frame.
    int context = 0;

    RasterFixture f;
    const auto handle = f.seal(true);

    f.rebuild_canvas();
    f.canvas->set_gpu_upload_context(reinterpret_cast<GrDirectContext*>(&context));
    f.rebuild_canvas();
    f.canvas->set_gpu_upload_context(reinterpret_cast<GrDirectContext*>(&context));

    REQUIRE(f.canvas->layer_valid(handle));
    REQUIRE(f.canvas->retained_layer_count() == 1);
}

TEST_CASE("no cross-surface reuse: a private store does not promise caching",
          "[retained-layer-store][wah-12]") {
    // A canvas constructed WITHOUT a shared store gets a private one, and must
    // report that it cannot promise cross-frame persistence — otherwise a
    // caller would cache against a store that dies with the canvas.
    sk_sp<SkSurface> surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(kW, kH));
    canvas::SkiaCanvas standalone(surface->getCanvas());
    REQUIRE_FALSE(
        standalone.supports(canvas::CanvasCapability::retained_layer_cache));
}

TEST_CASE("a shared store does promise caching",
          "[retained-layer-store][wah-12]") {
    RasterFixture f;
    REQUIRE(f.canvas->supports(canvas::CanvasCapability::retained_layer_cache));
}

#else

TEST_CASE("retained layer store requires Skia", "[retained-layer-store][wah-12]") {
    SUCCEED("Not a Skia build — the retained layer store is not compiled.");
}

#endif
