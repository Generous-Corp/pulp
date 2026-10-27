#include <catch2/catch_test_macros.hpp>

#include <pulp/runtime/crypto.hpp>
#include <pulp/view/rmlui_backend.hpp>

#include "support/unique_temp_dir.hpp"
#include <filesystem>
#include <fstream>
#include <vector>

using namespace pulp::view::rmlui;

TEST_CASE("headless RmlUi contract loads, renders, and dispatches") {
    Headless headless;
    headless.add_asset("editor.lua", "return true\n");
    Backend backend(headless, headless, headless, &headless);

    REQUIRE(backend.open(640, 480) == Status::ok);
    REQUIRE(backend.load_document("editor.lua") == Status::ok);
    REQUIRE(backend.dispatch_lua("onload") == Status::ok);
    REQUIRE(backend.invoke_action("parameter.gain") == Status::ok);
    backend.render();

    REQUIRE(backend.is_open());
    REQUIRE(headless.frame_width() == 640);
    REQUIRE(headless.frame_height() == 480);
    REQUIRE(headless.callbacks() == std::vector<std::string>{"editor.lua", "onload"});
    REQUIRE(headless.actions() == std::vector<std::string>{"parameter.gain"});

    backend.close();
    REQUIRE_FALSE(backend.is_open());
    REQUIRE(backend.dispatch_lua("late") == Status::not_open);
}

TEST_CASE("RmlUi backend parses and lowers a document through the Pulp renderer seam") {
    Headless headless;
    headless.add_asset("editor.rml", "<rml><body><div style='display:block;width:120px;height:40px;"
                                     "background-color:#204060;'></div></body></rml>");
    Backend backend(headless, headless, headless);

    REQUIRE(backend.open(640, 480) == Status::ok);
    REQUIRE(backend.load_document("editor.rml") == Status::ok);
    backend.render();
    REQUIRE(headless.frame_width() == 640);
    REQUIRE(headless.frame_height() == 480);
    REQUIRE_FALSE(headless.commands().empty());
    backend.close();
}

TEST_CASE("RmlUi backend admits manifest textures and records their source path") {
    Headless headless;
    // A valid PNG signature and IHDR dimensions are sufficient at this seam;
    // the eventual Skia/Dawn sink owns pixel upload and decoding.
    std::string png("\x89PNG\r\n\x1a\n", 8);
    png.append("\0\0\0\rIHDR\0\0\0\x10\0\0\0\x08\x08\06\0\0\0", 21);
    headless.add_asset("skin/button.png", std::move(png));
    headless.add_asset(
        "editor.rml",
        "<rml><body><img src='skin/button.png' style='width:16px;height:8px;'/></body></rml>");
    Backend backend(headless, headless, headless);

    REQUIRE(backend.open(64, 64) == Status::ok);
    REQUIRE(backend.load_document("editor.rml") == Status::ok);
    backend.render();
    const auto texture = std::find_if(
        headless.commands().begin(), headless.commands().end(),
        [](const DrawCommand& command) { return command.kind == DrawCommand::Kind::texture; });
    REQUIRE(texture != headless.commands().end());
    REQUIRE(texture->texture_path == "skin/button.png");
    REQUIRE(texture->texture_bytes != nullptr);
    REQUIRE(texture->texture_bytes->size() == 29);
    REQUIRE(texture->texture_width == 16);
    REQUIRE(texture->texture_height == 8);
    backend.close();
}

TEST_CASE("RmlUi backend routes host pointer and wheel events") {
    Headless headless;
    headless.add_asset(
        "editor.rml",
        "<rml><body><button style='display:block;width:40px;height:40px;'>x</button></body></rml>");
    Backend backend(headless, headless, headless);

    REQUIRE(backend.open(64, 64) == Status::ok);
    REQUIRE(backend.load_document("editor.rml") == Status::ok);
    backend.render();
    REQUIRE(backend.process_mouse_move(10, 10) == Status::ok);
    REQUIRE(backend.mouse_interacting());
    REQUIRE(backend.process_mouse_button_down(0) == Status::ok);
    REQUIRE(backend.process_mouse_button_up(0) == Status::ok);
    REQUIRE(backend.process_mouse_wheel(0.0f, 1.0f) == Status::ok);
    REQUIRE(backend.process_mouse_leave() == Status::ok);
    REQUIRE_FALSE(backend.mouse_interacting());
    backend.close();
    REQUIRE(backend.process_mouse_move(0, 0) == Status::not_open);
}

TEST_CASE("RmlUi backend fails closed instead of double-initializing process-global RmlUi") {
    Headless first_headless;
    Headless second_headless;
    Backend first(first_headless, first_headless, first_headless);
    Backend second(second_headless, second_headless, second_headless);

    REQUIRE(first.open(32, 32) == Status::ok);
    REQUIRE(second.open(32, 32) == Status::rejected);
    first.close();
    REQUIRE(second.open(32, 32) == Status::ok);
    second.close();
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
    headless.submit(DrawCommand{DrawCommand::Kind::rectangle, 1, 2, 3, 4, {}, {}});
    headless.end_frame();
    REQUIRE(headless.commands().size() == 1);
    headless.begin_frame(20, 10);
    REQUIRE(headless.commands().empty());
}

TEST_CASE("rooted RmlUi assets require a confined, hash-checked manifest") {
    const auto root = pulp::test::make_unique_temp_dir("pulp-rmlui-assets");
    std::ofstream(root / "editor.rml") << "<rml/>\n";
    const auto digest = pulp::runtime::sha256_hex(std::string_view("<rml/>\n"));
    RootedAssetProvider assets(root, {{"editor.rml", digest}});
    Asset loaded;

    REQUIRE(assets.load("editor.rml", loaded) == Status::ok);
    REQUIRE(loaded.bytes == "<rml/>\n");
    RootedAssetProvider capped(root, {{"editor.rml", digest}}, 3);
    REQUIRE(capped.load("editor.rml", loaded) == Status::rejected);
    REQUIRE(assets.load("../editor.rml", loaded) == Status::rejected);
    REQUIRE(assets.load("missing.rml", loaded) == Status::rejected);

    const auto outside = root.parent_path() / (root.filename().string() + "-outside");
    std::ofstream(outside) << "outside";
    std::error_code symlink_error;
    std::filesystem::create_symlink(outside, root / "escape.rml", symlink_error);
    if (!symlink_error) {
        RootedAssetProvider symlink_assets(root,
                                           {{"escape.rml", pulp::runtime::sha256_hex("outside")}});
        REQUIRE(symlink_assets.load("escape.rml", loaded) == Status::rejected);
    }

    std::ofstream(root / "editor.rml", std::ios::trunc) << "tampered";
    REQUIRE(assets.load("editor.rml", loaded) == Status::rejected);
    std::filesystem::remove_all(root);
    std::filesystem::remove(outside);
}
