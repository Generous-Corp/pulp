// FU-2 / WI-17 — partial-repaint correctness proof.
//
// Two layers of test, no window and no GPU:
//
//  1. Pure unit tests for `compute_effective_damage` — the damage model in
//     isolation: hazard-free trees return the bounded (snapped) rect; a
//     sampling hazard (blur / backdrop-filter / mask / transformed view)
//     reaching the damage escalates to full; the device-pixel snap-out is
//     exact.
//
//  2. A screenshot-equivalence harness (Skia raster). For each case it renders
//     (a) a FULL repaint of a mutation and (b) a clipped repaint of the SAME
//     mutation into a copy of the previous frame — mirroring the persistent-
//     scene host wiring — and asserts the two are BYTE-IDENTICAL. The hazard
//     cases additionally carry a "naive clip WOULD differ" negative proof: the
//     same mutation clipped to the un-escalated rect produces DIFFERENT pixels,
//     proving the hazard escalation does real work and the equivalence is never
//     a vacuous full == full.
//
// Tag: [view][partial-render][issue-6262]

#include <catch2/catch_test_macros.hpp>

#include <pulp/view/repaint_damage.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/window_host.hpp>

#include <functional>
#include <memory>
#include <vector>

using namespace pulp::view;

namespace {

// Minimal host so View::request_repaint(Rect) routes through the real producer
// (ancestor-origin mapping + transform/filter/scroll escalation) and we can read
// back what it decided.
class CapturingHost : public WindowHost {
public:
    void show() override {}
    void hide() override {}
    bool is_visible() const override { return true; }
    void repaint() override {}
    void set_close_callback(std::function<void()>) override {}
    void run_event_loop() override {}
};

// Route a bounded repaint request for `target` (its own local rect) through the
// producer and return what the host accumulated: the root-space bounds, or a
// "full" escalation.
struct Requested {
    bool full = false;
    Rect bounds{};
};
Requested request_through_producer(View& root, View& target, Rect local_rect) {
    CapturingHost host;
    root.set_window_host(&host);
    host.clear_pending_dirty();  // past the always-full first frame
    target.request_repaint(local_rect);
    Requested r;
    r.full = host.pending_repaint_is_full();
    if (!r.full && host.has_pending_dirty_bounds())
        r.bounds = host.pending_dirty_bounds();
    root.set_window_host(nullptr);
    return r;
}

}  // namespace

// ── Pure damage-model unit tests (no Skia) ───────────────────────────────

TEST_CASE("compute_effective_damage returns a bounded rect for a hazard-free tree",
          "[view][partial-render][issue-6262]") {
    auto root = std::make_unique<View>();
    root->set_bounds({0, 0, 400, 300});
    auto child = std::make_unique<View>();
    child->set_bounds({40, 30, 20, 20});
    root->add_child(std::move(child));

    const auto d = compute_effective_damage(*root, Rect{40, 30, 20, 20}, 1.0f);
    REQUIRE_FALSE(d.full);
    REQUIRE(d.bounds.x == 40);
    REQUIRE(d.bounds.y == 30);
    REQUIRE(d.bounds.width == 20);
    REQUIRE(d.bounds.height == 20);
}

TEST_CASE("compute_effective_damage escalates when a blurred view reaches the damage",
          "[view][partial-render][issue-6262]") {
    auto root = std::make_unique<View>();
    root->set_bounds({0, 0, 400, 300});
    auto blurred = std::make_unique<View>();
    blurred->set_bounds({30, 20, 60, 60});   // box overlaps the damage below
    blurred->set_filter_blur(8.0f);
    root->add_child(std::move(blurred));

    const auto d = compute_effective_damage(*root, Rect{40, 40, 20, 20}, 1.0f);
    REQUIRE(d.full);
}

TEST_CASE("compute_effective_damage escalates for a backdrop-filter view near the damage",
          "[view][partial-render][issue-6262]") {
    auto root = std::make_unique<View>();
    root->set_bounds({0, 0, 400, 300});
    auto panel = std::make_unique<View>();
    panel->set_bounds({70, 40, 40, 40});     // 10px gap from the damage's right edge
    panel->set_backdrop_blur(20.0f);         // reach exceeds the gap → hazard
    root->add_child(std::move(panel));

    const auto d = compute_effective_damage(*root, Rect{40, 40, 20, 20}, 1.0f);
    REQUIRE(d.full);
}

TEST_CASE("compute_effective_damage keeps a bounded rect when the hazard is far away",
          "[view][partial-render][issue-6262]") {
    auto root = std::make_unique<View>();
    root->set_bounds({0, 0, 400, 300});
    auto blurred = std::make_unique<View>();
    blurred->set_bounds({300, 200, 40, 40});  // far from the damage at (40,40)
    blurred->set_filter_blur(6.0f);
    root->add_child(std::move(blurred));

    const auto d = compute_effective_damage(*root, Rect{40, 40, 20, 20}, 1.0f);
    REQUIRE_FALSE(d.full);
    REQUIRE(d.bounds.x == 40);
}

TEST_CASE("compute_effective_damage: colour-only filter chain is not a hazard",
          "[view][partial-render][issue-6262]") {
    auto root = std::make_unique<View>();
    root->set_bounds({0, 0, 400, 300});
    auto tinted = std::make_unique<View>();
    tinted->set_bounds({30, 30, 50, 50});
    tinted->set_filter_chain({{View::FilterOp::Kind::brightness, 1.2f}});  // point-wise
    root->add_child(std::move(tinted));

    const auto d = compute_effective_damage(*root, Rect{40, 40, 20, 20}, 1.0f);
    REQUIRE_FALSE(d.full);
}

TEST_CASE("compute_effective_damage: a blur op inside a filter chain IS a hazard",
          "[view][partial-render][issue-6262]") {
    auto root = std::make_unique<View>();
    root->set_bounds({0, 0, 400, 300});
    auto v = std::make_unique<View>();
    v->set_bounds({30, 30, 50, 50});
    v->set_filter_chain({{View::FilterOp::Kind::blur, 6.0f}});
    root->add_child(std::move(v));

    const auto d = compute_effective_damage(*root, Rect{40, 40, 20, 20}, 1.0f);
    REQUIRE(d.full);
}

TEST_CASE("compute_effective_damage: a masked view intersecting the damage escalates",
          "[view][partial-render][issue-6262]") {
    auto root = std::make_unique<View>();
    root->set_bounds({0, 0, 400, 300});
    auto masked = std::make_unique<View>();
    masked->set_bounds({40, 40, 40, 40});
    masked->set_mask_image("linear-gradient(black, transparent)");
    root->add_child(std::move(masked));

    const auto d = compute_effective_damage(*root, Rect{50, 50, 10, 10}, 1.0f);
    REQUIRE(d.full);
}

TEST_CASE("compute_effective_damage: a render-transformed view has an unknown box → full",
          "[view][partial-render][issue-6262]") {
    auto root = std::make_unique<View>();
    root->set_bounds({0, 0, 400, 300});
    // The transformed view is ALSO a sampling hazard (its box is unknown, so it
    // is assumed to reach the damage). A plain transformed view with no sampling
    // is not a damage hazard by itself, so give it a blur to make the "unknown
    // box" branch observable.
    auto spun = std::make_unique<View>();
    spun->set_bounds({200, 150, 40, 40});
    spun->set_rotation(30.0f);
    spun->set_filter_blur(4.0f);
    root->add_child(std::move(spun));

    const auto d = compute_effective_damage(*root, Rect{40, 40, 20, 20}, 1.0f);
    REQUIRE(d.full);  // unknown box under a transform reaches everywhere
}

TEST_CASE("compute_effective_damage snaps the bounded rect out to the device grid",
          "[view][partial-render][issue-6262]") {
    auto root = std::make_unique<View>();
    root->set_bounds({0, 0, 400, 300});

    // A fractional rect at 2x: left/top floor to the device grid, right/bottom
    // ceil. x=10.3 → floor(20.6)/2 = 10.0; right=10.3+5.4=15.7 → ceil(31.4)/2 =
    // 16.0. So {10.0, .., 6.0 wide}.
    const auto d = compute_effective_damage(*root, Rect{10.3f, 10.3f, 5.4f, 5.4f}, 2.0f);
    REQUIRE_FALSE(d.full);
    REQUIRE(d.bounds.x == 10.0f);
    REQUIRE(d.bounds.y == 10.0f);
    REQUIRE(d.bounds.width == 6.0f);
    REQUIRE(d.bounds.height == 6.0f);
}

TEST_CASE("compute_effective_damage: an empty request escalates to full",
          "[view][partial-render][issue-6262]") {
    auto root = std::make_unique<View>();
    root->set_bounds({0, 0, 400, 300});
    const auto d = compute_effective_damage(*root, Rect{10, 10, 0, 0}, 1.0f);
    REQUIRE(d.full);
}

// ── Producer escalation (the rects the host would ever try to clip) ──────

TEST_CASE("producer maps a bounded child repaint to root space",
          "[view][partial-render][issue-6262]") {
    auto root = std::make_unique<View>();
    root->set_bounds({0, 0, 400, 300});
    auto panel = std::make_unique<View>();
    panel->set_bounds({100, 80, 200, 150});
    View* pptr = panel.get();
    auto meter = std::make_unique<View>();
    meter->set_bounds({20, 10, 30, 60});     // local to panel
    View* mptr = meter.get();
    pptr->add_child(std::move(meter));
    root->add_child(std::move(panel));

    const auto r = request_through_producer(*root, *mptr, Rect{0, 0, 30, 60});
    REQUIRE_FALSE(r.full);
    // origin = panel(100,80) + meter(20,10) = (120,90)
    REQUIRE(r.bounds.x == 120);
    REQUIRE(r.bounds.y == 90);
    REQUIRE(r.bounds.width == 30);
    REQUIRE(r.bounds.height == 60);
}

TEST_CASE("producer escalates a repaint under a scrolled container to full",
          "[view][partial-render][issue-6262]") {
    auto root = std::make_unique<View>();
    root->set_bounds({0, 0, 400, 300});
    auto scroller = std::make_unique<ScrollView>();
    scroller->set_bounds({0, 0, 200, 200});
    scroller->set_content_size({200, 600});   // content taller than the viewport
    scroller->set_scroll(0, 40);              // a real, non-clamped scroll offset
    ScrollView* sptr = scroller.get();
    auto item = std::make_unique<View>();
    item->set_bounds({10, 10, 40, 40});
    View* iptr = item.get();
    sptr->add_child(std::move(item));
    root->add_child(std::move(scroller));

    const auto r = request_through_producer(*root, *iptr, Rect{0, 0, 40, 40});
    REQUIRE(r.full);
}

TEST_CASE("producer escalates a repaint under a transformed ancestor to full",
          "[view][partial-render][issue-6262]") {
    auto root = std::make_unique<View>();
    root->set_bounds({0, 0, 400, 300});
    auto spun = std::make_unique<View>();
    spun->set_bounds({50, 50, 100, 100});
    spun->set_rotation(15.0f);
    View* sptr = spun.get();
    auto item = std::make_unique<View>();
    item->set_bounds({10, 10, 30, 30});
    View* iptr = item.get();
    sptr->add_child(std::move(item));
    root->add_child(std::move(spun));

    const auto r = request_through_producer(*root, *iptr, Rect{0, 0, 30, 30});
    REQUIRE(r.full);
}

// ── Screenshot-equivalence harness (Skia raster) ─────────────────────────

#ifdef PULP_HAS_SKIA

#include <pulp/canvas/skia_canvas.hpp>
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkSurface.h"

namespace {

constexpr int kW = 160;
constexpr int kH = 120;

using pulp::canvas::Color;
using pulp::canvas::SkiaCanvas;

sk_sp<SkSurface> make_surface() {
    SkImageInfo info = SkImageInfo::Make(kW, kH, kN32_SkColorType,
                                         kPremul_SkAlphaType,
                                         SkColorSpace::MakeSRGB());
    auto s = SkSurfaces::Raster(info);
    REQUIRE(s != nullptr);
    return s;
}

std::vector<uint8_t> readback(SkSurface& s) {
    SkImageInfo info = SkImageInfo::Make(kW, kH, kN32_SkColorType,
                                         kPremul_SkAlphaType,
                                         SkColorSpace::MakeSRGB());
    std::vector<uint8_t> out(static_cast<size_t>(kW) * kH * 4);
    REQUIRE(s.readPixels(info, out.data(), kW * 4, 0, 0));
    return out;
}

// Count differing pixels (4 bytes each). Returned as a scalar so Catch never has
// to stringify the multi-KB pixel vectors — a REQUIRE on the raw vectors both
// spams and (on a mismatch) crashes Catch's text-flow formatter.
size_t diff_pixels(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    if (a.size() != b.size()) return a.size() + b.size();
    size_t n = 0;
    for (size_t i = 0; i + 3 < a.size(); i += 4)
        if (a[i] != b[i] || a[i + 1] != b[i + 1] ||
            a[i + 2] != b[i + 2] || a[i + 3] != b[i + 3])
            ++n;
    return n;
}

// Paint the scene exactly the way MacGpuWindowHost::paint_scene does: an
// optional clip wrapping the background fill AND the tree paint.
void paint_host_scene(SkSurface& s, View& root, const Rect* clip) {
    SkCanvas* skc = s.getCanvas();
    SkiaCanvas canvas(skc);
    const int saved = canvas.save_count();
    if (clip) {
        canvas.save();
        canvas.clip_rect(clip->x, clip->y, clip->width, clip->height);
    }
    canvas.set_fill_color(Color::rgba8(18, 20, 28));
    canvas.fill_rect(0, 0, kW, kH);
    // NOTE: deliberately no layout_children() — these fixtures place every view
    // with explicit set_bounds(), and a Yoga pass would override those manual
    // bounds (flex zeroes/stacks children with no flex props). The damage model
    // and clip logic operate purely on the set bounds.
    root.paint_all(canvas);
    if (clip) canvas.restore_to_count(saved);
}

// The core equivalence check. `build` makes a fresh tree and reports the view to
// mutate + its local rect. `mutate` applies the change. The driver:
//   frame 1  → full paint (the "preserved" scene, surface A)
//   mutate
//   requested = producer(root, target, local)
//   decision  = compute_effective_damage(root, requested)
//   frame 2A  → repaint A with the decision's clip (or full when escalated)
//   frame 2B  → fresh FULL paint of the mutated tree (surface B, the reference)
//   REQUIRE(A == B)                       // byte-identical
// Returns whether a bounded clip was actually used (so callers can assert the
// equivalence is not a vacuous full == full).
struct EquivOutcome {
    bool bounded_used = false;
};
EquivOutcome assert_clip_equivalent(
        const std::function<std::unique_ptr<View>(View*&, Rect&)>& build,
        const std::function<void(View&)>& mutate) {
    View* target = nullptr;
    Rect local{};
    auto tree = build(target, local);
    REQUIRE(target != nullptr);

    auto surfaceA = make_surface();
    paint_host_scene(*surfaceA, *tree, nullptr);  // preserved previous frame

    mutate(*target);

    const auto requested = request_through_producer(*tree, *target, local);
    DamageDecision decision{true, {}};
    if (!requested.full)
        decision = compute_effective_damage(*tree, requested.bounds, 1.0f);

    const Rect* clip = decision.full ? nullptr : &decision.bounds;
    paint_host_scene(*surfaceA, *tree, clip);     // clipped (or full) repaint
    const auto actual = readback(*surfaceA);

    auto surfaceB = make_surface();
    paint_host_scene(*surfaceB, *tree, nullptr);  // reference full repaint
    const auto expected = readback(*surfaceB);

    const size_t diffs = diff_pixels(actual, expected);
    INFO("differing pixels: " << diffs << " (bounded_used=" << (clip != nullptr) << ")");
    REQUIRE(diffs == 0);                           // pixel-identical
    return {clip != nullptr};
}

// The negative proof: force the NAIVE (un-escalated) clip — the target's own
// root-space rect — and REQUIRE the clipped repaint DIFFERS from a full repaint.
// This proves the hazard the model escalated on is real, not hypothetical.
void assert_naive_clip_differs(
        const std::function<std::unique_ptr<View>(View*&, Rect&)>& build,
        const std::function<void(View&)>& mutate) {
    View* target = nullptr;
    Rect local{};
    auto tree = build(target, local);
    REQUIRE(target != nullptr);

    auto surfaceA = make_surface();
    paint_host_scene(*surfaceA, *tree, nullptr);

    mutate(*target);

    const auto requested = request_through_producer(*tree, *target, local);
    REQUIRE_FALSE(requested.full);  // the producer itself must not escalate here
    const Rect naive = requested.bounds;

    paint_host_scene(*surfaceA, *tree, &naive);   // naive clip, NO hazard escalation
    const auto actual = readback(*surfaceA);

    auto surfaceB = make_surface();
    paint_host_scene(*surfaceB, *tree, nullptr);
    const auto expected = readback(*surfaceB);

    const size_t diffs = diff_pixels(actual, expected);
    INFO("naive-clip differing pixels: " << diffs);
    REQUIRE(diffs > 0);   // the naive clip is visibly wrong
}

// A view that fills its box with a mutable colour.
class FillView : public View {
public:
    Color fill = Color::rgba8(200, 80, 60, 255);
    void paint(pulp::canvas::Canvas& c) override {
        c.set_fill_color(fill);
        c.fill_rect(0, 0, bounds().width, bounds().height);
    }
};

}  // namespace

TEST_CASE("partial repaint: a meter update inside a static panel is pixel-identical",
          "[view][partial-render][skia][issue-6262]") {
    const auto out = assert_clip_equivalent(
        [](View*& target, Rect& local) {
            auto root = std::make_unique<View>();
            root->set_background_color(Color::rgba8(60, 64, 72, 255));
            auto meter = std::make_unique<FillView>();
            meter->set_bounds({60, 40, 30, 40});
            target = meter.get();
            local = {0, 0, 30, 40};
            root->add_child(std::move(meter));
            return root;
        },
        [](View& t) { static_cast<FillView&>(t).fill = Color::rgba8(60, 200, 90, 255); });
    REQUIRE(out.bounded_used);   // the payoff case really clipped
}

TEST_CASE("partial repaint: a mutation under an opaque occluding sibling is identical",
          "[view][partial-render][skia][issue-6262]") {
    const auto out = assert_clip_equivalent(
        [](View*& target, Rect& local) {
            auto root = std::make_unique<View>();
            root->set_background_color(Color::rgba8(30, 34, 40, 255));
            auto meter = std::make_unique<FillView>();
            meter->set_bounds({50, 40, 40, 40});
            target = meter.get();
            local = {0, 0, 40, 40};
            root->add_child(std::move(meter));
            // An opaque sibling painted AFTER (on top of) the meter, overlapping it.
            auto cover = std::make_unique<FillView>();
            cover->fill = Color::rgba8(220, 220, 40, 255);
            cover->set_bounds({70, 40, 40, 40});
            root->add_child(std::move(cover));
            return root;
        },
        [](View& t) { static_cast<FillView&>(t).fill = Color::rgba8(60, 120, 220, 255); });
    REQUIRE(out.bounded_used);
}

TEST_CASE("partial repaint: an analytic drop-shadow sibling crossing the damage is identical",
          "[view][partial-render][skia][issue-6262]") {
    // A box-shadow is an analytic, point-wise draw (not a sampling layer), so a
    // sibling whose shadow crosses the damage stays bounded and identical.
    const auto out = assert_clip_equivalent(
        [](View*& target, Rect& local) {
            auto root = std::make_unique<View>();
            root->set_background_color(Color::rgba8(40, 44, 52, 255));
            auto shadowed = std::make_unique<FillView>();
            shadowed->fill = Color::rgba8(230, 230, 235, 255);
            shadowed->set_bounds({30, 40, 30, 30});
            shadowed->set_box_shadow(12, 0, 6, 0, Color::rgba8(0, 0, 0, 160), false);
            root->add_child(std::move(shadowed));   // shadow reaches right, into the meter
            auto meter = std::make_unique<FillView>();
            meter->set_bounds({66, 40, 30, 30});
            target = meter.get();
            local = {0, 0, 30, 30};
            root->add_child(std::move(meter));
            return root;
        },
        [](View& t) { static_cast<FillView&>(t).fill = Color::rgba8(80, 200, 120, 255); });
    REQUIRE(out.bounded_used);
}

TEST_CASE("partial repaint: naive-clipping a mutation inside a blurred container differs",
          "[view][partial-render][skia][issue-6262]") {
    // A filter blur SAMPLES its own subtree with SkTileMode::kClamp, so a
    // uniform blurred SIBLING truncated at a clip does not actually change
    // pixels — clamp extends the edge. The real filter hazard is a mutation
    // INSIDE a blurred container: the blur spreads the change beyond the
    // mutated view's rect, so a naive clip to that rect leaves a stale halo.
    // The PRODUCER already escalates this (ancestor filter), which is exactly
    // why the host never tries a bounded clip here — this proves that escalation
    // is real work, not caution for its own sake.
    const auto make = [](FillView*& meter_out) {
        auto root = std::make_unique<View>();
        root->set_background_color(Color::rgba8(40, 44, 52, 255));
        auto blurred = std::make_unique<View>();
        blurred->set_bounds({30, 25, 70, 70});
        blurred->set_background_color(Color::rgba8(72, 78, 92, 255));
        blurred->set_filter_blur(9.0f);
        auto meter = std::make_unique<FillView>();
        meter->fill = Color::rgba8(240, 240, 250, 255);
        meter->set_bounds({20, 20, 30, 30});   // local to blurred → root (50,45,30,30)
        meter_out = meter.get();
        blurred->add_child(std::move(meter));
        root->add_child(std::move(blurred));
        return root;
    };
    const Rect meter_root{50, 45, 30, 30};

    // (a) The producer escalates a bounded repaint under the blurred ancestor.
    {
        FillView* m = nullptr;
        auto tree = make(m);
        const auto r = request_through_producer(*tree, *m, Rect{0, 0, 30, 30});
        REQUIRE(r.full);
    }

    // (b) Prove why: naively clipping to the meter's own rect leaves the blurred
    //     halo (the meter's change spread beyond that rect, inside the
    //     container) stale → different pixels from a full repaint.
    {
        FillView* m = nullptr;
        auto tree = make(m);
        auto surfaceA = make_surface();
        paint_host_scene(*surfaceA, *tree, nullptr);      // preserved frame
        m->fill = Color::rgba8(20, 20, 26, 255);          // mutate bright → dark
        paint_host_scene(*surfaceA, *tree, &meter_root);  // NAIVE clip to meter rect
        const auto actual = readback(*surfaceA);

        auto surfaceB = make_surface();
        paint_host_scene(*surfaceB, *tree, nullptr);      // full reference
        const auto expected = readback(*surfaceB);

        const size_t diffs = diff_pixels(actual, expected);
        INFO("blurred-container naive-clip differing pixels: " << diffs);
        REQUIRE(diffs > 0);
    }
}

TEST_CASE("partial repaint: a backdrop-filter panel over the damage escalates to full",
          "[view][partial-render][skia][issue-6262]") {
    // A backdrop-blur panel drawn ON TOP of the meter and extending well beyond
    // its rect. The panel SAMPLES the meter (its backdrop) and spreads it across
    // the whole panel, so a naive clip to the meter's rect leaves the panel's
    // halo OUTSIDE the clip compositing the stale (pre-mutation) backdrop.
    const auto build = [](View*& target, Rect& local) {
        auto root = std::make_unique<View>();
        root->set_background_color(Color::rgba8(40, 44, 52, 255));
        auto meter = std::make_unique<FillView>();
        meter->fill = Color::rgba8(10, 10, 14, 255);
        meter->set_bounds({66, 48, 24, 24});     // painted first (behind the panel)
        target = meter.get();
        local = {0, 0, 24, 24};
        root->add_child(std::move(meter));
        // Transparent-fill backdrop panel: only the blurred backdrop shows, so a
        // backdrop change is visible at full strength across the whole panel.
        auto panel = std::make_unique<FillView>();
        panel->fill = Color::rgba8(0, 0, 0, 0);
        panel->set_bounds({30, 30, 100, 60});
        panel->set_backdrop_blur(16.0f);
        root->add_child(std::move(panel));
        return root;
    };
    const auto mutate = [](View& t) {
        static_cast<FillView&>(t).fill = Color::rgba8(250, 250, 255, 255);
    };

    const auto out = assert_clip_equivalent(build, mutate);
    REQUIRE_FALSE(out.bounded_used);   // escalated
    assert_naive_clip_differs(build, mutate);
}

#endif  // PULP_HAS_SKIA
