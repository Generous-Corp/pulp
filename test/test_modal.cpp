#include <catch2/catch_test_macros.hpp>
#include <pulp/view/modal.hpp>
#include <pulp/canvas/canvas.hpp>

#include <memory>

using namespace pulp::view;
using namespace pulp::canvas;

TEST_CASE("ModalOverlay dismiss on Escape", "[view][modal]") {
    ModalOverlay modal;
    bool dismissed = false;
    modal.on_dismiss = [&] { dismissed = true; };

    KeyEvent esc;
    esc.key = KeyCode::escape;
    esc.is_down = true;
    REQUIRE(modal.on_key_event(esc));
    REQUIRE(dismissed);
}

TEST_CASE("ModalOverlay paints backdrop", "[view][modal]") {
    ModalOverlay modal;
    modal.set_bounds({0, 0, 800, 600});
    modal.backdrop_opacity = 0.5f;

    RecordingCanvas canvas;
    modal.paint(canvas);
    REQUIRE(canvas.count(DrawCommand::Type::fill_rect) == 1);
}

TEST_CASE("ModalOverlay backdrop opacity configurable", "[view][modal]") {
    ModalOverlay modal;
    modal.backdrop_opacity = 0.3f;
    REQUIRE(modal.backdrop_opacity == 0.3f);
}

TEST_CASE("ModalOverlay dismiss_on_backdrop_click flag", "[view][modal]") {
    ModalOverlay modal;
    REQUIRE(modal.dismiss_on_backdrop_click); // default true

    modal.dismiss_on_backdrop_click = false;
    REQUIRE_FALSE(modal.dismiss_on_backdrop_click);
}

TEST_CASE("ModalOverlay non-Escape key not handled", "[view][modal]") {
    ModalOverlay modal;
    bool dismissed = false;
    modal.on_dismiss = [&] { dismissed = true; };

    KeyEvent enter;
    enter.key = KeyCode::enter;
    enter.is_down = true;
    REQUIRE_FALSE(modal.on_key_event(enter));
    REQUIRE_FALSE(dismissed);
}

TEST_CASE("ModalOverlay backdrop click dismisses only outside children",
          "[view][modal][coverage][issue-653]") {
    ModalOverlay modal;
    modal.set_bounds({0, 0, 200, 100});
    auto child = std::make_unique<View>();
    child->set_bounds({25, 20, 50, 40});
    modal.add_child(std::move(child));

    int dismiss_count = 0;
    modal.on_dismiss = [&] { ++dismiss_count; };

    MouseEvent child_click;
    child_click.position = {30, 25};
    child_click.is_down = true;
    modal.on_mouse_event(child_click);
    REQUIRE(dismiss_count == 0);

    MouseEvent backdrop_click;
    backdrop_click.position = {150, 80};
    backdrop_click.is_down = true;
    modal.on_mouse_event(backdrop_click);
    REQUIRE(dismiss_count == 1);

    modal.dismiss_on_backdrop_click = false;
    modal.on_mouse_event(backdrop_click);
    REQUIRE(dismiss_count == 1);
}

TEST_CASE("ModalOverlay ignores mouse-up and missing dismiss callback",
          "[view][modal][coverage][issue-653]") {
    ModalOverlay modal;
    modal.set_bounds({0, 0, 80, 40});

    MouseEvent up;
    up.position = {10, 10};
    up.is_down = false;
    modal.on_mouse_event(up);

    MouseEvent down;
    down.position = {10, 10};
    down.is_down = true;
    modal.on_mouse_event(down);
    SUCCEED("modal mouse paths tolerate missing callbacks");
}
