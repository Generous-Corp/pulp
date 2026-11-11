// MIDI Learn view layer: the right-click menu rows and commands, the menu opened
// from a bound control, the overlay states, and the engine-backed session
// (begin from the menu, bind from a controller, confirm, Escape, clear, undo).

#include <catch2/catch_test_macros.hpp>

#include <pulp/state/midi_learn_engine.hpp>
#include <pulp/state/midi_learn_undo.hpp>
#include <pulp/state/store.hpp>
#include <pulp/state/undo_manager.hpp>
#include <pulp/view/context_menu.hpp>
#include <pulp/view/midi_learn_menu.hpp>
#include <pulp/view/midi_learn_target.hpp>

#include <memory>
#include <vector>

using namespace pulp;
using view::MidiLearnParamState;
using view::MidiLearnTarget;
using Phase = MidiLearnParamState::Phase;

namespace {

constexpr state::ParamID kCut = 1;
constexpr state::ParamID kRes = 2;

struct Scripted {
    bool present = true;
    MidiLearnParamState st;
    std::vector<state::ParamID> begun, cleared;
    int cancels = 0;

    MidiLearnTarget target() {
        MidiLearnTarget t;
        t.available = [this] { return present; };
        t.state = [this](state::ParamID) { return st; };
        t.begin = [this](state::ParamID id) { begun.push_back(id); };
        t.cancel = [this] { ++cancels; };
        t.clear = [this](state::ParamID id) { cleared.push_back(id); };
        return t;
    }
};

std::vector<std::string> labels(const std::vector<view::ContextMenu::Item>& items) {
    std::vector<std::string> out;
    for (const auto& i : items)
        out.push_back(i.label);
    return out;
}

view::KeyEvent key(view::KeyCode k, bool down = true) {
    view::KeyEvent e;
    e.key = k;
    e.is_down = down;
    return e;
}

void populate(state::StateStore& s) {
    s.add_parameter({.id = kCut, .name = "Cut", .unit = "", .range = {0.0f, 1.0f, 0.5f, 0.0f}});
    s.add_parameter({.id = kRes, .name = "Res", .unit = "", .range = {0.0f, 1.0f, 0.0f, 0.0f}});
}

} // namespace

TEST_CASE("midi learn menu: rows follow the parameter's state", "[view][midi-learn][menu]") {
    Scripted s;
    auto t = s.target();

    auto items = view::midi_learn_menu_items(t, kCut);
    REQUIRE(items.size() == 1);
    CHECK(items[0].label == "MIDI Learn");
    CHECK(items[0].id == view::kMidiLearnCommandBegin);
    CHECK(items[0].enabled);

    s.st.phase = Phase::listening;
    items = view::midi_learn_menu_items(t, kCut);
    REQUIRE(items.size() == 1);
    CHECK(items[0].label == "Cancel MIDI Learn");
    CHECK(items[0].id == view::kMidiLearnCommandCancel);

    s.st.phase = Phase::mapped;
    s.st.mapping_label = "CC 74 ch 1";
    items = view::midi_learn_menu_items(t, kCut);
    REQUIRE(items.size() == 3);
    CHECK(labels(items) ==
          std::vector<std::string>{"MIDI Learn", "Mapped to CC 74 ch 1", "Clear MIDI Mapping"});
    CHECK_FALSE(items[1].enabled); // a label, not a command
    CHECK(items[2].id == view::kMidiLearnCommandClear);
}

TEST_CASE("midi learn menu: nothing is offered without MIDI Learn", "[view][midi-learn][menu]") {
    Scripted s;
    s.present = false;
    auto t = s.target();
    CHECK(view::midi_learn_menu_items(t, kCut).empty());
    CHECK_FALSE(view::run_midi_learn_menu_command(t, kCut, view::kMidiLearnCommandBegin));
    CHECK(s.begun.empty());

    MidiLearnTarget empty;
    CHECK_FALSE(empty.is_available());
    CHECK(view::midi_learn_menu_items(empty, kCut).empty());

    view::View root;
    root.set_bounds({0, 0, 300, 200});
    CHECK(view::show_midi_learn_menu(&root, {10, 10}, t, kCut) == nullptr);
    CHECK(root.child_count() == 0);
}

TEST_CASE("midi learn menu: commands reach the target", "[view][midi-learn][menu]") {
    Scripted s;
    auto t = s.target();
    CHECK(view::run_midi_learn_menu_command(t, kRes, view::kMidiLearnCommandBegin));
    CHECK(view::run_midi_learn_menu_command(t, kRes, view::kMidiLearnCommandCancel));
    CHECK(view::run_midi_learn_menu_command(t, kRes, view::kMidiLearnCommandClear));
    CHECK_FALSE(view::run_midi_learn_menu_command(t, kRes, 12345));
    CHECK(s.begun == std::vector<state::ParamID>{kRes});
    CHECK(s.cancels == 1);
    CHECK(s.cleared == std::vector<state::ParamID>{kRes});

    t.begin = nullptr;
    CHECK_FALSE(view::run_midi_learn_menu_command(t, kRes, view::kMidiLearnCommandBegin));
}

TEST_CASE("midi learn menu: a right-click on a bound control opens it and the row runs",
          "[view][midi-learn][menu]") {
    Scripted s;
    view::View root;
    root.set_bounds({0, 0, 400, 300});
    auto owned = std::make_unique<view::View>();
    owned->set_bounds({50, 60, 80, 80});
    view::View* control = owned.get();
    root.add_child(std::move(owned));
    view::attach_midi_learn_menu(*control, &root, kCut, s.target());

    REQUIRE(root.simulate_context_click({70, 80}));
    REQUIRE(root.child_count() == 2); // the control and the open menu
    auto* menu = dynamic_cast<view::ContextMenu*>(root.child_at(1));
    REQUIRE(menu != nullptr);
    REQUIRE(menu->items().size() == 1);
    CHECK(menu->items()[0].label == "MIDI Learn");

    REQUIRE(menu->on_key_event(key(view::KeyCode::down)));
    REQUIRE(menu->on_key_event(key(view::KeyCode::enter)));
    CHECK(s.begun == std::vector<state::ParamID>{kCut});
    CHECK(root.child_count() == 1); // the menu closed

    // Escape closes without running anything.
    REQUIRE(root.simulate_context_click({70, 80}));
    auto* again = dynamic_cast<view::ContextMenu*>(root.child_at(1));
    REQUIRE(again != nullptr);
    REQUIRE(again->on_key_event(key(view::KeyCode::escape)));
    CHECK(s.begun.size() == 1);
    CHECK(root.child_count() == 1);

    // Without MIDI Learn the right-click opens nothing.
    s.present = false;
    root.simulate_context_click({70, 80});
    CHECK(root.child_count() == 1);
}

TEST_CASE("midi learn overlay: shows listening, a bind confirmation and a mapped dot",
          "[view][midi-learn][overlay]") {
    Scripted s;
    view::MidiLearnOverlay overlay(kCut, s.target());
    CHECK(overlay.current().phase == Phase::idle);
    CHECK_FALSE(overlay.hit_testable()); // never steals the control's click

    s.st.phase = Phase::listening;
    overlay.refresh();
    CHECK(overlay.current().phase == Phase::listening);

    s.st = {Phase::mapped, "CC 74 ch 1", true};
    overlay.refresh();
    CHECK(overlay.current().phase == Phase::mapped);
    CHECK(overlay.current().just_bound);
    CHECK(overlay.current().mapping_label == "CC 74 ch 1");

    s.st = {Phase::mapped, "CC 74 ch 1", false};
    overlay.refresh();
    CHECK_FALSE(overlay.current().just_bound);

    s.present = false;
    overlay.refresh();
    CHECK(overlay.current().phase == Phase::idle);
}

TEST_CASE("midi learn session: begin from the menu, bind from a controller, confirm, clear",
          "[view][midi-learn][session]") {
    state::StateStore store;
    populate(store);
    state::MidiLearnEngine engine;
    state::UndoManager undo;
    state::MidiLearnEditor editor(engine, undo);
    std::uint64_t now = 1000;
    view::MidiLearnSession session(engine, [&] { return now; }, &editor);
    auto target = session.target();
    CHECK(target.is_available());

    CHECK(target.state(kCut).phase == Phase::idle);
    REQUIRE(view::run_midi_learn_menu_command(target, kCut, view::kMidiLearnCommandBegin));
    CHECK(target.state(kCut).phase == Phase::listening);
    CHECK(target.state(kRes).phase == Phase::idle);

    // The controller moves; the engine sees it on the audio side.
    engine.begin_block();
    engine.route(store, state::RawMidi{0xB0, 74, 20});
    engine.route(store, state::RawMidi{0xB0, 74, 70});
    now += 10;
    CHECK_FALSE(session.tick()); // gathered, still settling
    now += 400;
    CHECK(session.tick()); // bound

    auto st = target.state(kCut);
    CHECK(st.phase == Phase::mapped);
    CHECK(st.mapping_label == "CC 74 ch 1");
    CHECK(st.just_bound);
    now += view::MidiLearnSession::kBoundConfirmMs + 1;
    CHECK_FALSE(target.state(kCut).just_bound);

    // The learn was recorded as one undo step.
    CHECK(undo.can_undo());
    undo.undo();
    CHECK(target.state(kCut).phase == Phase::idle);
    undo.redo();
    CHECK(target.state(kCut).phase == Phase::mapped);

    // Clear Mapping goes through the editor, so it undoes too.
    REQUIRE(view::run_midi_learn_menu_command(target, kCut, view::kMidiLearnCommandClear));
    CHECK(target.state(kCut).phase == Phase::idle);
    undo.undo();
    CHECK(target.state(kCut).phase == Phase::mapped);
}

TEST_CASE("midi learn session: Escape cancels only a listening learn",
          "[view][midi-learn][session]") {
    state::MidiLearnEngine engine;
    std::uint64_t now = 0;
    view::MidiLearnSession session(engine, [&] { return now; });
    auto target = session.target();

    CHECK_FALSE(session.handle_key(key(view::KeyCode::escape))); // nothing listening

    target.begin(kCut);
    CHECK_FALSE(session.handle_key(key(view::KeyCode::a)));
    CHECK_FALSE(session.handle_key(key(view::KeyCode::escape, false))); // key-up
    CHECK(target.state(kCut).phase == Phase::listening);

    CHECK(session.handle_key(key(view::KeyCode::escape)));
    CHECK(target.state(kCut).phase == Phase::idle);

    // The menu's own Cancel row does the same.
    target.begin(kCut);
    CHECK(view::run_midi_learn_menu_command(target, kCut, view::kMidiLearnCommandCancel));
    CHECK(target.state(kCut).phase == Phase::idle);
}

TEST_CASE("midi learn session: without an editor a clear still works",
          "[view][midi-learn][session]") {
    state::StateStore store;
    populate(store);
    state::MidiLearnEngine engine;
    state::MidiMapping m;
    m.source = {state::MidiSourceKind::cc7, 0, 9};
    m.id = kRes;
    REQUIRE(engine.set_mapping(m));
    view::MidiLearnSession session(engine, [] { return 0; });
    auto target = session.target();
    CHECK(target.state(kRes).phase == Phase::mapped);
    target.clear(kRes);
    CHECK(target.state(kRes).phase == Phase::idle);
}

// ── Refusal reason ──

namespace {

struct ClaimCc0 : state::MidiSourceReservation {
    bool claims(const state::MidiSource& s) const noexcept override {
        return s.kind == state::MidiSourceKind::cc7 && s.number == 0;
    }
    const char* reason() const noexcept override {
        return "Bank Select / Program Change selects presets";
    }
};

} // namespace

TEST_CASE("midi learn menu: a refusal reason shows as a disabled row", "[view][midi-learn][menu]") {
    Scripted s;
    auto t = s.target();
    s.st.refused_reason = "Bank Select / Program Change selects presets";
    auto items = view::midi_learn_menu_items(t, kCut);
    REQUIRE(items.size() == 2);
    CHECK(items[0].label == "Cannot learn: Bank Select / Program Change selects presets");
    CHECK_FALSE(items[0].enabled);
    CHECK(items[1].label == "MIDI Learn");
    CHECK(items[1].enabled);

    // While listening there is nothing to explain.
    s.st.phase = Phase::listening;
    items = view::midi_learn_menu_items(t, kCut);
    REQUIRE(items.size() == 1);
    CHECK(items[0].label == "Cancel MIDI Learn");

    // An existing mapping stays visible next to the reason.
    s.st.phase = Phase::mapped;
    s.st.mapping_label = "CC 74 ch 1";
    items = view::midi_learn_menu_items(t, kCut);
    CHECK(labels(items) ==
          std::vector<std::string>{"Cannot learn: Bank Select / Program Change selects presets",
                                   "MIDI Learn", "Mapped to CC 74 ch 1", "Clear MIDI Mapping"});
}

TEST_CASE("midi learn session: learning a reserved controller reports why, then it fades",
          "[view][midi-learn][session]") {
    state::StateStore store;
    populate(store);
    state::MidiLearnEngine engine;
    ClaimCc0 claim;
    engine.set_reservation(&claim);
    std::uint64_t now = 1000;
    view::MidiLearnSession session(engine, [&] { return now; });
    auto target = session.target();

    REQUIRE(view::run_midi_learn_menu_command(target, kCut, view::kMidiLearnCommandBegin));
    engine.begin_block();
    engine.route(store, state::RawMidi{0xB0, 0, 20});
    engine.route(store, state::RawMidi{0xB0, 0, 90});
    now += 10;
    CHECK_FALSE(session.tick());
    now += 400;
    CHECK_FALSE(session.tick()); // refused, not bound

    auto st = target.state(kCut);
    CHECK(st.phase == Phase::idle);
    CHECK(st.refused_reason == "Bank Select / Program Change selects presets");
    // Only the parameter that was being learned reports it.
    CHECK(target.state(kRes).refused_reason.empty());
    auto items = view::midi_learn_menu_items(target, kCut);
    REQUIRE(items.size() == 2);
    CHECK(items[0].label.rfind("Cannot learn: ", 0) == 0);

    now += view::MidiLearnSession::kRefusalShowMs + 1;
    CHECK(target.state(kCut).refused_reason.empty());

    // A successful learn afterwards carries no reason.
    REQUIRE(view::run_midi_learn_menu_command(target, kCut, view::kMidiLearnCommandBegin));
    engine.begin_block();
    engine.route(store, state::RawMidi{0xB0, 74, 20});
    engine.route(store, state::RawMidi{0xB0, 74, 70});
    now += 10;
    session.tick();
    now += 400;
    CHECK(session.tick());
    CHECK(target.state(kCut).refused_reason.empty());
}
