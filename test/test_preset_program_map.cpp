#include <catch2/catch_test_macros.hpp>

#include "preset_test_sandbox.hpp"
#include <pulp/state/preset_library.hpp>
#include <pulp/state/preset_manager.hpp>
#include <pulp/state/preset_program_map.hpp>

#include <filesystem>
#include <fstream>
#include <string>
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

const char* kPreset = "{\"parameters\":{\"Gain\":-3}}";

struct Rig {
    explicit Rig(const char* tag)
        : sandbox(tag), overlay(sandbox.root / "overlay" / "library.json"),
          factory(sandbox.root / "factory"), content_root(sandbox.root / "data") {
        store.add_parameter({.id = 1, .name = "Gain", .range = {-60, 12, 0}});
        manager = std::make_unique<PresetManager>(store, "TestCo", "MapPlugin");
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
    fs::path content_file(const std::string& pack, const std::string& name) const {
        return content_root / "Content" / "plug" / pack / "1.0.0" / "presets" / (name + ".json");
    }
    std::unique_ptr<PresetLibrary> open() {
        return std::make_unique<PresetLibrary>(*manager, overlay);
    }

    pulp::test::PresetTestSandbox sandbox;
    StateStore store;
    std::unique_ptr<PresetManager> manager;
    fs::path overlay, factory, content_root;
};

} // namespace

TEST_CASE("bank numbers are 14-bit and positions order by bank then program",
          "[state][preset][program]") {
    REQUIRE(make_bank(0, 0) == 0);
    REQUIRE(make_bank(1, 2) == 130);
    REQUIRE(make_bank(127, 127) == kMaxProgramBank);
    REQUIRE(make_bank(0xFF, 0xFF) == kMaxProgramBank); // high bits masked
    REQUIRE(BankProgram{1, 5} < BankProgram{2, 0});
    REQUIRE(BankProgram{1, 5} < BankProgram{1, 6});
}

TEST_CASE("assign moves a preset between slots and clear empties one", "[state][preset][program]") {
    ProgramMap map;
    REQUIRE(map.assign({0, 3}, "factory:A"));
    REQUIRE(map.key_at({0, 3}) == "factory:A");
    REQUIRE(map.position_of("factory:A") == BankProgram{0, 3});

    REQUIRE(map.assign({2, 9}, "factory:A")); // moves, does not duplicate
    REQUIRE_FALSE(map.key_at({0, 3}).has_value());
    REQUIRE(map.position_of("factory:A") == BankProgram{2, 9});

    REQUIRE(map.assign({2, 9}, "factory:B")); // replaces the occupant
    REQUIRE_FALSE(map.position_of("factory:A").has_value());

    REQUIRE(map.clear({2, 9}));
    REQUIRE_FALSE(map.clear({2, 9}));
    REQUIRE_FALSE(map.assign({kMaxProgramBank + 1, 0}, "x"));
    REQUIRE_FALSE(map.assign({0, 128}, "x"));
    REQUIRE_FALSE(map.assign({0, 0}, ""));
}

TEST_CASE("banks can be named and renumbered but not onto an existing number",
          "[state][preset][program]") {
    ProgramMap map;
    map.assign({4, 0}, "k1");
    map.assign({9, 0}, "k2");
    REQUIRE(map.set_bank_name(4, "Leads"));
    REQUIRE_FALSE(map.set_bank_name(5, "Nope"));
    REQUIRE(map.renumber_bank(4, 20));
    REQUIRE(map.position_of("k1") == BankProgram{20, 0});
    REQUIRE(map.find_bank(20)->name == "Leads");
    REQUIRE_FALSE(map.renumber_bank(20, 9)); // taken
    REQUIRE_FALSE(map.renumber_bank(20, kMaxProgramBank + 1));
    REQUIRE_FALSE(map.renumber_bank(3, 7));   // no such bank
    REQUIRE(map.banks().front().number == 9); // kept sorted
}

TEST_CASE("pinning gives new keys free slots and never moves existing ones",
          "[state][preset][program]") {
    ProgramMap map;
    ProgramPinGroup factory{"factory:", "Factory", {"factory:A", "factory:B", "factory:C"}};
    REQUIRE(map.pin_defaults({factory}));
    REQUIRE(map.position_of("factory:A") == BankProgram{0, 0});
    REQUIRE(map.position_of("factory:C") == BankProgram{0, 2});
    REQUIRE_FALSE(map.pin_defaults({factory})); // idempotent

    // An update adds a preset that sorts first and removes nothing: the
    // existing numbers do not shift; the newcomer takes the next free slot.
    ProgramPinGroup updated{
        "factory:", "Factory", {"factory:0New", "factory:A", "factory:B", "factory:C"}};
    REQUIRE(map.pin_defaults({updated}));
    REQUIRE(map.position_of("factory:A") == BankProgram{0, 0});
    REQUIRE(map.position_of("factory:0New") == BankProgram{0, 3});

    // A preset that disappears leaves its slot empty; the next newcomer fills
    // the gap rather than renumbering the others.
    ProgramPinGroup shrunk{"factory:", "Factory", {"factory:A", "factory:C", "factory:Z"}};
    map.clear({0, 1});
    REQUIRE(map.pin_defaults({shrunk}));
    REQUIRE(map.position_of("factory:Z") == BankProgram{0, 1});
    REQUIRE(map.position_of("factory:C") == BankProgram{0, 2});
}

TEST_CASE("sources get their own banks: factory 0, user 1, packs next free",
          "[state][preset][program]") {
    ProgramMap map;
    // The user source is seen first, but bank numbers belong to the source.
    REQUIRE(map.pin_defaults({{"user:", "User", {"user:U"}},
                              {"factory:", "Factory", {"factory:F"}},
                              {"content:expansion:", "expansion", {"content:expansion:X"}}}));
    REQUIRE(map.position_of("factory:F") == BankProgram{0, 0});
    REQUIRE(map.position_of("user:U") == BankProgram{1, 0});
    REQUIRE(map.position_of("content:expansion:X") == BankProgram{2, 0});
    REQUIRE(map.find_bank(2)->name == "expansion");
}

TEST_CASE("a source with more than 128 presets spills into a further bank",
          "[state][preset][program]") {
    ProgramMap map;
    ProgramPinGroup group{"factory:", "Factory", {}};
    for (int i = 0; i < 130; ++i)
        group.keys.push_back("factory:p" + std::to_string(1000 + i));
    REQUIRE(map.pin_defaults({group}));
    REQUIRE(map.position_of("factory:p1000") == BankProgram{0, 0});
    REQUIRE(map.position_of("factory:p1127") == BankProgram{0, 127});
    REQUIRE(map.position_of("factory:p1128") == BankProgram{2, 0}); // bank 1 is the user bank's
    REQUIRE(map.find_bank(2)->scope == "factory:");

    // A later newcomer from the same source fills the first free slot of its
    // banks. The preset whose slot was cleared stays unassigned: a key is
    // pinned once.
    map.clear({0, 5});
    group.keys.push_back("factory:late");
    REQUIRE(map.pin_defaults({group}));
    REQUIRE(map.position_of("factory:late") == BankProgram{0, 5});
    REQUIRE_FALSE(map.position_of("factory:p1005").has_value());
}

TEST_CASE("a cleared slot is not refilled by a later pin, and a rekey carries the slot",
          "[state][preset][program]") {
    ProgramMap map;
    ProgramPinGroup group{"factory:", "Factory", {"factory:A", "factory:B"}};
    map.pin_defaults({group});
    REQUIRE(map.clear({0, 0}));
    REQUIRE_FALSE(map.pin_defaults({group})); // A was seen: nothing to do
    REQUIRE_FALSE(map.position_of("factory:A").has_value());
    // Assigning it by hand works as ever.
    REQUIRE(map.assign({4, 4}, "factory:A"));

    REQUIRE(map.rekey("factory:B", "factory:B2"));
    REQUIRE(map.position_of("factory:B2") == BankProgram{0, 1});
    REQUIRE_FALSE(map.position_of("factory:B").has_value());
    // The new name is a seen key too: pinning does not hand it a second slot.
    ProgramPinGroup renamed{"factory:", "Factory", {"factory:A", "factory:B2"}};
    REQUIRE_FALSE(map.pin_defaults({renamed}));
    REQUIRE_FALSE(map.rekey("factory:nope", "factory:x"));
    REQUIRE_FALSE(map.rekey("factory:B2", "factory:A")); // target already has a slot
}

TEST_CASE("the map round-trips through JSON and refuses damaged documents",
          "[state][preset][program]") {
    ProgramMap map;
    map.pin_defaults({{"factory:", "Fac\"tory", {"factory:A", "factory:B"}}});
    map.assign({300, 127}, "user:Mine");
    map.set_bank_name(300, "Mine");

    auto again = ProgramMap::from_json(map.to_json());
    REQUIRE(again.has_value());
    REQUIRE(*again == map);
    REQUIRE(again->find_bank(0)->name == "Fac\"tory");

    REQUIRE_FALSE(ProgramMap::from_json("not json").has_value());
    REQUIRE_FALSE(ProgramMap::from_json("{}").has_value());
    REQUIRE_FALSE(ProgramMap::from_json(R"({"banks":[{"number":99999,"slots":{}}]})").has_value());
    REQUIRE_FALSE(
        ProgramMap::from_json(R"({"banks":[{"number":0,"slots":{"200":"k"}}]})").has_value());
    REQUIRE_FALSE(
        ProgramMap::from_json(R"({"banks":[{"number":0,"slots":{"x":"k"}}]})").has_value());
    // One key in two slots would make the position ambiguous.
    REQUIRE_FALSE(
        ProgramMap::from_json(R"({"banks":[{"number":0,"slots":{"0":"k","1":"k"}}]})").has_value());
    // Two banks with the same number.
    REQUIRE_FALSE(
        ProgramMap::from_json(R"({"banks":[{"number":0,"slots":{}},{"number":0,"slots":{}}]})")
            .has_value());
}

TEST_CASE("the library writes no program map until it is enabled",
          "[state][preset][program][library]") {
    Rig rig("pulp-map-optin");
    write_file(rig.factory / "A.json", kPreset);
    auto lib = rig.open();
    REQUIRE_FALSE(lib->program_map_enabled());
    REQUIRE_FALSE(lib->capabilities().program_map);
    lib->refresh();
    REQUIRE(lib->program_map().empty());
    REQUIRE_FALSE(fs::exists(rig.overlay)); // nothing written

    REQUIRE(lib->set_favourite("factory:A", true)); // an unrelated edit...
    REQUIRE(lib->program_map().empty());            // ...still no map
    auto s = lib->find("factory:A");
    REQUIRE_FALSE(s->program.has_value());
}

TEST_CASE("enabling the program map pins every source and saves it",
          "[state][preset][program][library]") {
    Rig rig("pulp-map-pin");
    write_file(rig.factory / "Bass.json", kPreset);
    write_file(rig.factory / "Pad.json", kPreset);
    write_file(rig.content_file("expansion", "Init"), kPreset);
    REQUIRE(rig.manager->save("Mine"));
    {
        auto lib = rig.open();
        lib->enable_program_map();
        REQUIRE(lib->capabilities().program_map);
        REQUIRE(lib->program_of("factory:Bass") == BankProgram{0, 0});
        REQUIRE(lib->program_of("factory:Pad") == BankProgram{0, 1});
        REQUIRE(lib->program_of("user:Mine") == BankProgram{1, 0});
        REQUIRE(lib->program_of("content:expansion:Init") == BankProgram{2, 0});
        REQUIRE(lib->find("factory:Pad")->program == BankProgram{0, 1});
        REQUIRE(lib->preset_at({0, 1})->key == "factory:Pad");
        REQUIRE_FALSE(lib->preset_at({0, 9}).has_value()); // empty slot
    }
    // A new instance reads the saved map without enabling it, and nothing moves
    // when it is enabled again.
    auto lib = rig.open();
    REQUIRE(lib->program_of("factory:Pad") == BankProgram{0, 1});
    lib->enable_program_map();
    REQUIRE(lib->program_of("factory:Pad") == BankProgram{0, 1});
}

TEST_CASE("numbers survive an update that adds a preset and one that removes another",
          "[state][preset][program][library]") {
    Rig rig("pulp-map-stable");
    write_file(rig.factory / "B.json", kPreset);
    write_file(rig.factory / "C.json", kPreset);
    auto lib = rig.open();
    lib->enable_program_map();
    REQUIRE(lib->program_of("factory:B") == BankProgram{0, 0});
    REQUIRE(lib->program_of("factory:C") == BankProgram{0, 1});

    write_file(rig.factory / "A.json", kPreset); // sorts before B
    fs::remove(rig.factory / "B.json");
    lib->refresh();
    REQUIRE(lib->program_of("factory:C") == BankProgram{0, 1}); // unmoved
    REQUIRE(lib->program_of("factory:A") == BankProgram{0, 2}); // next free slot
    // B's slot still names B, but B is not installed, so it resolves to nothing.
    REQUIRE(lib->program_map().key_at({0, 0}) == "factory:B");
    REQUIRE_FALSE(lib->preset_at({0, 0}).has_value());

    // B comes back and answers to the same number again.
    write_file(rig.factory / "B.json", kPreset);
    lib->refresh();
    REQUIRE(lib->preset_at({0, 0})->key == "factory:B");
}

TEST_CASE("user edits to the map persist and are visible to another instance",
          "[state][preset][program][library]") {
    Rig rig("pulp-map-edit");
    write_file(rig.factory / "A.json", kPreset);
    write_file(rig.factory / "B.json", kPreset);
    auto first = rig.open();
    auto second = rig.open();
    first->enable_program_map();

    REQUIRE(first->assign_program({5, 20}, "factory:B"));
    REQUIRE(first->set_bank_name(5, "Leads"));
    REQUIRE_FALSE(first->assign_program({5, 21}, "factory:Missing")); // not installed
    REQUIRE_FALSE(first->assign_program({kMaxProgramBank + 1, 0}, "factory:A"));
    REQUIRE(first->clear_program({0, 0}));
    REQUIRE_FALSE(first->program_of("factory:A").has_value());

    second->refresh();
    REQUIRE(second->program_of("factory:B") == BankProgram{5, 20});
    REQUIRE(second->program_map().find_bank(5)->name == "Leads");
    REQUIRE_FALSE(second->program_of("factory:A").has_value());

    REQUIRE(second->renumber_bank(5, 6));
    first->refresh();
    REQUIRE(first->program_of("factory:B") == BankProgram{6, 20});
}

TEST_CASE("search can filter and sort by program", "[state][preset][program][library]") {
    Rig rig("pulp-map-query");
    write_file(rig.factory / "A.json", kPreset);
    write_file(rig.factory / "B.json", kPreset);
    write_file(rig.factory / "C.json", kPreset);
    auto lib = rig.open();
    lib->enable_program_map();
    REQUIRE(lib->assign_program({3, 7}, "factory:A"));
    REQUIRE(lib->assign_program({3, 2}, "factory:C"));

    PresetQuery q;
    q.bank = 3;
    q.sort = PresetSort::program;
    auto page = lib->query(q);
    REQUIRE(page.total == 2);
    REQUIRE(page.items[0].key == "factory:C");
    REQUIRE(page.items[1].key == "factory:A");

    q = {};
    q.sort = PresetSort::program;
    auto all = lib->query(q);
    REQUIRE(all.items.size() == 3);
    REQUIRE(all.items[0].key == "factory:B"); // bank 0 comes before bank 3
    REQUIRE(all.items[1].key == "factory:C");
    REQUIRE(all.items[2].key == "factory:A");
}
