// Widgets must request a BOUNDED repaint on their hot value path.
//
// The rect-less View::request_repaint() marks the whole surface dirty by
// design. Using it for a value tick means a knob drag re-composites a plug-in's
// entire static chrome on every mouse move, and partial repaint can never
// engage no matter how the host is wired. These tests pin the opt-in so a
// future edit cannot silently drop a widget back to full-surface invalidation.
//
// The halo matters for correctness, not just cost: a bounded repaint is only
// legal if it is pixel-identical to a full one, so the rect must cover every
// pixel the widget can touch (glow, focus ring, modulation arc).

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/canvas_widget.hpp>
#include <pulp/view/frame_cost_probe.hpp>
#include <pulp/view/plugin_view_host.hpp>
#include <pulp/view/svg_path_widget.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/window_host.hpp>

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>

using namespace pulp::view;

namespace {

class CapturingHost : public WindowHost {
public:
    void show() override {}
    void hide() override {}
    bool is_visible() const override { return true; }
    void repaint() override {}
    void set_close_callback(std::function<void()>) override {}
    void run_event_loop() override {}
};

struct Damage {
    bool full = false;
    Rect bounds{};
};

// Run `mutate` with a host attached and report what damage the producer
// accumulated. The first frame is always full, so clear before mutating.
template <typename F>
Damage damage_of(View& root, F&& mutate) {
    CapturingHost host;
    root.set_window_host(&host);
    host.clear_pending_dirty();
    mutate();
    Damage d;
    d.full = host.pending_repaint_is_full();
    if (!d.full && host.has_pending_dirty_bounds())
        d.bounds = host.pending_dirty_bounds();
    root.set_window_host(nullptr);
    return d;
}

}  // namespace

TEST_CASE("Knob value changes request a bounded repaint, not the whole surface",
          "[view][widgets][partial-repaint]") {
    auto root = std::make_unique<View>();
    root->set_bounds({0, 0, 400, 300});
    auto knob_owned = std::make_unique<Knob>();
    auto* knob = knob_owned.get();
    // Size through flex: layout_children() overwrites manual set_bounds().
    knob_owned->flex().preferred_width = 64;
    knob_owned->flex().preferred_height = 64;
    root->add_child(std::move(knob_owned));
    root->layout_children();
    const auto kb = knob->local_bounds();
    REQUIRE(kb.width > 0.0f);
    REQUIRE(kb.width < 200.0f);   // the knob really is small vs the surface

    const auto d = damage_of(*root, [&] { knob->set_value(0.75f); });

    REQUIRE_FALSE(d.full);
    // Covers the knob plus a halo, and nothing like the 400x300 surface.
    CHECK(d.bounds.width >= kb.width);
    CHECK(d.bounds.height >= kb.height);
    CHECK(d.bounds.width <= kb.width + 32.0f);
    CHECK(d.bounds.height <= kb.height + 32.0f);
    CHECK(d.bounds.width < 400.0f);
}

TEST_CASE("Knob bounded repaint includes a halo for glow and rings",
          "[view][widgets][partial-repaint]") {
    auto root = std::make_unique<View>();
    root->set_bounds({0, 0, 400, 300});
    auto knob_owned = std::make_unique<Knob>();
    auto* knob = knob_owned.get();
    // Size through flex: layout_children() overwrites manual set_bounds().
    knob_owned->flex().preferred_width = 64;
    knob_owned->flex().preferred_height = 64;
    root->add_child(std::move(knob_owned));
    root->layout_children();
    const auto kb = knob->local_bounds();
    REQUIRE(kb.width > 0.0f);
    REQUIRE(kb.width < 200.0f);   // the knob really is small vs the surface

    const auto d = damage_of(*root, [&] { knob->set_value(0.6f); });
    REQUIRE_FALSE(d.full);
    // Strictly larger than the widget box — a clipped repaint that stopped at
    // local_bounds() would clip a glow or focus ring and stop being
    // pixel-identical to a full repaint.
    CHECK(d.bounds.width > kb.width);
    CHECK(d.bounds.height > kb.height);
}

TEST_CASE("A redundant Knob write requests no repaint at all",
          "[view][widgets][partial-repaint]") {
    auto root = std::make_unique<View>();
    root->set_bounds({0, 0, 400, 300});
    auto knob_owned = std::make_unique<Knob>();
    auto* knob = knob_owned.get();
    // Size through flex: layout_children() overwrites manual set_bounds().
    knob_owned->flex().preferred_width = 64;
    knob_owned->flex().preferred_height = 64;
    root->add_child(std::move(knob_owned));
    root->layout_children();
    const auto kb = knob->local_bounds();
    REQUIRE(kb.width > 0.0f);
    REQUIRE(kb.width < 200.0f);   // the knob really is small vs the surface
    knob->set_value(0.5f);

    // Bridge sync loops write the same value repeatedly; that must stay free.
    const auto d = damage_of(*root, [&] { knob->set_value(0.5f); });
    CHECK_FALSE(d.full);
    CHECK(d.bounds.width == 0.0f);
    CHECK(d.bounds.height == 0.0f);
}

TEST_CASE("Fader value changes request a bounded repaint",
          "[view][widgets][partial-repaint]") {
    auto root = std::make_unique<View>();
    root->set_bounds({0, 0, 400, 300});
    auto fader_owned = std::make_unique<Fader>();
    auto* fader = fader_owned.get();
    fader_owned->flex().preferred_width = 30;
    fader_owned->flex().preferred_height = 180;
    root->add_child(std::move(fader_owned));
    root->layout_children();
    const auto fb = fader->local_bounds();
    REQUIRE(fb.width > 0.0f);

    const auto d = damage_of(*root, [&] { fader->set_value(0.9f); });
    REQUIRE_FALSE(d.full);
    CHECK(d.bounds.width >= fb.width);
    CHECK(d.bounds.width <= fb.width + 32.0f);
}

TEST_CASE("A structural Knob change still requests a full repaint",
          "[view][widgets][partial-repaint]") {
    // Only the hot VALUE path is bounded. Configuration changes may move
    // geometry or text metrics, so they keep the conservative full
    // invalidation — bounding those would be a correctness bug, not a win.
    auto root = std::make_unique<View>();
    root->set_bounds({0, 0, 400, 300});
    auto knob_owned = std::make_unique<Knob>();
    auto* knob = knob_owned.get();
    // Size through flex: layout_children() overwrites manual set_bounds().
    knob_owned->flex().preferred_width = 64;
    knob_owned->flex().preferred_height = 64;
    root->add_child(std::move(knob_owned));
    root->layout_children();
    const auto kb = knob->local_bounds();
    REQUIRE(kb.width > 0.0f);
    REQUIRE(kb.width < 200.0f);   // the knob really is small vs the surface

    const auto d = damage_of(*root, [&] { knob->set_label("Cutoff"); });
    CHECK(d.full);
}

// ── Modulated value display ─────────────────────────────────────────────────
//
// A modulator moving a control is drawn at display rate, so the played-value
// marker must be the cheapest thing a control can do: its own box, no layout,
// and never an edit (no on_change, value() untouched).

namespace {

struct KnobRig {
    std::unique_ptr<View> root = std::make_unique<View>();
    Knob* knob = nullptr;
    KnobRig() {
        root->set_bounds({0, 0, 400, 300});
        auto owned = std::make_unique<Knob>();
        knob = owned.get();
        owned->flex().preferred_width = 64;
        owned->flex().preferred_height = 64;
        root->add_child(std::move(owned));
        root->layout_children();
    }
};

} // namespace

TEST_CASE("Knob modulated value is display only and repaints its own box",
          "[view][widgets][partial-repaint][modulation]") {
    KnobRig rig;
    auto* knob = rig.knob;
    knob->set_value(0.8f);
    int changes = 0;
    knob->on_change = [&](float) { ++changes; };
    const auto kb = knob->local_bounds();
    const auto generation = View::layout_generation();

    const auto d = damage_of(*rig.root, [&] { knob->set_modulated_value(0.3f); });
    REQUIRE_FALSE(d.full);
    CHECK(d.bounds.width >= kb.width);
    CHECK(d.bounds.width <= kb.width + 32.0f);
    CHECK(knob->has_modulated_value());
    CHECK(knob->modulated_display_value() == 0.3f);
    // The base is what the user set, what a drag starts from and what the host
    // records: modulation moves none of it.
    CHECK(knob->value() == 0.8f);
    CHECK(changes == 0);
    CHECK(View::layout_generation() == generation);

    // The same played value again is free.
    const auto again = damage_of(*rig.root, [&] { knob->set_modulated_value(0.3f); });
    CHECK_FALSE(again.full);
    CHECK(again.bounds.width == 0.0f);

    const auto cleared = damage_of(*rig.root, [&] { knob->clear_modulated_value(); });
    CHECK_FALSE(cleared.full);
    CHECK(cleared.bounds.width > 0.0f);
    CHECK_FALSE(knob->has_modulated_value());
    CHECK(knob->modulated_display_value() == 0.8f);
    CHECK(changes == 0);
}

TEST_CASE("Fader modulated value is display only and repaints its own box",
          "[view][widgets][partial-repaint][modulation]") {
    auto root = std::make_unique<View>();
    root->set_bounds({0, 0, 400, 300});
    auto owned = std::make_unique<Fader>();
    auto* fader = owned.get();
    owned->flex().preferred_width = 30;
    owned->flex().preferred_height = 180;
    root->add_child(std::move(owned));
    root->layout_children();
    fader->set_value(0.6f);
    int changes = 0;
    fader->on_change = [&](float) { ++changes; };
    const auto fb = fader->local_bounds();

    const auto d = damage_of(*root, [&] { fader->set_modulated_value(0.2f); });
    REQUIRE_FALSE(d.full);
    CHECK(d.bounds.height >= fb.height);
    CHECK(d.bounds.width <= fb.width + 32.0f);
    CHECK(fader->value() == 0.6f);
    CHECK(fader->modulated_display_value() == 0.2f);
    CHECK(changes == 0);
}

// ── SvgPathWidget: an animated path repaints the old and new geometry ───────

TEST_CASE("SvgPathWidget path changes repaint the union of old and new geometry",
          "[view][widgets][partial-repaint][svg]") {
    auto root = std::make_unique<View>();
    root->set_bounds({0, 0, 400, 300});
    auto owned = std::make_unique<SvgPathWidget>();
    auto* path = owned.get();
    owned->flex().preferred_width = 200;
    owned->flex().preferred_height = 200;
    root->add_child(std::move(owned));
    root->layout_children();
    path->clear_fill();
    path->set_stroke_color(pulp::canvas::Color::rgba8(255, 255, 255));
    path->set_stroke_width(2.0f);
    path->set_path("M 10 10 L 20 10");

    // A needle that moves from the top-left to the bottom-right: the damage
    // must hold BOTH ends, or the old needle stays on screen.
    const auto d = damage_of(*root, [&] { path->set_path("M 150 150 L 160 160"); });
    REQUIRE_FALSE(d.full);
    CHECK(d.bounds.x <= 10.0f - 1.0f);
    CHECK(d.bounds.y <= 10.0f - 1.0f);
    CHECK(d.bounds.x + d.bounds.width >= 160.0f + 1.0f);
    CHECK(d.bounds.y + d.bounds.height >= 160.0f + 1.0f);
    // ... and not the whole surface.
    CHECK(d.bounds.width < 200.0f);

    // An unchanged string repaints nothing.
    const auto same = damage_of(*root, [&] { path->set_path("M 150 150 L 160 160"); });
    CHECK_FALSE(same.full);
    CHECK(same.bounds.width == 0.0f);
}

TEST_CASE("SvgPathWidget paint extent covers a stroke and follows the viewBox",
          "[view][widgets][partial-repaint][svg]") {
    SvgPathWidget path;
    path.set_bounds({0, 0, 100, 100});
    path.clear_fill();
    CHECK(path.paint_extent().width == 0.0f); // no fill and no stroke: draws nothing
    path.set_stroke_color(pulp::canvas::Color::rgba8(255, 255, 255));
    path.set_stroke_width(4.0f);
    path.set_path("M 40 50 L 60 50");
    auto e = path.paint_extent();
    // Half the stroke, its miter reach and anti-aliasing past the end points.
    CHECK(e.x <= 40.0f - 2.0f);
    CHECK(e.x + e.width >= 60.0f + 2.0f);
    CHECK(e.y <= 50.0f - 2.0f);
    // A 50 x 50 viewBox on a 100 x 100 box doubles every coordinate.
    path.set_viewbox(50.0f, 50.0f);
    e = path.paint_extent();
    CHECK(e.x <= 80.0f - 4.0f);
    CHECK(e.x + e.width >= 120.0f);
}

// ── Whole-surface requests on a plug-in host stay whole-surface ─────────────

TEST_CASE("A rect-less repaint on a plug-in host marks the whole surface",
          "[view][widgets][partial-repaint]") {
    // A frame that carries a bounded request and a rect-less one must report
    // the whole surface: a partial-repaint host that clipped to the bounded
    // rect would leave the rect-less change unpainted.
    class Host final : public PluginViewHost {
      public:
        NativeViewHandle native_handle() override {
            return {};
        }
        void attach_to_parent(NativeViewHandle) override {}
        void detach() override {}
        void repaint() override {}
        void set_size(std::uint32_t, std::uint32_t) override {}
        Size get_size() const override {
            return {};
        }
    };
    KnobRig rig;
    Host host;
    rig.root->set_plugin_view_host(&host);
    host.clear_pending_dirty();
    rig.knob->set_value(0.25f); // bounded
    REQUIRE_FALSE(host.pending_repaint_is_full());
    REQUIRE(host.has_pending_dirty_bounds());
    rig.knob->set_label("Cutoff"); // structural: rect-less
    CHECK(host.pending_repaint_is_full());
    rig.root->set_plugin_view_host(nullptr);
}

// ── FrameCostProbe ──────────────────────────────────────────────────────────

TEST_CASE("FrameCostProbe holds an animated modulated knob to a bounded budget",
          "[view][widgets][partial-repaint][frame-cost]") {
    KnobRig rig;
    FrameCostProbe::Budget budget;
    budget.max_full_damage_frames = 0;
    budget.max_layout_frames = 0;
    budget.max_mean_damage_area = 0.05 * 400.0 * 300.0;
    budget.min_painted_frames = 50;

    FrameCostProbe probe(*rig.root, {400, 300});
    for (int frame = 0; frame < 60; ++frame)
        probe.measure([&] {
            rig.knob->set_modulated_value(0.5f + 0.4f * static_cast<float>(std::sin(frame * 0.3)));
            rig.root->layout_children_if_needed();
        });
    const auto summary = probe.summary();
    const auto breaches = FrameCostProbe::check(summary, budget);
    for (const auto& line : breaches)
        UNSCOPED_INFO(line);
    CHECK(breaches.empty());
    CHECK(summary.painted_frames >= 50);
    CHECK(summary.full_damage_frames == 0);
    CHECK(summary.layout_frames == 0);
}

TEST_CASE("FrameCostProbe fails a frame that invalidates the whole tree",
          "[view][widgets][partial-repaint][frame-cost]") {
    // The negative control: the same animation, plus a structural write per
    // frame. A probe that cannot see this would pass every budget.
    KnobRig rig;
    FrameCostProbe::Budget budget;
    budget.max_mean_damage_area = 0.05 * 400.0 * 300.0;
    budget.min_painted_frames = 50;

    FrameCostProbe probe(*rig.root, {400, 300});
    for (int frame = 0; frame < 60; ++frame)
        probe.measure([&] {
            rig.knob->set_modulated_value(0.5f + 0.4f * static_cast<float>(std::sin(frame * 0.3)));
            rig.knob->flex().preferred_width = (frame & 1) ? 64.0f : 65.0f;
            rig.knob->invalidate_layout();
            rig.root->invalidate_layout();
            rig.root->request_repaint();
            rig.root->layout_children_if_needed();
        });
    const auto summary = probe.summary();
    const auto breaches = FrameCostProbe::check(summary, budget);
    CHECK(summary.full_damage_frames == 60);
    CHECK(summary.layout_frames > 0);
    CHECK(breaches.size() >= 2);
}

TEST_CASE("FrameCostProbe reports a run that painted nothing as a breach",
          "[view][widgets][partial-repaint][frame-cost]") {
    KnobRig rig;
    FrameCostProbe probe(*rig.root, {400, 300});
    for (int frame = 0; frame < 10; ++frame)
        probe.measure([] {});
    FrameCostProbe::Budget budget;
    budget.min_painted_frames = 5;
    const auto breaches = FrameCostProbe::check(probe.summary(), budget);
    REQUIRE(breaches.size() == 1);
    CHECK(breaches.front().find("positive control") != std::string::npos);
}

TEST_CASE("FrameCostProbe restores the root's previous host", "[view][widgets][frame-cost]") {
    KnobRig rig;
    REQUIRE(rig.root->plugin_view_host() == nullptr);
    {
        FrameCostProbe probe(*rig.root, {400, 300});
        CHECK(rig.root->plugin_view_host() != nullptr);
        CHECK(rig.knob->plugin_view_host() == rig.root->plugin_view_host());
    }
    CHECK(rig.root->plugin_view_host() == nullptr);
    CHECK(rig.knob->plugin_view_host() == nullptr);
}

// ── CanvasWidget: a redrawn canvas repaints its own box ─────────────────────

TEST_CASE("A redrawn CanvasWidget requests a repaint of its own box only",
          "[view][widgets][partial-repaint][canvas]") {
    // paint() clips every replay to the widget bounds, so a new frame of
    // commands cannot reach a pixel outside them. An analyzer or a modulated
    // plot redraws every frame; a whole-surface request there repaints the
    // editor's static chrome each frame on a partial-repaint host.
    auto root = std::make_unique<View>();
    root->set_bounds({0, 0, 400, 300});
    auto owned = std::make_unique<CanvasWidget>();
    auto* canvas = owned.get();
    owned->flex().preferred_width = 120;
    owned->flex().preferred_height = 80;
    root->add_child(std::move(owned));
    root->layout_children();
    const auto cb = canvas->local_bounds();
    REQUIRE(cb.width == 120.0f);

    const auto d = damage_of(*root, [&] {
        canvas->clear_commands();
        CanvasDrawCmd cmd;
        cmd.type = CanvasDrawCmd::Type::fill_rect;
        cmd.x = 10.0f;
        cmd.y = 10.0f;
        cmd.w = 50.0f;
        cmd.h = 20.0f;
        canvas->add_command(cmd);
    });
    REQUIRE_FALSE(d.full);
    CHECK(d.bounds.width >= cb.width);
    CHECK(d.bounds.height >= cb.height);
    CHECK(d.bounds.width <= cb.width + 4.0f);
    CHECK(d.bounds.height <= cb.height + 4.0f);
}

// ── Bounded repaint maps through transforms and scroll offsets ──────────────

namespace {

struct DamageHost final : public PluginViewHost {
    NativeViewHandle native_handle() override { return {}; }
    void attach_to_parent(NativeViewHandle) override {}
    void detach() override {}
    void repaint() override {}
    void set_size(std::uint32_t, std::uint32_t) override {}
    Size get_size() const override { return {}; }
};

// A 1000 x 1000 root holding `wrapper` holding a 20 x 20 knob at (100, 100).
struct TransformRig {
    std::unique_ptr<View> root = std::make_unique<View>();
    View* wrapper = nullptr;
    Knob* knob = nullptr;
    DamageHost host;
    TransformRig() {
        // Bounds set directly and no layout pass: the mapping is the subject.
        root->set_bounds({0, 0, 1000, 1000});
        auto w = std::make_unique<View>();
        wrapper = w.get();
        w->set_bounds({0, 0, 1000, 1000});
        auto k = std::make_unique<Knob>();
        knob = k.get();
        k->set_bounds({100, 100, 20, 20});
        w->add_child(std::move(k));
        root->add_child(std::move(w));
        root->set_plugin_view_host(&host);
    }
    ~TransformRig() { root->set_plugin_view_host(nullptr); }
};

}  // namespace

TEST_CASE("A bounded repaint under a scaled ancestor maps through the scale",
          "[view][widgets][partial-repaint][transform]") {
    TransformRig rig;
    // A design-viewport style scale on the wrapper, about its top-left.
    rig.wrapper->set_transform_matrix(0.5f, 0.0f, 0.0f, 0.5f, 0.0f, 0.0f);
    rig.host.clear_pending_dirty();
    rig.knob->set_value(0.9f);
    REQUIRE_FALSE(rig.host.pending_repaint_is_full());
    REQUIRE(rig.host.has_pending_dirty_bounds());
    const auto b = rig.host.pending_dirty_bounds();
    // The knob paints at (100, 100) x 0.5 = (50, 50), 10 x 10, plus its halo.
    CHECK(b.x <= 50.0f);
    CHECK(b.y <= 50.0f);
    CHECK(b.x + b.width >= 60.0f);
    CHECK(b.y + b.height >= 60.0f);
    CHECK(b.width < 30.0f);
    CHECK(b.x > 40.0f);  // not the unscaled (100, 100) position
}

TEST_CASE("A bounded repaint under a pixel-spreading filter stays whole-surface",
          "[view][widgets][partial-repaint][transform]") {
    TransformRig rig;
    rig.wrapper->set_filter_blur(4.0f);
    rig.host.clear_pending_dirty();
    rig.knob->set_value(0.9f);
    CHECK(rig.host.pending_repaint_is_full());
}

TEST_CASE("A paint-only setter is counted as an unmarked paint mutation",
          "[view][widgets][partial-repaint]") {
    // The script bridge skips its blanket whole-surface request only when the
    // work left no unmarked paint change; a setter that changes paint without
    // requesting a repaint must therefore be visible to that count.
    View v;
    const auto before = View::unmarked_paint_mutation_count();
    v.set_opacity(0.5f);
    v.set_background_color(pulp::canvas::Color::rgba8(10, 20, 30));
    CHECK(View::unmarked_paint_mutation_count() >= before + 2);
    const auto marks = View::damage_request_count();
    const auto unmarked = View::unmarked_paint_mutation_count();
    v.request_repaint();
    CHECK(View::damage_request_count() == marks + 1);
    CHECK(View::unmarked_paint_mutation_count() == unmarked);
}

TEST_CASE("A fixed-box Label repaints its own box when its copy changes",
          "[view][widgets][partial-repaint][label]") {
    auto root = std::make_unique<View>();
    root->set_bounds({0, 0, 400, 300});
    auto owned = std::make_unique<Label>("32 BANDS");
    auto* label = owned.get();
    owned->flex().preferred_width = 80;
    owned->flex().preferred_height = 16;
    root->add_child(std::move(owned));
    root->layout_children();
    const auto lb = label->local_bounds();
    REQUIRE(lb.width == 80.0f);

    const auto d = damage_of(*root, [&] { label->set_text("64 BANDS"); });
    REQUIRE_FALSE(d.full);
    CHECK(d.bounds.width >= lb.width);
    CHECK(d.bounds.width < 200.0f);

    // A label whose width follows its copy moves its siblings: whole surface.
    auto free_owned = std::make_unique<Label>("x");
    auto* free_label = free_owned.get();
    root->add_child(std::move(free_owned));
    root->layout_children();
    const auto d2 = damage_of(*root, [&] { free_label->set_text("a much longer string"); });
    CHECK(d2.full);
}
