#include <catch2/catch_test_macros.hpp>
#include <pulp/view/modal.hpp>
#include <pulp/canvas/canvas.hpp>

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
