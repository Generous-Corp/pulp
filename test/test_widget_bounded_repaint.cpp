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
#include <pulp/view/widgets.hpp>
#include <pulp/view/window_host.hpp>

#include <memory>

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
