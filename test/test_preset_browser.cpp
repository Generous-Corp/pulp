#include <catch2/catch_test_macros.hpp>
#include <pulp/view/preset_browser.hpp>
#include <pulp/canvas/canvas.hpp>
#include <filesystem>
#include <fstream>
#include <chrono>

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

TEST_CASE("PresetBrowser paint produces draw commands", "[view][preset_browser]") {
    TestPresetFixture f;
    PresetBrowser browser(*f.pm);
    browser.set_bounds({0, 0, 300, 400});

    RecordingCanvas canvas;
    browser.paint(canvas);

    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) > 0);
    REQUIRE(canvas.count(DrawCommand::Type::fill_text) > 0);
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
