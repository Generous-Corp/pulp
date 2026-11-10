#include <catch2/catch_test_macros.hpp>

#include "preset_test_sandbox.hpp"
#include <pulp/state/preset_library.hpp>
#include <pulp/state/preset_manager.hpp>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace pulp::state;
namespace fs = std::filesystem;

namespace {

void write_file(const fs::path& path, const std::string& text) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream f(path);
    REQUIRE(f.is_open());
    f << text;
}

std::string preset_json(const std::string& params, const std::string& extra = "") {
    return "{" + extra + "\"parameters\":{" + params + "}}";
}

// A manager with a factory folder, a user folder and a stand-in content-pack
// source that scans <root>/Content/<plugin>/<pack>/<version>/presets.
struct Rig {
    explicit Rig(const char* tag)
        : sandbox(tag), overlay(sandbox.root / "overlay" / "library.json"),
          factory(sandbox.root / "factory"), content_root(sandbox.root / "data") {
        store.add_parameter({.id = 1, .name = "Gain", .range = {-60, 12, 0}});
        store.add_parameter({.id = 2, .name = "Mix", .range = {0, 100, 50}});
        manager = std::make_unique<PresetManager>(store, "TestCo", "LibPlugin");
        manager->set_factory_presets_dir(factory);
        manager->set_content_plugin_id("plug");
        const auto root = content_root;
        manager->set_content_preset_source(
            [root](const ContentCapabilityManifest&, const fs::path&) {
                std::vector<PresetInfo> out;
                std::error_code ec;
                if (!fs::exists(root / "Content", ec))
                    return out;
                for (const auto& e : fs::recursive_directory_iterator(root / "Content")) {
                    if (!e.is_regular_file() || e.path().extension() != ".json")
                        continue;
                    PresetInfo info;
                    info.name = e.path().stem().string();
                    info.path = e.path();
                    info.is_factory = true;
                    out.push_back(info);
                }
                return out;
            });
    }

    fs::path content_file(const std::string& pack, const std::string& version,
                          const std::string& name) const {
        return content_root / "Content" / "plug" / pack / version / "presets" / (name + ".json");
    }

    std::unique_ptr<PresetLibrary> open() {
        return std::make_unique<PresetLibrary>(*manager, overlay);
    }

    pulp::test::PresetTestSandbox sandbox;
    StateStore store;
    std::unique_ptr<PresetManager> manager;
    fs::path overlay, factory, content_root;
};

std::vector<std::string> names(const PresetPage& page) {
    std::vector<std::string> out;
    for (const auto& s : page.items)
        out.push_back(s.display_name);
    return out;
}

} // namespace

TEST_CASE("preset keys follow the documented grammar", "[state][preset][library]") {
    REQUIRE(make_preset_key(PresetSource::factory, "Pads/Warm") == "factory:Pads/Warm");
    REQUIRE(make_preset_key(PresetSource::user, "Mine") == "user:Mine");
    REQUIRE(make_preset_key(PresetSource::content, "Init", "expansion") ==
            "content:expansion:Init");

    Rig rig("pulp-lib-keys");
    write_file(rig.factory / "Pads" / "Warm.json", preset_json("\"Gain\":-3"));
    write_file(rig.content_file("expansion", "1.0.0", "Init"), preset_json("\"Gain\":-1"));
    REQUIRE(rig.manager->save("Mine"));
    auto lib = rig.open();

    REQUIRE(lib->find("factory:Pads/Warm").has_value());
    REQUIRE(lib->find("user:Mine").has_value());
    // The pack version is not part of the key.
    REQUIRE(lib->find("content:expansion:Init").has_value());
    REQUIRE(lib->key_for(rig.factory / "Pads" / "Warm.json") == "factory:Pads/Warm");
    REQUIRE(lib->find("factory:Nope") == std::nullopt);
}

TEST_CASE("favourites, tags and display names persist across library instances",
          "[state][preset][library]") {
    Rig rig("pulp-lib-persist");
    write_file(rig.factory / "Warm.json",
               preset_json("\"Gain\":-3", "\"tags\":[\"pad\",\"wip\"],"));
    write_file(rig.factory / "Bright.json", preset_json("\"Gain\":-6"));
    {
        auto lib = rig.open();
        REQUIRE(lib->capabilities().persistence);
        REQUIRE(lib->set_favourite("factory:Warm", true));
        REQUIRE(lib->add_tag("factory:Warm", "Mine"));
        REQUIRE(lib->remove_tag("factory:Warm", "wip"));
        REQUIRE(lib->set_display_name("factory:Warm", "Warm Dusk"));
    }
    auto lib = rig.open();
    auto warm = lib->find("factory:Warm");
    REQUIRE(warm.has_value());
    REQUIRE(warm->favourite);
    REQUIRE(warm->display_name == "Warm Dusk");
    REQUIRE(warm->name == "Warm");
    REQUIRE(warm->tags == std::vector<std::string>{"pad", "Mine"});
    REQUIRE_FALSE(lib->find("factory:Bright")->favourite);

    // Clearing the name restores the file name; tags compare case-insensitively.
    REQUIRE(lib->set_display_name("factory:Warm", ""));
    REQUIRE(lib->find("factory:Warm")->display_name == "Warm");
    REQUIRE(lib->add_tag("factory:Warm", "MINE"));
    REQUIRE(lib->find("factory:Warm")->tags == std::vector<std::string>{"pad", "Mine"});

    REQUIRE_FALSE(lib->set_favourite("factory:Missing", true));
}

TEST_CASE("edits survive a changed preset, a pack update, and a pack coming back",
          "[state][preset][library]") {
    Rig rig("pulp-lib-updates");
    write_file(rig.factory / "Warm.json", preset_json("\"Gain\":-3"));
    write_file(rig.content_file("expansion", "1.0.0", "Init"), preset_json("\"Gain\":-1"));
    auto lib = rig.open();
    REQUIRE(lib->set_favourite("factory:Warm", true));
    REQUIRE(lib->set_favourite("content:expansion:Init", true));

    // The factory preset's values change in an update; the key is unchanged.
    write_file(rig.factory / "Warm.json", preset_json("\"Gain\":-9"));
    // The pack is reinstalled as a newer version.
    fs::rename(rig.content_root / "Content" / "plug" / "expansion" / "1.0.0",
               rig.content_root / "Content" / "plug" / "expansion" / "1.1.0");
    lib->refresh();
    REQUIRE(lib->find("factory:Warm")->favourite);
    REQUIRE(lib->find("content:expansion:Init")->favourite);
    REQUIRE(lib->orphan_count() == 0);

    // The pack disappears: its edit is kept as an orphan and returns with it.
    fs::rename(rig.content_root / "Content" / "plug" / "expansion" / "1.1.0",
               rig.content_root / "parked");
    lib->refresh();
    REQUIRE_FALSE(lib->find("content:expansion:Init").has_value());
    REQUIRE(lib->orphan_count() == 1);
    fs::rename(rig.content_root / "parked",
               rig.content_root / "Content" / "plug" / "expansion" / "1.1.0");
    lib->refresh();
    REQUIRE(lib->orphan_count() == 0);
    REQUIRE(lib->find("content:expansion:Init")->favourite);

    lib->clear_orphans();
    REQUIRE(lib->find("content:expansion:Init")->favourite); // nothing to clear
}

TEST_CASE("a renamed preset keeps its edits when exactly one file matches by digest",
          "[state][preset][library]") {
    Rig rig("pulp-lib-relink");
    rig.store.set_value(1, -7.0f);
    REQUIRE(rig.manager->save("Old"));
    auto lib = rig.open();
    REQUIRE(lib->set_favourite("user:Old", true));
    REQUIRE(lib->add_tag("user:Old", "keeper"));

    auto info = rig.manager->user_presets().front();
    REQUIRE(rig.manager->rename(info, "New"));
    lib->refresh();
    auto moved = lib->find("user:New");
    REQUIRE(moved.has_value());
    REQUIRE(moved->favourite);
    REQUIRE(moved->tags == std::vector<std::string>{"keeper"});
    REQUIRE(lib->orphan_count() == 0);
}

TEST_CASE("an ambiguous digest does not re-link an edit", "[state][preset][library]") {
    Rig rig("pulp-lib-ambiguous");
    rig.store.set_value(1, -7.0f);
    REQUIRE(rig.manager->save("Old"));
    auto lib = rig.open();
    REQUIRE(lib->set_favourite("user:Old", true));

    // Two files now hold the same values: the edit cannot tell which is "Old".
    const auto old_path = rig.manager->user_presets().front().path;
    fs::copy_file(old_path, old_path.parent_path() / "CopyA.json");
    fs::copy_file(old_path, old_path.parent_path() / "CopyB.json");
    fs::remove(old_path);
    lib->refresh();
    REQUIRE(lib->orphan_count() == 1);
    REQUIRE_FALSE(lib->find("user:CopyA")->favourite);
    REQUIRE_FALSE(lib->find("user:CopyB")->favourite);
}

TEST_CASE("the overlay never lives where the preset scanner would list it",
          "[state][preset][library]") {
    Rig rig("pulp-lib-location");
    const auto overlay = PresetLibrary::default_overlay_path("TestCo", "LibPlugin");
    REQUIRE_FALSE(overlay.empty());
    REQUIRE(overlay.filename() == "library.json");
    const auto user_dir = rig.manager->user_presets_dir().lexically_normal();
    const auto rel = overlay.lexically_normal().lexically_relative(user_dir);
    REQUIRE((rel.empty() || *rel.begin() == ".."));

    // Negative control: a file of the same shape under the preset folder IS
    // listed as a preset, which is exactly what the rule prevents.
    const auto before = rig.manager->user_presets().size();
    write_file(user_dir / "library" / "library.json", "{\"records\":[]}");
    REQUIRE(rig.manager->user_presets().size() == before + 1);

    // The overlay a library writes adds no preset.
    write_file(rig.factory / "Warm.json", preset_json("\"Gain\":-3"));
    auto lib = rig.open();
    const auto listed = rig.manager->all_presets().size();
    REQUIRE(lib->set_favourite("factory:Warm", true));
    REQUIRE(fs::exists(rig.overlay));
    REQUIRE(rig.manager->all_presets().size() == listed);
}

TEST_CASE("search matches, ranks and filters deterministically", "[state][preset][library]") {
    Rig rig("pulp-lib-search");
    write_file(rig.factory / "Warm Pad.json", preset_json("\"Gain\":-1", "\"tags\":[\"pad\"],"));
    write_file(rig.factory / "Cold Warm Lead.json",
               preset_json("\"Gain\":-2", "\"tags\":[\"lead\"],\"author\":\"Ada\","));
    write_file(rig.factory / "Bass" / "Subwarm.json", preset_json("\"Gain\":-3"));
    write_file(rig.factory / "Bright.json",
               preset_json("\"Gain\":-4", "\"description\":\"shimmer top end\","));
    REQUIRE(rig.manager->save("My Warm"));
    auto lib = rig.open();

    PresetQuery q;
    q.text = "warm";
    // Prefix first, then word start, then substring; the tie falls back to name.
    REQUIRE(names(lib->query(q)) ==
            std::vector<std::string>{"Warm Pad", "Cold Warm Lead", "My Warm", "Subwarm"});

    q.text = "WARM lead"; // case-insensitive, every word must match
    REQUIRE(names(lib->query(q)) == std::vector<std::string>{"Cold Warm Lead"});

    q.text = "ada"; // author field
    REQUIRE(names(lib->query(q)) == std::vector<std::string>{"Cold Warm Lead"});
    q.text = "shimmer"; // description field
    REQUIRE(names(lib->query(q)) == std::vector<std::string>{"Bright"});
    q.text = "bass"; // folder field
    REQUIRE(names(lib->query(q)) == std::vector<std::string>{"Subwarm"});
    q.text = "nothingmatches";
    REQUIRE(lib->query(q).items.empty());

    q = {};
    q.tags_all = {"PAD"};
    REQUIRE(names(lib->query(q)) == std::vector<std::string>{"Warm Pad"});

    REQUIRE(lib->set_favourite("factory:Bright", true));
    q = {};
    q.favourite = true;
    REQUIRE(names(lib->query(q)) == std::vector<std::string>{"Bright"});
    q.favourite = false;
    REQUIRE(lib->query(q).total == 4);

    q = {};
    q.source = PresetSource::user;
    REQUIRE(names(lib->query(q)) == std::vector<std::string>{"My Warm"});

    q = {};
    q.sort = PresetSort::favourites_first;
    REQUIRE(names(lib->query(q)).front() == "Bright");
    q.sort = PresetSort::source;
    REQUIRE(names(lib->query(q)).back() == "My Warm");

    q = {};
    q.offset = 1;
    q.limit = 2;
    auto page = lib->query(q);
    REQUIRE(page.total == 5);
    REQUIRE(page.items.size() == 2);
    REQUIRE(names(page) == std::vector<std::string>{"Cold Warm Lead", "My Warm"});
}

TEST_CASE("query reads the index, not the disk, until refresh", "[state][preset][library]") {
    Rig rig("pulp-lib-index");
    write_file(rig.factory / "Warm.json", preset_json("\"Gain\":-3"));
    auto lib = rig.open();
    const auto gen = lib->generation();

    fs::remove(rig.factory / "Warm.json");
    write_file(rig.factory / "Late.json", preset_json("\"Gain\":-1"));
    REQUIRE(names(lib->query({})) == std::vector<std::string>{"Warm"});
    REQUIRE(lib->generation() == gen);

    int notified = 0;
    lib->on_changed = [&] { ++notified; };
    lib->refresh();
    REQUIRE(names(lib->query({})) == std::vector<std::string>{"Late"});
    REQUIRE(lib->generation() > gen);
    REQUIRE(notified == 1);
}

TEST_CASE("two libraries editing one overlay file do not lose each other's edits",
          "[state][preset][library]") {
    Rig rig("pulp-lib-merge");
    write_file(rig.factory / "A.json", preset_json("\"Gain\":-1"));
    write_file(rig.factory / "B.json", preset_json("\"Gain\":-2"));
    auto first = rig.open();
    auto second = rig.open(); // opened before the first one's edit reaches the file

    REQUIRE(first->set_favourite("factory:A", true));
    REQUIRE(second->set_favourite("factory:B", true));

    second->refresh();
    first->refresh();
    REQUIRE(first->find("factory:A")->favourite);
    REQUIRE(first->find("factory:B")->favourite);
    REQUIRE(second->find("factory:A")->favourite);
    REQUIRE(rig.open()->find("factory:B")->favourite);
}

TEST_CASE("the newest edit wins and a removal is not resurrected by a merge",
          "[state][preset][library]") {
    Rig rig("pulp-lib-newest");
    write_file(rig.factory / "A.json", preset_json("\"Gain\":-1"));
    auto first = rig.open();
    auto second = rig.open();

    REQUIRE(first->set_favourite("factory:A", true));
    REQUIRE(second->set_favourite("factory:A", false)); // later, so it wins
    first->refresh();
    REQUIRE_FALSE(first->find("factory:A")->favourite);

    REQUIRE(first->set_favourite("factory:A", true));
    REQUIRE(first->set_favourite("factory:A", false)); // record becomes empty
    second->refresh();
    REQUIRE_FALSE(second->find("factory:A")->favourite);
    REQUIRE(rig.open()->orphan_count() == 0);
}

TEST_CASE("concurrent writers to one overlay file keep every edit", "[state][preset][library]") {
    pulp::test::PresetTestSandbox sandbox("pulp-lib-hammer");
    const auto file = sandbox.root / "overlay" / "library.json";
    constexpr int kThreads = 4;
    constexpr int kPerThread = 20;
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            PresetMetadataStore store(file);
            store.load();
            for (int i = 0; i < kPerThread; ++i)
                if (!store.set_favourite("user:t" + std::to_string(t) + "_" + std::to_string(i),
                                         true, "sha256:x"))
                    ++failures;
        });
    }
    for (auto& th : threads)
        th.join();
    REQUIRE(failures == 0);

    PresetMetadataStore reader(file);
    REQUIRE(reader.load());
    REQUIRE(reader.records().size() == static_cast<std::size_t>(kThreads * kPerThread));
}

TEST_CASE("without storage, or with unwritable storage, edits last for the session",
          "[state][preset][library]") {
    Rig rig("pulp-lib-nostorage");
    write_file(rig.factory / "A.json", preset_json("\"Gain\":-1"));

    PresetLibrary memory(*rig.manager, {});
    REQUIRE_FALSE(memory.capabilities().persistence);
    REQUIRE(memory.set_favourite("factory:A", true));
    REQUIRE(memory.find("factory:A")->favourite);

    // The overlay's parent is a regular file, so it can never be created.
    write_file(rig.sandbox.root / "blocker", "x");
    PresetLibrary blocked(*rig.manager, rig.sandbox.root / "blocker" / "library.json");
    REQUIRE(blocked.capabilities().persistence);
    REQUIRE_FALSE(blocked.set_favourite("factory:A", true));
    REQUIRE(blocked.find("factory:A")->favourite);
    REQUIRE_FALSE(blocked.capabilities().persistence);
}

TEST_CASE("preset digests depend on parameter values only", "[state][preset][library]") {
    pulp::test::PresetTestSandbox sandbox("pulp-lib-digest");
    const auto a = sandbox.root / "a.json";
    const auto b = sandbox.root / "b.json";
    const auto c = sandbox.root / "c.json";
    write_file(a, "{\"name\":\"One\",\"parameters\":{\"Gain\":-3,\"Mix\":10}}");
    write_file(b,
               "{\"tags\":[\"x\"],\"author\":\"Z\",\"parameters\":{\"Mix\":10.0,\"Gain\":-3.0}}");
    write_file(c, "{\"parameters\":{\"Gain\":-3,\"Mix\":11}}");

    const auto da = preset_digest(a);
    REQUIRE(da.rfind("sha256:", 0) == 0);
    REQUIRE(da == preset_digest(b));
    REQUIRE(da != preset_digest(c));

    write_file(sandbox.root / "bad.json", "not json at all");
    REQUIRE(preset_digest(sandbox.root / "bad.json").rfind("raw:", 0) == 0);
    REQUIRE(preset_digest(sandbox.root / "missing.json").empty());
}
