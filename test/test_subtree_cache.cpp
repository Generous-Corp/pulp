// Subtree scene cache — View::set_subtree_cached + Canvas::record_scene /
// draw_scene (FU-3, WI-20).
//
// The cache records a subtree's painted CONTENT once (background/border/
// paint()/children/decorations — everything paint_content draws) as a
// replayable SkPicture and replays it until the subtree is invalidated. The
// view's own opacity/filter/mask compositing layers stay LIVE outside the
// cache. These tests pin the behavior that matters, with no tautologies:
//
//   1. cached render is pixel-identical to the uncached render;
//   2. the cache is REUSED when nothing changed — a spied child paint() is NOT
//      re-invoked on the second frame (the non-tautology);
//   3. a child mutation INVALIDATES the cache and the pixels update;
//   4. a structural change (add_child) invalidates;
//   5. a resize (set_bounds) invalidates;
//   6. a non-scene_cache backend (RecordingCanvas) falls back to direct paint
//      every frame — no crash, no silent blank, counter climbs;
//   7. layer independence — animating the view's opacity does NOT re-record,
//      yet the pixels track the new opacity (the opacity layer is live,
//      outside the cache).
//
// Plus the ScopedAllocAllowed contract the cache-miss record relies on.

#include <catch2/catch_test_macros.hpp>

#include <pulp/canvas/canvas.hpp>
#include <pulp/canvas/recording_canvas.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>
#include <pulp/view/view.hpp>

#include <memory>
#include <vector>

#ifdef PULP_HAS_SKIA
#include <pulp/canvas/skia_canvas.hpp>
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkSurface.h"
#endif

using namespace pulp::canvas;
using pulp::view::View;

namespace {

// A View whose paint() both increments a counter (so a cache HIT — which skips
// the subtree walk — is observable as "paint() was NOT called again") and
// fills its box with a solid color (so the content is visible in the pixel
// readback and a color change is a real, observable mutation).
class CountingView : public View {
public:
    int paints = 0;
    Color fill = Color::rgba8(30, 120, 200, 255);

    void paint(Canvas& c) override {
        ++paints;
        c.set_fill_color(fill);
        c.fill_rect(0, 0, bounds().width, bounds().height);
    }
};

}  // namespace

// ── ScopedAllocAllowed contract ──────────────────────────────────────────
// The cache-miss record allocates (SkPicture command buffer, per-view
// corner-path strings) inside paint_all's ScopedNoAlloc region. ScopedAllocAllowed
// suspends the contract for exactly that record. These pin the semantics in
// both build modes: real suspend behavior in debug, documented no-op in NDEBUG.
TEST_CASE("ScopedAllocAllowed suspends the no-alloc contract while alive",
          "[runtime][no-alloc][issue-6262]") {
    REQUIRE_FALSE(pulp::runtime::is_in_no_alloc_scope());
    {
        pulp::runtime::ScopedNoAlloc guard;
#ifdef NDEBUG
        // Release: the guard is a no-op, so the scope always reads "not in one".
        REQUIRE_FALSE(pulp::runtime::is_in_no_alloc_scope());
        REQUIRE(pulp::runtime::no_alloc_scope_depth() == 0);
#else
        REQUIRE(pulp::runtime::is_in_no_alloc_scope());
        REQUIRE(pulp::runtime::no_alloc_scope_depth() == 1);
        {
            pulp::runtime::ScopedAllocAllowed allow;
            // Contract suspended even though a ScopedNoAlloc is still on the stack.
            REQUIRE_FALSE(pulp::runtime::is_in_no_alloc_scope());
            REQUIRE(pulp::runtime::no_alloc_allowed_depth() == 1);
            // The no-alloc DEPTH is unchanged — the allow-scope overrides, it
            // does not pop the guard. A nested ScopedNoAlloc (a child paint
            // during the record) stays suspended too.
            REQUIRE(pulp::runtime::no_alloc_scope_depth() == 1);
            {
                pulp::runtime::ScopedNoAlloc nested;
                REQUIRE_FALSE(pulp::runtime::is_in_no_alloc_scope());
                REQUIRE(pulp::runtime::no_alloc_scope_depth() == 2);
            }
        }
        // Allow-scope gone: the contract snaps back on immediately.
        REQUIRE(pulp::runtime::is_in_no_alloc_scope());
        REQUIRE(pulp::runtime::no_alloc_allowed_depth() == 0);
#endif
    }
    REQUIRE_FALSE(pulp::runtime::is_in_no_alloc_scope());
}

// ── Backend fallback (no Skia required) ──────────────────────────────────
// A backend that does not support scene_cache (RecordingCanvas advertises
// nothing) must fall back to painting the subtree directly EVERY frame:
// record_scene is never engaged, so the child's paint() runs each frame and
// the command stream is emitted each frame. Proves the opt-in cache never
// blanks a non-Skia backend.
TEST_CASE("Subtree cache falls back to direct paint on a non-scene_cache backend",
          "[view][subtree-cache][issue-6262]") {
    auto root = std::make_unique<View>();
    root->set_bounds({0, 0, 64, 64});
    auto child = std::make_unique<CountingView>();
    child->set_bounds({8, 8, 48, 48});
    CountingView* cp = child.get();
    root->add_child(std::move(child));
    root->set_subtree_cached(true);
    REQUIRE(root->subtree_cached());

    RecordingCanvas rec;
    REQUIRE_FALSE(rec.supports(CanvasCapability::scene_cache));

    root->paint_all(rec);
    const std::size_t after_first = rec.commands().size();
    root->paint_all(rec);

    // No caching engaged: paint() ran on BOTH frames, and both frames appended
    // commands to the stream.
    REQUIRE(cp->paints == 2);
    REQUIRE(rec.commands().size() > after_first);
}

#ifdef PULP_HAS_SKIA

namespace {

constexpr int kW = 128;
constexpr int kH = 128;

// Render a view into a fresh WxH raster surface cleared to white and return the
// premultiplied RGBA pixels. The surface is independent per call, so replaying a
// recording built on an earlier surface exercises the real cross-surface path.
std::vector<uint8_t> render_view(View& v) {
    SkImageInfo info = SkImageInfo::Make(kW, kH, kN32_SkColorType,
                                         kPremul_SkAlphaType,
                                         SkColorSpace::MakeSRGB());
    auto surface = SkSurfaces::Raster(info);
    REQUIRE(surface != nullptr);
    SkCanvas* skc = surface->getCanvas();
    skc->clear(SK_ColorWHITE);
    SkiaCanvas canvas(skc);
    v.paint_all(canvas);

    std::vector<uint8_t> out(static_cast<std::size_t>(kW) * kH * 4);
    REQUIRE(surface->readPixels(info, out.data(), kW * 4, 0, 0));
    return out;
}

// Render a view into a (kW*scale)x(kH*scale) raster surface with the backing
// scale pre-applied to the canvas (simulating a HiDPI display). The recording
// is in logical space, so a cached replay must rasterize identically to a
// direct paint at the same scale — that is the resolution-independence claim.
std::vector<uint8_t> render_view_at_scale(View& v, float scale) {
    const int pw = static_cast<int>(kW * scale);
    const int ph = static_cast<int>(kH * scale);
    SkImageInfo info = SkImageInfo::Make(pw, ph, kN32_SkColorType,
                                         kPremul_SkAlphaType,
                                         SkColorSpace::MakeSRGB());
    auto surface = SkSurfaces::Raster(info);
    REQUIRE(surface != nullptr);
    SkCanvas* skc = surface->getCanvas();
    skc->clear(SK_ColorWHITE);
    skc->scale(scale, scale);  // backing-scale CTM, like a 2x display
    SkiaCanvas canvas(skc);
    v.paint_all(canvas);

    std::vector<uint8_t> out(static_cast<std::size_t>(pw) * ph * 4);
    REQUIRE(surface->readPixels(info, out.data(), pw * 4, 0, 0));
    return out;
}

// Build a nontrivial tree: a gradient-filled root holding a bordered, rounded,
// inset-shadowed CountingView child. `out_child` receives the child pointer.
std::unique_ptr<View> build_tree(CountingView** out_child) {
    auto root = std::make_unique<View>();
    root->set_bounds({0, 0, kW, kH});
    root->set_background_gradient_linear(
        0, 0, 0, kH,
        {Color::rgba8(40, 44, 52, 255), Color::rgba8(90, 98, 112, 255)},
        {0.0f, 1.0f});

    auto child = std::make_unique<CountingView>();
    child->set_bounds({16, 16, 96, 96});
    child->set_background_color(Color::rgba8(230, 230, 235, 255));
    child->set_border(Color::rgba8(20, 20, 24, 255), 3.0f, 12.0f);
    child->set_box_shadow(0, 0, 6, 0, Color::rgba8(0, 0, 0, 120), /*inset=*/true);
    *out_child = child.get();
    root->add_child(std::move(child));
    return root;
}

}  // namespace

TEST_CASE("Cached subtree renders pixel-identical to the uncached path",
          "[view][subtree-cache][skia][issue-6262]") {
    CountingView* c_off = nullptr;
    CountingView* c_on = nullptr;
    auto uncached = build_tree(&c_off);
    auto cached = build_tree(&c_on);
    cached->set_subtree_cached(true);

    std::vector<uint8_t> px_off = render_view(*uncached);
    std::vector<uint8_t> px_on = render_view(*cached);

    // The cache recorded on the first frame (a miss) and replayed the picture;
    // the replayed pixels are byte-for-byte the direct-paint pixels.
    REQUIRE(px_on == px_off);
    REQUIRE(c_off->paints == 1);
    REQUIRE(c_on->paints == 1);
}

TEST_CASE("Cache is reused across frames — child paint() is not re-invoked",
          "[view][subtree-cache][skia][issue-6262]") {
    CountingView* child = nullptr;
    auto root = build_tree(&child);
    root->set_subtree_cached(true);

    std::vector<uint8_t> frame1 = render_view(*root);  // miss: records + draws
    REQUIRE(child->paints == 1);

    std::vector<uint8_t> frame2 = render_view(*root);  // hit: replays only
    // The non-tautology: with nothing mutated, the second frame replays the
    // recording and does NOT walk into the child, so paint() stayed at 1.
    REQUIRE(child->paints == 1);
    REQUIRE(frame2 == frame1);
}

TEST_CASE("Child mutation invalidates the cache and the pixels update",
          "[view][subtree-cache][skia][issue-6262]") {
    CountingView* child = nullptr;
    auto root = build_tree(&child);
    root->set_subtree_cached(true);

    std::vector<uint8_t> frame1 = render_view(*root);
    render_view(*root);  // establish a valid, reused cache
    REQUIRE(child->paints == 1);

    // Mutate visible content and announce it the way a real widget does. The
    // repaint request walks up the parent chain and stales the cached root.
    child->fill = Color::rgba8(220, 60, 50, 255);
    child->request_repaint();

    std::vector<uint8_t> frame3 = render_view(*root);
    REQUIRE(child->paints == 2);  // re-recorded → paint() ran again

    // Reference: a fresh, never-cached tree with the same mutation applied.
    CountingView* ref_child = nullptr;
    auto ref = build_tree(&ref_child);
    ref_child->fill = Color::rgba8(220, 60, 50, 255);
    std::vector<uint8_t> ref_px = render_view(*ref);

    REQUIRE(frame3 == ref_px);   // cache updated correctly
    REQUIRE(frame3 != frame1);   // and the mutation is actually visible
}

TEST_CASE("Structural change (add_child) invalidates the cache",
          "[view][subtree-cache][skia][issue-6262]") {
    CountingView* child = nullptr;
    auto root = build_tree(&child);
    root->set_subtree_cached(true);

    std::vector<uint8_t> frame1 = render_view(*root);
    REQUIRE(child->paints == 1);

    // Graft a second child. add_child stales the (cached) parent.
    auto extra = std::make_unique<CountingView>();
    extra->set_bounds({40, 40, 40, 40});
    extra->fill = Color::rgba8(250, 210, 60, 255);
    root->add_child(std::move(extra));

    std::vector<uint8_t> frame2 = render_view(*root);
    REQUIRE(child->paints == 2);   // subtree re-recorded
    REQUIRE(frame2 != frame1);     // the new child is visible

    // Reference: a fresh tree assembled with both children, never cached.
    CountingView* ref_child = nullptr;
    auto ref = build_tree(&ref_child);
    auto ref_extra = std::make_unique<CountingView>();
    ref_extra->set_bounds({40, 40, 40, 40});
    ref_extra->fill = Color::rgba8(250, 210, 60, 255);
    ref->add_child(std::move(ref_extra));
    REQUIRE(frame2 == render_view(*ref));
}

TEST_CASE("Resize (set_bounds) invalidates the cache",
          "[view][subtree-cache][skia][issue-6262]") {
    CountingView* child = nullptr;
    auto root = build_tree(&child);
    root->set_subtree_cached(true);

    std::vector<uint8_t> frame1 = render_view(*root);
    REQUIRE(child->paints == 1);

    child->set_bounds({16, 16, 64, 64});  // shrink the child

    std::vector<uint8_t> frame2 = render_view(*root);
    REQUIRE(child->paints == 2);
    REQUIRE(frame2 != frame1);

    CountingView* ref_child = nullptr;
    auto ref = build_tree(&ref_child);
    ref_child->set_bounds({16, 16, 64, 64});
    REQUIRE(frame2 == render_view(*ref));
}

TEST_CASE("Layer independence — animating opacity does not re-record",
          "[view][subtree-cache][skia][issue-6262]") {
    CountingView* child = nullptr;
    auto root = build_tree(&child);
    root->set_opacity(0.5f);
    root->set_subtree_cached(true);

    std::vector<uint8_t> frame1 = render_view(*root);
    REQUIRE(child->paints == 1);

    // Opacity is applied by the LIVE compositing layer (push_effect_layers),
    // outside the cached content. Changing it does not stale the cache.
    root->set_opacity(0.8f);
    std::vector<uint8_t> frame2 = render_view(*root);
    REQUIRE(child->paints == 1);   // NOT re-recorded
    REQUIRE(frame2 != frame1);     // but the new opacity is applied live

    // Reference: a fresh, never-cached tree painted at opacity 0.8.
    CountingView* ref_child = nullptr;
    auto ref = build_tree(&ref_child);
    ref->set_opacity(0.8f);
    REQUIRE(frame2 == render_view(*ref));
}

// FIX 1 coverage: content-mutating setters that do NOT call request_repaint
// must still stale the cache. Without the invalidate wiring these leave stale
// pixels — the exact gap the review flagged. This exercises the setters
// DIRECTLY (no request_repaint), simulating the continuous-frames case where an
// unrelated force-repaint replays the stale picture.
TEST_CASE("set_background_color invalidates the cache without a repaint call",
          "[view][subtree-cache][skia][issue-6262]") {
    auto build = [](Color bg) {
        auto root = std::make_unique<View>();
        root->set_bounds({0, 0, kW, kH});
        root->set_background_color(bg);
        auto child = std::make_unique<CountingView>();
        child->set_bounds({40, 40, 48, 48});  // leaves the bg visible around it
        root->add_child(std::move(child));
        return root;
    };

    auto root = build(Color::rgba8(40, 60, 200, 255));  // blue
    root->set_subtree_cached(true);
    std::vector<uint8_t> frame1 = render_view(*root);
    render_view(*root);  // establish a valid, reused cache

    // Direct setter — no request_repaint(). Must invalidate anyway.
    root->set_background_color(Color::rgba8(200, 60, 40, 255));  // red
    std::vector<uint8_t> frame3 = render_view(*root);
    REQUIRE(frame3 != frame1);  // stale-pixel bug would keep this blue

    auto ref = build(Color::rgba8(200, 60, 40, 255));
    REQUIRE(frame3 == render_view(*ref));
}

TEST_CASE("set_visible on a child invalidates the cached parent without a repaint",
          "[view][subtree-cache][skia][issue-6262]") {
    auto root = std::make_unique<View>();
    root->set_bounds({0, 0, kW, kH});
    root->set_background_color(Color::rgba8(230, 230, 235, 255));
    auto child = std::make_unique<CountingView>();
    child->set_bounds({32, 32, 64, 64});
    CountingView* cp = child.get();
    root->add_child(std::move(child));
    root->set_subtree_cached(true);

    std::vector<uint8_t> frame1 = render_view(*root);  // child visible
    render_view(*root);

    cp->set_visible(false);  // direct, no request_repaint()
    std::vector<uint8_t> frame2 = render_view(*root);
    REQUIRE(frame2 != frame1);  // the child must disappear from the replay

    // Reference: fresh, uncached, child hidden from the start.
    auto ref = std::make_unique<View>();
    ref->set_bounds({0, 0, kW, kH});
    ref->set_background_color(Color::rgba8(230, 230, 235, 255));
    auto ref_child = std::make_unique<CountingView>();
    ref_child->set_bounds({32, 32, 64, 64});
    ref_child->set_visible(false);
    ref->add_child(std::move(ref_child));
    REQUIRE(frame2 == render_view(*ref));
}

// FIX 4 coverage: resolution independence. The recording is logical-space, so a
// cached replay must equal a direct paint at any backing scale.
TEST_CASE("Cached replay equals direct paint at 1x and 2x backing scale",
          "[view][subtree-cache][skia][issue-6262]") {
    for (float scale : {1.0f, 2.0f}) {
        INFO("backing scale " << scale);
        CountingView* c_off = nullptr;
        CountingView* c_on = nullptr;
        auto uncached = build_tree(&c_off);
        auto cached = build_tree(&c_on);
        cached->set_subtree_cached(true);

        std::vector<uint8_t> direct = render_view_at_scale(*uncached, scale);
        std::vector<uint8_t> replay = render_view_at_scale(*cached, scale);
        REQUIRE(replay == direct);
    }
}

#endif  // PULP_HAS_SKIA
