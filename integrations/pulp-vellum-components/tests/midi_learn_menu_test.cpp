// MIDI Learn view layer: the right-click menu rows and commands, the menu opened
// from a bound control, the overlay states, and the engine-backed session
// (begin from the menu, bind from a controller, confirm, Escape, clear, undo).

#include <catch2/catch_test_macros.hpp>

#include <pulp/state/midi_learn_engine.hpp>
#include <pulp/state/midi_learn_undo.hpp>
#include <pulp/state/store.hpp>
#include <pulp/state/undo_manager.hpp>
#include <pulp/vellum_components/midi_learn_menu.hpp>
#include <pulp/vellum_components/midi_learn_target.hpp>

#include <vellum/view/context_menu.hpp>

#include "render_helpers.hpp"

#include <memory>
#include <vector>

using namespace pulp;
namespace vc = pulp::vellum_components;
namespace vv = ::vellum::view;
using vc::MidiLearnParamState;
using vc::MidiLearnTarget;
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

std::vector<std::string> labels(const std::vector<vv::ContextMenu::Item>& items) {
    std::vector<std::string> out;
    for (const auto& i : items)
        out.push_back(i.label);
    return out;
}

vv::KeyEvent key(vv::KeyCode k, bool down = true) {
    vv::KeyEvent e;
    e.key = k;
    e.is_down = down;
    return e;
}

void populate(state::StateStore& s) {
    s.add_parameter({.id = kCut, .name = "Cut", .unit = "", .range = {0.0f, 1.0f, 0.5f, 0.0f}});
    s.add_parameter({.id = kRes, .name = "Res", .unit = "", .range = {0.0f, 1.0f, 0.0f, 0.0f}});
}

} // namespace

TEST_CASE("midi learn menu: rows follow the parameter's state", "[midi-learn][menu]") {
    Scripted s;
    auto t = s.target();

    auto items = vc::midi_learn_menu_items(t, kCut);
    REQUIRE(items.size() == 1);
    CHECK(items[0].label == "MIDI Learn");
    CHECK(items[0].id == vc::kMidiLearnCommandBegin);
    CHECK(items[0].enabled);

    s.st.phase = Phase::listening;
    items = vc::midi_learn_menu_items(t, kCut);
    REQUIRE(items.size() == 1);
    CHECK(items[0].label == "Cancel MIDI Learn");
    CHECK(items[0].id == vc::kMidiLearnCommandCancel);

    s.st.phase = Phase::mapped;
    s.st.mapping_label = "CC 74 ch 1";
    items = vc::midi_learn_menu_items(t, kCut);
    REQUIRE(items.size() == 3);
    CHECK(labels(items) ==
          std::vector<std::string>{"MIDI Learn", "Mapped to CC 74 ch 1", "Clear MIDI Mapping"});
    CHECK_FALSE(items[1].enabled); // a label, not a command
    CHECK(items[2].id == vc::kMidiLearnCommandClear);
}

TEST_CASE("midi learn menu: nothing is offered without MIDI Learn", "[midi-learn][menu]") {
    Scripted s;
    s.present = false;
    auto t = s.target();
    CHECK(vc::midi_learn_menu_items(t, kCut).empty());
    CHECK_FALSE(vc::run_midi_learn_menu_command(t, kCut, vc::kMidiLearnCommandBegin));
    CHECK(s.begun.empty());

    MidiLearnTarget empty;
    CHECK_FALSE(empty.is_available());
    CHECK(vc::midi_learn_menu_items(empty, kCut).empty());

    vv::View root;
    root.set_bounds({0, 0, 300, 200});
    CHECK(vc::show_midi_learn_menu(&root, {10, 10}, t, kCut) == nullptr);
    CHECK(root.child_count() == 0);
}

TEST_CASE("midi learn menu: commands reach the target", "[midi-learn][menu]") {
    Scripted s;
    auto t = s.target();
    CHECK(vc::run_midi_learn_menu_command(t, kRes, vc::kMidiLearnCommandBegin));
    CHECK(vc::run_midi_learn_menu_command(t, kRes, vc::kMidiLearnCommandCancel));
    CHECK(vc::run_midi_learn_menu_command(t, kRes, vc::kMidiLearnCommandClear));
    CHECK_FALSE(vc::run_midi_learn_menu_command(t, kRes, 12345));
    CHECK(s.begun == std::vector<state::ParamID>{kRes});
    CHECK(s.cancels == 1);
    CHECK(s.cleared == std::vector<state::ParamID>{kRes});

    t.begin = nullptr;
    CHECK_FALSE(vc::run_midi_learn_menu_command(t, kRes, vc::kMidiLearnCommandBegin));
}

TEST_CASE("midi learn menu: a right-click on a bound control opens it and the row runs",
          "[midi-learn][menu]") {
    Scripted s;
    vv::View root;
    root.set_bounds({0, 0, 400, 300});
    auto owned = std::make_unique<vv::View>();
    owned->set_bounds({50, 60, 80, 80});
    vv::View* control = owned.get();
    root.add_child(std::move(owned));
    vc::attach_midi_learn_menu(*control, &root, kCut, s.target());

    REQUIRE(root.simulate_context_click({70, 80}));
    REQUIRE(root.child_count() == 2); // the control and the open menu
    auto* menu = dynamic_cast<vv::ContextMenu*>(root.child_at(1));
    REQUIRE(menu != nullptr);
    REQUIRE(menu->items().size() == 1);
    CHECK(menu->items()[0].label == "MIDI Learn");

    REQUIRE(menu->on_key_event(key(vv::KeyCode::down)));
    REQUIRE(menu->on_key_event(key(vv::KeyCode::enter)));
    CHECK(s.begun == std::vector<state::ParamID>{kCut});
    CHECK(root.child_count() == 1); // the menu closed

    // Escape closes without running anything.
    REQUIRE(root.simulate_context_click({70, 80}));
    auto* again = dynamic_cast<vv::ContextMenu*>(root.child_at(1));
    REQUIRE(again != nullptr);
    REQUIRE(again->on_key_event(key(vv::KeyCode::escape)));
    CHECK(s.begun.size() == 1);
    CHECK(root.child_count() == 1);

    // Without MIDI Learn the right-click opens nothing.
    s.present = false;
    root.simulate_context_click({70, 80});
    CHECK(root.child_count() == 1);
}

TEST_CASE("midi learn overlay: shows listening, a bind confirmation and a mapped dot",
          "[midi-learn][overlay]") {
    Scripted s;
    vc::MidiLearnOverlay overlay(kCut, s.target());
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
          "[midi-learn][session]") {
    state::StateStore store;
    populate(store);
    state::MidiLearnEngine engine;
    state::UndoManager undo;
    state::MidiLearnEditor editor(engine, undo);
    std::uint64_t now = 1000;
    vc::MidiLearnSession session(engine, [&] { return now; }, &editor);
    auto target = session.target();
    CHECK(target.is_available());

    CHECK(target.state(kCut).phase == Phase::idle);
    REQUIRE(vc::run_midi_learn_menu_command(target, kCut, vc::kMidiLearnCommandBegin));
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
    now += vc::MidiLearnSession::kBoundConfirmMs + 1;
    CHECK_FALSE(target.state(kCut).just_bound);

    // The learn was recorded as one undo step.
    CHECK(undo.can_undo());
    undo.undo();
    CHECK(target.state(kCut).phase == Phase::idle);
    undo.redo();
    CHECK(target.state(kCut).phase == Phase::mapped);

    // Clear Mapping goes through the editor, so it undoes too.
    REQUIRE(vc::run_midi_learn_menu_command(target, kCut, vc::kMidiLearnCommandClear));
    CHECK(target.state(kCut).phase == Phase::idle);
    undo.undo();
    CHECK(target.state(kCut).phase == Phase::mapped);
}

TEST_CASE("midi learn session: Escape cancels only a listening learn", "[midi-learn][session]") {
    state::MidiLearnEngine engine;
    std::uint64_t now = 0;
    vc::MidiLearnSession session(engine, [&] { return now; });
    auto target = session.target();

    CHECK_FALSE(session.handle_key(key(vv::KeyCode::escape))); // nothing listening

    target.begin(kCut);
    CHECK_FALSE(session.handle_key(key(vv::KeyCode::a)));
    CHECK_FALSE(session.handle_key(key(vv::KeyCode::escape, false))); // key-up
    CHECK(target.state(kCut).phase == Phase::listening);

    CHECK(session.handle_key(key(vv::KeyCode::escape)));
    CHECK(target.state(kCut).phase == Phase::idle);

    // The menu's own Cancel row does the same.
    target.begin(kCut);
    CHECK(vc::run_midi_learn_menu_command(target, kCut, vc::kMidiLearnCommandCancel));
    CHECK(target.state(kCut).phase == Phase::idle);
}

TEST_CASE("midi learn session: without an editor a clear still works", "[midi-learn][session]") {
    state::StateStore store;
    populate(store);
    state::MidiLearnEngine engine;
    state::MidiMapping m;
    m.source = {state::MidiSourceKind::cc7, 0, 9};
    m.id = kRes;
    REQUIRE(engine.set_mapping(m));
    vc::MidiLearnSession session(engine, [] { return 0; });
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

TEST_CASE("midi learn menu: a refusal reason shows as a disabled row", "[midi-learn][menu]") {
    Scripted s;
    auto t = s.target();
    s.st.refused_reason = "Bank Select / Program Change selects presets";
    auto items = vc::midi_learn_menu_items(t, kCut);
    REQUIRE(items.size() == 2);
    CHECK(items[0].label == "Cannot learn: Bank Select / Program Change selects presets");
    CHECK_FALSE(items[0].enabled);
    CHECK(items[1].label == "MIDI Learn");
    CHECK(items[1].enabled);

    // While listening there is nothing to explain.
    s.st.phase = Phase::listening;
    items = vc::midi_learn_menu_items(t, kCut);
    REQUIRE(items.size() == 1);
    CHECK(items[0].label == "Cancel MIDI Learn");

    // An existing mapping stays visible next to the reason.
    s.st.phase = Phase::mapped;
    s.st.mapping_label = "CC 74 ch 1";
    items = vc::midi_learn_menu_items(t, kCut);
    CHECK(labels(items) ==
          std::vector<std::string>{"Cannot learn: Bank Select / Program Change selects presets",
                                   "MIDI Learn", "Mapped to CC 74 ch 1", "Clear MIDI Mapping"});
}

TEST_CASE("midi learn session: learning a reserved controller reports why, then it fades",
          "[midi-learn][session]") {
    state::StateStore store;
    populate(store);
    state::MidiLearnEngine engine;
    ClaimCc0 claim;
    engine.set_reservation(&claim);
    std::uint64_t now = 1000;
    vc::MidiLearnSession session(engine, [&] { return now; });
    auto target = session.target();

    REQUIRE(vc::run_midi_learn_menu_command(target, kCut, vc::kMidiLearnCommandBegin));
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
    auto items = vc::midi_learn_menu_items(target, kCut);
    REQUIRE(items.size() == 2);
    CHECK(items[0].label.rfind("Cannot learn: ", 0) == 0);

    now += vc::MidiLearnSession::kRefusalShowMs + 1;
    CHECK(target.state(kCut).refused_reason.empty());

    // A successful learn afterwards carries no reason.
    REQUIRE(vc::run_midi_learn_menu_command(target, kCut, vc::kMidiLearnCommandBegin));
    engine.begin_block();
    engine.route(store, state::RawMidi{0xB0, 74, 20});
    engine.route(store, state::RawMidi{0xB0, 74, 70});
    now += 10;
    session.tick();
    now += 400;
    CHECK(session.tick());
    CHECK(target.state(kCut).refused_reason.empty());
}

TEST_CASE("midi learn session: a successful learn clears an earlier refusal at once",
          "[midi-learn][session]") {
    state::StateStore store;
    populate(store);
    state::MidiLearnEngine engine;
    ClaimCc0 claim;
    engine.set_reservation(&claim);
    std::uint64_t now = 1000;
    vc::MidiLearnSession session(engine, [&] { return now; });
    auto target = session.target();

    REQUIRE(vc::run_midi_learn_menu_command(target, kCut, vc::kMidiLearnCommandBegin));
    engine.begin_block();
    engine.route(store, state::RawMidi{0xB0, 0, 20});
    engine.route(store, state::RawMidi{0xB0, 0, 90});
    now += 10;
    session.tick();
    now += 400;
    session.tick();
    REQUIRE_FALSE(target.state(kCut).refused_reason.empty());

    // Retry inside the refusal window and succeed.
    REQUIRE(vc::run_midi_learn_menu_command(target, kCut, vc::kMidiLearnCommandBegin));
    engine.begin_block();
    engine.route(store, state::RawMidi{0xB0, 74, 20});
    engine.route(store, state::RawMidi{0xB0, 74, 70});
    now += 10;
    session.tick();
    now += 400;
    REQUIRE(session.tick());
    CHECK(now - 1000 < vc::MidiLearnSession::kRefusalShowMs);
    CHECK(target.state(kCut).refused_reason.empty());
}

// ── Mapping options ──

namespace {

struct OptionCalls {
    std::vector<std::pair<state::ParamID, vc::MidiLearnMappingOptions>> calls;
};

} // namespace

TEST_CASE("midi learn menu: a mapped parameter shows its settings rows",
          "[midi-learn][menu][options]") {
    Scripted s;
    OptionCalls rec;
    auto t = s.target();
    t.set_options = [&](state::ParamID id, const vc::MidiLearnMappingOptions& o) {
        rec.calls.push_back({id, o});
    };
    s.st.phase = Phase::mapped;
    s.st.mapping_label = "CC 74 ch 1";

    auto items = vc::midi_learn_menu_items(t, kCut);
    CHECK(labels(items) == std::vector<std::string>{"MIDI Learn", "Mapped to CC 74 ch 1", "Invert",
                                                    "Relative mode", "Takeover",
                                                    "Clear MIDI Mapping"});

    // The feedback row appears only where the format can send feedback.
    s.st.feedback_supported = true;
    items = vc::midi_learn_menu_items(t, kCut);
    CHECK(labels(items) == std::vector<std::string>{"MIDI Learn", "Mapped to CC 74 ch 1", "Invert",
                                                    "Relative mode", "Takeover", "Send feedback",
                                                    "Clear MIDI Mapping"});

    // Not mapped, or no way to change settings: no settings rows.
    s.st.phase = Phase::idle;
    CHECK(labels(vc::midi_learn_menu_items(t, kCut)) == std::vector<std::string>{"MIDI Learn"});
    s.st.phase = Phase::mapped;
    t.set_options = nullptr;
    items = vc::midi_learn_menu_items(t, kCut);
    CHECK(labels(items) ==
          std::vector<std::string>{"MIDI Learn", "Mapped to CC 74 ch 1", "Clear MIDI Mapping"});
}

TEST_CASE("midi learn menu: changing a row sends the full settings with that field changed",
          "[midi-learn][menu][options]") {
    Scripted s;
    OptionCalls rec;
    auto t = s.target();
    t.set_options = [&](state::ParamID id, const vc::MidiLearnMappingOptions& o) {
        rec.calls.push_back({id, o});
        s.st.options = o; // the target applies it
    };
    s.st.phase = Phase::mapped;
    s.st.mapping_label = "CC 1";
    s.st.feedback_supported = true;
    s.st.options = {2, 1, false, true};

    auto items = vc::midi_learn_menu_items(t, kCut);
    auto find = [&](const char* label) -> vv::ContextMenu::Item& {
        for (auto& i : items)
            if (i.label == label)
                return i;
        FAIL("no row " << label);
        return items.front();
    };

    // Rows start at the current values.
    CHECK_FALSE(find("Invert").checked);
    CHECK(find("Relative mode").choice_index == 2);
    CHECK(find("Takeover").choice_index == 1);
    CHECK(find("Send feedback").checked);

    find("Invert").set_toggled(true);
    REQUIRE(rec.calls.size() == 1);
    CHECK(rec.calls[0].first == kCut);
    CHECK((rec.calls[0].second == vc::MidiLearnMappingOptions{2, 1, true, true}));

    find("Relative mode").set_choice_index(3);
    CHECK((rec.calls.back().second == vc::MidiLearnMappingOptions{3, 1, true, true}));
    find("Takeover").set_choice_index(2);
    CHECK((rec.calls.back().second == vc::MidiLearnMappingOptions{3, 2, true, true}));
    find("Send feedback").set_toggled(false);
    CHECK((rec.calls.back().second == vc::MidiLearnMappingOptions{3, 2, true, false}));
    CHECK(rec.calls.size() == 4);
}

TEST_CASE("midi learn menu: settings rows apply themselves and are not routed commands",
          "[midi-learn][menu][options]") {
    Scripted s;
    OptionCalls rec;
    auto t = s.target();
    t.set_options = [&](state::ParamID id, const vc::MidiLearnMappingOptions& o) {
        rec.calls.push_back({id, o});
        s.st.options = o;
    };
    s.st.phase = Phase::mapped;
    s.st.mapping_label = "CC 74 ch 1";
    s.st.feedback_supported = true;

    // The setting rows carry their own change callbacks, so routing their
    // ids through the command function does nothing and never applies twice.
    for (int id : {vc::kMidiLearnCommandInvert, vc::kMidiLearnCommandRelativeMode,
                   vc::kMidiLearnCommandTakeover, vc::kMidiLearnCommandFeedback})
        CHECK_FALSE(vc::run_midi_learn_menu_command(t, kCut, id));
    CHECK(rec.calls.empty());

    // Activating the Invert row in a live menu applies it exactly once and
    // leaves the learn commands untouched.
    vv::View root;
    root.set_bounds({0, 0, 400, 300});
    auto* menu = vc::show_midi_learn_menu(&root, {10, 10}, t, kCut);
    REQUIRE(menu != nullptr);
    int guard = 0;
    while (menu->hovered_index() != 2 && guard++ < 8)
        REQUIRE(menu->on_key_event(key(vv::KeyCode::down)));
    REQUIRE(menu->hovered_index() == 2);
    REQUIRE(menu->items()[2].label == "Invert");
    REQUIRE(menu->on_key_event(key(vv::KeyCode::enter)));
    REQUIRE(rec.calls.size() == 1);
    CHECK(rec.calls[0].second.invert);
    CHECK(s.begun.empty());
    CHECK(s.cleared.empty());
    CHECK(s.cancels == 0);
}

TEST_CASE("midi learn session: settings change the mapping, route correctly and undo",
          "[midi-learn][session][options]") {
    state::StateStore store;
    populate(store);
    state::MidiLearnEngine engine;
    state::UndoManager undo;
    state::MidiLearnEditor editor(engine, undo);
    std::uint64_t now = 0;
    vc::MidiLearnSession session(engine, [&] { return now; }, &editor);
    session.set_feedback_supported(true);
    auto target = session.target();

    state::MidiMapping m;
    m.source = {state::MidiSourceKind::cc7, 0, 74};
    m.id = kCut;
    REQUIRE(editor.set_mapping(m));
    CHECK(target.state(kCut).feedback_supported);
    CHECK((target.state(kCut).options == vc::MidiLearnMappingOptions{}));

    // Invert: the response flips for the audio thread.
    auto o = target.state(kCut).options;
    o.invert = true;
    target.set_options(kCut, o);
    CHECK(target.state(kCut).options.invert);
    engine.begin_block();
    engine.route(store, state::RawMidi{0xB0, 74, 127});
    CHECK(store.get_normalized(kCut) < 0.01f);

    o = target.state(kCut).options;
    o.relative_mode = 1;
    o.takeover = 2;
    o.feedback = true;
    target.set_options(kCut, o);
    const auto stored = engine.mapping_for(kCut);
    REQUIRE(stored.has_value());
    CHECK(stored->mode == state::RelativeMode::twos_complement);
    CHECK(stored->takeover == state::TakeoverMode::scale);
    CHECK(stored->feedback);
    CHECK(stored->scale.min > stored->scale.max); // still inverted

    // Turning invert back off restores the original window.
    o = target.state(kCut).options;
    o.invert = false;
    target.set_options(kCut, o);
    CHECK(engine.mapping_for(kCut)->scale.min < engine.mapping_for(kCut)->scale.max);

    // Each change is one undo step; unchanged settings record nothing.
    undo.undo();
    CHECK(target.state(kCut).options.invert);
    const bool had_undo = undo.can_undo();
    target.set_options(kCut, target.state(kCut).options);
    CHECK(undo.can_undo() == had_undo);

    // An unmapped parameter ignores the call.
    target.set_options(kRes, vc::MidiLearnMappingOptions{1, 1, true, true});
    CHECK_FALSE(engine.mapping_for(kRes).has_value());
}

TEST_CASE("midi learn overlay: pixels show the outline, chip and dot for each phase",
          "[midi-learn][overlay][render]") {
    if (!::vellum::view::raw_rgba_render_available())
        SKIP("Vellum CPU raster backend unavailable");
    using namespace pulp::vellum_components::testing;

    Scripted s;
    vv::View root;
    root.set_bounds({0, 0, 120, 80});
    auto owned = std::make_unique<vc::MidiLearnOverlay>(kCut, s.target());
    auto* overlay = owned.get();
    root.add_child(std::move(owned));
    // The root lays its child out: pad it in to (20, 24) and size it 80 x 40.
    root.flex().direction = ::vellum::view::FlexDirection::row;
    root.flex().align_items = ::vellum::view::FlexAlign::start;
    root.flex().padding_left = 20;
    root.flex().padding_top = 24;
    overlay->flex().preferred_width = 80;
    overlay->flex().preferred_height = 40;

    const auto accent =
        overlay->resolve_color("accent.primary", ::vellum::canvas::Color::rgba8(100, 150, 255));
    const int ar = static_cast<int>(accent.r * 255.0f + 0.5f);
    const int ag = static_cast<int>(accent.g * 255.0f + 0.5f);
    const int ab = static_cast<int>(accent.b * 255.0f + 0.5f);
    const auto bg = render(root, 120, 80);
    const auto bg_px = bg.at(60, 24);

    // Idle draws nothing.
    CHECK(near(bg_px, bg_px.r, bg_px.g, bg_px.b, 0));
    CHECK_FALSE(near(bg_px, ar, ag, ab, 20));

    // Listening: a 2 px accent outline along the top edge and a chip above it.
    s.st.phase = Phase::listening;
    overlay->refresh();
    const auto listening = render(root, 120, 80);
    CHECK(near(listening.at(60, 24), ar, ag, ab, 25));
    CHECK(near(listening.at(60, 23), ar, ag, ab, 25));
    CHECK(near(listening.at(60, 40), bg_px.r, bg_px.g, bg_px.b, 3));       // interior untouched
    CHECK_FALSE(near(listening.at(24, 14), bg_px.r, bg_px.g, bg_px.b, 3)); // chip body

    // Mapped and settled: just the corner dot, no outline and no chip.
    s.st = {Phase::mapped, "CC 74 ch 1", false};
    overlay->refresh();
    const auto mapped = render(root, 120, 80);
    CHECK(near(mapped.at(96, 28), ar, ag, ab, 25));
    CHECK(near(mapped.at(60, 24), bg_px.r, bg_px.g, bg_px.b, 3));
    CHECK(near(mapped.at(24, 14), bg_px.r, bg_px.g, bg_px.b, 3));

    // Just bound: outline and chip come back alongside the dot.
    s.st = {Phase::mapped, "CC 74 ch 1", true};
    overlay->refresh();
    const auto bound = render(root, 120, 80);
    CHECK(near(bound.at(60, 24), ar, ag, ab, 25));
    CHECK_FALSE(near(bound.at(24, 14), bg_px.r, bg_px.g, bg_px.b, 3));
    maybe_write_png(root, 120, 80, "midi-learn-overlay.png");
}
