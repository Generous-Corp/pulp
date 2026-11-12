#include <catch2/catch_test_macros.hpp>
#include <pulp/view/preset_browser.hpp>
#include <pulp/canvas/canvas.hpp>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace pulp::view;
using namespace pulp::state;
using namespace pulp::canvas;
namespace fs = std::filesystem;

namespace {

uint64_t current_process_id() {
#ifdef _WIN32
    return static_cast<uint64_t>(_getpid());
#else
    return static_cast<uint64_t>(getpid());
#endif
}

std::string unique_plugin_name() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return "BrowserTest-" + std::to_string(current_process_id()) + "-" + std::to_string(now);
}

MouseEvent mouse_down(float x, float y, int click_count = 1) {
    MouseEvent event;
    event.position = {x, y};
    event.is_down = true;
    event.click_count = click_count;
    return event;
}

bool has_text_command(const RecordingCanvas& canvas, const std::string& text) {
    for (const auto& cmd : canvas.commands()) {
        if (cmd.type == DrawCommand::Type::fill_text && cmd.text == text) {
            return true;
        }
    }
    return false;
}

} // namespace

// Helper to set up a store and preset manager with test presets
struct TestPresetFixture {
    StateStore store;
    std::unique_ptr<PresetManager> pm;
    fs::path tmp_dir;

    TestPresetFixture() {
        store.add_parameter({.id = 1, .name = "Gain", .range = {-60, 12, 0}});
        store.add_parameter({.id = 2, .name = "Mix", .range = {0, 100, 50}});
        pm = std::make_unique<PresetManager>(store, "TestCo", unique_plugin_name());
        tmp_dir = pm->user_presets_dir();

        // Create some test presets
        store.set_value(1, -6.0f);
        pm->save("Bass Heavy");
        store.set_value(1, 0.0f);
        pm->save("Clean");
        store.set_value(1, 6.0f);
        pm->save("Bright Lead");
    }

    ~TestPresetFixture() {
        if (!tmp_dir.empty() && fs::exists(tmp_dir)) {
            std::error_code ec;
            fs::remove_all(tmp_dir, ec);
        }
    }
};

TEST_CASE("PresetBrowser shows all presets", "[view][preset_browser]") {
    TestPresetFixture f;
    PresetBrowser browser(*f.pm);
    REQUIRE(browser.visible_count() == 3);
}

TEST_CASE("PresetBrowser filter narrows list", "[view][preset_browser]") {
    TestPresetFixture f;
    PresetBrowser browser(*f.pm);

    browser.set_filter("bass");
    REQUIRE(browser.visible_count() == 1);

    browser.set_filter("");
    REQUIRE(browser.visible_count() == 3);
}

TEST_CASE("PresetBrowser filter is case-insensitive", "[view][preset_browser]") {
    TestPresetFixture f;
    PresetBrowser browser(*f.pm);

    browser.set_filter("BRIGHT");
    REQUIRE(browser.visible_count() == 1);
}

TEST_CASE("PresetBrowser filter matches folder case-insensitively", "[view][preset_browser]") {
    TestPresetFixture f;
    f.store.set_value(1, 3.0f);
    REQUIRE(f.pm->save("Soft Pad", "Keys/Analog"));

    PresetBrowser browser(*f.pm);
    browser.set_filter("keys");

    REQUIRE(browser.visible_count() == 1);
    browser.select_next();
    REQUIRE(browser.selected_preset() != nullptr);
    REQUIRE(browser.selected_preset()->name == "Soft Pad");
}

TEST_CASE("PresetBrowser filtering clamps stale selection", "[view][preset_browser]") {
    TestPresetFixture f;
    PresetBrowser browser(*f.pm);

    browser.select_next();
    browser.select_next();
    browser.select_next();
    REQUIRE(browser.selected_index() == 2);

    browser.set_filter("bass");

    REQUIRE(browser.visible_count() == 1);
    REQUIRE(browser.selected_index() == 0);
    REQUIRE(browser.selected_preset() != nullptr);
    REQUIRE(browser.selected_preset()->name == "Bass Heavy");
}

TEST_CASE("PresetBrowser select next/previous", "[view][preset_browser]") {
    TestPresetFixture f;
    PresetBrowser browser(*f.pm);

    browser.select_next();
    REQUIRE(browser.selected_index() == 0);

    browser.select_next();
    REQUIRE(browser.selected_index() == 1);

    browser.select_previous();
    REQUIRE(browser.selected_index() == 0);
}

TEST_CASE("PresetBrowser select wraps around", "[view][preset_browser]") {
    TestPresetFixture f;
    PresetBrowser browser(*f.pm);

    // Navigate to last item
    browser.select_next(); // 0
    browser.select_next(); // 1
    browser.select_next(); // 2 (last)
    REQUIRE(browser.selected_index() == 2);

    browser.select_next(); // wraps to 0
    REQUIRE(browser.selected_index() == 0);

    browser.select_previous(); // wraps to 2
    REQUIRE(browser.selected_index() == 2);
}

TEST_CASE("PresetBrowser on_preset_selected fires", "[view][preset_browser]") {
    TestPresetFixture f;
    PresetBrowser browser(*f.pm);

    std::string selected_name;
    browser.on_preset_selected = [&](const PresetInfo& p) {
        selected_name = p.name;
    };

    browser.select_next();
    REQUIRE_FALSE(selected_name.empty());
}

TEST_CASE("PresetBrowser key up/down navigates", "[view][preset_browser]") {
    TestPresetFixture f;
    PresetBrowser browser(*f.pm);
    browser.select_next(); // select first

    KeyEvent down;
    down.key = KeyCode::down;
    down.is_down = true;
    REQUIRE(browser.on_key_event(down));
    REQUIRE(browser.selected_index() == 1);

    KeyEvent up;
    up.key = KeyCode::up;
    up.is_down = true;
    REQUIRE(browser.on_key_event(up));
    REQUIRE(browser.selected_index() == 0);
}

TEST_CASE("PresetBrowser show mode filters factory/user", "[view][preset_browser]") {
    TestPresetFixture f;
    PresetBrowser browser(*f.pm);

    // All presets are user presets in this fixture
    browser.set_show_mode(PresetBrowser::ShowMode::factory_only);
    REQUIRE(browser.visible_count() == 0);

    browser.set_show_mode(PresetBrowser::ShowMode::user_only);
    REQUIRE(browser.visible_count() == 3);

    browser.set_show_mode(PresetBrowser::ShowMode::all);
    REQUIRE(browser.visible_count() == 3);
}

TEST_CASE("PresetBrowser empty filtered list ignores navigation and enter", "[view][preset_browser]") {
    TestPresetFixture f;
    PresetBrowser browser(*f.pm);

    int selected_count = 0;
    int activated_count = 0;
    browser.on_preset_selected = [&](const PresetInfo&) { ++selected_count; };
    browser.on_preset_activated = [&](const PresetInfo&) { ++activated_count; };

    browser.set_show_mode(PresetBrowser::ShowMode::factory_only);
    browser.select_next();
    browser.select_previous();

    KeyEvent enter;
    enter.key = KeyCode::enter;
    enter.is_down = true;

    REQUIRE(browser.visible_count() == 0);
    REQUIRE(browser.selected_index() == -1);
    REQUIRE_FALSE(browser.on_key_event(enter));
    REQUIRE(selected_count == 0);
    REQUIRE(activated_count == 0);
}

TEST_CASE("PresetBrowser key release and unknown key are not handled", "[view][preset_browser]") {
    TestPresetFixture f;
    PresetBrowser browser(*f.pm);

    KeyEvent release;
    release.key = KeyCode::down;
    release.is_down = false;
    REQUIRE_FALSE(browser.on_key_event(release));

    KeyEvent unknown;
    unknown.key = KeyCode::escape;
    unknown.is_down = true;
    REQUIRE_FALSE(browser.on_key_event(unknown));
}

TEST_CASE("PresetBrowser enter activates selected preset", "[view][preset_browser]") {
    TestPresetFixture f;
    PresetBrowser browser(*f.pm);
    browser.select_next();

    std::string activated_name;
    browser.on_preset_activated = [&](const PresetInfo& p) {
        activated_name = p.name;
    };

    KeyEvent enter;
    enter.key = KeyCode::enter;
    enter.is_down = true;

    REQUIRE(browser.on_key_event(enter));
    REQUIRE(activated_name == "Bass Heavy");
}

TEST_CASE("PresetBrowser mouse header arrows navigate and activate", "[view][preset_browser]") {
    TestPresetFixture f;
    PresetBrowser browser(*f.pm);
    browser.set_bounds({0, 0, 120, 160});

    std::vector<std::string> activated;
    browser.on_preset_activated = [&](const PresetInfo& p) {
        activated.push_back(p.name);
    };

    MouseEvent release = mouse_down(8.0f, 12.0f);
    release.is_down = false;
    browser.on_mouse_event(release);
    REQUIRE(activated.empty());
    REQUIRE(browser.selected_index() == -1);

    browser.on_mouse_event(mouse_down(8.0f, 12.0f));
    REQUIRE(browser.selected_index() == 2);
    REQUIRE(activated == std::vector<std::string>{"Clean"});

    browser.on_mouse_event(mouse_down(110.0f, 12.0f));
    REQUIRE(browser.selected_index() == 0);
    REQUIRE(activated == std::vector<std::string>{"Clean", "Bass Heavy"});

    browser.on_mouse_event(mouse_down(60.0f, 12.0f));
    REQUIRE(browser.selected_index() == 0);
    REQUIRE(activated.size() == 2);
}

TEST_CASE("PresetBrowser mouse list selects and double-click activates", "[view][preset_browser]") {
    TestPresetFixture f;
    PresetBrowser browser(*f.pm);
    browser.set_bounds({0, 0, 160, 180});

    std::string selected_name;
    std::string activated_name;
    browser.on_preset_selected = [&](const PresetInfo& p) {
        selected_name = p.name;
    };
    browser.on_preset_activated = [&](const PresetInfo& p) {
        activated_name = p.name;
    };

    browser.on_mouse_event(mouse_down(40.0f, 62.0f));
    REQUIRE(browser.selected_index() == 1);
    REQUIRE(selected_name == "Bright Lead");
    REQUIRE(activated_name.empty());

    browser.on_mouse_event(mouse_down(40.0f, 86.0f, 2));
    REQUIRE(browser.selected_index() == 2);
    REQUIRE(selected_name == "Clean");
    REQUIRE(activated_name == "Clean");

    browser.on_mouse_event(mouse_down(40.0f, 500.0f));
    REQUIRE(browser.selected_index() == 2);
}

TEST_CASE("PresetBrowser filtered list click uses filter offset and row height",
          "[view][preset_browser][issue-655]") {
    TestPresetFixture f;
    PresetBrowser browser(*f.pm);
    browser.set_bounds({0, 0, 180, 180});
    browser.set_row_height(30.0f);
    browser.set_filter("clean");

    std::string selected_name;
    browser.on_preset_selected = [&](const PresetInfo& p) {
        selected_name = p.name;
    };

    browser.on_mouse_event(mouse_down(40.0f, 40.0f));
    REQUIRE(browser.selected_index() == -1);
    REQUIRE(selected_name.empty());

    browser.on_mouse_event(mouse_down(40.0f, 58.0f));
    REQUIRE(browser.selected_index() == 0);
    REQUIRE(selected_name == "Clean");
    REQUIRE(browser.row_height() == 30.0f);
}

TEST_CASE("PresetBrowser ignores filtered clicks before the list starts",
          "[view][preset_browser][issue-655]") {
    TestPresetFixture f;
    PresetBrowser browser(*f.pm);
    browser.set_bounds({0, 0, 180, 180});
    browser.set_filter("clean");

    int selected_count = 0;
    browser.on_preset_selected = [&](const PresetInfo&) { ++selected_count; };

    browser.on_mouse_event(mouse_down(40.0f, 41.0f));

    REQUIRE(browser.selected_index() == -1);
    REQUIRE(selected_count == 0);
}

TEST_CASE("PresetBrowser paint produces draw commands", "[view][preset_browser]") {
    TestPresetFixture f;
    PresetBrowser browser(*f.pm);
    browser.set_bounds({0, 0, 300, 400});

    RecordingCanvas canvas;
    browser.paint(canvas);

    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) > 0);
    REQUIRE(canvas.count(DrawCommand::Type::fill_text) > 0);
}

TEST_CASE("PresetBrowser paint includes filter and modified current preset text", "[view][preset_browser]") {
    TestPresetFixture f;
    f.pm->set_current_preset_name("Clean");
    f.pm->mark_as_changed();

    PresetBrowser browser(*f.pm);
    browser.set_bounds({0, 0, 220, 140});
    browser.set_filter("lead");
    browser.select_next();

    RecordingCanvas canvas;
    browser.paint(canvas);

    REQUIRE(has_text_command(canvas, "Clean *"));
    REQUIRE(has_text_command(canvas, "Filter: lead"));
    REQUIRE(has_text_command(canvas, "Bright Lead"));
    REQUIRE(canvas.count(DrawCommand::Type::clip_rect) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::restore) == 1);
}

TEST_CASE("PresetBrowser refresh updates list", "[view][preset_browser]") {
    TestPresetFixture f;
    PresetBrowser browser(*f.pm);
    REQUIRE(browser.visible_count() == 3);

    // Save another preset
    f.store.set_value(1, 12.0f);
    f.pm->save("New One");

    browser.refresh();
    REQUIRE(browser.visible_count() == 4);
}

// ── Library mode ────────────────────────────────────────────────────────────

namespace {

struct LibraryFixture : TestPresetFixture {
    fs::path overlay;
    std::unique_ptr<PresetLibrary> library;

    LibraryFixture() {
        overlay = tmp_dir.parent_path() / (unique_plugin_name() + "-overlay") / "library.json";
        library = std::make_unique<PresetLibrary>(*pm, overlay);
    }
    ~LibraryFixture() {
        std::error_code ec;
        fs::remove_all(overlay.parent_path(), ec);
    }
    // Select the row whose key is `key` by walking the list.
    void select(PresetBrowser& b, const std::string& key) {
        for (int i = 0; i < b.visible_count(); ++i)
            if (b.summary_at(i)->key == key) {
                b.on_mouse_event(mouse_down(60, 32 + 4 + static_cast<float>(i) * 24 + 12));
                return;
            }
        FAIL("no row for " << key);
    }
};

} // namespace

TEST_CASE("PresetBrowser library mode lists the library and falls back when detached",
          "[view][preset_browser][library]") {
    LibraryFixture f;
    PresetBrowser browser(*f.pm);
    browser.set_bounds({0, 0, 300, 400});
    REQUIRE_FALSE(browser.has_library());
    REQUIRE(browser.summary_at(0) == nullptr);

    browser.set_library(f.library.get());
    REQUIRE(browser.has_library());
    REQUIRE(browser.visible_count() == 3);
    REQUIRE(browser.summary_at(0) != nullptr);

    browser.set_library(nullptr);
    REQUIRE(browser.visible_count() == 3);
    REQUIRE(browser.summary_at(0) == nullptr);
}

TEST_CASE("PresetBrowser clicking the star column toggles the favourite and keeps the row selected",
          "[view][preset_browser][library]") {
    LibraryFixture f;
    PresetBrowser browser(*f.pm);
    browser.set_bounds({0, 0, 300, 400});
    browser.set_library(f.library.get());
    f.select(browser, "user:Clean");
    REQUIRE(browser.selected_summary()->key == "user:Clean");
    REQUIRE_FALSE(browser.selected_summary()->favourite);

    const int row = browser.selected_index();
    browser.on_mouse_event(mouse_down(8, 32 + 4 + static_cast<float>(row) * 24 + 12));
    REQUIRE(browser.selected_summary()->key == "user:Clean");
    REQUIRE(browser.selected_summary()->favourite);
    REQUIRE(f.library->find("user:Clean")->favourite);

    // Without the library the same click is a plain selection (no edit).
    browser.set_library(nullptr);
    browser.on_mouse_event(mouse_down(8, 32 + 4 + 12));
    REQUIRE(f.library->find("user:Clean")->favourite);
}

TEST_CASE("PresetBrowser F key toggles the favourite only in library mode",
          "[view][preset_browser][library]") {
    LibraryFixture f;
    PresetBrowser browser(*f.pm);
    browser.set_bounds({0, 0, 300, 400});
    browser.select_next();

    KeyEvent key;
    key.key = KeyCode::f;
    key.is_down = true;
    REQUIRE_FALSE(browser.on_key_event(key)); // no library: unhandled

    browser.set_library(f.library.get());
    f.select(browser, "user:Bright Lead");
    REQUIRE(browser.on_key_event(key));
    REQUIRE(f.library->find("user:Bright Lead")->favourite);
    REQUIRE(browser.on_key_event(key));
    REQUIRE_FALSE(f.library->find("user:Bright Lead")->favourite);

    key.modifiers = kModCmd; // Cmd+F is not ours
    REQUIRE_FALSE(browser.on_key_event(key));
}

TEST_CASE("PresetBrowser favourites and tag filters narrow the library list",
          "[view][preset_browser][library]") {
    LibraryFixture f;
    PresetBrowser browser(*f.pm);
    browser.set_library(f.library.get());

    f.select(browser, "user:Clean");
    REQUIRE(browser.toggle_favourite_selected());
    REQUIRE(browser.add_tag_to_selected("pad"));
    browser.set_favourites_only(true);
    REQUIRE(browser.visible_count() == 1);
    REQUIRE(browser.summary_at(0)->key == "user:Clean");
    browser.set_favourites_only(false);

    browser.set_tag_filter("PAD");
    REQUIRE(browser.visible_count() == 1);
    browser.set_tag_filter("");
    REQUIRE(browser.visible_count() == 3);

    f.select(browser, "user:Clean");
    REQUIRE(browser.remove_tag_from_selected("pad"));
    browser.set_tag_filter("pad");
    REQUIRE(browser.visible_count() == 0);
}

TEST_CASE("PresetBrowser rename changes the shown name, not the file",
          "[view][preset_browser][library]") {
    LibraryFixture f;
    PresetBrowser browser(*f.pm);
    browser.set_bounds({0, 0, 300, 400});
    browser.set_library(f.library.get());
    f.select(browser, "user:Clean");
    REQUIRE(browser.rename_selected("Squeaky Clean"));

    RecordingCanvas canvas;
    browser.paint(canvas);
    REQUIRE(has_text_command(canvas, "Squeaky Clean"));
    REQUIRE_FALSE(has_text_command(canvas, "Clean"));
    REQUIRE(fs::exists(f.tmp_dir / "Clean.json"));
    // The callback payload keeps the file's own name, so loading is unaffected.
    REQUIRE(browser.selected_preset()->name == "Clean");

    REQUIRE(browser.rename_selected(""));
    RecordingCanvas again;
    browser.paint(again);
    REQUIRE(has_text_command(again, "Clean"));
}

TEST_CASE("PresetBrowser library search takes every word and survives edits",
          "[view][preset_browser][library]") {
    LibraryFixture f;
    PresetBrowser browser(*f.pm);
    browser.set_library(f.library.get());
    browser.set_filter("bright lead");
    REQUIRE(browser.visible_count() == 1);
    browser.set_filter("lead bass");
    REQUIRE(browser.visible_count() == 0); // both words must match one preset
    browser.set_filter("");
    REQUIRE(browser.visible_count() == 3);
}

TEST_CASE("PresetBrowser library rows draw the star, tags and program number",
          "[view][preset_browser][library]") {
    LibraryFixture f;
    f.library->enable_program_map();
    PresetBrowser browser(*f.pm);
    browser.set_bounds({0, 0, 300, 400});
    browser.set_library(f.library.get());
    f.select(browser, "user:Clean");
    browser.toggle_favourite_selected();
    browser.add_tag_to_selected("pad");

    RecordingCanvas canvas;
    browser.paint(canvas);
    REQUIRE(has_text_command(canvas, "\xe2\x98\x85")); // filled star on the favourite
    REQUIRE(has_text_command(canvas, "\xe2\x98\x86")); // outline stars on the others
    bool tags_and_program = false;
    for (const auto& cmd : canvas.commands())
        if (cmd.type == DrawCommand::Type::fill_text &&
            cmd.text.find("#pad") != std::string::npos && cmd.text.find("1:") != std::string::npos)
            tags_and_program = true;
    REQUIRE(tags_and_program);

    // Without a library none of this appears.
    browser.set_library(nullptr);
    RecordingCanvas plain;
    browser.paint(plain);
    REQUIRE_FALSE(has_text_command(plain, "\xe2\x98\x85"));
}

TEST_CASE("PresetBrowser edit helpers refuse without a library or a selection",
          "[view][preset_browser][library]") {
    LibraryFixture f;
    PresetBrowser browser(*f.pm);
    REQUIRE_FALSE(browser.toggle_favourite_selected());
    REQUIRE_FALSE(browser.add_tag_to_selected("x"));
    browser.set_library(f.library.get());
    REQUIRE_FALSE(browser.rename_selected("x")); // nothing selected yet
}
