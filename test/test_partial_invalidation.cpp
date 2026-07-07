// Sub-view rect-level partial invalidation: a live sub-view (meter, grid,
// custom overlay) invalidates only its own area via request_repaint(Rect), so a
// host that consults the accumulated dirty region can skip re-compositing the
// static chrome. These tests pin the platform-agnostic core: the local->root
// coordinate mapping, the WindowHost dirty-region accumulation, the
// full-repaint escalations (no-arg repaint, render transform, empty rect), and
// that a bounded update is a small fraction of the full surface.

#include <catch2/catch_test_macros.hpp>

#include <pulp/view/view.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/window_host.hpp>

#include <functional>
#include <memory>

using namespace pulp::view;

namespace {

// Minimal headless WindowHost: no window, no render loop. schedule_repaint()
// falls through to repaint() (a no-op here); the dirty-region accumulation
// under test lives entirely in the WindowHost base class.
class TestWindowHost : public WindowHost {
public:
    void show() override {}
    void hide() override {}
    bool is_visible() const override { return true; }
    void repaint() override { ++repaint_calls; }
    void set_close_callback(std::function<void()>) override {}
    void run_event_loop() override {}

    int repaint_calls = 0;
};

// Build a root with `child` (kept as a raw pointer) added, then attach the host
// (set_window_host walks existing children) and clear the always-full first
// frame so bounded marks accumulate.
std::unique_ptr<View> make_root_with(View*& out_child, Rect root_bounds, Rect child_bounds) {
    auto root = std::make_unique<View>();
    root->set_bounds(root_bounds);
    auto child = std::make_unique<View>();
    child->set_bounds(child_bounds);
    out_child = child.get();
    root->add_child(std::move(child));
    return root;
}

}  // namespace

TEST_CASE("bounded request_repaint invalidates only the sub-view's root rect",
          "[view][partial-render]") {
    TestWindowHost host;
    View* meter = nullptr;
    auto root = make_root_with(meter, {0, 0, 400, 300}, {10, 20, 30, 120});
    root->set_window_host(&host);

    // Past the always-full first frame (a host clears after it paints).
    host.clear_pending_dirty();
    REQUIRE_FALSE(host.pending_repaint_is_full());

    // The meter repaints only its own local area.
    meter->request_repaint(meter->local_bounds());  // local {0,0,30,120}

    REQUIRE_FALSE(host.pending_repaint_is_full());
    REQUIRE(host.has_pending_dirty_bounds());
    const Rect d = host.pending_dirty_bounds();
    REQUIRE(d.x == 10.0f);
    REQUIRE(d.y == 20.0f);
    REQUIRE(d.width == 30.0f);
    REQUIRE(d.height == 120.0f);

    // The whole point: the dirty area is a small fraction of the surface, so
    // the static chrome does not re-composite.
    const float dirty_area = d.width * d.height;   // 3600
    const float full_area = 400.0f * 300.0f;       // 120000
    REQUIRE(dirty_area < full_area * 0.1f);
}

TEST_CASE("a nested sub-view maps through accumulated parent offsets",
          "[view][partial-render]") {
    TestWindowHost host;
    // root(0,0,400,300) > panel(50,40,200,200) > meter(10,20,30,120 local)
    auto root = std::make_unique<View>();
    root->set_bounds({0, 0, 400, 300});
    auto panel = std::make_unique<View>();
    panel->set_bounds({50, 40, 200, 200});
    auto meter_o = std::make_unique<View>();
    meter_o->set_bounds({10, 20, 30, 120});
    View* meter = meter_o.get();
    panel->add_child(std::move(meter_o));
    root->add_child(std::move(panel));
    root->set_window_host(&host);
    host.clear_pending_dirty();

    meter->request_repaint(meter->local_bounds());

    REQUIRE_FALSE(host.pending_repaint_is_full());
    const Rect d = host.pending_dirty_bounds();
    // root-space origin = panel.origin (50,40) + meter.origin (10,20) = (60,60)
    REQUIRE(d.x == 60.0f);
    REQUIRE(d.y == 60.0f);
    REQUIRE(d.width == 30.0f);
    REQUIRE(d.height == 120.0f);
}

TEST_CASE("two disjoint sub-view invalidations coalesce to their bounding box",
          "[view][partial-render]") {
    TestWindowHost host;
    auto root = std::make_unique<View>();
    root->set_bounds({0, 0, 400, 300});
    auto a_o = std::make_unique<View>(); a_o->set_bounds({10, 10, 20, 20});
    auto b_o = std::make_unique<View>(); b_o->set_bounds({100, 100, 20, 20});
    View* a = a_o.get();
    View* b = b_o.get();
    root->add_child(std::move(a_o));
    root->add_child(std::move(b_o));
    root->set_window_host(&host);
    host.clear_pending_dirty();

    a->request_repaint(a->local_bounds());  // root {10,10,20,20}
    b->request_repaint(b->local_bounds());  // root {100,100,20,20}

    REQUIRE_FALSE(host.pending_repaint_is_full());
    const Rect d = host.pending_dirty_bounds();
    REQUIRE(d.x == 10.0f);
    REQUIRE(d.y == 10.0f);
    REQUIRE(d.width == 110.0f);   // 100+20 - 10
    REQUIRE(d.height == 110.0f);
}

TEST_CASE("no-arg request_repaint (e.g. theme change) forces a full repaint",
          "[view][partial-render]") {
    TestWindowHost host;
    View* child = nullptr;
    auto root = make_root_with(child, {0, 0, 400, 300}, {10, 20, 30, 120});
    root->set_window_host(&host);
    host.clear_pending_dirty();

    root->request_repaint();

    REQUIRE(host.pending_repaint_is_full());
}

TEST_CASE("a render transform on the view or an ancestor falls back to full repaint",
          "[view][partial-render]") {
    TestWindowHost host;
    View* meter = nullptr;
    auto root = make_root_with(meter, {0, 0, 400, 300}, {10, 20, 30, 120});
    // A transform anywhere on the ancestor chain makes the plain offset mapping
    // wrong, so bounded invalidation must escalate to full.
    root->set_translate(5.0f, 0.0f);
    root->set_window_host(&host);
    host.clear_pending_dirty();

    meter->request_repaint(meter->local_bounds());

    REQUIRE(host.pending_repaint_is_full());
}

TEST_CASE("an empty dirty rect escalates to a full repaint", "[view][partial-render]") {
    TestWindowHost host;
    View* meter = nullptr;
    auto root = make_root_with(meter, {0, 0, 400, 300}, {10, 20, 30, 120});
    root->set_window_host(&host);
    host.clear_pending_dirty();

    meter->request_repaint(Rect{0, 0, 0, 0});

    REQUIRE(host.pending_repaint_is_full());
}

TEST_CASE("a full mark this frame is sticky against a later bounded mark",
          "[view][partial-render]") {
    TestWindowHost host;
    View* meter = nullptr;
    auto root = make_root_with(meter, {0, 0, 400, 300}, {10, 20, 30, 120});
    root->set_window_host(&host);
    host.clear_pending_dirty();

    root->request_repaint();                          // full
    meter->request_repaint(meter->local_bounds());    // bounded, but too late

    REQUIRE(host.pending_repaint_is_full());  // never shrinks a full repaint
}

TEST_CASE("a scrolled ScrollView ancestor escalates a child to full repaint",
          "[view][partial-render]") {
    // ScrollView::paint_all translates children by (-scroll_x, -scroll_y), so a
    // scrolled sub-view no longer sits at a plain bounds offset. The bounded
    // path must escalate to full rather than target the unscrolled root rect.
    TestWindowHost host;
    auto scroll = std::make_unique<ScrollView>();
    scroll->set_bounds({0, 0, 400, 300});
    scroll->set_content_size({400, 1000});  // taller than viewport → scrollable
    auto meter_o = std::make_unique<View>();
    meter_o->set_bounds({10, 20, 30, 120});
    View* meter = meter_o.get();
    scroll->add_child(std::move(meter_o));
    scroll->set_window_host(&host);

    // Unscrolled: the plain offset holds, so a bounded mark stays bounded.
    host.clear_pending_dirty();
    meter->request_repaint(meter->local_bounds());
    REQUIRE_FALSE(host.pending_repaint_is_full());

    // Scroll down; now the child's painted position is offset by the scroll.
    scroll->set_scroll(0.0f, 40.0f);
    REQUIRE(scroll->scroll_y() == 40.0f);
    REQUIRE(scroll->applies_child_paint_offset());

    host.clear_pending_dirty();
    meter->request_repaint(meter->local_bounds());
    REQUIRE(host.pending_repaint_is_full());  // escalated: never the wrong rect
}

TEST_CASE("a pixel-spreading filter on an ancestor escalates to full repaint",
          "[view][partial-render]") {
    // filter: blur re-samples neighbors, so a child's changed pixels spread
    // past its mapped box. Bounded invalidation must escalate to full to avoid
    // leaving a stale blurred halo around the updated region.
    TestWindowHost host;
    View* meter = nullptr;
    auto root = make_root_with(meter, {0, 0, 400, 300}, {10, 20, 30, 120});
    root->set_filter_blur(8.0f);
    root->set_window_host(&host);
    host.clear_pending_dirty();

    REQUIRE(root->has_filter_effect());
    meter->request_repaint(meter->local_bounds());

    REQUIRE(host.pending_repaint_is_full());
}

TEST_CASE("a live Meter level update invalidates only the meter's own rect",
          "[view][partial-render]") {
    // The canonical per-frame case: a meter driven from an audio source repaints
    // every tick. With bounded invalidation it dirties only its own rect, so a
    // plugin's static chrome is not re-composited on every level change.
    TestWindowHost host;
    auto root = std::make_unique<View>();
    root->set_bounds({0, 0, 400, 300});
    auto meter_o = std::make_unique<Meter>();
    meter_o->set_bounds({40, 30, 20, 120});
    Meter* meter = meter_o.get();
    root->add_child(std::move(meter_o));
    root->set_window_host(&host);
    host.clear_pending_dirty();

    meter->set_level(0.6f, 0.85f);

    REQUIRE_FALSE(host.pending_repaint_is_full());
    const Rect d = host.pending_dirty_bounds();
    // Must fully CONTAIN the meter's mapped rect [40,30]-[60,150] (a small
    // peak-line overscan past the box is allowed — the meter strokes its peak
    // line centered on the extreme edge), and must stay a small fraction of the
    // 400x300 surface (bounded, not full).
    REQUIRE(d.x <= 40.0f);
    REQUIRE(d.y <= 30.0f);
    REQUIRE(d.x + d.width >= 60.0f);
    REQUIRE(d.y + d.height >= 150.0f);
    REQUIRE(d.width * d.height < 400.0f * 300.0f * 0.1f);
}

TEST_CASE("a Meter ballistic update also stays bounded to its own rect",
          "[view][partial-render]") {
    TestWindowHost host;
    auto root = std::make_unique<View>();
    root->set_bounds({0, 0, 400, 300});
    auto meter_o = std::make_unique<Meter>();
    meter_o->set_bounds({10, 10, 30, 200});
    Meter* meter = meter_o.get();
    root->add_child(std::move(meter_o));
    root->set_window_host(&host);
    host.clear_pending_dirty();

    meter->update(0.9f, 0.5f, 0.016f);

    REQUIRE_FALSE(host.pending_repaint_is_full());
    const Rect d = host.pending_dirty_bounds();
    // Contains the meter's mapped rect [10,10]-[40,210] (peak-line overscan
    // allowed), still bounded well under the full surface.
    REQUIRE(d.x <= 10.0f);
    REQUIRE(d.y <= 10.0f);
    REQUIRE(d.x + d.width >= 40.0f);
    REQUIRE(d.y + d.height >= 210.0f);
    REQUIRE(d.width * d.height < 400.0f * 300.0f * 0.1f);
}

TEST_CASE("a Meter's bounded invalidation overscans its peak-line edge bleed",
          "[view][partial-render]") {
    // The peak line is stroked centered on the extreme edge, bleeding ~1px past
    // local_bounds() at full scale. The invalidation must extend past every edge
    // so the fringe is re-cleared when the peak decays (no stale line on chrome).
    TestWindowHost host;
    auto root = std::make_unique<View>();
    root->set_bounds({0, 0, 400, 300});
    auto meter_o = std::make_unique<Meter>();
    meter_o->set_bounds({40, 30, 20, 120});
    Meter* meter = meter_o.get();
    root->add_child(std::move(meter_o));
    root->set_window_host(&host);
    host.clear_pending_dirty();

    meter->set_level(1.0f, 1.0f);  // peak at the very top edge → 1px spill

    REQUIRE_FALSE(host.pending_repaint_is_full());
    const Rect d = host.pending_dirty_bounds();
    REQUIRE(d.x < 40.0f);            // past the left edge
    REQUIRE(d.y < 30.0f);            // past the top edge (the peak-line bleed)
    REQUIRE(d.x + d.width > 60.0f);  // past the right edge
    REQUIRE(d.y + d.height > 150.0f);// past the bottom edge
}

TEST_CASE("bounded request_repaint with no host attached is a safe no-op",
          "[view][partial-render]") {
    // No window host: the bounded path has no invalidator, so it falls back to
    // the (also no-op) full request_repaint(). Must not crash or dereference.
    View orphan;
    orphan.set_bounds({0, 0, 100, 100});
    REQUIRE_NOTHROW(orphan.request_repaint(orphan.local_bounds()));
}

TEST_CASE("bounded invalidation schedules a repaint like the no-arg path",
          "[view][partial-render]") {
    TestWindowHost host;
    View* meter = nullptr;
    auto root = make_root_with(meter, {0, 0, 400, 300}, {10, 20, 30, 120});
    root->set_window_host(&host);
    host.clear_pending_dirty();

    const int before = host.repaint_calls;
    meter->request_repaint(meter->local_bounds());
    REQUIRE(host.repaint_calls == before + 1);  // no render loop → direct repaint()
}
