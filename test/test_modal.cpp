#include <catch2/catch_test_macros.hpp>
#include <pulp/view/modal.hpp>
#include <pulp/canvas/canvas.hpp>
#include <memory>

using namespace pulp::view;
using namespace pulp::canvas;

namespace {

MouseEvent mouse_down(float x, float y) {
    MouseEvent event;
    event.position = {x, y};
    event.is_down = true;
    return event;
}

} // namespace

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

TEST_CASE("ModalOverlay ignores key releases and handles Escape without callback",
          "[view][modal][coverage][issue-493]") {
    ModalOverlay modal;

    KeyEvent release;
    release.key = KeyCode::escape;
    release.is_down = false;
    REQUIRE_FALSE(modal.on_key_event(release));

    KeyEvent esc;
    esc.key = KeyCode::escape;
    esc.is_down = true;
    REQUIRE(modal.on_key_event(esc));
}

TEST_CASE("ModalOverlay paints backdrop", "[view][modal]") {
    ModalOverlay modal;
    modal.set_bounds({0, 0, 800, 600});
    modal.backdrop_opacity = 0.5f;

    RecordingCanvas canvas;
    modal.paint(canvas);
    REQUIRE(canvas.count(DrawCommand::Type::fill_rect) == 1);
}

TEST_CASE("ModalOverlay paint applies backdrop alpha to fill color",
          "[view][modal][coverage][issue-493]") {
    ModalOverlay modal;
    modal.set_bounds({0, 0, 320, 200});
    modal.backdrop_opacity = 0.25f;

    RecordingCanvas canvas;
    modal.paint(canvas);

    REQUIRE(canvas.count(DrawCommand::Type::set_fill_color) == 1);
    REQUIRE(canvas.commands().front().type == DrawCommand::Type::set_fill_color);
    REQUIRE(canvas.commands().front().color.a8() == 63);
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

TEST_CASE("ModalOverlay backdrop mouse dismissal respects hit target and flag",
          "[view][modal][coverage][issue-493]") {
    ModalOverlay modal;
    modal.set_bounds({0, 0, 200, 120});

    auto child = std::make_unique<View>();
    child->set_bounds({20, 20, 80, 40});
    modal.add_child(std::move(child));

    int dismissed = 0;
    modal.on_dismiss = [&] { ++dismissed; };

    auto release = mouse_down(10.0f, 10.0f);
    release.is_down = false;
    modal.on_mouse_event(release);
    REQUIRE(dismissed == 0);

    modal.on_mouse_event(mouse_down(30.0f, 30.0f));
    REQUIRE(dismissed == 0);

    modal.on_mouse_event(mouse_down(150.0f, 100.0f));
    REQUIRE(dismissed == 1);

    modal.dismiss_on_backdrop_click = false;
    modal.on_mouse_event(mouse_down(150.0f, 100.0f));
    REQUIRE(dismissed == 1);
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
