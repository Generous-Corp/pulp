#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "preset_test_sandbox.hpp"
#include <pulp/state/preset_manager.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string_view>

using namespace pulp::state;
namespace fs = std::filesystem;

// Helper to set up a StateStore with test parameters
static void setup_test_store(StateStore& store) {
    store.add_parameter({.id = 1, .name = "Gain", .range = {-60, 12, 0}});
    store.add_parameter({.id = 2, .name = "Mix", .range = {0, 100, 50}});
}

static void write_text_file(const fs::path& path, std::string_view content) {
    if (!path.parent_path().empty()) {
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
    }

    std::ofstream file(path);
    REQUIRE(file.is_open());
    file << content;
}

static PresetInfo require_user_preset(PresetManager& manager, const std::string& name) {
    auto presets = manager.user_presets();
    auto it = std::find_if(presets.begin(), presets.end(), [&](const PresetInfo& preset) {
        return preset.name == name;
    });
    REQUIRE(it != presets.end());
    return *it;
}

TEST_CASE("PresetManager construction sets paths", "[state][preset]") {
    pulp::test::PresetTestSandbox sandbox("pulp-preset-construction");
    StateStore store;
    setup_test_store(store);
    PresetManager pm(store, "TestCo", "TestPlugin");

    REQUIRE_FALSE(pm.user_presets_dir().empty());
    REQUIRE(pm.current_preset_name().empty());
    REQUIRE_FALSE(pm.has_unsaved_changes());
}

TEST_CASE("PresetManager save and load round-trip", "[state][preset]") {
    pulp::test::PresetTestSandbox sandbox("pulp-preset-roundtrip");
    StateStore store;
    setup_test_store(store);

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

    REQUIRE(found);
    REQUIRE(store.get_value(1) < -5.0f); // approximately -6
    REQUIRE(store.get_value(2) > 70.0f); // approximately 75
}

TEST_CASE("PresetManager unsaved changes tracking", "[state][preset]") {
    pulp::test::PresetTestSandbox sandbox("pulp-preset-unsaved");
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
    pulp::test::PresetTestSandbox sandbox("pulp-preset-factory-delete");
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
    pulp::test::PresetTestSandbox sandbox("pulp-preset-factory-rename");
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
    pulp::test::PresetTestSandbox sandbox("pulp-preset-loaded-callback");
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
    pulp::test::PresetTestSandbox sandbox("pulp-preset-import-missing");
    StateStore store;
    setup_test_store(store);
    PresetManager pm(store, "TestCo", "TestPlugin");

    auto result = pm.import_file("/definitely/not/real.json");
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("PresetManager refresh resets cache", "[state][preset]") {
    pulp::test::PresetTestSandbox sandbox("pulp-preset-refresh");
    StateStore store;
    setup_test_store(store);
    PresetManager pm(store, "TestCo", "TestPlugin");

    // Just verify it doesn't crash
    pm.refresh();
}

TEST_CASE("PresetManager navigation returns false with no presets",
          "[state][preset][codecov]") {
    pulp::test::PresetTestSandbox sandbox("pulp-preset-empty-navigation");
    StateStore store;
    setup_test_store(store);
    PresetManager pm(store, "TestCo", "TestPlugin");

    REQUIRE(pm.all_presets().empty());
    REQUIRE(pm.user_presets().empty());
    REQUIRE(pm.factory_presets().empty());
    REQUIRE_FALSE(pm.load_next());
    REQUIRE_FALSE(pm.load_previous());
}

TEST_CASE("PresetManager load rejects malformed files without changing dirty state", "[state][preset]") {
    pulp::test::PresetTestSandbox sandbox("pulp-preset-load-reject");
    StateStore store;
    setup_test_store(store);
    PresetManager pm(store, "TestCo", "TestPlugin");

    pm.set_current_preset_name("DirtyPreset");
    pm.mark_as_changed();

    REQUIRE_FALSE(pm.load(sandbox.root / "missing.json"));
    REQUIRE(pm.current_preset_name() == "DirtyPreset");
    REQUIRE(pm.has_unsaved_changes());

    const auto no_params = sandbox.root / "no-parameters.json";
    write_text_file(no_params, R"json({"name":"NoParameters"})json");
    REQUIRE_FALSE(pm.load(no_params));
    REQUIRE(pm.current_preset_name() == "DirtyPreset");
    REQUIRE(pm.has_unsaved_changes());

    const auto no_brace = sandbox.root / "no-brace.json";
    write_text_file(no_brace, R"json({"parameters": 42})json");
    REQUIRE_FALSE(pm.load(no_brace));
    REQUIRE(pm.current_preset_name() == "DirtyPreset");
    REQUIRE(pm.has_unsaved_changes());
}

TEST_CASE("PresetManager load skips malformed parameter values", "[state][preset]") {
    pulp::test::PresetTestSandbox sandbox("pulp-preset-load-partial");
    StateStore store;
    setup_test_store(store);
    PresetManager pm(store, "TestCo", "TestPlugin");

    store.set_value(1, -12.0f);
    store.set_value(2, 40.0f);

    const auto preset_path = sandbox.root / "Partial.json";
    write_text_file(preset_path,
                    R"json({"parameters":{"Gain":"not-a-number","Mix":25.5,"Ignored":99}})json");

    int load_callbacks = 0;
    pm.on_preset_loaded = [&](const PresetInfo& preset) {
        ++load_callbacks;
        REQUIRE(preset.name == "Partial");
        REQUIRE(preset.path == preset_path);
    };

    REQUIRE(pm.load(preset_path));
    REQUIRE(store.get_value(1) == Catch::Approx(-12.0f));
    REQUIRE(store.get_value(2) == Catch::Approx(25.5f));
    REQUIRE(pm.current_preset_name() == "Partial");
    REQUIRE_FALSE(pm.has_unsaved_changes());
    REQUIRE(load_callbacks == 1);
}

TEST_CASE("PresetManager load skips parameter keys with missing value separators",
          "[state][preset][coverage][phase3]") {
    pulp::test::PresetTestSandbox sandbox("pulp-preset-load-missing-separator");
    StateStore store;
    setup_test_store(store);
    PresetManager pm(store, "TestCo", "TestPlugin");

    store.set_value(1, -12.0f);
    store.set_value(2, 40.0f);

    const auto preset_path = sandbox.root / "MissingSeparator.json";
    write_text_file(preset_path, R"json({"parameters":{"Gain" "bad"}})json");

    REQUIRE(pm.load(preset_path));
    REQUIRE(store.get_value(1) == Catch::Approx(-12.0f));
    REQUIRE(store.get_value(2) == Catch::Approx(40.0f));
    REQUIRE(pm.current_preset_name() == "MissingSeparator");
    REQUIRE_FALSE(pm.has_unsaved_changes());
}

TEST_CASE("PresetManager load clamps out-of-range parameter values",
          "[state][preset][coverage][issue-647]") {
    pulp::test::PresetTestSandbox sandbox("pulp-preset-load-clamp");
    StateStore store;
    setup_test_store(store);
    PresetManager pm(store, "TestCo", "TestPlugin");

    const auto preset_path = sandbox.root / "Clamped.json";
    write_text_file(preset_path,
                    R"json({"parameters":{"Gain":999,"Mix":-50}})json");

    REQUIRE(pm.load(preset_path));
    REQUIRE(store.get_value(1) == Catch::Approx(12.0f));
    REQUIRE(store.get_value(2) == Catch::Approx(0.0f));
    REQUIRE(pm.current_preset_name() == "Clamped");
}

TEST_CASE("PresetManager discovery ignores non-json files and reports nested folders",
          "[state][preset][codecov]") {
    pulp::test::PresetTestSandbox sandbox("pulp-preset-discovery-filter");
    StateStore store;
    setup_test_store(store);
    PresetManager pm(store, "TestCo", "TestPlugin");

    write_text_file(pm.user_presets_dir() / "readme.txt", "not a preset");
    write_text_file(pm.user_presets_dir() / "Bank" / "Lead.json",
                    R"json({"parameters":{"Gain":-9,"Mix":60}})json");
    write_text_file(pm.user_presets_dir() / "Bank" / "Nested" / "Pad.json",
                    R"json({"parameters":{"Gain":-18,"Mix":80}})json");

    auto presets = pm.user_presets();
    REQUIRE(presets.size() == 2);
    REQUIRE(presets[0].name == "Lead");
    REQUIRE(presets[0].folder == "Bank");
    REQUIRE_FALSE(presets[0].is_factory);
    REQUIRE(presets[1].name == "Pad");
    REQUIRE(presets[1].folder.find("Nested") != std::string::npos);
}

TEST_CASE("PresetManager load via PresetInfo and rename preserves other current names",
          "[state][preset][codecov]") {
    pulp::test::PresetTestSandbox sandbox("pulp-preset-info-load-rename");
    StateStore store;
    setup_test_store(store);
    PresetManager pm(store, "TestCo", "TestPlugin");

    store.set_value(1, -6.0f);
    REQUIRE(pm.save("First"));
    store.set_value(1, -12.0f);
    REQUIRE(pm.save("Second"));

    auto first = require_user_preset(pm, "First");
    auto second = require_user_preset(pm, "Second");

    store.set_value(1, 0.0f);
    REQUIRE(pm.load(first));
    REQUIRE(pm.current_preset_name() == "First");
    REQUIRE(store.get_value(1) == Catch::Approx(-6.0f));

    int list_changes = 0;
    pm.on_list_changed = [&] { ++list_changes; };
    REQUIRE(pm.rename(second, "RenamedSecond"));

    REQUIRE(pm.current_preset_name() == "First");
    REQUIRE(list_changes == 1);
    REQUIRE_FALSE(fs::exists(second.path));
    REQUIRE(fs::exists(pm.user_presets_dir() / "RenamedSecond.json"));
}

TEST_CASE("PresetManager save scans subfolders and reports list changes", "[state][preset]") {
    pulp::test::PresetTestSandbox sandbox("pulp-preset-save-subfolder");
    StateStore store;
    setup_test_store(store);
    PresetManager pm(store, "TestCo", "TestPlugin");

    int list_changes = 0;
    pm.on_list_changed = [&] { ++list_changes; };

    REQUIRE(pm.save("Beta", "Bank"));
    REQUIRE(list_changes == 1);

    auto beta = require_user_preset(pm, "Beta");
    REQUIRE(beta.folder == "Bank");
    REQUIRE(beta.path.parent_path().filename() == "Bank");
    REQUIRE(fs::exists(beta.path));

    REQUIRE(pm.save("Alpha"));
    REQUIRE(list_changes == 2);

    auto presets = pm.all_presets();
    REQUIRE(presets.size() == 2);
    REQUIRE(presets[0].name == "Alpha");
    REQUIRE(presets[1].name == "Beta");
}

TEST_CASE("PresetManager user preset scan ignores non-json files and records nested folders",
          "[state][preset][coverage][issue-647]") {
    pulp::test::PresetTestSandbox sandbox("pulp-preset-scan-filter");
    StateStore store;
    setup_test_store(store);
    PresetManager pm(store, "TestCo", "TestPlugin");

    REQUIRE(pm.save("Root"));
    write_text_file(pm.user_presets_dir() / "Bank" / "Lead.json",
                    R"json({"parameters":{"Gain":-6}})json");
    write_text_file(pm.user_presets_dir() / "Bank" / "Ignore.txt", "not a preset");
    std::error_code ec;
    fs::create_directories(pm.user_presets_dir() / "EmptyDir", ec);
    REQUIRE_FALSE(ec);

    auto presets = pm.user_presets();
    REQUIRE(presets.size() == 2);
    REQUIRE(presets[0].name == "Lead");
    REQUIRE(presets[0].folder == "Bank");
    REQUIRE(presets[1].name == "Root");
    REQUIRE(presets[1].folder.empty());
}

TEST_CASE("PresetManager rename and delete update current preset and callbacks", "[state][preset]") {
    pulp::test::PresetTestSandbox sandbox("pulp-preset-rename-delete");
    StateStore store;
    setup_test_store(store);
    PresetManager pm(store, "TestCo", "TestPlugin");

    REQUIRE(pm.save("OldName"));
    auto old_preset = require_user_preset(pm, "OldName");

    int list_changes = 0;
    pm.on_list_changed = [&] { ++list_changes; };

    REQUIRE(pm.rename(old_preset, "NewName"));
    REQUIRE(list_changes == 1);
    REQUIRE(pm.current_preset_name() == "NewName");
    REQUIRE_FALSE(fs::exists(old_preset.path));

    auto new_preset = require_user_preset(pm, "NewName");
    REQUIRE(fs::exists(new_preset.path));

    REQUIRE(pm.delete_preset(new_preset));
    REQUIRE(list_changes == 2);
    REQUIRE_FALSE(fs::exists(new_preset.path));
    REQUIRE(pm.user_presets().empty());

    REQUIRE_FALSE(pm.rename(new_preset, "Again"));
    REQUIRE_FALSE(pm.delete_preset(new_preset));
    REQUIRE(list_changes == 2);
}

TEST_CASE("PresetManager delete missing user preset is inert",
          "[state][preset][coverage][issue-647]") {
    pulp::test::PresetTestSandbox sandbox("pulp-preset-delete-missing");
    StateStore store;
    setup_test_store(store);
    PresetManager pm(store, "TestCo", "TestPlugin");

    int list_changes = 0;
    pm.on_list_changed = [&] { ++list_changes; };

    PresetInfo missing;
    missing.name = "Missing";
    missing.path = sandbox.root / "missing.json";
    missing.is_factory = false;

    REQUIRE_FALSE(pm.delete_preset(missing));
    REQUIRE(list_changes == 0);
}

TEST_CASE("PresetManager import copies external presets into the user directory", "[state][preset]") {
    pulp::test::PresetTestSandbox sandbox("pulp-preset-import-copy");
    StateStore store;
    setup_test_store(store);
    PresetManager pm(store, "TestCo", "TestPlugin");

    const auto external = sandbox.root / "external-source" / "Imported.json";
    write_text_file(external, R"json({"parameters":{"Gain":-18,"Mix":33}})json");

    int list_changes = 0;
    pm.on_list_changed = [&] { ++list_changes; };

    auto imported = pm.import_file(external);
    REQUIRE(imported.has_value());
    REQUIRE(imported->name == "Imported");
    REQUIRE(imported->path.parent_path() == pm.user_presets_dir());
    REQUIRE_FALSE(imported->is_factory);
    REQUIRE(fs::exists(imported->path));
    REQUIRE(list_changes == 1);

    auto listed = require_user_preset(pm, "Imported");
    REQUIRE(listed.path == imported->path);
}

TEST_CASE("PresetManager navigation wraps sorted presets and handles missing current", "[state][preset]") {
    pulp::test::PresetTestSandbox sandbox("pulp-preset-navigation");
    StateStore store;
    setup_test_store(store);
    PresetManager pm(store, "TestCo", "TestPlugin");

    store.set_value(1, -24.0f);
    REQUIRE(pm.save("Alpha"));
    store.set_value(1, -12.0f);
    REQUIRE(pm.save("Beta"));

    pm.set_current_preset_name("Missing");
    store.set_value(1, 0.0f);

    REQUIRE(pm.load_next());
    REQUIRE(pm.current_preset_name() == "Alpha");
    REQUIRE(store.get_value(1) == Catch::Approx(-24.0f));

    REQUIRE(pm.load_previous());
    REQUIRE(pm.current_preset_name() == "Beta");
    REQUIRE(store.get_value(1) == Catch::Approx(-12.0f));

    REQUIRE(pm.load_next());
    REQUIRE(pm.current_preset_name() == "Alpha");
}

TEST_CASE("PresetManager navigation on empty preset list returns false",
          "[state][preset][coverage][issue-647]") {
    pulp::test::PresetTestSandbox sandbox("pulp-preset-navigation-empty");
    StateStore store;
    setup_test_store(store);
    PresetManager pm(store, "TestCo", "TestPlugin");

    REQUIRE_FALSE(pm.load_next());
    REQUIRE_FALSE(pm.load_previous());
    REQUIRE(pm.current_preset_name().empty());
}

TEST_CASE("PresetManager saved file includes metadata and parameter entries",
          "[state][preset][coverage][phase3-large]") {
    pulp::test::PresetTestSandbox sandbox("pulp-preset-save-metadata");
    StateStore store;
    setup_test_store(store);
    PresetManager pm(store, "TestCo", "TestPlugin");

    store.set_value(1, -3.0f);
    REQUIRE(pm.save("Meta"));

    auto preset = require_user_preset(pm, "Meta");
    std::ifstream file(preset.path);
    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());

    REQUIRE(content.find(R"("name": "Meta")") != std::string::npos);
    REQUIRE(content.find(R"("manufacturer": "TestCo")") != std::string::npos);
    REQUIRE(content.find(R"("plugin": "TestPlugin")") != std::string::npos);
    REQUIRE(content.find(R"("Gain")") != std::string::npos);
    REQUIRE(content.find(R"("Mix")") != std::string::npos);
}

TEST_CASE("PresetManager saving the same name overwrites the user preset",
          "[state][preset][coverage][phase3-large]") {
    pulp::test::PresetTestSandbox sandbox("pulp-preset-save-overwrite");
    StateStore store;
    setup_test_store(store);
    PresetManager pm(store, "TestCo", "TestPlugin");

    store.set_value(1, -6.0f);
    REQUIRE(pm.save("Same"));
    store.set_value(1, -18.0f);
    REQUIRE(pm.save("Same"));
    store.set_value(1, 0.0f);

    auto presets = pm.user_presets();
    REQUIRE(presets.size() == 1);
    REQUIRE(pm.load(presets.front()));
    REQUIRE(store.get_value(1) == Catch::Approx(-18.0f));
    REQUIRE(pm.current_preset_name() == "Same");
}

TEST_CASE("PresetManager direct path load reports stem in callback",
          "[state][preset][coverage][phase3-large]") {
    pulp::test::PresetTestSandbox sandbox("pulp-preset-direct-load-callback");
    StateStore store;
    setup_test_store(store);
    PresetManager pm(store, "TestCo", "TestPlugin");

    const auto preset_path = sandbox.root / "external" / "DirectName.json";
    write_text_file(preset_path, R"json({"parameters":{"Gain":-10,"Mix":30}})json");

    PresetInfo loaded;
    pm.on_preset_loaded = [&](const PresetInfo& info) {
        loaded = info;
    };

    REQUIRE(pm.load(preset_path));
    REQUIRE(loaded.name == "DirectName");
    REQUIRE(loaded.path == preset_path);
    REQUIRE_FALSE(loaded.is_factory);
    REQUIRE(store.get_value(1) == Catch::Approx(-10.0f));
}

TEST_CASE("PresetManager import creates the user preset directory",
          "[state][preset][coverage][phase3-large]") {
    pulp::test::PresetTestSandbox sandbox("pulp-preset-import-creates-dir");
    StateStore store;
    setup_test_store(store);
    PresetManager pm(store, "TestCo", "TestPlugin");

    const auto external = sandbox.root / "external" / "Imported.json";
    write_text_file(external, R"json({"parameters":{"Gain":-9}})json");
    std::error_code ec;
    fs::remove_all(pm.user_presets_dir(), ec);
    REQUIRE_FALSE(fs::exists(pm.user_presets_dir()));

    int list_changes = 0;
    pm.on_list_changed = [&] { ++list_changes; };

    auto imported = pm.import_file(external);

    REQUIRE(imported.has_value());
    REQUIRE(imported->name == "Imported");
    REQUIRE(fs::exists(pm.user_presets_dir()));
    REQUIRE(fs::exists(imported->path));
    REQUIRE(list_changes == 1);
    REQUIRE(pm.user_presets().size() == 1);
}

TEST_CASE("PresetManager import of duplicate destination preserves existing preset",
          "[state][preset][coverage][phase3]") {
    pulp::test::PresetTestSandbox sandbox("pulp-preset-import-duplicate");
    StateStore store;
    setup_test_store(store);
    PresetManager pm(store, "TestCo", "TestPlugin");

    REQUIRE(pm.save("Imported"));

    const auto external = sandbox.root / "external" / "Imported.json";
    write_text_file(external, R"json({"parameters":{"Gain":-9}})json");

    int list_changes = 0;
    pm.on_list_changed = [&] { ++list_changes; };

    auto imported = pm.import_file(external);

    REQUIRE(imported.has_value());
    REQUIRE(imported->name == "Imported");
    REQUIRE(list_changes == 1);
    REQUIRE(pm.user_presets().size() == 1);

    store.set_value(1, 0.0f);
    auto listed = require_user_preset(pm, "Imported");
    REQUIRE(pm.load(listed));
    REQUIRE(store.get_value(1) == Catch::Approx(0.0f));
}

TEST_CASE("PresetManager rename failure leaves current preset unchanged",
          "[state][preset][coverage][phase3-large]") {
    pulp::test::PresetTestSandbox sandbox("pulp-preset-rename-missing-current");
    StateStore store;
    setup_test_store(store);
    PresetManager pm(store, "TestCo", "TestPlugin");

    pm.set_current_preset_name("Current");

    PresetInfo missing;
    missing.name = "Current";
    missing.path = sandbox.root / "missing.json";
    missing.is_factory = false;

    REQUIRE_FALSE(pm.rename(missing, "Renamed"));
    REQUIRE(pm.current_preset_name() == "Current");
    REQUIRE_FALSE(fs::exists(sandbox.root / "Renamed.json"));
}
