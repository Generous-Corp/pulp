#include <catch2/catch_test_macros.hpp>

#include <pulp/view/rmlui_backend.hpp>

using namespace pulp::view::rmlui;

TEST_CASE("headless RmlUi contract loads, renders, and dispatches") {
    Headless headless;
    headless.add_asset("editor.rml", "<rml/>\n");
    Backend backend(headless, headless, headless, &headless);

    REQUIRE(backend.open(640, 480) == Status::ok);
    REQUIRE(backend.load_document("editor.rml") == Status::ok);
    REQUIRE(backend.dispatch_lua("onload") == Status::ok);
    REQUIRE(backend.invoke_action("parameter.gain") == Status::ok);
    backend.render();

    REQUIRE(backend.is_open());
    REQUIRE(headless.frame_width() == 640);
    REQUIRE(headless.frame_height() == 480);
    REQUIRE(headless.callbacks() == std::vector<std::string>{"editor.rml", "onload"});
    REQUIRE(headless.actions() == std::vector<std::string>{"parameter.gain"});

    backend.close();
    REQUIRE_FALSE(backend.is_open());
    REQUIRE(backend.dispatch_lua("late") == Status::not_open);
}

TEST_CASE("headless RmlUi contract fails closed for missing and rejected assets") {
    Headless headless;
    headless.reject_path("../escape.rml");
    Backend backend(headless, headless, headless);

    REQUIRE(backend.load_document("editor.rml") == Status::not_open);
    REQUIRE(backend.open(1, 1) == Status::ok);
    REQUIRE(backend.load_document("missing.rml") == Status::missing_asset);
    REQUIRE(backend.load_document("../escape.rml") == Status::rejected);
    REQUIRE(backend.invoke_action("unsupported") == Status::rejected);
}

TEST_CASE("headless RmlUi render commands are isolated per frame") {
    Headless headless;
    headless.begin_frame(100, 80);
    headless.submit(DrawCommand{DrawCommand::Kind::rectangle, 1, 2, 3, 4, {}});
    headless.end_frame();
    REQUIRE(headless.commands().size() == 1);
    headless.begin_frame(20, 10);
    REQUIRE(headless.commands().empty());
}
