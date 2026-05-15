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
          "[view][preset_browser][coverage][issue-655]") {
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
