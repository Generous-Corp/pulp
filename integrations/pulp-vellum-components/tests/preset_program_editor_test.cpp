#include <catch2/catch_test_macros.hpp>

#include "preset_test_sandbox.hpp"
#include "render_helpers.hpp"
#include <pulp/state/preset_library.hpp>
#include <pulp/state/preset_manager.hpp>
#include <pulp/vellum_components/preset_program_editor.hpp>
#include <vellum/canvas/recording_canvas.hpp>

#include <filesystem>
#include <fstream>

using namespace pulp::vellum_components;
using namespace pulp::state;
using namespace vellum::canvas;
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

bool has_text(const RecordingCanvas& c, const std::string& text) {
    for (const auto& cmd : c.commands())
        if (cmd.type == DrawCommand::Type::fill_text && cmd.text == text)
            return true;
    return false;
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

TEST_CASE("program editor shows a bank's slots and navigates banks",
          "[view][preset_browser][program][editor]") {
    Rig rig;
    PresetProgramEditor editor(*rig.library);
    editor.set_bounds({0, 0, 300, 400});
    REQUIRE(editor.has_banks());
    REQUIRE(editor.current_bank()->number == 0);
    REQUIRE(editor.preset_in(0)->display_name == "Alpha");
    REQUIRE_FALSE(editor.preset_in(50).has_value());

    RecordingCanvas canvas;
    editor.paint(canvas);
    REQUIRE(has_text(canvas, "Bank 0  Factory"));
    REQUIRE(has_text(canvas, "Alpha"));
    REQUIRE(has_text(canvas, "(empty)"));

    // A second bank to move to.
    REQUIRE(rig.library->assign_program({4, 3}, "factory:Charlie"));
    editor.refresh();
    REQUIRE(editor.bank_count() == 2);
    editor.next_bank();
    REQUIRE(editor.current_bank()->number == 4);
    editor.next_bank(); // wraps
    REQUIRE(editor.current_bank()->number == 0);
    editor.previous_bank(); // wraps back
    REQUIRE(editor.current_bank()->number == 4);

    // Header arrows are clickable.
    editor.on_mouse_event(click(5, 10));
    REQUIRE(editor.current_bank()->number == 0);
    editor.on_mouse_event(click(295, 10));
    REQUIRE(editor.current_bank()->number == 4);
}

TEST_CASE("program editor edits slots through the library and the host's answers",
          "[view][preset_browser][program][editor]") {
    Rig rig;
    PresetProgramEditor editor(*rig.library);
    editor.set_bounds({0, 0, 300, 400});

    // Clicking a row selects its program.
    editor.on_mouse_event(click(60, 32 + 4 + 24 * 2 + 12));
    REQUIRE(editor.selected_program() == 2);
    REQUIRE(editor.selected_slot() == BankProgram{0, 2});

    std::vector<BankProgram> asked;
    editor.on_assign_request = [&](BankProgram slot) { asked.push_back(slot); };
    editor.on_mouse_event(click(60, 32 + 4 + 24 * 10 + 12, 2)); // double-click row 10
    REQUIRE(asked == std::vector<BankProgram>{{0, 10}});

    // The host picks Alpha for slot 10: it moves there from slot 0.
    REQUIRE(editor.complete_assign("factory:Alpha"));
    REQUIRE(rig.library->program_of("factory:Alpha") == BankProgram{0, 10});
    REQUIRE_FALSE(editor.preset_in(0).has_value());
    REQUIRE_FALSE(editor.complete_assign("factory:Missing"));

    // Keys: arrows move, Delete clears, Enter asks.
    KeyEvent down;
    down.key = KeyCode::down;
    REQUIRE(editor.on_key_event(down));
    REQUIRE(editor.selected_program() == 11);
    KeyEvent up;
    up.key = KeyCode::up;
    editor.on_key_event(up);
    REQUIRE(editor.selected_program() == 10);
    KeyEvent del;
    del.key = KeyCode::delete_;
    REQUIRE(editor.on_key_event(del));
    REQUIRE_FALSE(rig.library->program_of("factory:Alpha").has_value());
    KeyEvent enter;
    enter.key = KeyCode::enter;
    REQUIRE(editor.on_key_event(enter));
    REQUIRE(asked.back() == BankProgram{0, 10});

    // Selection clamps at both ends.
    editor.select_program(-5);
    REQUIRE(editor.selected_program() == 0);
    editor.select_program(500);
    REQUIRE(editor.selected_program() == 127);
}

TEST_CASE("program editor renames and renumbers a bank",
          "[view][preset_browser][program][editor]") {
    Rig rig;
    // A second bank sorted before the one that will be renumbered, so a
    // renumbered bank that is not followed would fall back to the first bank.
    REQUIRE(rig.library->assign_program({4, 0}, "factory:Charlie"));
    PresetProgramEditor editor(*rig.library);
    editor.set_bounds({0, 0, 300, 400});
    REQUIRE(editor.current_bank()->number == 0);

    std::vector<std::pair<PresetProgramEditor::BankTextKind, std::string>> asked;
    editor.on_bank_text_request = [&](PresetProgramEditor::BankTextKind k,
                                      const std::string& init) { asked.emplace_back(k, init); };
    editor.on_mouse_event(click(150, 10, 2)); // double-click the title
    REQUIRE(asked.size() == 1);
    REQUIRE(asked[0].second == "Factory");
    REQUIRE(editor.request_bank_text(PresetProgramEditor::BankTextKind::renumber));
    REQUIRE(asked[1].second == "0");

    REQUIRE(editor.complete_bank_text(PresetProgramEditor::BankTextKind::rename, "Stock"));
    REQUIRE(editor.current_bank()->name == "Stock");
    REQUIRE(editor.complete_bank_text(PresetProgramEditor::BankTextKind::renumber, "12"));
    REQUIRE(editor.current_bank()->number == 12); // still showing the same bank
    REQUIRE(rig.library->program_of("factory:Alpha") == BankProgram{12, 0});
    REQUIRE_FALSE(editor.complete_bank_text(PresetProgramEditor::BankTextKind::renumber, "oops"));
    REQUIRE_FALSE(editor.complete_bank_text(PresetProgramEditor::BankTextKind::renumber, "99999"));
    REQUIRE(editor.current_bank()->number == 12);
}

TEST_CASE("program editor with an empty map shows nothing to edit",
          "[view][preset_browser][program][editor]") {
    Rig rig;
    PresetLibrary bare(*rig.manager, rig.sandbox.root / "bare" / "library.json");
    PresetProgramEditor editor(bare); // program map never enabled
    editor.set_bounds({0, 0, 300, 400});
    REQUIRE_FALSE(editor.has_banks());
    REQUIRE_FALSE(editor.selected_slot().has_value());
    REQUIRE_FALSE(editor.clear_selected());
    REQUIRE_FALSE(editor.complete_assign("factory:Alpha"));
    RecordingCanvas canvas;
    editor.paint(canvas);
    REQUIRE(has_text(canvas, "(no banks)"));
}

TEST_CASE("program editor scrolls so every slot can be reached",
          "[view][preset_browser][program][editor]") {
    Rig rig;
    PresetProgramEditor editor(*rig.library);
    editor.set_bounds({0, 0, 300, 400}); // 364 px of list: about 15 of 128 rows
    const float list_h = 400.0f - 32.0f - 4.0f;
    REQUIRE(editor.max_scroll_offset() == 128.0f * editor.row_height() - list_h);

    // Keyboard selection keeps the selected row on screen.
    KeyEvent down;
    down.key = KeyCode::down;
    for (int i = 0; i < 40; ++i)
        editor.on_key_event(down);
    REQUIRE(editor.selected_program() == 40);
    const float top = 40.0f * editor.row_height();
    REQUIRE(editor.scroll_offset() <= top);
    REQUIRE(top + editor.row_height() <= editor.scroll_offset() + list_h);
    RecordingCanvas canvas;
    editor.paint(canvas);
    REQUIRE(has_text(canvas, "40"));
    REQUIRE_FALSE(has_text(canvas, "0"));

    // A click after scrolling selects the row under the pointer.
    editor.on_mouse_event(click(60, 32 + 4 + 12));
    REQUIRE(editor.selected_program() ==
            static_cast<int>((12 + editor.scroll_offset()) / editor.row_height()));

    // Selecting upwards scrolls back.
    editor.select_program(0);
    REQUIRE(editor.scroll_offset() == 0.0f);

    // The wheel scrolls, clamped at both ends; the last slot is reachable.
    MouseEvent wheel;
    wheel.is_wheel = true;
    wheel.scroll_delta_y = 100000.0f;
    editor.on_mouse_event(wheel);
    REQUIRE(editor.scroll_offset() == editor.max_scroll_offset());
    editor.on_mouse_event(click(60, 399));
    REQUIRE(editor.selected_program() == 127);
    wheel.scroll_delta_y = -100000.0f;
    editor.on_mouse_event(wheel);
    REQUIRE(editor.scroll_offset() == 0.0f);
}

TEST_CASE("program editor raster draws the header and the selected slot only",
          "[view][preset_browser][program][editor][render]") {
    namespace vt = pulp::vellum_components::testing;
    Rig rig;
    PresetProgramEditor editor(*rig.library);
    editor.set_bounds({0, 0, 300, 400});

    auto first = vt::render(editor, 300, 400);
    vt::maybe_write_png(editor, 300, 400, "preset-program-editor.png");
    REQUIRE(first.width == 300);
    // Header band, the highlighted first slot, and the unselected slot below it.
    CHECK(vt::near(first.at(200, 10), 0x1a, 0x1a, 0x2e));
    CHECK(vt::near(first.at(200, 36 + 12), 0x0f, 0x34, 0x60));
    CHECK(vt::near(first.at(200, 36 + 24 + 12), 0x16, 0x21, 0x3e));

    // Moving the selection moves the highlight with it.
    editor.select_program(5);
    auto moved = vt::render(editor, 300, 400);
    CHECK(vt::near(moved.at(200, 36 + 5 * 24 + 12), 0x0f, 0x34, 0x60));
    CHECK(vt::near(moved.at(200, 36 + 12), 0x16, 0x21, 0x3e));

    // A slot far down the list scrolls into view and is highlighted there.
    editor.select_program(120);
    REQUIRE(editor.scroll_offset() > 0.0f);
    auto scrolled = vt::render(editor, 300, 400);
    vt::maybe_write_png(editor, 300, 400, "preset-program-editor-scrolled.png");
    const int y = 36 + static_cast<int>(120 * editor.row_height() - editor.scroll_offset()) + 12;
    REQUIRE(y < 400);
    CHECK(vt::near(scrolled.at(200, static_cast<std::uint32_t>(y)), 0x0f, 0x34, 0x60));
}
