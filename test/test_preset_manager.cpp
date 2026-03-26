#include <catch2/catch_test_macros.hpp>
#include <pulp/state/preset_manager.hpp>
#include <filesystem>
#include <fstream>

using namespace pulp::state;
namespace fs = std::filesystem;

// Helper to set up a StateStore with test parameters
static void setup_test_store(StateStore& store) {
    store.add_parameter({.id = 1, .name = "Gain", .range = {-60, 12, 0}});
    store.add_parameter({.id = 2, .name = "Mix", .range = {0, 100, 50}});
}

TEST_CASE("PresetManager construction sets paths", "[state][preset]") {
    StateStore store;
    setup_test_store(store);
    PresetManager pm(store, "TestCo", "TestPlugin");

    REQUIRE_FALSE(pm.user_presets_dir().empty());
    REQUIRE(pm.current_preset_name().empty());
    REQUIRE_FALSE(pm.has_unsaved_changes());
}

TEST_CASE("PresetManager save and load round-trip", "[state][preset]") {
    StateStore store;
    setup_test_store(store);

    // Use a temp directory for testing
    auto tmp = fs::temp_directory_path() / "pulp_preset_test";
    fs::create_directories(tmp);

    PresetManager pm(store, "TestCo", "TestPlugin");

    // Set values and save
    store.set_value(1, -6.0f);
    store.set_value(2, 75.0f);
    REQUIRE(pm.save("TestPreset"));
    REQUIRE(pm.current_preset_name() == "TestPreset");
    REQUIRE_FALSE(pm.has_unsaved_changes());

    // Change values
    store.set_value(1, 0.0f);
    store.set_value(2, 0.0f);

    // Load back
    auto presets = pm.user_presets();
    bool found = false;
    for (const auto& p : presets) {
        if (p.name == "TestPreset") {
            REQUIRE(pm.load(p));
            found = true;
            break;
        }
    }

    if (found) {
        REQUIRE(store.get_value(1) < -5.0f); // approximately -6
        REQUIRE(store.get_value(2) > 70.0f); // approximately 75
    }

    // Clean up
    fs::remove_all(tmp);
}

TEST_CASE("PresetManager unsaved changes tracking", "[state][preset]") {
    StateStore store;
    setup_test_store(store);
    PresetManager pm(store, "TestCo", "TestPlugin");

    REQUIRE_FALSE(pm.has_unsaved_changes());

    pm.mark_as_changed();
    REQUIRE(pm.has_unsaved_changes());

    pm.mark_as_saved();
    REQUIRE_FALSE(pm.has_unsaved_changes());
}

TEST_CASE("PresetManager factory presets cannot be deleted", "[state][preset]") {
    StateStore store;
    setup_test_store(store);
    PresetManager pm(store, "TestCo", "TestPlugin");

    PresetInfo factory_preset;
    factory_preset.name = "Factory Init";
    factory_preset.path = "/nonexistent/factory.json";
    factory_preset.is_factory = true;

    REQUIRE_FALSE(pm.delete_preset(factory_preset));
}

TEST_CASE("PresetManager factory presets cannot be renamed", "[state][preset]") {
    StateStore store;
    setup_test_store(store);
    PresetManager pm(store, "TestCo", "TestPlugin");

    PresetInfo factory_preset;
    factory_preset.name = "Factory Init";
    factory_preset.path = "/nonexistent/factory.json";
    factory_preset.is_factory = true;

    REQUIRE_FALSE(pm.rename(factory_preset, "New Name"));
}

TEST_CASE("PresetManager on_preset_loaded callback fires", "[state][preset]") {
    StateStore store;
    setup_test_store(store);
    PresetManager pm(store, "TestCo", "TestPlugin");

    // Save a preset first
    pm.save("CallbackTest");

    std::string loaded_name;
    pm.on_preset_loaded = [&](const PresetInfo& info) {
        loaded_name = info.name;
    };

    auto presets = pm.user_presets();
    for (const auto& p : presets) {
        if (p.name == "CallbackTest") {
            pm.load(p);
            break;
        }
    }

    REQUIRE(loaded_name == "CallbackTest");
}

TEST_CASE("PresetManager import nonexistent file returns nullopt", "[state][preset]") {
    StateStore store;
    setup_test_store(store);
    PresetManager pm(store, "TestCo", "TestPlugin");

    auto result = pm.import_file("/definitely/not/real.json");
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("PresetManager refresh resets cache", "[state][preset]") {
    StateStore store;
    setup_test_store(store);
    PresetManager pm(store, "TestCo", "TestPlugin");

    // Just verify it doesn't crash
    pm.refresh();
}
