#include <catch2/catch_test_macros.hpp>
#include <pulp/canvas/canvas.hpp>
#include <pulp/view/splash_screen.hpp>

#include <algorithm>
#include <string>

using namespace pulp::view;
using pulp::canvas::DrawCommand;
using pulp::canvas::RecordingCanvas;

namespace {

bool has_text(const RecordingCanvas& canvas, const std::string& text) {
    return std::any_of(canvas.commands().begin(),
                       canvas.commands().end(),
                       [&](const DrawCommand& cmd) {
                           return cmd.type == DrawCommand::Type::fill_text &&
                                  cmd.text == text;
                       });
}

bool has_image(const RecordingCanvas& canvas, const std::string& path) {
    return std::any_of(canvas.commands().begin(),
                       canvas.commands().end(),
                       [&](const DrawCommand& cmd) {
                           return cmd.type == DrawCommand::Type::draw_image &&
                                  cmd.text == path;
                       });
}

} // namespace

TEST_CASE("SplashScreen advances through automatic fade lifecycle",
          "[view][splash]") {
    SplashScreen splash;
    REQUIRE_FALSE(splash.advance(0.1f));

    int dismissed = 0;
    splash.on_dismissed = [&] { ++dismissed; };
    splash.set_fade_in(0.2f);
    splash.set_duration(0.3f);
    splash.set_fade_out(0.4f);

    splash.show();
    REQUIRE(splash.is_showing());

    REQUIRE(splash.advance(0.1f));
    REQUIRE(splash.advance(0.1f));
    REQUIRE(splash.advance(0.3f));
    REQUIRE(splash.advance(0.2f));
    REQUIRE(splash.is_showing());

    REQUIRE_FALSE(splash.advance(0.2f));
    REQUIRE_FALSE(splash.is_showing());
    REQUIRE(dismissed == 1);

    REQUIRE_FALSE(splash.advance(1.0f));
    REQUIRE(dismissed == 1);
}

TEST_CASE("SplashScreen click dismissal honors the click toggle",
          "[view][splash]") {
    SplashScreen splash;
    splash.set_fade_in(0.1f);
    splash.set_duration(10.0f);
    splash.set_fade_out(0.25f);

    int dismissed = 0;
    splash.on_dismissed = [&] { ++dismissed; };

    splash.show();
    splash.set_dismiss_on_click(false);
    splash.on_mouse_down({12.0f, 8.0f});
    REQUIRE(splash.advance(0.5f));
    REQUIRE(splash.is_showing());
    REQUIRE(dismissed == 0);

    splash.set_dismiss_on_click(true);
    splash.on_mouse_down({12.0f, 8.0f});
    REQUIRE_FALSE(splash.advance(0.25f));
    REQUIRE_FALSE(splash.is_showing());
    REQUIRE(dismissed == 1);
}

TEST_CASE("SplashScreen paint records default text and image paths",
          "[view][splash][paint]") {
    SplashScreen splash;
    splash.set_bounds({0.0f, 0.0f, 240.0f, 120.0f});

    RecordingCanvas hidden;
    splash.paint(hidden);
    REQUIRE(hidden.command_count() == 0);

    splash.show();

    RecordingCanvas default_canvas;
    splash.paint(default_canvas);
    REQUIRE(default_canvas.count(DrawCommand::Type::fill_rect) == 1);
    REQUIRE(default_canvas.count(DrawCommand::Type::fill_text) == 1);
    REQUIRE(has_text(default_canvas, "Loading..."));

    splash.set_image("assets/splash.png");
    RecordingCanvas image_canvas;
    splash.paint(image_canvas);
    REQUIRE(image_canvas.count(DrawCommand::Type::fill_rect) == 1);
    REQUIRE(image_canvas.count(DrawCommand::Type::draw_image) == 1);
    REQUIRE(has_image(image_canvas, "assets/splash.png"));
}

TEST_CASE("SplashScreen manual dismiss waits for fade-out before callback",
          "[view][splash][coverage][phase3]") {
    SplashScreen splash;
    splash.set_fade_in(0.1f);
    splash.set_duration(10.0f);
    splash.set_fade_out(0.5f);

    int dismissed = 0;
    splash.on_dismissed = [&] { ++dismissed; };

    splash.show();
    REQUIRE(splash.advance(0.1f));
    splash.dismiss();
    REQUIRE(splash.is_showing());
    REQUIRE(splash.advance(0.25f));
    REQUIRE(dismissed == 0);

    REQUIRE_FALSE(splash.advance(0.25f));
    REQUIRE_FALSE(splash.is_showing());
    REQUIRE(dismissed == 1);
}

TEST_CASE("SplashScreen show restarts state after hidden dismiss",
          "[view][splash][coverage][phase3]") {
    SplashScreen splash;
    splash.set_fade_in(0.2f);
    splash.set_duration(0.2f);
    splash.set_fade_out(0.2f);

    splash.dismiss();
    REQUIRE_FALSE(splash.is_showing());
    REQUIRE_FALSE(splash.advance(1.0f));

    splash.show();
    REQUIRE(splash.is_showing());
    REQUIRE(splash.advance(0.1f));

    RecordingCanvas canvas;
    splash.paint(canvas);
    REQUIRE(canvas.count(DrawCommand::Type::fill_rect) == 1);
    REQUIRE(has_text(canvas, "Loading..."));
}

TEST_CASE("SplashScreen can be shown again after completion",
          "[view][splash][coverage][phase3]") {
    SplashScreen splash;
    splash.set_fade_in(0.01f);
    splash.set_duration(0.01f);
    splash.set_fade_out(0.01f);

    int dismissed = 0;
    splash.on_dismissed = [&] { ++dismissed; };

    splash.show();
    REQUIRE(splash.advance(0.01f));
    REQUIRE(splash.advance(0.01f));
    REQUIRE_FALSE(splash.advance(0.01f));
    REQUIRE(dismissed == 1);

    splash.show();
    REQUIRE(splash.is_showing());
    REQUIRE(splash.advance(0.01f));
    REQUIRE(splash.advance(0.01f));
    REQUIRE_FALSE(splash.advance(0.01f));
    REQUIRE(dismissed == 2);
}

TEST_CASE("SplashScreen zero-duration phases finish deterministically",
          "[view][splash][coverage][phase3]") {
    SplashScreen splash;
    splash.set_fade_in(0.0f);
    splash.set_duration(0.0f);
    splash.set_fade_out(0.0f);

    int dismissed = 0;
    splash.on_dismissed = [&] { ++dismissed; };

    splash.show();
    REQUIRE(splash.is_showing());

    REQUIRE(splash.advance(0.0f));
    REQUIRE(splash.advance(0.0f));
    REQUIRE_FALSE(splash.advance(0.0f));
    REQUIRE_FALSE(splash.is_showing());
    REQUIRE(dismissed == 1);
}
