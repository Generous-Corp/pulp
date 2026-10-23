#include <catch2/catch_test_macros.hpp>
#include "support/unique_temp_dir.hpp"
#include <filesystem>
#include <fstream>
#include <pulp/view/scripted_ui.hpp>
#include <pulp/view/widgets.hpp>
#include <string>

using namespace pulp::view;
using namespace pulp::state;
namespace fs = std::filesystem;

namespace {

void write_text(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

std::string label_text(ScriptedUiSession& session, const char* id) {
    REQUIRE(session.bridge() != nullptr);
    auto* label = dynamic_cast<Label*>(session.bridge()->widget(id));
    REQUIRE(label != nullptr);
    return label->text();
}

} // namespace

TEST_CASE("ScriptedUiSession loads an embedded script source without a file",
          "[view][scripted-ui][embedded]") {
    View root;
    root.set_bounds({0, 0, 320, 240});
    StateStore store;
    ScriptedUiSession session(root, store,
                              {.enable_hot_reload = false,
                               .enable_theme_reload = false,
                               .script_source = "createLabel('embedded-status', 'ok', '');",
                               .script_name = "ui.js"});

    REQUIRE(session.uses_embedded_source());
    REQUIRE(session.script_path().empty());
    REQUIRE(session.theme_path().empty());
    REQUIRE(session.script_label() == "ui.js");

    std::string error;
    REQUIRE(session.load(&error));
    REQUIRE(error.empty());
    REQUIRE(label_text(session, "embedded-status") == "ok");

    // reload() re-evaluates the same embedded source rather than a file.
    REQUIRE(session.reload(&error));
    REQUIRE(error.empty());
    REQUIRE(label_text(session, "embedded-status") == "ok");
}

TEST_CASE("an embedded session refuses to load an empty source",
          "[view][scripted-ui][embedded]") {
    View root;
    root.set_bounds({0, 0, 320, 240});
    StateStore store;
    ScriptedUiSession session(root, store, {.script_name = "ui.js"});

    std::string error;
    REQUIRE_FALSE(session.load_deferrable(&error));
    REQUIRE(error.find("embedded script source is empty") != std::string::npos);
    REQUIRE(error.find("ui.js") != std::string::npos);
    REQUIRE(session.bridge() == nullptr);
}

TEST_CASE("an embedded session with hot reload enabled creates no file watcher",
          "[view][scripted-ui][embedded][hotreload]") {
    View root;
    root.set_bounds({0, 0, 320, 240});
    StateStore store;

    SECTION("embedded: the flag is kept, nothing is watched") {
        ScriptedUiSession session(root, store,
                                  {.enable_hot_reload = true,
                                   .enable_theme_reload = true,
                                   .script_source = "createLabel('s', 'embedded', '');",
                                   .script_name = "ui.js"});
        std::string error;
        REQUIRE(session.load(&error));
        REQUIRE(error.empty());
        REQUIRE(session.hot_reload_enabled());
        REQUIRE_FALSE(session.hot_reload_watching());
        REQUIRE_FALSE(session.hot_reload_pending());
        // A poll finds no file to re-read and reports no change from the
        // reload or theme watchers.
        std::string poll_error;
        (void)session.poll(&poll_error);
        REQUIRE(poll_error.empty());
        REQUIRE(label_text(session, "s") == "embedded");
    }

    SECTION("control: a file-backed session with the same flag watches its file") {
        const auto temp_dir = pulp::test::make_unique_temp_dir("pulp-scripted-embed-control");
        const auto script_path = temp_dir / "main.js";
        write_text(script_path, "createLabel('s', 'file', '');");
        ScriptedUiSession session(root, store,
                                  {.script_path = script_path,
                                   .enable_hot_reload = true,
                                   .enable_theme_reload = false});
        std::string error;
        REQUIRE(session.load(&error));
        REQUIRE(error.empty());
        REQUIRE_FALSE(session.uses_embedded_source());
        REQUIRE(session.hot_reload_watching());
        fs::remove_all(temp_dir);
    }
}

TEST_CASE("an embedded theme override applies without a theme file",
          "[view][scripted-ui][embedded][theme]") {
    View root;
    root.set_bounds({0, 0, 320, 240});
    root.set_theme(Theme::dark());
    StateStore store;
    ScriptedUiSession session(root, store,
                              {.enable_hot_reload = false,
                               .enable_theme_reload = true,
                               .script_source = "createLabel('t', 'themed', '');",
                               .theme_source = R"({"colors":{"bg.primary":"#ff0000"}})",
                               .script_name = "ui.js"});

    std::string error;
    REQUIRE(session.load(&error));
    REQUIRE(error.empty());
    REQUIRE(session.theme_path().empty());

    const auto bg = root.theme().color("bg.primary");
    REQUIRE(bg.has_value());
    REQUIRE(bg->r8() == 0xFF);
    REQUIRE(bg->g8() == 0x00);
    REQUIRE(bg->b8() == 0x00);
}

TEST_CASE("reload_from an empty path returns a session to its embedded source",
          "[view][scripted-ui][embedded]") {
    const auto temp_dir = pulp::test::make_unique_temp_dir("pulp-scripted-embed-swap");
    const auto other = temp_dir / "other" / "main.js";
    write_text(other, "createLabel('swap', 'file', '');");

    View root;
    root.set_bounds({0, 0, 320, 240});
    StateStore store;
    ScriptedUiSession session(root, store,
                              {.enable_hot_reload = false,
                               .enable_theme_reload = false,
                               .script_source = "createLabel('swap', 'embedded', '');",
                               .script_name = "ui.js"});

    std::string error;
    REQUIRE(session.load(&error));
    REQUIRE(label_text(session, "swap") == "embedded");

    REQUIRE(session.reload_from(other, &error));
    REQUIRE(error.empty());
    REQUIRE_FALSE(session.uses_embedded_source());
    REQUIRE(session.script_path() == other);
    REQUIRE(label_text(session, "swap") == "file");

    // The swap-unit rollback path: the pre-swap script_path() was empty.
    REQUIRE(session.reload_from(fs::path{}, &error));
    REQUIRE(error.empty());
    REQUIRE(session.uses_embedded_source());
    REQUIRE(session.theme_path().empty());
    REQUIRE(label_text(session, "swap") == "embedded");

    fs::remove_all(temp_dir);
}

TEST_CASE("reload_from an empty path fails closed without an embedded source",
          "[view][scripted-ui][embedded]") {
    const auto temp_dir = pulp::test::make_unique_temp_dir("pulp-scripted-embed-noswap");
    const auto script_path = temp_dir / "main.js";
    write_text(script_path, "createLabel('n', 'file', '');");

    View root;
    root.set_bounds({0, 0, 320, 240});
    StateStore store;
    ScriptedUiSession session(root, store,
                              {.script_path = script_path,
                               .enable_hot_reload = false,
                               .enable_theme_reload = false});
    std::string error;
    REQUIRE(session.load(&error));
    REQUIRE_FALSE(session.reload_from(fs::path{}, &error));
    REQUIRE(error.find("no embedded script source") != std::string::npos);
    REQUIRE(session.script_path() == script_path);
    REQUIRE(label_text(session, "n") == "file");
    fs::remove_all(temp_dir);
}
