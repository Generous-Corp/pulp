// test_command_registry.cpp — coverage for the Pulp-native CommandRegistry,
// CommandHandler, CommandInfo, ShortcutMap, and KeyMappingEditor primitives.
//
// Section 6.4 of planning/2026-05-24-macos-plugin-authoring-plan.md:
//   "a Pulp app registers commands + key bindings; keypress routes to the
//    focused command handler; rebind UI persists via
//    pulp::state::ApplicationProperties."
//
// Each test below pins one row of that contract.

#include <catch2/catch_test_macros.hpp>

#include <pulp/state/properties_file.hpp>
#include <pulp/view/command_registry.hpp>
#include <pulp/view/input_events.hpp>
#include <pulp/view/key_mapping_editor.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

using namespace pulp::view;

namespace {

constexpr CommandID kCmdOpen = 0x1001;
constexpr CommandID kCmdSave = 0x1002;
constexpr CommandID kCmdUndo = 0x1003;

class CountingHandler : public CommandHandler {
public:
    CountingHandler(std::vector<CommandID> known, bool claim = true)
        : known_(std::move(known)), claim_(claim) {}

    std::vector<CommandID> commands() const override { return known_; }

    bool perform_command(CommandID id) override {
        last_id_ = id;
        ++calls_;
        return claim_;
    }

    int calls_ = 0;
    CommandID last_id_ = kInvalidCommandID;
private:
    std::vector<CommandID> known_;
    bool claim_;
};

std::string temp_props_path(const char* suffix) {
    auto dir = std::filesystem::temp_directory_path();
    auto p = dir / (std::string("pulp-cmdregistry-") + suffix + "-" +
                    std::to_string(std::rand()) + ".json");
    return p.string();
}

} // namespace

TEST_CASE("CommandRegistry registers commands and binds default chords",
          "[view][command_registry][issue-6.4]") {
    CommandRegistry reg;

    CommandInfo open{kCmdOpen, "Open File", "File", KeyCode::o, kModCmd};
    CommandInfo save{kCmdSave, "Save",      "File", KeyCode::s, kModCmd};
    reg.register_command(open);
    reg.register_command(save);

    REQUIRE(reg.commands().size() == 2);
    REQUIRE(reg.command_info(kCmdOpen)->name == "Open File");
    REQUIRE(reg.command_info(kCmdSave)->category == "File");
    REQUIRE_FALSE(reg.command_info(0x9999).has_value());

    // Default chords got bound automatically.
    REQUIRE(reg.shortcuts().find(KeyCode::o, kModCmd) == kCmdOpen);
    REQUIRE(reg.shortcuts().find(KeyCode::s, kModCmd) == kCmdSave);
}

TEST_CASE("CommandRegistry re-registration replaces existing info",
          "[view][command_registry][issue-6.4]") {
    CommandRegistry reg;
    reg.register_command({kCmdOpen, "Open", "File"});
    reg.register_command({kCmdOpen, "Open File…", "File"});
    REQUIRE(reg.commands().size() == 1);
    REQUIRE(reg.command_info(kCmdOpen)->name == "Open File…");
}

TEST_CASE("CommandRegistry dispatch walks handler chain top-to-bottom",
          "[view][command_registry][issue-6.4]") {
    CommandRegistry reg;
    reg.register_command({kCmdOpen, "Open", "File"});

    CountingHandler first({kCmdOpen}, /*claim=*/false);
    CountingHandler second({kCmdOpen}, /*claim=*/true);

    reg.add_handler(&second);   // added first → ends up behind…
    reg.add_handler(&first);    // …the handler added last (priority front).
    REQUIRE(reg.handler_count() == 2);

    REQUIRE(reg.dispatch(kCmdOpen));
    REQUIRE(first.calls_ == 1);   // got it first, declined to claim
    REQUIRE(second.calls_ == 1);  // chain fell through and second claimed
    REQUIRE(second.last_id_ == kCmdOpen);

    // Disabled commands don't dispatch.
    reg.set_enabled(kCmdOpen, false);
    REQUIRE_FALSE(reg.dispatch(kCmdOpen));
    REQUIRE(first.calls_ == 1);
    REQUIRE(second.calls_ == 1);
}

TEST_CASE("CommandRegistry::dispatch_key_event routes through ShortcutMap",
          "[view][command_registry][issue-6.4]") {
    CommandRegistry reg;
    reg.register_command({kCmdOpen, "Open", "File", KeyCode::o, kModCmd});

    CountingHandler handler({kCmdOpen, kCmdSave});
    reg.add_handler(&handler);

    KeyEvent press_open;
    press_open.key = KeyCode::o;
    press_open.modifiers = kModCmd;
    press_open.is_down = true;
    REQUIRE(reg.dispatch_key_event(press_open));
    REQUIRE(handler.calls_ == 1);

    KeyEvent press_release = press_open;
    press_release.is_down = false;
    REQUIRE_FALSE(reg.dispatch_key_event(press_release));  // key-up ignored

    KeyEvent press_unbound;
    press_unbound.key = KeyCode::z;
    press_unbound.modifiers = 0;
    press_unbound.is_down = true;
    REQUIRE_FALSE(reg.dispatch_key_event(press_unbound));
    REQUIRE(handler.calls_ == 1);
}

TEST_CASE("CommandRegistry remove_handler stops dispatching to it",
          "[view][command_registry][issue-6.4]") {
    CommandRegistry reg;
    reg.register_command({kCmdOpen, "Open", "File"});

    CountingHandler handler({kCmdOpen});
    reg.add_handler(&handler);
    REQUIRE(reg.dispatch(kCmdOpen));
    REQUIRE(handler.calls_ == 1);

    reg.remove_handler(&handler);
    REQUIRE(reg.handler_count() == 0);
    REQUIRE_FALSE(reg.dispatch(kCmdOpen));
    REQUIRE(handler.calls_ == 1);

    // remove_handler with unknown pointer is a no-op.
    reg.remove_handler(&handler);
    reg.remove_handler(nullptr);
}

TEST_CASE("ShortcutMap binds, unbinds, and lists chords",
          "[view][command_registry][issue-6.4]") {
    ShortcutMap map;
    map.bind(KeyCode::o, kModCmd, kCmdOpen);
    map.bind(KeyCode::f5, 0, kCmdOpen);   // two chords → same command
    map.bind(KeyCode::s, kModCmd, kCmdSave);

    REQUIRE(map.find(KeyCode::o, kModCmd) == kCmdOpen);
    REQUIRE(map.find(KeyCode::f5, 0) == kCmdOpen);
    REQUIRE(map.find(KeyCode::s, kModCmd) == kCmdSave);
    REQUIRE(map.find(KeyCode::z, 0) == kInvalidCommandID);

    auto open_chords = map.chords_for(kCmdOpen);
    REQUIRE(open_chords.size() == 2);
    std::unordered_set<int> keys;
    for (auto c : open_chords) keys.insert(static_cast<int>(c.key));
    REQUIRE(keys.count(static_cast<int>(KeyCode::o)) == 1);
    REQUIRE(keys.count(static_cast<int>(KeyCode::f5)) == 1);

    map.unbind(KeyCode::o, kModCmd);
    REQUIRE(map.find(KeyCode::o, kModCmd) == kInvalidCommandID);
    REQUIRE(map.chords_for(kCmdOpen).size() == 1);

    map.unbind_command(kCmdOpen);
    REQUIRE(map.chords_for(kCmdOpen).empty());
    REQUIRE(map.find(KeyCode::s, kModCmd) == kCmdSave);

    // Binding to kInvalidCommandID is an unbind.
    map.bind(KeyCode::s, kModCmd, kInvalidCommandID);
    REQUIRE(map.find(KeyCode::s, kModCmd) == kInvalidCommandID);
}

TEST_CASE("ShortcutMap rebinding replaces the previous owner",
          "[view][command_registry][issue-6.4]") {
    ShortcutMap map;
    map.bind(KeyCode::o, kModCmd, kCmdOpen);
    map.bind(KeyCode::o, kModCmd, kCmdSave);   // steal the chord
    REQUIRE(map.find(KeyCode::o, kModCmd) == kCmdSave);
}

TEST_CASE("ShortcutMap persists through PropertiesFile round-trip",
          "[view][command_registry][issue-6.4]") {
    ShortcutMap map;
    map.bind(KeyCode::o, kModCmd, kCmdOpen);
    map.bind(KeyCode::s, kModCmd, kCmdSave);
    map.bind(KeyCode::f5, 0, kCmdUndo);

    pulp::state::PropertiesFile props;
    map.save_to(props, "shortcuts");
    // Independent reading round-trip — props is the only carrier.

    ShortcutMap restored;
    REQUIRE(restored.load_from(props, "shortcuts"));
    REQUIRE(restored.find(KeyCode::o, kModCmd) == kCmdOpen);
    REQUIRE(restored.find(KeyCode::s, kModCmd) == kCmdSave);
    REQUIRE(restored.find(KeyCode::f5, 0) == kCmdUndo);
    REQUIRE(restored.size() == 3);

    // Round-trip with explicit save/load through a temp JSON file too.
    const auto path = temp_props_path("roundtrip");
    REQUIRE(props.save(path));
    pulp::state::PropertiesFile loaded;
    REQUIRE(loaded.load(path));
    ShortcutMap restored_from_disk;
    REQUIRE(restored_from_disk.load_from(loaded, "shortcuts"));
    REQUIRE(restored_from_disk.find(KeyCode::s, kModCmd) == kCmdSave);
    std::remove(path.c_str());
}

TEST_CASE("CommandRegistry loaded ShortcutMap takes priority over defaults",
          "[view][command_registry][issue-6.4]") {
    // The header documents: "load() before register_command so user
    // rebindings take priority." This pins that ordering.
    pulp::state::PropertiesFile props;
    ShortcutMap saved;
    saved.bind(KeyCode::p, kModCmd, kCmdOpen);   // user rebound Open → Cmd-P
    saved.save_to(props);

    CommandRegistry reg;
    REQUIRE(reg.shortcuts().load_from(props));

    // Register with a *different* default chord — the loaded user binding
    // should be honoured and the default chord should not get bound.
    reg.register_command({kCmdOpen, "Open", "File", KeyCode::o, kModCmd});

    REQUIRE(reg.shortcuts().find(KeyCode::p, kModCmd) == kCmdOpen);
    REQUIRE(reg.shortcuts().find(KeyCode::o, kModCmd) == kInvalidCommandID);
}

TEST_CASE("KeyMappingEditor: click row + press chord rebinds and persists",
          "[view][command_registry][issue-6.4]") {
    CommandRegistry reg;
    reg.register_command({kCmdOpen, "Open", "File", KeyCode::o, kModCmd});
    reg.register_command({kCmdSave, "Save", "File", KeyCode::s, kModCmd});

    pulp::state::PropertiesFile props;
    KeyMappingEditor editor(reg);
    editor.set_persistence(&props);
    editor.set_bounds({0, 0, 400, 200});

    // Row 0 is the header; row 1 = first command (Open).
    pulp::view::Point click{50.0f, editor.row_height() * 1.5f};
    editor.on_mouse_down(click);
    REQUIRE(editor.is_capturing());
    REQUIRE(editor.capturing_command() == kCmdOpen);

    KeyEvent press;
    press.key = KeyCode::p;
    press.modifiers = kModCmd | kModShift;
    press.is_down = true;
    REQUIRE(editor.on_key_event(press));
    REQUIRE_FALSE(editor.is_capturing());

    REQUIRE(reg.shortcuts().find(KeyCode::p, kModCmd | kModShift) == kCmdOpen);
    REQUIRE(reg.shortcuts().find(KeyCode::o, kModCmd) == kInvalidCommandID);

    // Persistence flushed straight into the PropertiesFile.
    ShortcutMap restored;
    REQUIRE(restored.load_from(props));
    REQUIRE(restored.find(KeyCode::p, kModCmd | kModShift) == kCmdOpen);

    // ESC during capture cancels without rebinding.
    editor.begin_capture(kCmdSave);
    REQUIRE(editor.is_capturing());
    KeyEvent esc;
    esc.key = KeyCode::escape;
    esc.modifiers = 0;
    esc.is_down = true;
    REQUIRE(editor.on_key_event(esc));
    REQUIRE_FALSE(editor.is_capturing());
    REQUIRE(reg.shortcuts().find(KeyCode::s, kModCmd) == kCmdSave);
}

TEST_CASE("KeyMappingEditor::rebind() bypasses capture",
          "[view][command_registry][issue-6.4]") {
    CommandRegistry reg;
    reg.register_command({kCmdOpen, "Open", "File", KeyCode::o, kModCmd});
    KeyMappingEditor editor(reg);
    std::vector<std::tuple<CommandID, KeyCode, std::uint16_t>> events;
    editor.on_rebound = [&](CommandID id, KeyCode k, std::uint16_t m) {
        events.emplace_back(id, k, m);
    };
    editor.rebind(kCmdOpen, KeyCode::f9, 0);
    REQUIRE(reg.shortcuts().find(KeyCode::f9, 0) == kCmdOpen);
    REQUIRE(events.size() == 1);
    REQUIRE(std::get<0>(events.back()) == kCmdOpen);
    REQUIRE(std::get<1>(events.back()) == KeyCode::f9);
    REQUIRE(std::get<2>(events.back()) == 0);

    // Unbind by passing KeyCode::unknown.
    editor.rebind(kCmdOpen, KeyCode::unknown, 0);
    REQUIRE(reg.shortcuts().chords_for(kCmdOpen).empty());
    REQUIRE(events.size() == 2);
    REQUIRE(std::get<1>(events.back()) == KeyCode::unknown);
}

TEST_CASE("KeyMappingEditor accepts_text_input toggles with capture",
          "[view][command_registry][issue-6.4]") {
    CommandRegistry reg;
    reg.register_command({kCmdOpen, "Open", "File"});
    KeyMappingEditor editor(reg);
    REQUIRE_FALSE(editor.accepts_text_input());
    editor.begin_capture(kCmdOpen);
    REQUIRE(editor.accepts_text_input());
    editor.cancel_capture();
    REQUIRE_FALSE(editor.accepts_text_input());
}

TEST_CASE("format_chord renders modifiers in canonical order",
          "[view][command_registry][issue-6.4]") {
    REQUIRE(format_chord(KeyCode::s, kModCmd) == "Cmd+S");
    REQUIRE(format_chord(KeyCode::o, kModCmd | kModShift) == "Shift+Cmd+O");
    REQUIRE(format_chord(KeyCode::f5, 0) == "F5");
    REQUIRE(format_chord(KeyCode::escape, kModCtrl | kModAlt) ==
            "Ctrl+Alt+Escape");
    REQUIRE(format_chord(KeyCode::unknown, 0) == "—");
}
