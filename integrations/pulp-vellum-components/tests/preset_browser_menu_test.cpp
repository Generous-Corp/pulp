#include <catch2/catch_test_macros.hpp>

#include "preset_test_sandbox.hpp"
#include <pulp/state/preset_library.hpp>
#include <pulp/state/preset_manager.hpp>
#include <pulp/vellum_components/preset_browser.hpp>

#include <filesystem>
#include <fstream>

using namespace pulp::vellum_components;
using namespace pulp::state;
namespace fs = std::filesystem;

namespace {

MouseEvent click(float x, float y, int count = 1, MouseButton button = MouseButton::left) {
    MouseEvent e;
    e.position = {x, y};
    e.is_down = true;
    e.click_count = count;
    e.button = button;
    return e;
}

struct Rig {
    pulp::test::PresetTestSandbox sandbox{"pulp-editor"};
    StateStore store;
    std::unique_ptr<PresetManager> manager;
    std::unique_ptr<PresetLibrary> library;

    Rig() {
        store.add_parameter({.id = 1, .name = "Gain", .range = {-60, 12, 0}});
        manager = std::make_unique<PresetManager>(store, "TestCo", "EditorPlugin");
        const auto factory = sandbox.root / "factory";
        fs::create_directories(factory);
        for (const char* name : {"Alpha", "Bravo", "Charlie"}) {
            std::ofstream f(factory / (std::string(name) + ".json"));
            f << "{\"parameters\":{\"Gain\":-3}}";
        }
        manager->set_factory_presets_dir(factory);
        library =
            std::make_unique<PresetLibrary>(*manager, sandbox.root / "overlay" / "library.json");
        library->enable_program_map();
    }
};

} // namespace

TEST_CASE("bank:program text parses and formats", "[state][preset][program]") {
    REQUIRE(parse_bank_program("3:12") == BankProgram{3, 12});
    REQUIRE(parse_bank_program(" 3 : 12 ") == BankProgram{3, 12});
    REQUIRE(parse_bank_program("12") == BankProgram{0, 12});
    REQUIRE(parse_bank_program("16383:127") == BankProgram{16383, 127});
    REQUIRE_FALSE(parse_bank_program("16384:0").has_value());
    REQUIRE_FALSE(parse_bank_program("0:128").has_value());
    REQUIRE_FALSE(parse_bank_program("-1").has_value());
    REQUIRE_FALSE(parse_bank_program("a:b").has_value());
    REQUIRE_FALSE(parse_bank_program("1:2:3").has_value());
    REQUIRE_FALSE(parse_bank_program("").has_value());
    REQUIRE(format_bank_program({3, 12}) == "3:12");
    REQUIRE(*parse_bank_program(format_bank_program({77, 5})) == BankProgram{77, 5});
}

TEST_CASE("browser context menu lists the actions for the selected preset",
          "[view][preset_browser][library][menu]") {
    Rig rig;
    PresetBrowser browser(*rig.manager);
    browser.set_bounds({0, 0, 300, 400});
    REQUIRE(browser.context_menu_items().empty()); // no library
    browser.set_library(rig.library.get());
    REQUIRE(browser.context_menu_items().empty()); // nothing selected

    browser.on_mouse_event(click(60, 32 + 4 + 12));
    REQUIRE(browser.selected_summary()->key == "factory:Alpha");
    auto items = browser.context_menu_items();
    auto find = [&](int id) -> const ContextMenu::Item* {
        for (const auto& i : items)
            if (i.id == id && !i.separator && !i.header)
                return &i;
        return nullptr;
    };
    REQUIRE(items.front().header);
    REQUIRE(items.front().label == "Alpha");
    REQUIRE(find(static_cast<int>(PresetBrowser::MenuAction::toggle_favourite))->label ==
            "Add to favourites");
    REQUIRE(find(static_cast<int>(PresetBrowser::MenuAction::rename)) != nullptr);
    // Alpha already has slot 0:0 from the program map.
    REQUIRE(find(static_cast<int>(PresetBrowser::MenuAction::assign_program))->label ==
            "MIDI program 0:0...");
    REQUIRE(find(static_cast<int>(PresetBrowser::MenuAction::clear_program))->enabled);

    // With no program the clear item is disabled and the assign item offers to assign.
    REQUIRE(rig.library->clear_program({0, 0}));
    browser.refresh();
    items = browser.context_menu_items();
    REQUIRE_FALSE(find(static_cast<int>(PresetBrowser::MenuAction::clear_program))->enabled);
    REQUIRE(find(static_cast<int>(PresetBrowser::MenuAction::assign_program))->label ==
            "Assign MIDI program...");

    browser.add_tag_to_selected("pad");
    items = browser.context_menu_items();
    bool has_remove = false;
    for (const auto& i : items)
        if (i.label == "Remove tag #pad")
            has_remove = true;
    REQUIRE(has_remove);
}

TEST_CASE("right-click selects the row and offers the menu only in library mode",
          "[view][preset_browser][library][menu]") {
    Rig rig;
    PresetBrowser browser(*rig.manager);
    browser.set_bounds({0, 0, 300, 400});
    int offered = 0;
    std::size_t item_count = 0;
    browser.on_context_menu = [&](Point, std::vector<ContextMenu::Item> items) {
        ++offered;
        item_count = items.size();
    };
    browser.on_mouse_event(click(60, 32 + 4 + 12, 1, MouseButton::right));
    REQUIRE(offered == 0); // no library: a right-click is not special

    browser.set_library(rig.library.get());
    browser.on_mouse_event(click(60, 32 + 4 + 24 + 12, 1, MouseButton::right)); // second row
    REQUIRE(offered == 1);
    REQUIRE(item_count >= 6);
    REQUIRE(browser.selected_summary()->key == "factory:Bravo");

    browser.on_mouse_event(click(60, 5, 1, MouseButton::right));   // header: nothing
    browser.on_mouse_event(click(60, 390, 1, MouseButton::right)); // below the rows
    REQUIRE(offered == 1);
}

TEST_CASE("menu actions edit the library and ask the host for text",
          "[view][preset_browser][library][menu]") {
    Rig rig;
    PresetBrowser browser(*rig.manager);
    browser.set_bounds({0, 0, 300, 400});
    browser.set_library(rig.library.get());
    browser.on_mouse_event(click(60, 32 + 4 + 24 + 12)); // Bravo
    using A = PresetBrowser::MenuAction;

    REQUIRE(browser.perform_menu_action(static_cast<int>(A::toggle_favourite)));
    REQUIRE(rig.library->find("factory:Bravo")->favourite);

    // Without a text handler the text actions do nothing.
    REQUIRE_FALSE(browser.perform_menu_action(static_cast<int>(A::rename)));

    std::vector<PresetBrowser::TextRequest> asked;
    browser.on_text_request = [&](const PresetBrowser::TextRequest& r) { asked.push_back(r); };
    REQUIRE(browser.perform_menu_action(static_cast<int>(A::rename)));
    REQUIRE(browser.perform_menu_action(static_cast<int>(A::add_tag)));
    REQUIRE(browser.perform_menu_action(static_cast<int>(A::assign_program)));
    REQUIRE(asked.size() == 3);
    REQUIRE(asked[0].kind == PresetBrowser::TextRequestKind::rename);
    REQUIRE(asked[0].initial == "Bravo");
    REQUIRE(asked[2].initial == "0:1"); // Bravo's current program

    REQUIRE(browser.complete_text_request(PresetBrowser::TextRequestKind::rename, "Bravissimo"));
    REQUIRE(rig.library->find("factory:Bravo")->display_name == "Bravissimo");
    REQUIRE(browser.complete_text_request(PresetBrowser::TextRequestKind::add_tag, "lead"));
    REQUIRE_FALSE(browser.complete_text_request(PresetBrowser::TextRequestKind::add_tag, ""));
    REQUIRE(rig.library->find("factory:Bravo")->tags == std::vector<std::string>{"lead"});

    REQUIRE(browser.complete_text_request(PresetBrowser::TextRequestKind::assign_program, "5:9"));
    REQUIRE(rig.library->program_of("factory:Bravo") == BankProgram{5, 9});
    REQUIRE_FALSE(
        browser.complete_text_request(PresetBrowser::TextRequestKind::assign_program, "nonsense"));
    REQUIRE_FALSE(
        browser.complete_text_request(PresetBrowser::TextRequestKind::assign_program, "99999:1"));
    REQUIRE(rig.library->program_of("factory:Bravo") == BankProgram{5, 9});

    // The row stays selected after the edits and the menu reflects the new number.
    REQUIRE(browser.selected_summary()->key == "factory:Bravo");
    REQUIRE(browser.perform_menu_action(static_cast<int>(A::clear_program)));
    REQUIRE_FALSE(rig.library->program_of("factory:Bravo").has_value());
    REQUIRE_FALSE(browser.perform_menu_action(static_cast<int>(A::clear_program))); // already clear

    REQUIRE(browser.perform_menu_action(static_cast<int>(A::remove_tag_base) + 0));
    REQUIRE(rig.library->find("factory:Bravo")->tags.empty());
    REQUIRE_FALSE(browser.perform_menu_action(static_cast<int>(A::remove_tag_base) + 7));
    REQUIRE_FALSE(browser.perform_menu_action(9999));
}
