#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <choc/text/choc_JSON.h>
#include "preset_test_sandbox.hpp"
#include <pulp/runtime/crypto.hpp>
#include <pulp/state/content_registry.hpp>
#include <pulp/state/preset_manager.hpp>
#include "../external/miniz/miniz.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string_view>
#include <vector>

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

static fs::path write_content_pack(const fs::path& data_root,
                                   const std::string& plugin_id,
                                   const std::string& package_id,
                                   const std::string& version,
                                   std::string_view capabilities_json = R"json(["content.presets.v1","content.themes.v1","content.samples.v1","content.wavetables.v1"])json") {
    const auto root = data_root / "Content" / plugin_id / package_id / version;
    write_text_file(root / "pulp.package.json",
                    std::string(R"json({
  "schema": "pulp-package-v1",
  "id": ")json") + package_id + R"json(",
  "name": "Expansion One",
  "version": ")json" + version + R"json(",
  "license": "MIT",
  "kind": ["content-pack"],
  "capabilities": )json" + std::string(capabilities_json) + R"json(,
  "exports": {
    "presets": ["presets"],
    "themes": ["themes"],
    "samples": ["samples"],
    "wavetables": ["wavetables"]
  },
  "dependencies": {"pulp": [], "packages": []},
  "validation": {}
})json");
    write_text_file(root / "presets" / "ExpansionInit.json",
                    R"json({"name":"Expansion Init","parameters":{"Gain":-12,"Mix":25}})json");
    write_text_file(root / "themes" / "Dark.json", R"json({"name":"Dark"})json");
    write_text_file(root / "samples" / "Kick.wav", "PULP fixture sample placeholder.");
    write_text_file(root / "wavetables" / "Sine.wavetable.json",
                    R"json({"name":"Sine","frames":1,"samples":[0,1,0,-1]})json");
    return root;
}

static fs::path write_source_content_pack(const fs::path& root,
                                          const std::string& package_id,
                                          const std::string& version,
                                          std::string_view capabilities_json = R"json(["content.presets.v1","content.themes.v1"])json") {
    write_text_file(root / "pulp.package.json",
                    std::string(R"json({
  "schema": "pulp-package-v1",
  "id": ")json") + package_id + R"json(",
  "name": "Dropped Expansion",
  "version": ")json" + version + R"json(",
  "license": "MIT",
  "kind": ["content-pack"],
  "capabilities": )json" + std::string(capabilities_json) + R"json(,
  "exports": {
    "presets": ["presets"],
    "themes": ["themes"]
  },
  "dependencies": {"pulp": [], "packages": []},
  "validation": {}
})json");
    write_text_file(root / "presets" / "DropInit.json",
                    R"json({"name":"Drop Init","parameters":{"Gain":-18,"Mix":30}})json");
    write_text_file(root / "themes" / "DropDark.json", R"json({"name":"Drop Dark"})json");
    return root;
}

static fs::path write_content_pack_archive(const fs::path& archive,
                                           const fs::path& source_root,
                                           bool add_unlisted_payload = false) {
    mz_zip_archive zip{};
    REQUIRE(mz_zip_writer_init_file(&zip, archive.string().c_str(), 0));
    std::error_code ec;
    std::vector<std::pair<std::string, std::string>> hashes;
    for (fs::recursive_directory_iterator it(source_root, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const auto rel = fs::relative(it->path(), source_root, ec).generic_string();
        std::ifstream file(it->path(), std::ios::binary);
        std::string body((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        REQUIRE(mz_zip_writer_add_mem(&zip, rel.c_str(), body.data(), body.size(),
                                      MZ_DEFAULT_COMPRESSION));
        hashes.push_back({rel, "sha256-" + pulp::runtime::sha256_hex(body)});
    }
    std::sort(hashes.begin(), hashes.end());
    std::string sha_manifest = "{\n  \"schema\": \"pulp-files-sha256-v1\",\n  \"files\": {";
    for (std::size_t i = 0; i < hashes.size(); ++i) {
        sha_manifest += i == 0 ? "\n" : ",\n";
        sha_manifest += "    \"" + hashes[i].first + "\": \"" + hashes[i].second + "\"";
    }
    sha_manifest += hashes.empty() ? "\n  }\n}\n" : "\n  }\n}\n";
    REQUIRE(mz_zip_writer_add_mem(&zip, "files.sha256.json",
                                  sha_manifest.data(), sha_manifest.size(),
                                  MZ_DEFAULT_COMPRESSION));
    if (add_unlisted_payload) {
        const std::string extra = "not declared in files.sha256.json";
        REQUIRE(mz_zip_writer_add_mem(&zip, "extras/unlisted.txt",
                                      extra.data(), extra.size(),
                                      MZ_DEFAULT_COMPRESSION));
    }
    REQUIRE(mz_zip_writer_finalize_archive(&zip));
    mz_zip_writer_end(&zip);
    return archive;
}

static fs::path write_content_pack_archive_without_hashes(const fs::path& archive,
                                                          const fs::path& source_root) {
    mz_zip_archive zip{};
    REQUIRE(mz_zip_writer_init_file(&zip, archive.string().c_str(), 0));
    std::error_code ec;
    for (fs::recursive_directory_iterator it(source_root, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const auto rel = fs::relative(it->path(), source_root, ec).generic_string();
        std::ifstream file(it->path(), std::ios::binary);
        std::string body((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        REQUIRE(mz_zip_writer_add_mem(&zip, rel.c_str(), body.data(), body.size(),
                                      MZ_DEFAULT_COMPRESSION));
    }
    REQUIRE(mz_zip_writer_finalize_archive(&zip));
    mz_zip_writer_end(&zip);
    return archive;
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

TEST_CASE("PresetManager save is atomic and leaves no temp file behind",
          "[state][preset][reliability]") {
    pulp::test::PresetTestSandbox sandbox("pulp-preset-atomic");
    StateStore store;
    setup_test_store(store);
    PresetManager pm(store, "TestCo", "TestPlugin");

    REQUIRE(pm.save("Atomic"));

    const fs::path preset = fs::path(pm.user_presets_dir()) / "Atomic.json";
    REQUIRE(fs::exists(preset));
    REQUIRE_FALSE(fs::exists(fs::path(pm.user_presets_dir()) / "Atomic.json.tmp"));

    // The written file is valid, parseable JSON (not a half-written fragment).
    std::ifstream f(preset);
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    REQUIRE_NOTHROW(choc::json::parse(content));
}

TEST_CASE("PresetManager escapes metadata so special characters stay valid JSON",
          "[state][preset][reliability]") {
    pulp::test::PresetTestSandbox sandbox("pulp-preset-escape");
    StateStore store;
    setup_test_store(store);

    // Manufacturer / plugin / preset name carrying quotes and backslashes used
    // to produce a corrupt, unparseable preset file via raw `<<` interpolation.
    // Windows preset filenames cannot contain quotes or backslashes, so keep
    // the preset name filesystem-safe there while still exercising metadata
    // escaping through the manufacturer and plugin fields.
    PresetManager pm(store, R"(Acme "Audio" \ Co)", R"(Plug"in)");
#ifdef _WIN32
    const std::string preset_name = "My Best Preset";
#else
    const std::string preset_name = R"(My "Best" \ Preset)";
#endif
    REQUIRE(pm.save(preset_name));

    const fs::path preset = fs::path(pm.user_presets_dir()) / (preset_name + ".json");
    REQUIRE(fs::exists(preset));

    std::ifstream f(preset);
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());

    // The file parses, and the escaped metadata round-trips through a real
    // JSON parser with the exact original (unescaped) values.
    auto doc = choc::json::parse(content);
    REQUIRE(doc.isObject());
    REQUIRE(std::string(doc["manufacturer"].getString()) == R"(Acme "Audio" \ Co)");
    REQUIRE(std::string(doc["plugin"].getString()) == R"(Plug"in)");
    REQUIRE(std::string(doc["name"].getString()) == preset_name);
}

#if defined(_WIN32)
TEST_CASE("PresetManager sanitizes Windows preset identity path components",
          "[state][preset][reliability]") {
    pulp::test::PresetTestSandbox sandbox("pulp-preset-windows-path-components");
    StateStore store;
    setup_test_store(store);

    PresetManager pm(store, "..", R"(C:\Temp\..\Plug|In?)");

    REQUIRE(fs::path(pm.user_presets_dir()) ==
            sandbox.root / "AppData" / "_" / "C__Temp_.._Plug_In_" / "Presets");
    REQUIRE(pm.save("PathSafe"));
    REQUIRE(fs::exists(fs::path(pm.user_presets_dir()) / "PathSafe.json"));
}
#endif

TEST_CASE("PresetManager writes valid JSON even for non-finite param values",
          "[state][preset][reliability]") {
    // std::clamp does not filter NaN/inf, so a misbehaving setter or corrupt
    // restored state can leave a non-finite param value. Streaming it raw would
    // emit the bare token `nan`/`inf` and produce an unparseable preset file;
    // save() substitutes a finite value so the JSON always parses.
    pulp::test::PresetTestSandbox sandbox("pulp-preset-nonfinite");
    StateStore store;
    setup_test_store(store);  // params id 1 (Gain) and 2 (Mix)

    // setup_test_store defaults: Gain (id 1) → 0, Mix (id 2) → 50.
    // NaN is the reachable non-finite case: std::clamp(±inf) returns a finite
    // range bound, but std::clamp(NaN) returns NaN, so a NaN set_value survives
    // into get_value() and would stream as the bare `nan` token.
    PresetManager pm(store, "TestCo", "TestPlugin");
    store.set_value(1, std::numeric_limits<float>::quiet_NaN());
    store.set_value(2, std::numeric_limits<float>::quiet_NaN());
    REQUIRE(pm.save("NonFinite"));

    const fs::path preset = fs::path(pm.user_presets_dir()) / "NonFinite.json";
    REQUIRE(fs::exists(preset));
    std::ifstream f(preset);
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());

    // The bug streamed bare `nan`/`inf` tokens, which are NOT valid JSON, so
    // choc::json::parse would throw. Parsing must now succeed.
    auto doc = choc::json::parse(content);  // throws on the old bare-token output
    REQUIRE(doc.isObject());
    REQUIRE(doc.hasObjectMember("parameters"));

    // End-to-end: a non-finite param is persisted as its registered DEFAULT
    // (not a blanket 0, which for Mix's [0,100] default=50 would be a real
    // non-default value / range-min on reload). Reload and confirm each param
    // restored to its default.
    store.set_value(1, 5.0f);
    store.set_value(2, 25.0f);   // perturb before reloading
    auto restored = require_user_preset(pm, "NonFinite");
    REQUIRE(pm.load(restored));
    REQUIRE(std::fabs(store.get_value(1) - 0.0f) < 0.01f);    // Gain default 0
    REQUIRE(std::fabs(store.get_value(2) - 50.0f) < 0.01f);   // Mix default 50
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
          "[state][preset][coverage]") {
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

TEST_CASE("PresetManager load accepts parameter values that end at EOF",
          "[state][preset][coverage]") {
    pulp::test::PresetTestSandbox sandbox("pulp-preset-load-terminal-value");
    StateStore store;
    setup_test_store(store);
    PresetManager pm(store, "TestCo", "TestPlugin");

    store.set_value(1, 0.0f);
    store.set_value(2, 40.0f);

    const auto preset_path = sandbox.root / "TerminalValue.json";
    write_text_file(preset_path, R"json({"parameters":{"Gain":-6)json");

    REQUIRE(pm.load(preset_path));
    REQUIRE(store.get_value(1) == Catch::Approx(-6.0f));
    REQUIRE(store.get_value(2) == Catch::Approx(40.0f));
    REQUIRE(pm.current_preset_name() == "TerminalValue");
    REQUIRE_FALSE(pm.has_unsaved_changes());
}

TEST_CASE("PresetManager load rejects partial numeric parameter values", "[state][preset]") {
    pulp::test::PresetTestSandbox sandbox("pulp-preset-load-partial-numeric");
    StateStore store;
    setup_test_store(store);
    PresetManager pm(store, "TestCo", "TestPlugin");

    store.set_value(1, -12.0f);
    store.set_value(2, 40.0f);

    const auto preset_path = sandbox.root / "PartialNumeric.json";
    write_text_file(preset_path,
                    R"json({"parameters":{"Gain":-6.0,"Mix":25.5Hz}})json");

    REQUIRE(pm.load(preset_path));
    REQUIRE(store.get_value(1) == Catch::Approx(-6.0f));
    REQUIRE(store.get_value(2) == Catch::Approx(40.0f));
    REQUIRE(pm.current_preset_name() == "PartialNumeric");
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
          "[state][preset][coverage][large]") {
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
          "[state][preset][coverage][large]") {
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
          "[state][preset][coverage][large]") {
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
          "[state][preset][coverage][large]") {
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
          "[state][preset][coverage]") {
    pulp::test::PresetTestSandbox sandbox("pulp-preset-import-duplicate");
    StateStore store;
    setup_test_store(store);
    PresetManager pm(store, "TestCo", "TestPlugin");

    store.set_value(1, -6.0f);
    store.set_value(2, 80.0f);
    REQUIRE(pm.save("Imported"));

    const auto external = sandbox.root / "external" / "Imported.json";
    write_text_file(external, R"json({"parameters":{"Gain":-30,"Mix":20}})json");

    int list_changes = 0;
    pm.on_list_changed = [&] { ++list_changes; };

    auto imported = pm.import_file(external);

    REQUIRE(imported.has_value());
    REQUIRE(imported->name == "Imported");
    REQUIRE(imported->path == pm.user_presets_dir() / "Imported.json");
    REQUIRE(list_changes == 1);
    REQUIRE(pm.user_presets().size() == 1);

    store.set_value(1, 0.0f);
    store.set_value(2, 0.0f);
    REQUIRE(pm.load(*imported));
    REQUIRE(store.get_value(1) == Catch::Approx(-6.0f));
    REQUIRE(store.get_value(2) == Catch::Approx(80.0f));
}

TEST_CASE("PresetManager rename failure leaves current preset unchanged",
          "[state][preset][coverage][large]") {
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

TEST_CASE("ContentRegistry enumerates installed content packs by plugin capability",
          "[state][preset][content]") {
    pulp::test::PresetTestSandbox sandbox("pulp-content-registry");
    const auto data_root = sandbox.root / "user-data";
    const auto pack_root = write_content_pack(data_root,
                                              "dev.pulp.test.plugin",
                                              "dev.pulp.test.expansion",
                                              "0.1.0");
    const auto backup_root =
        pack_root.parent_path() / ".pulp-content-update-backup-dev.pulp.test.expansion-0.1.0";
    fs::copy(pack_root, backup_root,
             fs::copy_options::recursive | fs::copy_options::overwrite_existing);

    ContentRegistry registry(data_root);

    auto all = registry.packs_for_plugin("dev.pulp.test.plugin");
    REQUIRE(all.size() == 1);
    REQUIRE(all.front().id == "dev.pulp.test.expansion");
    REQUIRE(all.front().root == pack_root);
    REQUIRE(all.front().presets.size() == 1);
    REQUIRE(all.front().themes.size() == 1);
    REQUIRE(all.front().samples.size() == 1);
    REQUIRE(all.front().wavetables.size() == 1);

    ContentCapabilityManifest matching;
    matching.plugin_id = "dev.pulp.test.plugin";
    matching.capabilities = {"content.presets.v1"};
    matching.content_kinds = {"presets"};
    REQUIRE(registry.packs_for_plugin(matching).size() == 1);

    matching.capabilities = {"content.samples.v1"};
    matching.content_kinds = {"samples"};
    auto sample_packs = registry.packs_for_plugin(matching);
    REQUIRE(sample_packs.size() == 1);
    REQUIRE(sample_packs.front().samples.front().filename() == "Kick.wav");
    REQUIRE(registry.presets_for_plugin(matching).empty());

    matching.capabilities = {"content.themes.v1"};
    matching.content_kinds = {"themes"};
    auto theme_packs = registry.packs_for_plugin(matching);
    REQUIRE(theme_packs.size() == 1);
    REQUIRE(theme_packs.front().themes.front().filename() == "Dark.json");
    REQUIRE(registry.presets_for_plugin(matching).empty());

    matching.capabilities = {"content.wavetables.v1"};
    matching.content_kinds = {"wavetables"};
    auto wavetable_packs = registry.packs_for_plugin(matching);
    REQUIRE(wavetable_packs.size() == 1);
    REQUIRE(wavetable_packs.front().wavetables.front().filename() == "Sine.wavetable.json");

    matching.capabilities = {"content.ir.v1"};
    matching.content_kinds = {"samples"};
    REQUIRE(registry.packs_for_plugin(matching).empty());
}

TEST_CASE("ContentRegistry ignores installed packs with unsafe export paths",
          "[state][preset][content][security]") {
    pulp::test::PresetTestSandbox sandbox("pulp-content-registry-unsafe-export");
    const auto data_root = sandbox.root / "user-data";
    const auto pack_root = write_content_pack(data_root,
                                              "dev.pulp.test.plugin",
                                              "dev.pulp.test.unsafe",
                                              "0.1.0");
    write_text_file(pack_root / "pulp.package.json", R"json({
  "schema": "pulp-package-v1",
  "id": "dev.pulp.test.unsafe",
  "name": "Unsafe",
  "version": "0.1.0",
  "kind": ["content-pack"],
  "capabilities": ["content.presets.v1"],
  "exports": {
    "presets": ["../../Documents"]
  }
})json");

    ContentRegistry registry(data_root);
    REQUIRE(registry.packs_for_plugin("dev.pulp.test.plugin").empty());

    ContentCapabilityManifest manifest;
    manifest.plugin_id = "dev.pulp.test.plugin";
    manifest.capabilities = {"content.presets.v1"};
    manifest.content_kinds = {"presets"};
    REQUIRE(registry.presets_for_plugin(manifest).empty());
}

TEST_CASE("ContentCapabilityManifest parses plugin runtime content opt-in",
          "[state][preset][content]") {
    std::string error;
    auto manifest = parse_content_capability_manifest(R"json({
  "schema": "pulp.plugin-runtime.v1",
  "pluginId": "dev.pulp.test.plugin",
  "content": {
    "capabilities": ["content.presets.v1", "content.themes.hotReload.v1"],
    "kinds": ["presets", "themes"],
    "reload": {
      "hotReloadKinds": ["themes"],
      "manualRescanKinds": ["presets"]
    }
  }
})json", &error);

    REQUIRE(manifest.has_value());
    REQUIRE(error.empty());
    REQUIRE(manifest->schema == "pulp.plugin-runtime.v1");
    REQUIRE(manifest->plugin_id == "dev.pulp.test.plugin");
    REQUIRE(manifest->capabilities.size() == 2);
    REQUIRE(manifest->content_kinds.size() == 2);
    REQUIRE(manifest->hot_reload_kinds == std::vector<std::string>{"themes"});
    REQUIRE(manifest->manual_rescan_kinds == std::vector<std::string>{"presets"});

    const auto json = content_capability_manifest_to_json(*manifest);
    auto round_trip = parse_content_capability_manifest(json, &error);
    REQUIRE(round_trip.has_value());
    REQUIRE(round_trip->plugin_id == manifest->plugin_id);
    REQUIRE(round_trip->capabilities == manifest->capabilities);
    REQUIRE(round_trip->content_kinds == manifest->content_kinds);
    REQUIRE(round_trip->hot_reload_kinds == manifest->hot_reload_kinds);
    REQUIRE(round_trip->manual_rescan_kinds == manifest->manual_rescan_kinds);

    REQUIRE_FALSE(parse_content_capability_manifest(R"json({
  "schema": "pulp.plugin-runtime.v1",
  "pluginId": "dev.pulp.test.plugin",
  "content": { "capabilities": [], "kinds": ["presets"] }
})json", &error));
    REQUIRE(error.find("content.capabilities") != std::string::npos);

    REQUIRE_FALSE(parse_content_capability_manifest(R"json({
  "schema": "pulp.plugin-runtime.v1",
  "pluginId": "dev.pulp.test.plugin",
  "content": {
    "capabilities": ["content.presets.v1"],
    "kinds": ["presets"],
    "reload": { "hotReloadKinds": ["themes"] }
  }
})json", &error));
    REQUIRE(error.find("hotReloadKinds") != std::string::npos);
}

TEST_CASE("ContentCapabilityManifest loads from disk and drives PresetManager content opt-in",
          "[state][preset][content]") {
    pulp::test::PresetTestSandbox sandbox("pulp-content-manifest");
    const auto data_root = sandbox.root / "user-data";
    write_content_pack(data_root,
                       "dev.pulp.test.plugin",
                       "dev.pulp.test.expansion",
                       "0.1.0");

    const auto manifest_path = sandbox.root / "bundle" / "pulp.plugin-runtime.json";
    write_text_file(manifest_path, R"json({
  "schema": "pulp.plugin-runtime.v1",
  "pluginId": "dev.pulp.test.plugin",
  "content": {
    "capabilities": ["content.presets.v1"],
    "kinds": ["presets"]
  }
})json");

    std::string error;
    auto manifest = load_content_capability_manifest(manifest_path, &error);
    REQUIRE(manifest.has_value());
    REQUIRE(error.empty());

    StateStore store;
    setup_test_store(store);
    PresetManager pm(store, "TestCo", "TestPlugin");
    pm.set_content_data_root(data_root);
    pm.set_content_manifest(*manifest);

    auto content = pm.content_presets();
    REQUIRE(content.size() == 1);
    REQUIRE(content.front().name == "ExpansionInit");

    manifest->content_kinds = {"samples"};
    pm.set_content_manifest(*manifest);
    REQUIRE(pm.content_presets().empty());
}

TEST_CASE("PresetManager exposes content-pack presets as read-only expansions",
          "[state][preset][content]") {
    pulp::test::PresetTestSandbox sandbox("pulp-preset-content-pack");
    const auto data_root = sandbox.root / "user-data";
    write_content_pack(data_root,
                       "dev.pulp.test.plugin",
                       "dev.pulp.test.expansion",
                       "0.1.0");

    StateStore store;
    setup_test_store(store);
    PresetManager pm(store, "TestCo", "TestPlugin");
    pm.set_content_data_root(data_root);
    pm.set_content_plugin_id("dev.pulp.test.plugin");
    pm.set_content_capabilities({"content.presets.v1"});

    auto content = pm.content_presets();
    REQUIRE(content.size() == 1);
    REQUIRE(content.front().name == "ExpansionInit");
    REQUIRE(content.front().is_factory);
    REQUIRE(content.front().folder == "Expansion One");
    REQUIRE_FALSE(pm.delete_preset(content.front()));
    REQUIRE_FALSE(pm.rename(content.front(), "RenamedExpansion"));

    store.set_value(1, 0.0f);
    store.set_value(2, 0.0f);
    REQUIRE(pm.load(content.front()));
    REQUIRE(store.get_value(1) == Catch::Approx(-12.0f));
    REQUIRE(store.get_value(2) == Catch::Approx(25.0f));

    auto all = pm.all_presets();
    REQUIRE(std::any_of(all.begin(), all.end(), [](const PresetInfo& preset) {
        return preset.name == "ExpansionInit" && preset.is_factory;
    }));
    REQUIRE(pm.user_presets().empty());

    store.set_value(1, -3.0f);
    REQUIRE(pm.save("Expansion Edit"));
    auto user = require_user_preset(pm, "Expansion Edit");
    REQUIRE_FALSE(user.is_factory);
    REQUIRE(user.path.parent_path() == pm.user_presets_dir());

    auto content_after_save = pm.content_presets();
    REQUIRE(content_after_save.size() == 1);
    REQUIRE(fs::exists(content_after_save.front().path));
}

TEST_CASE("runtime content installer previews and installs only after approval",
          "[state][preset][content]") {
    pulp::test::PresetTestSandbox sandbox("pulp-content-runtime-install");
    const auto data_root = sandbox.root / "user-data";
    const auto source = write_source_content_pack(sandbox.root / "incoming",
                                                  "dev.pulp.test.dropped-expansion",
                                                  "0.1.0");

    ContentCapabilityManifest manifest;
    manifest.plugin_id = "dev.pulp.test.plugin";
    manifest.capabilities = {"content.presets.v1", "content.themes.v1"};
    manifest.content_kinds = {"presets", "themes"};
    manifest.hot_reload_kinds = {"themes"};
    manifest.manual_rescan_kinds = {"presets"};

    auto preview = preview_content_pack_install(source, manifest, data_root);
    REQUIRE(preview.ok);
    REQUIRE(preview.pack.id == "dev.pulp.test.dropped-expansion");
    REQUIRE(preview.install_root == data_root / "Content" / "dev.pulp.test.plugin" /
                                      "dev.pulp.test.dropped-expansion" / "0.1.0");
    REQUIRE(preview.policies.size() == 2);
    REQUIRE_FALSE(fs::exists(preview.install_root));

    auto refused = install_content_pack(source, manifest, data_root, false);
    REQUIRE_FALSE(refused.ok);
    REQUIRE_FALSE(fs::exists(preview.install_root));
    REQUIRE(std::any_of(refused.issues.begin(), refused.issues.end(), [](const std::string& issue) {
        return issue.find("approval-required") != std::string::npos;
    }));

    auto installed = install_content_pack(source, manifest, data_root, true);
    REQUIRE(installed.ok);
    REQUIRE(fs::exists(preview.install_root / "pulp.package.json"));
    REQUIRE(fs::exists(preview.install_root / "presets" / "DropInit.json"));
    REQUIRE(fs::exists(data_root / "Content" / "index.json"));

    auto duplicate = install_content_pack(source, manifest, data_root, true);
    REQUIRE_FALSE(duplicate.ok);
    REQUIRE(std::any_of(duplicate.issues.begin(), duplicate.issues.end(), [](const std::string& issue) {
        return issue.find("already-installed") != std::string::npos;
    }));
    REQUIRE(fs::exists(preview.install_root / "presets" / "DropInit.json"));

    const auto index = [&] {
        std::ifstream f(data_root / "Content" / "index.json");
        return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }();
    REQUIRE(index.find(R"("plugin_id":"dev.pulp.test.plugin")") != std::string::npos);

    ContentRegistry registry(data_root);
    auto packs = registry.packs_for_plugin(manifest);
    REQUIRE(packs.size() == 1);
    REQUIRE(packs.front().id == "dev.pulp.test.dropped-expansion");
}

TEST_CASE("runtime content installer rejects bare manifest files before copying parent directories",
          "[state][preset][content][security]") {
    pulp::test::PresetTestSandbox sandbox("pulp-content-runtime-manifest-file");
    const auto data_root = sandbox.root / "user-data";
    const auto source = write_source_content_pack(sandbox.root / "incoming",
                                                  "dev.pulp.test.manifest-file",
                                                  "0.1.0");
    write_text_file(source / "unrelated-secret.txt", "not content\n");

    ContentCapabilityManifest manifest;
    manifest.plugin_id = "dev.pulp.test.plugin";
    manifest.capabilities = {"content.presets.v1", "content.themes.v1"};
    manifest.content_kinds = {"presets", "themes"};

    auto installed = install_content_pack(source / "pulp.package.json", manifest, data_root, true);
    REQUIRE_FALSE(installed.ok);
    REQUIRE(std::any_of(installed.issues.begin(), installed.issues.end(), [](const std::string& issue) {
        return issue.find("bare manifest file") != std::string::npos;
    }));
    REQUIRE_FALSE(fs::exists(data_root / "Content"));
}

TEST_CASE("runtime content installer rejects incompatible drops before mutation",
          "[state][preset][content]") {
    pulp::test::PresetTestSandbox sandbox("pulp-content-runtime-reject");
    const auto data_root = sandbox.root / "user-data";
    const auto source = write_source_content_pack(sandbox.root / "incoming",
                                                  "dev.pulp.test.incompatible-expansion",
                                                  "0.1.0",
                                                  R"json(["content.themes.v1"])json");

    ContentCapabilityManifest manifest;
    manifest.plugin_id = "dev.pulp.test.plugin";
    manifest.capabilities = {"content.presets.v1"};
    manifest.content_kinds = {"presets"};

    auto preview = preview_content_pack_install(source, manifest, data_root);
    REQUIRE_FALSE(preview.ok);
    REQUIRE(std::any_of(preview.issues.begin(), preview.issues.end(), [](const std::string& issue) {
        return issue.find("capability-mismatch") != std::string::npos;
    }));

    auto installed = install_content_pack(source, manifest, data_root, true);
    REQUIRE_FALSE(installed.ok);
    REQUIRE_FALSE(fs::exists(data_root / "Content" / "dev.pulp.test.plugin" /
                             "dev.pulp.test.incompatible-expansion"));
}

TEST_CASE("runtime content installer rejects unsafe manifest paths before mutation",
          "[state][preset][content]") {
    pulp::test::PresetTestSandbox sandbox("pulp-content-runtime-unsafe");
    const auto data_root = sandbox.root / "user-data";

    ContentCapabilityManifest manifest;
    manifest.plugin_id = "dev.pulp.test.plugin";
    manifest.capabilities = {"content.presets.v1"};
    manifest.content_kinds = {"presets"};

    const auto unsafe_id = write_source_content_pack(sandbox.root / "unsafe-id",
                                                     "../outside",
                                                     "0.1.0");
    auto preview = preview_content_pack_install(unsafe_id, manifest, data_root);
    REQUIRE_FALSE(preview.ok);
    REQUIRE(std::any_of(preview.issues.begin(), preview.issues.end(), [](const std::string& issue) {
        return issue.find("invalid-id") != std::string::npos;
    }));
    REQUIRE_FALSE(fs::exists(data_root / "Content"));

    const auto unsafe_export = sandbox.root / "unsafe-export";
    write_text_file(unsafe_export / "pulp.package.json", R"json({
  "schema": "pulp-package-v1",
  "id": "dev.pulp.test.unsafe-export",
  "name": "Unsafe Export",
  "version": "0.1.0",
  "license": "MIT",
  "kind": ["content-pack"],
  "capabilities": ["content.presets.v1"],
  "exports": { "presets": ["../outside"] },
  "dependencies": {"pulp": [], "packages": []},
  "validation": {}
})json");
    write_text_file(unsafe_export / "presets" / "Safe.json", R"json({"name":"Safe"})json");
    preview = preview_content_pack_install(unsafe_export, manifest, data_root);
    REQUIRE_FALSE(preview.ok);
    REQUIRE(std::any_of(preview.issues.begin(), preview.issues.end(), [](const std::string& issue) {
        return issue.find("unsafe-export") != std::string::npos;
    }));
    REQUIRE_FALSE(fs::exists(data_root / "Content"));

    ContentCapabilityManifest unsafe_plugin = manifest;
    unsafe_plugin.plugin_id = "../plugin";
    const auto source = write_source_content_pack(sandbox.root / "safe-source",
                                                  "dev.pulp.test.safe-expansion",
                                                  "0.1.0");
    preview = preview_content_pack_install(source, unsafe_plugin, data_root);
    REQUIRE_FALSE(preview.ok);
    REQUIRE(std::any_of(preview.issues.begin(), preview.issues.end(), [](const std::string& issue) {
        return issue.find("invalid-plugin-id") != std::string::npos;
    }));
    REQUIRE_FALSE(fs::exists(data_root / "Content"));
}

TEST_CASE("runtime content installer rejects directories missing declared exports",
          "[state][preset][content][security]") {
    pulp::test::PresetTestSandbox sandbox("pulp-content-runtime-missing-export");
    const auto data_root = sandbox.root / "user-data";
    const auto source = write_source_content_pack(sandbox.root / "incoming",
                                                  "dev.pulp.test.missing-export",
                                                  "0.1.0");
    fs::remove_all(source / "presets");

    ContentCapabilityManifest manifest;
    manifest.plugin_id = "dev.pulp.test.plugin";
    manifest.capabilities = {"content.presets.v1"};
    manifest.content_kinds = {"presets"};

    auto preview = preview_content_pack_install(source, manifest, data_root);
    REQUIRE_FALSE(preview.ok);
    REQUIRE(std::any_of(preview.issues.begin(), preview.issues.end(), [](const std::string& issue) {
        return issue.find("missing-path: `presets`") != std::string::npos;
    }));

    auto installed = install_content_pack(source, manifest, data_root, true);
    REQUIRE_FALSE(installed.ok);
    REQUIRE_FALSE(fs::exists(data_root / "Content"));
}

TEST_CASE("runtime content installer removes copied files when index write fails",
          "[state][preset][content][security]") {
    pulp::test::PresetTestSandbox sandbox("pulp-content-runtime-index-rollback");
    const auto data_root = sandbox.root / "user-data";
    const auto source = write_source_content_pack(sandbox.root / "incoming",
                                                  "dev.pulp.test.index-rollback",
                                                  "0.1.0");

    ContentCapabilityManifest manifest;
    manifest.plugin_id = "dev.pulp.test.plugin";
    manifest.capabilities = {"content.presets.v1"};
    manifest.content_kinds = {"presets"};

    const auto install_root = data_root / "Content" / manifest.plugin_id /
                              "dev.pulp.test.index-rollback" / "0.1.0";
    fs::create_directories(data_root / "Content" / "index.json");

    auto installed = install_content_pack(source, manifest, data_root, true);
    REQUIRE_FALSE(installed.ok);
    REQUIRE(std::any_of(installed.issues.begin(), installed.issues.end(), [](const std::string& issue) {
        return issue.find("index-write") != std::string::npos;
    }));
    REQUIRE_FALSE(fs::exists(install_root));
}

TEST_CASE("runtime content installer accepts packed pulpcontent archives",
          "[state][preset][content]") {
    pulp::test::PresetTestSandbox sandbox("pulp-content-runtime-archive");
    const auto data_root = sandbox.root / "user-data";
    const auto source = write_source_content_pack(sandbox.root / "incoming",
                                                  "dev.pulp.test.archive-expansion",
                                                  "0.1.0");
    const auto archive = write_content_pack_archive(sandbox.root / "archive.pulpcontent", source);

    ContentCapabilityManifest manifest;
    manifest.plugin_id = "dev.pulp.test.plugin";
    manifest.capabilities = {"content.presets.v1"};
    manifest.content_kinds = {"presets"};

    auto preview = preview_content_pack_install(archive, manifest, data_root);
    REQUIRE(preview.ok);
    REQUIRE(preview.pack.id == "dev.pulp.test.archive-expansion");

    auto installed = install_content_pack(archive, manifest, data_root, true);
    REQUIRE(installed.ok);
    REQUIRE(fs::exists(data_root / "Content" / "dev.pulp.test.plugin" /
                       "dev.pulp.test.archive-expansion" / "0.1.0" /
                       "presets" / "DropInit.json"));
}

TEST_CASE("runtime content installer rejects pulpcontent archives without hash manifests",
          "[state][preset][content][security]") {
    pulp::test::PresetTestSandbox sandbox("pulp-content-runtime-archive-no-sha");
    const auto data_root = sandbox.root / "user-data";
    const auto source = write_source_content_pack(sandbox.root / "incoming",
                                                  "dev.pulp.test.archive-no-sha",
                                                  "0.1.0");
    const auto archive = write_content_pack_archive_without_hashes(
        sandbox.root / "archive-no-sha.pulpcontent", source);

    ContentCapabilityManifest manifest;
    manifest.plugin_id = "dev.pulp.test.plugin";
    manifest.capabilities = {"content.presets.v1"};
    manifest.content_kinds = {"presets"};

    auto preview = preview_content_pack_install(archive, manifest, data_root);
    REQUIRE_FALSE(preview.ok);
    REQUIRE(std::any_of(preview.issues.begin(), preview.issues.end(), [](const std::string& issue) {
        return issue.find("sha256: missing files.sha256.json") != std::string::npos;
    }));
    REQUIRE_FALSE(fs::exists(data_root / "Content"));
}

TEST_CASE("runtime content installer rejects pulpcontent archives with unlisted payload files",
          "[state][preset][content][security]") {
    pulp::test::PresetTestSandbox sandbox("pulp-content-runtime-archive-unlisted");
    const auto data_root = sandbox.root / "user-data";
    const auto source = write_source_content_pack(sandbox.root / "incoming",
                                                  "dev.pulp.test.archive-unlisted",
                                                  "0.1.0");
    const auto archive = write_content_pack_archive(
        sandbox.root / "archive-unlisted.pulpcontent", source, true);

    ContentCapabilityManifest manifest;
    manifest.plugin_id = "dev.pulp.test.plugin";
    manifest.capabilities = {"content.presets.v1"};
    manifest.content_kinds = {"presets"};

    auto preview = preview_content_pack_install(archive, manifest, data_root);
    REQUIRE_FALSE(preview.ok);
    REQUIRE(std::any_of(preview.issues.begin(), preview.issues.end(), [](const std::string& issue) {
        return issue.find("sha256: unlisted archived file `extras/unlisted.txt`") != std::string::npos;
    }));
    REQUIRE_FALSE(fs::exists(data_root / "Content"));
}

TEST_CASE("runtime content installer rejects archives missing declared exports",
          "[state][preset][content][security]") {
    pulp::test::PresetTestSandbox sandbox("pulp-content-runtime-archive-missing-export");
    const auto data_root = sandbox.root / "user-data";
    const auto source = write_source_content_pack(sandbox.root / "incoming",
                                                  "dev.pulp.test.archive-missing-export",
                                                  "0.1.0");
    fs::remove_all(source / "presets");
    const auto archive = write_content_pack_archive(
        sandbox.root / "archive-missing-export.pulpcontent", source);

    ContentCapabilityManifest manifest;
    manifest.plugin_id = "dev.pulp.test.plugin";
    manifest.capabilities = {"content.presets.v1"};
    manifest.content_kinds = {"presets"};

    auto preview = preview_content_pack_install(archive, manifest, data_root);
    REQUIRE_FALSE(preview.ok);
    REQUIRE(std::any_of(preview.issues.begin(), preview.issues.end(), [](const std::string& issue) {
        return issue.find("missing-path: `presets`") != std::string::npos;
    }));
    REQUIRE_FALSE(fs::exists(data_root / "Content"));
}
