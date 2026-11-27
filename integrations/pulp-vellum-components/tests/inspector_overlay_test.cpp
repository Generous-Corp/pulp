#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <pulp/vellum_components/inspector_overlay.hpp>

#include <vellum/canvas/recording_canvas.hpp>
#include <vellum/view/view.hpp>

#include "render_helpers.hpp"

#include <memory>

using namespace pulp::vellum_components;
using namespace pulp::vellum_components::testing;
using ::vellum::view::KeyCode;
using ::vellum::view::KeyEvent;
using ::vellum::view::kModAlt;
using ::vellum::view::kModCmd;
using ::vellum::view::kModCtrl;
using ::vellum::view::MouseEvent;
using ::vellum::view::Rect;
using Mode = InspectorOverlay::SelectionMode;
using CA = overlay::CursorAffordance;

namespace {

KeyEvent key(KeyCode code, std::uint16_t modifiers = 0) {
    KeyEvent e;
    e.key = code;
    e.modifiers = modifiers;
    e.is_down = true;
    return e;
}

MouseEvent press(float x, float y, std::uint16_t modifiers = 0) {
    MouseEvent e;
    e.position = {x, y};
    e.modifiers = modifiers;
    e.is_down = true;
    return e;
}

MouseEvent move(float x, float y, std::uint16_t modifiers = 0) {
    MouseEvent e;
    e.position = {x, y};
    e.modifiers = modifiers;
    e.is_down = false;
    return e;
}

#ifdef __APPLE__
constexpr std::uint16_t kMainModifier = kModCmd;
#else
constexpr std::uint16_t kMainModifier = kModCtrl;
#endif

// Two siblings side by side under a 500x300 root.
struct Pair {
    View root;
    View* a = nullptr;
    View* b = nullptr;
    Pair() {
        root.set_id("root");
        root.set_bounds({0, 0, 500, 300});
        auto first = std::make_unique<View>();
        first->set_id("a");
        first->set_bounds({10, 10, 60, 60});
        a = first.get();
        root.add_child(std::move(first));
        auto second = std::make_unique<View>();
        second->set_id("b");
        second->set_bounds({100, 10, 60, 60});
        b = second.get();
        root.add_child(std::move(second));
    }
};

// Pin a view to a rectangle so the layout pass a render runs keeps it there.
void place(View& v, float x, float y, float w, float h) {
    v.set_position(View::Position::absolute);
    v.set_left(x);
    v.set_top(y);
    v.flex().preferred_width = w;
    v.flex().preferred_height = h;
    v.set_bounds({x, y, w, h});
}

} // namespace

TEST_CASE("the overlay toggles and ignores input while inactive",
          "[pulp-vellum-components][inspector-overlay]") {
    View root;
    InspectorOverlay inspector(root);
    REQUIRE_FALSE(inspector.is_active());

    // Inactive: nothing is consumed except the toggle chord.
    REQUIRE_FALSE(inspector.handle_key_event(key(KeyCode::escape)));
    REQUIRE_FALSE(inspector.handle_mouse_event(press(10, 10)));
    REQUIRE_FALSE(inspector.handle_key_event(key(KeyCode::m)));

    REQUIRE(inspector.handle_key_event(key(KeyCode::i, kMainModifier)));
    REQUIRE(inspector.is_active());
    REQUIRE(inspector.handle_key_event(key(KeyCode::i, kMainModifier)));
    REQUIRE_FALSE(inspector.is_active());

    inspector.toggle();
    REQUIRE(inspector.is_active());
}

TEST_CASE("clicking selects the deepest view and Escape deselects without leaving",
          "[pulp-vellum-components][inspector-overlay]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    auto container = std::make_unique<View>();
    container->set_bounds({20, 20, 200, 120});
    View* container_ptr = container.get();
    auto nested = std::make_unique<View>();
    nested->set_bounds({10, 10, 80, 24});
    View* nested_ptr = nested.get();
    container->add_child(std::move(nested));
    root.add_child(std::move(container));

    InspectorOverlay inspector(root);
    inspector.set_active(true);

    // The nested view sits at (30, 30) in root space.
    REQUIRE(inspector.handle_mouse_event(press(40, 35)));
    REQUIRE(inspector.selected_view() == nested_ptr);

    REQUIRE(inspector.handle_key_event(key(KeyCode::escape)));
    REQUIRE(inspector.selected_view() == nullptr);
    REQUIRE(inspector.is_active());

    REQUIRE(inspector.handle_mouse_event(press(40, 35)));
    REQUIRE(inspector.selected_view() == nested_ptr);
    REQUIRE(inspector.selected_view()->parent() == container_ptr);

    // Escape with nothing selected is consumed and never dismisses.
    REQUIRE(inspector.handle_key_event(key(KeyCode::escape)));
    REQUIRE(inspector.handle_key_event(key(KeyCode::escape)));
    REQUIRE(inspector.is_active());
}

TEST_CASE("select_parent walks up and stops at the root",
          "[pulp-vellum-components][inspector-overlay]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    auto mid = std::make_unique<View>();
    mid->set_bounds({0, 0, 200, 200});
    View* mid_ptr = mid.get();
    auto leaf = std::make_unique<View>();
    leaf->set_bounds({0, 0, 50, 50});
    View* leaf_ptr = leaf.get();
    mid->add_child(std::move(leaf));
    root.add_child(std::move(mid));

    InspectorOverlay inspector(root);
    REQUIRE_FALSE(inspector.select_parent()); // nothing selected

    inspector.set_selected_view(leaf_ptr);
    REQUIRE(inspector.select_parent());
    REQUIRE(inspector.selected_view() == mid_ptr);
    REQUIRE(inspector.select_parent());
    REQUIRE(inspector.selected_view() == &root);
    REQUIRE_FALSE(inspector.select_parent());
    REQUIRE(inspector.selected_view() == &root);
}

TEST_CASE("a plain move tracks the hover without being consumed, a press is consumed",
          "[pulp-vellum-components][inspector-overlay]") {
    Pair t;
    InspectorOverlay inspector(t.root);
    inspector.set_active(true);

    REQUIRE_FALSE(inspector.handle_mouse_event(move(20, 20)));
    REQUIRE(inspector.hovered_view() == t.a);
    REQUIRE(inspector.selected_view() == nullptr);

    REQUIRE(inspector.handle_mouse_event(press(20, 20)));
    REQUIRE(inspector.selected_view() == t.a);

    // A press over nothing under the root's children lands on the root.
    REQUIRE(inspector.handle_mouse_event(press(300, 200)));
    REQUIRE(inspector.selected_view() == &t.root);
}

TEST_CASE("turning the overlay off drops selection, hover and the measurement anchor",
          "[pulp-vellum-components][inspector-overlay]") {
    Pair t;
    InspectorOverlay inspector(t.root);
    inspector.set_active(true);
    REQUIRE(inspector.handle_mouse_event(press(20, 20)));
    REQUIRE(inspector.handle_mouse_event(press(130, 20, kModAlt)));
    REQUIRE(inspector.distance_anchor() == t.b);
    REQUIRE(inspector.selected_view() == t.a);

    inspector.set_active(false);
    REQUIRE(inspector.selected_view() == nullptr);
    REQUIRE(inspector.hovered_view() == nullptr);
    REQUIRE(inspector.distance_anchor() == nullptr);
}

TEST_CASE("Alt+press pins an anchor on the first view and selects on the second",
          "[pulp-vellum-components][inspector-overlay]") {
    Pair t;
    InspectorOverlay inspector(t.root);
    inspector.set_active(true);

    REQUIRE(inspector.handle_mouse_event(press(20, 20, kModAlt)));
    REQUIRE(inspector.distance_anchor() == t.a);
    REQUIRE(inspector.selected_view() == nullptr);

    REQUIRE(inspector.handle_mouse_event(press(130, 20, kModAlt)));
    REQUIRE(inspector.selected_view() == t.b);
    REQUIRE(inspector.distance_anchor() == t.a);

    // A plain press clears the anchor.
    REQUIRE(inspector.handle_mouse_event(press(20, 20)));
    REQUIRE(inspector.distance_anchor() == nullptr);
}

TEST_CASE("Alt-hover targets the view under the pointer only while something is selected",
          "[pulp-vellum-components][inspector-overlay]") {
    Pair t;
    InspectorOverlay inspector(t.root);
    inspector.set_active(true);

    // No selection: Alt-hover reaches toward nothing.
    inspector.handle_mouse_event(move(130, 30, kModAlt));
    REQUIRE(inspector.alt_hover_target() == nullptr);

    REQUIRE(inspector.handle_mouse_event(press(20, 20)));
    inspector.handle_mouse_event(move(130, 30, kModAlt));
    REQUIRE(inspector.alt_hover_target() == t.b);
    REQUIRE(inspector.selected_view() == t.a);

    // Hovering the selection itself, or releasing Alt, clears it.
    inspector.handle_mouse_event(move(20, 20, kModAlt));
    REQUIRE(inspector.alt_hover_target() == nullptr);
    inspector.handle_mouse_event(move(130, 30, kModAlt));
    inspector.handle_mouse_event(move(130, 30));
    REQUIRE(inspector.alt_hover_target() == nullptr);
}

TEST_CASE("M flips the selection mode while active and only then",
          "[pulp-vellum-components][inspector-overlay]") {
    View root;
    root.set_bounds({0, 0, 200, 200});
    InspectorOverlay inspector(root);
    REQUIRE(inspector.selection_mode() == Mode::follows_focus);

    inspector.set_active(true);
    REQUIRE(inspector.handle_key_event(key(KeyCode::m)));
    REQUIRE(inspector.selection_mode() == Mode::follows_mouse);
    REQUIRE(inspector.handle_key_event(key(KeyCode::m)));
    REQUIRE(inspector.selection_mode() == Mode::follows_focus);

    inspector.set_active(false);
    REQUIRE_FALSE(inspector.handle_key_event(key(KeyCode::m)));
    REQUIRE(inspector.selection_mode() == Mode::follows_focus);

    // A modified M is not the mode toggle.
    inspector.set_active(true);
    REQUIRE_FALSE(inspector.handle_key_event(key(KeyCode::m, kMainModifier)));
    REQUIRE(inspector.selection_mode() == Mode::follows_focus);
}

TEST_CASE("follows_focus keeps the selection pinned while the pointer moves",
          "[pulp-vellum-components][inspector-overlay]") {
    Pair t;
    InspectorOverlay inspector(t.root);
    inspector.set_active(true);
    REQUIRE(inspector.handle_mouse_event(press(20, 20)));

    REQUIRE_FALSE(inspector.handle_mouse_event(move(130, 30)));
    REQUIRE(inspector.hovered_view() == t.b);
    REQUIRE(inspector.selected_view() == t.a);
}

TEST_CASE("follows_mouse re-selects on a move but not on Alt-move or a press elsewhere",
          "[pulp-vellum-components][inspector-overlay]") {
    Pair t;
    InspectorOverlay inspector(t.root);
    inspector.set_active(true);
    inspector.set_selection_mode(Mode::follows_mouse);
    REQUIRE(inspector.handle_mouse_event(press(20, 20)));
    REQUIRE(inspector.selected_view() == t.a);

    REQUIRE_FALSE(inspector.handle_mouse_event(move(130, 30)));
    REQUIRE(inspector.selected_view() == t.b);
    REQUIRE_FALSE(inspector.handle_mouse_event(move(30, 30)));
    REQUIRE(inspector.selected_view() == t.a);

    // Alt-move keeps the selection pinned for measuring.
    REQUIRE_FALSE(inspector.handle_mouse_event(move(130, 30, kModAlt)));
    REQUIRE(inspector.selected_view() == t.a);
}

TEST_CASE("an explicit-phase drag tick never re-selects, and is consumed",
          "[pulp-vellum-components][inspector-overlay]") {
    Pair t;
    InspectorOverlay inspector(t.root);
    inspector.set_active(true);
    inspector.set_selection_mode(Mode::follows_mouse);
    REQUIRE(inspector.handle_mouse_event(press(20, 20)));
    REQUIRE(inspector.selected_view() == t.a);

    MouseEvent drag = move(130, 30);
    drag.is_down = true;
    drag.phase = ::vellum::view::MousePhase::drag;
    REQUIRE(inspector.handle_mouse_event(drag));
    REQUIRE(inspector.selected_view() == t.a);

    MouseEvent release = move(130, 30);
    release.phase = ::vellum::view::MousePhase::release;
    REQUIRE(inspector.handle_mouse_event(release));
    REQUIRE(inspector.selected_view() == t.a);
}

TEST_CASE("a view removed from the tree is dropped at the next paint",
          "[pulp-vellum-components][inspector-overlay]") {
    Pair t;
    InspectorOverlay inspector(t.root);
    inspector.set_active(true);
    REQUIRE(inspector.handle_mouse_event(press(20, 20)));
    REQUIRE_FALSE(inspector.handle_mouse_event(move(130, 30)));
    inspector.set_selected_view(t.a);
    REQUIRE(inspector.hovered_view() == t.b);

    // Freed views must never be dereferenced: the overlay compares pointers
    // against the live tree.
    t.root.remove_child(t.a);
    t.root.remove_child(t.b);

    ::vellum::canvas::RecordingCanvas canvas;
    inspector.paint(canvas);
    REQUIRE(inspector.selected_view() == nullptr);
    REQUIRE(inspector.hovered_view() == nullptr);
}

TEST_CASE("cursor and select_parent on a removed selection never dereference it",
          "[pulp-vellum-components][inspector-overlay]") {
    Pair t;
    InspectorOverlay inspector(t.root);
    inspector.set_active(true);
    inspector.set_dragging_enabled(true);
    inspector.set_selected_view(t.a);
    REQUIRE(inspector.cursor_affordance_at({20, 20}) == CA::move);

    // An inactive overlay leaves the widget's own cursor alone even when a
    // host restored a selection.
    inspector.set_active(false);
    inspector.set_selected_view(t.a);
    REQUIRE(inspector.cursor_affordance_at({20, 20}) == CA::none);
    inspector.set_active(true);
    inspector.set_selected_view(t.a);

    // The cursor hook fires before any paint has dropped the stale views.
    t.root.remove_child(t.a);
    REQUIRE(inspector.cursor_affordance_at({20, 20}) == CA::none);
    REQUIRE(inspector.cursor_style_for({20, 20}) == -1);
    REQUIRE(inspector.selected_view() == nullptr);

    inspector.set_selected_view(t.b);
    t.root.remove_child(t.b);
    REQUIRE_FALSE(inspector.select_parent());
    REQUIRE(inspector.selected_view() == nullptr);
}

TEST_CASE("cursor affordance is move on the body, resize on a handle, none outside",
          "[pulp-vellum-components][inspector-overlay]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    auto box = std::make_unique<View>();
    box->set_bounds({100, 80, 120, 60});
    View* box_ptr = box.get();
    root.add_child(std::move(box));

    InspectorOverlay inspector(root);
    inspector.set_active(true);
    inspector.set_selected_view(box_ptr);

    // Dragging is off by default: no affordance, even over the body.
    REQUIRE_FALSE(inspector.dragging_enabled());
    REQUIRE(inspector.cursor_affordance_at({160, 110}) == CA::none);
    REQUIRE(inspector.cursor_style_for({160, 110}) == -1);

    inspector.set_dragging_enabled(true);
    REQUIRE(inspector.cursor_affordance_at({160, 110}) == CA::move);
    REQUIRE(inspector.cursor_style_for({160, 110}) ==
            static_cast<int>(View::CursorStyle::multi_directional_resize));
    REQUIRE(inspector.cursor_affordance_at({100, 80}) == CA::resize_nw_se);
    REQUIRE(inspector.cursor_style_for({100, 80}) ==
            static_cast<int>(View::CursorStyle::top_left_resize));
    REQUIRE(inspector.cursor_affordance_at({220, 140}) == CA::resize_nw_se);
    REQUIRE(inspector.cursor_affordance_at({220, 80}) == CA::resize_ne_sw);
    REQUIRE(inspector.cursor_affordance_at({100, 140}) == CA::resize_ne_sw);
    REQUIRE(inspector.cursor_affordance_at({160, 80}) == CA::resize_ns);
    REQUIRE(inspector.cursor_affordance_at({220, 110}) == CA::resize_ew);
    REQUIRE(inspector.cursor_affordance_at({350, 280}) == CA::none);

    inspector.set_selected_view(nullptr);
    REQUIRE(inspector.cursor_affordance_at({160, 110}) == CA::none);
}

TEST_CASE("compute_badge_placement flips below near the top and clamps to the root",
          "[pulp-vellum-components][inspector-overlay]") {
    constexpr float kBadgeW = 80.0f;
    constexpr float kBadgeH = 16.0f;
    constexpr float kRootW = 400.0f;
    constexpr float kGap = 2.0f;

    SECTION("room above puts the badge above") {
        const auto p =
            compute_badge_placement(100, 200, 50, kBadgeW, kBadgeH, kRootW, kGap, kBadgeH);
        REQUIRE_FALSE(p.below);
        REQUIRE(p.y == Catch::Approx(200.0f - kGap - kBadgeH));
        REQUIRE(p.x == Catch::Approx(100.0f));
    }
    SECTION("a selection at the top flips the badge below") {
        const auto p = compute_badge_placement(100, 2, 40, kBadgeW, kBadgeH, kRootW, kGap, kBadgeH);
        REQUIRE(p.below);
        REQUIRE(p.y == Catch::Approx(2.0f + 40.0f + kGap));
    }
    SECTION("x clamps off the left edge") {
        const auto p =
            compute_badge_placement(-20, 200, 50, kBadgeW, kBadgeH, kRootW, kGap, kBadgeH);
        REQUIRE(p.x == Catch::Approx(0.0f));
    }
    SECTION("x clamps off the right edge") {
        const auto p =
            compute_badge_placement(380, 200, 50, kBadgeW, kBadgeH, kRootW, kGap, kBadgeH);
        REQUIRE(p.x == Catch::Approx(kRootW - kBadgeW));
    }
    SECTION("a badge wider than the root pins to the left") {
        const auto p = compute_badge_placement(10, 200, 50, 500, kBadgeH, kRootW, kGap, kBadgeH);
        REQUIRE(p.x == Catch::Approx(0.0f));
    }
    SECTION("a zero root width disables the right clamp") {
        const auto p = compute_badge_placement(380, 200, 50, kBadgeW, kBadgeH, 0.0f, kGap, kBadgeH);
        REQUIRE(p.x == Catch::Approx(380.0f));
    }
}

TEST_CASE("painting does nothing while inactive and paints once something is selected",
          "[pulp-vellum-components][inspector-overlay]") {
    Pair t;
    InspectorOverlay inspector(t.root);

    ::vellum::canvas::RecordingCanvas idle;
    inspector.paint(idle);
    REQUIRE(idle.command_count() == 0);

    inspector.set_active(true);
    ::vellum::canvas::RecordingCanvas nothing_picked;
    inspector.paint(nothing_picked);
    REQUIRE(nothing_picked.command_count() == 0);

    inspector.set_selected_view(t.a);
    ::vellum::canvas::RecordingCanvas selected;
    inspector.paint(selected);
    const auto selected_count = selected.command_count();
    REQUIRE(selected_count > 0);

    // Dragging adds the eight handles.
    inspector.set_dragging_enabled(true);
    ::vellum::canvas::RecordingCanvas with_handles;
    inspector.paint(with_handles);
    REQUIRE(with_handles.command_count() > selected_count);
}

TEST_CASE("the hover box is suppressed over the selection chain",
          "[pulp-vellum-components][inspector-overlay]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    auto parent = std::make_unique<View>();
    parent->set_bounds({20, 20, 200, 150});
    View* parent_ptr = parent.get();
    auto child = std::make_unique<View>();
    child->set_bounds({10, 10, 60, 40});
    View* child_ptr = child.get();
    parent->add_child(std::move(child));
    root.add_child(std::move(parent));
    auto stranger = std::make_unique<View>();
    stranger->set_bounds({260, 180, 100, 80});
    View* stranger_ptr = stranger.get();
    root.add_child(std::move(stranger));

    InspectorOverlay inspector(root);
    inspector.set_active(true);
    inspector.set_selected_view(parent_ptr);

    ::vellum::canvas::RecordingCanvas baseline;
    inspector.paint(baseline);

    // Hovering the selected view's child adds no second box: the selection shows one affordance.
    REQUIRE_FALSE(inspector.handle_mouse_event(move(40, 40)));
    REQUIRE(inspector.hovered_view() == child_ptr);
    ::vellum::canvas::RecordingCanvas over_child;
    inspector.paint(over_child);
    REQUIRE(over_child.command_count() == baseline.command_count());

    // Hovering a view outside the chain does add the blue box and badge.
    REQUIRE_FALSE(inspector.handle_mouse_event(move(300, 200)));
    REQUIRE(inspector.hovered_view() == stranger_ptr);
    ::vellum::canvas::RecordingCanvas over_other;
    inspector.paint(over_other);
    REQUIRE(over_other.command_count() > baseline.command_count());
}

TEST_CASE("installed hooks paint only the inspected root and go inert on uninstall",
          "[pulp-vellum-components][inspector-overlay]") {
    Pair t;
    View other_root;
    other_root.set_bounds({0, 0, 100, 100});

    InspectorOverlay inspector(t.root);
    inspector.set_active(true);
    inspector.set_selected_view(t.a);
    install_inspector_hooks(inspector);

    ::vellum::canvas::RecordingCanvas on_root;
    View::paint_overlays(on_root, &t.root);
    REQUIRE(on_root.command_count() > 0);

    // The inspector window paints its own root: nothing may leak into it.
    ::vellum::canvas::RecordingCanvas on_other;
    View::paint_overlays(on_other, &other_root);
    REQUIRE(on_other.command_count() == 0);

    // An unknown root paints.
    ::vellum::canvas::RecordingCanvas unknown;
    View::paint_overlays(unknown, nullptr);
    REQUIRE(unknown.command_count() > 0);

    // Events from another root are not routed to the overlay.
    REQUIRE_FALSE(View::call_inspector_mouse_hook(press(130, 20), &other_root));
    REQUIRE(inspector.selected_view() == t.a);
    REQUIRE(View::call_inspector_mouse_hook(press(130, 20), &t.root));
    REQUIRE(inspector.selected_view() == t.b);
    REQUIRE(View::call_inspector_key_hook(key(KeyCode::escape)));
    REQUIRE(inspector.selected_view() == nullptr);

    inspector.set_dragging_enabled(true);
    inspector.set_selected_view(t.a);
    REQUIRE(View::call_inspector_cursor_hook(move(40, 40), &t.root) ==
            static_cast<int>(View::CursorStyle::multi_directional_resize));
    REQUIRE(View::call_inspector_cursor_hook(move(40, 40), &other_root) == -1);

    uninstall_inspector_hooks();
    ::vellum::canvas::RecordingCanvas after;
    View::paint_overlays(after, &t.root);
    REQUIRE(after.command_count() == 0);
    REQUIRE_FALSE(View::call_inspector_key_hook(key(KeyCode::escape)));
}

TEST_CASE("destroying the owning overlay releases the hooks, and an older one cannot",
          "[pulp-vellum-components][inspector-overlay]") {
    Pair t;
    auto first = std::make_unique<InspectorOverlay>(t.root);
    first->set_active(true);
    first->set_selected_view(t.a);
    install_inspector_hooks(*first);

    auto second = std::make_unique<InspectorOverlay>(t.root);
    second->set_active(true);
    second->set_selected_view(t.b);
    install_inspector_hooks(*second);

    // The older overlay no longer owns the hooks, so destroying it leaves the
    // newer one's installed.
    first.reset();
    ::vellum::canvas::RecordingCanvas still_owned;
    View::paint_overlays(still_owned, &t.root);
    REQUIRE(still_owned.command_count() > 0);

    second.reset();
    ::vellum::canvas::RecordingCanvas released;
    View::paint_overlays(released, &t.root);
    REQUIRE(released.command_count() == 0);
}

TEST_CASE("the selection box and hover box render at their view's bounds",
          "[pulp-vellum-components][inspector-overlay][pixels]") {
    View root;
    root.set_bounds({0, 0, 240, 140});
    auto selected = std::make_unique<View>();
    place(*selected, 20, 30, 80, 60);
    View* selected_ptr = selected.get();
    root.add_child(std::move(selected));
    auto hovered = std::make_unique<View>();
    place(*hovered, 140, 30, 80, 60);
    View* hovered_ptr = hovered.get();
    root.add_child(std::move(hovered));

    InspectorOverlay inspector(root);
    inspector.set_active(true);
    inspector.set_dragging_enabled(true);
    inspector.set_selected_view(selected_ptr);
    REQUIRE_FALSE(inspector.handle_mouse_event(move(150, 40)));
    REQUIRE(inspector.hovered_view() == hovered_ptr);
    install_inspector_hooks(inspector);

    const auto frame = render(root, 240, 140);
    maybe_write_png(root, 240, 140, "inspector-overlay-selection.png");
    uninstall_inspector_hooks();
    REQUIRE(frame.pixels.size() == 240u * 140u * 4u);

    // The render fills the surface, so every pixel is judged against the
    // empty background beside both boxes.
    const auto bg = frame.at(120, 120);

    // Orange selection stroke, 2 px wide on the box's left edge (x = 20),
    // clear of any handle (handles sit at the corners and mid-edges).
    const auto edge = frame.at(20, 45);
    REQUIRE(edge.r > bg.r + 80);
    REQUIRE(edge.r > edge.b + 40);

    // The selection's interior carries the faint orange fill: warmer than the
    // background, and not the solid stroke.
    const auto inside = frame.at(60, 45);
    REQUIRE(inside.r > bg.r + 5);
    REQUIRE(inside.r < edge.r);
    REQUIRE(inside.b <= bg.b);

    // A corner handle is a solid square on the box corner.
    const auto handle = frame.at(21, 31);
    REQUIRE(handle.r > 200);
    REQUIRE(handle.b < 80);

    // The hovered view carries the blue box: a bluer edge and interior, and
    // no handles on its corner.
    const auto hover_edge = frame.at(140, 45);
    REQUIRE(hover_edge.b > bg.b + 50);
    REQUIRE(hover_edge.b > hover_edge.r + 40);
    const auto hover_inside = frame.at(180, 45);
    REQUIRE(hover_inside.b > bg.b);
    const auto hover_corner = frame.at(141, 31);
    REQUIRE(hover_corner.r < 120);
}
