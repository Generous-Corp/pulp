#include <catch2/catch_test_macros.hpp>
#include <pulp/vellum_components/inspector_overlay.hpp>

#include <vellum/canvas/recording_canvas.hpp>
#include <vellum/view/view.hpp>

#include "render_helpers.hpp"

#include <algorithm>
#include <memory>

using namespace pulp::vellum_components;
using namespace pulp::vellum_components::testing;
using ::vellum::view::kModAlt;
using ::vellum::view::MouseEvent;

namespace {

MouseEvent at(float x, float y, std::uint16_t modifiers = 0, bool down = false) {
    MouseEvent e;
    e.position = {x, y};
    e.modifiers = modifiers;
    e.is_down = down;
    return e;
}

// Pin a view to a rectangle so the layout pass a render runs keeps it there.
void place(View& v, float x, float y, float w, float h) {
    v.set_position(View::Position::absolute);
    v.set_left(x);
    v.set_top(y);
    v.flex().preferred_width = w;
    v.flex().preferred_height = h;
    v.set_bounds({x, y, w, h});
}

// Two siblings side by side under a 500x300 root.
struct Pair {
    View root;
    View* a = nullptr;
    View* b = nullptr;
    Pair() {
        root.set_bounds({0, 0, 500, 300});
        auto first = std::make_unique<View>();
        first->set_bounds({10, 10, 60, 60});
        a = first.get();
        root.add_child(std::move(first));
        auto second = std::make_unique<View>();
        second->set_bounds({100, 10, 60, 60});
        b = second.get();
        root.add_child(std::move(second));
    }
};

std::size_t paint_count(InspectorOverlay& inspector) {
    ::vellum::canvas::RecordingCanvas canvas;
    inspector.paint(canvas);
    return canvas.command_count();
}

// A tinted pixel against the empty background: how far red moved.
int red_lift(const Pixel& p, const Pixel& bg) {
    return p.r - bg.r;
}

} // namespace

TEST_CASE("Alt-hover draws a distance line to the hovered view and releasing Alt removes it",
          "[pulp-vellum-components][inspector-overlay][measure]") {
    Pair t;
    InspectorOverlay inspector(t.root);
    inspector.set_active(true);
    REQUIRE(inspector.handle_mouse_event(at(20, 20, 0, true)));
    REQUIRE_FALSE(inspector.handle_mouse_event(at(130, 30)));
    const auto baseline = paint_count(inspector);
    REQUIRE(baseline > 0);

    REQUIRE_FALSE(inspector.handle_mouse_event(at(130, 30, kModAlt)));
    REQUIRE(paint_count(inspector) > baseline);

    REQUIRE_FALSE(inspector.handle_mouse_event(at(130, 30)));
    REQUIRE(paint_count(inspector) == baseline);
}

TEST_CASE("Alt-hover without a selection adds nothing to the paint",
          "[pulp-vellum-components][inspector-overlay][measure]") {
    Pair t;
    InspectorOverlay inspector(t.root);
    inspector.set_active(true);
    const auto baseline = paint_count(inspector);

    inspector.handle_mouse_event(at(30, 30, kModAlt));
    // The hover box now shows; compare with the same hover without Alt.
    const auto with_alt = paint_count(inspector);
    inspector.handle_mouse_event(at(30, 30));
    REQUIRE(with_alt == paint_count(inspector));
    REQUIRE(with_alt >= baseline);
}

TEST_CASE("the sticky anchor draws a line to the selection and clears with a plain press",
          "[pulp-vellum-components][inspector-overlay][measure]") {
    Pair t;
    InspectorOverlay inspector(t.root);
    inspector.set_active(true);
    REQUIRE(inspector.handle_mouse_event(at(20, 20, kModAlt, true)));
    REQUIRE(inspector.distance_anchor() == t.a);
    const auto anchored_only = paint_count(inspector);

    REQUIRE(inspector.handle_mouse_event(at(130, 20, kModAlt, true)));
    REQUIRE(inspector.selected_view() == t.b);
    const auto measuring = paint_count(inspector);
    REQUIRE(measuring > anchored_only);

    // Anchor plus a live Alt-hover over a third place draws both lines.
    inspector.handle_mouse_event(at(20, 20, kModAlt));
    REQUIRE(paint_count(inspector) > measuring);

    REQUIRE(inspector.handle_mouse_event(at(20, 20, 0, true)));
    REQUIRE(inspector.distance_anchor() == nullptr);
}

TEST_CASE("the distance line and its pixel label render between the view centres",
          "[pulp-vellum-components][inspector-overlay][measure][pixels]") {
    View root;
    root.set_bounds({0, 0, 280, 120});
    auto left = std::make_unique<View>();
    place(*left, 20, 30, 60, 60);
    View* left_ptr = left.get();
    root.add_child(std::move(left));
    auto right = std::make_unique<View>();
    place(*right, 200, 30, 60, 60);
    View* right_ptr = right.get();
    root.add_child(std::move(right));

    InspectorOverlay inspector(root);
    inspector.set_active(true);
    inspector.set_selected_view(left_ptr);
    REQUIRE_FALSE(inspector.handle_mouse_event(at(230, 60, kModAlt)));
    REQUIRE(inspector.alt_hover_target() == right_ptr);
    install_inspector_hooks(inspector);
    const auto frame = render(root, 280, 120);
    maybe_write_png(root, 280, 120, "inspector-overlay-distance.png");
    uninstall_inspector_hooks();

    const auto bg = frame.at(140, 110);
    // The line runs along y = 60 from x = 50 to x = 230 and is a half-pixel on
    // each of rows 59 and 60. Sample between the left view and the label.
    const auto row59 = frame.at(100, 59);
    const auto row60 = frame.at(100, 60);
    CHECK(std::max(red_lift(row59, bg), red_lift(row60, bg)) > 40);
    // The label sits on the midpoint (x = 140), a solid red pill.
    const auto pill = frame.at(140 - 22, 60);
    CHECK(red_lift(pill, bg) > 60);
    // Clear of the line and the boxes, nothing is drawn.
    const auto clear = frame.at(140, 100);
    CHECK(clear.r == bg.r);
    CHECK(clear.g == bg.g);
}

TEST_CASE("the box model tints padding inside the selection and margin outside it",
          "[pulp-vellum-components][inspector-overlay][measure][pixels]") {
    View root;
    root.set_bounds({0, 0, 240, 160});
    auto frame_view = std::make_unique<View>();
    place(*frame_view, 10, 10, 220, 140);
    View* parent_ptr = frame_view.get();
    auto child = std::make_unique<View>();
    place(*child, 60, 40, 120, 70);
    child->flex().padding = 14.0f;
    child->flex().margin = 10.0f;
    View* child_ptr = child.get();
    parent_ptr->add_child(std::move(child));
    root.add_child(std::move(frame_view));

    InspectorOverlay inspector(root);
    inspector.set_active(true);
    inspector.set_selected_view(child_ptr);
    install_inspector_hooks(inspector);
    const auto frame = render(root, 240, 160);
    maybe_write_png(root, 240, 160, "inspector-overlay-box-model.png");
    uninstall_inspector_hooks();

    // Layout offsets the child by its margin, so read where it landed.
    const auto box = overlay::bounds_in_root(child_ptr, root);
    REQUIRE(box.width == 120.0f);
    const auto x = static_cast<std::uint32_t>(box.x);
    const auto y = static_cast<std::uint32_t>(box.y + box.height / 2);
    const auto bg = frame.at(2, 2);
    const auto padding = frame.at(x + 6, y);  // inside, in the left padding band
    const auto content = frame.at(x + 60, y); // inside, past the bands
    const auto margin = frame.at(x - 5, y);   // outside, in the left margin band
    const auto beyond = frame.at(x - 15, y);  // outside every band

    // Padding is green: green gains over the selection's own orange wash.
    CHECK(padding.g > content.g + 8);
    // Margin is orange and sits outside the selection fill.
    CHECK(red_lift(margin, bg) > 8);
    CHECK(margin.r > margin.b);
    // Past the margin the surface is untouched.
    CHECK(beyond.r == bg.r);
    CHECK(beyond.g == bg.g);
    CHECK(beyond.b == bg.b);
}

TEST_CASE("per-side padding and margin override the uniform value, and none draws no band",
          "[pulp-vellum-components][inspector-overlay][measure]") {
    View root;
    root.set_bounds({0, 0, 300, 200});
    auto child = std::make_unique<View>();
    child->set_bounds({50, 50, 100, 60});
    View* child_ptr = child.get();
    root.add_child(std::move(child));

    InspectorOverlay inspector(root);
    inspector.set_active(true);
    inspector.set_selected_view(child_ptr);
    const auto plain = paint_count(inspector);

    child_ptr->flex().padding_top = 8.0f;
    const auto top_padding = paint_count(inspector);
    REQUIRE(top_padding > plain);

    child_ptr->flex().margin_left = 6.0f;
    REQUIRE(paint_count(inspector) > top_padding);
}

TEST_CASE("the root has no box model, and parent distances label only gaps above 2 px",
          "[pulp-vellum-components][inspector-overlay][measure]") {
    View root;
    root.set_bounds({0, 0, 300, 200});
    root.flex().padding = 20.0f;
    root.flex().margin = 20.0f;
    auto child = std::make_unique<View>();
    child->set_bounds({0, 0, 300, 200}); // flush with every parent edge
    View* child_ptr = child.get();
    root.add_child(std::move(child));

    InspectorOverlay inspector(root);
    inspector.set_active(true);

    inspector.set_selected_view(&root);
    const auto root_selected = paint_count(inspector);
    root.flex().padding = 0.0f;
    root.flex().margin = 0.0f;
    REQUIRE(paint_count(inspector) == root_selected);

    // A child flush with its parent labels no edge distance; moving it in
    // labels all four.
    inspector.set_selected_view(child_ptr);
    const auto flush = paint_count(inspector);
    child_ptr->set_bounds({20, 20, 260, 160});
    REQUIRE(paint_count(inspector) == flush + 4);
    // A gap of 2 px or less is not labelled.
    child_ptr->set_bounds({2, 2, 296, 196});
    REQUIRE(paint_count(inspector) == flush);
}
