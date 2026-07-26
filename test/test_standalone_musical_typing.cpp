#include <catch2/catch_test_macros.hpp>

#include <pulp/format/detail/standalone_musical_typing.hpp>
#include <pulp/format/standalone.hpp>

#include <memory>
#include <utility>
#include <vector>

using namespace pulp;

namespace {

class TestWindowHost final : public view::WindowHost {
  public:
    void show() override {
        visible = true;
    }
    void hide() override {
        visible = false;
    }
    bool is_visible() const override {
        return visible;
    }
    bool is_gpu_backed() const override {
        return gpu_backed;
    }
    void repaint() override {}
    void set_close_callback(std::function<void()> callback) override {
        close_callback = std::move(callback);
    }
    void run_event_loop() override {}
    void set_app_key_monitor(std::function<bool(const view::KeyEvent&)> handler) override {
        app_key_monitor = std::move(handler);
    }

    bool visible = false;
    bool gpu_backed = true;
    std::function<void()> close_callback;
    std::function<bool(const view::KeyEvent&)> app_key_monitor;
};

class TestTextInput final : public view::View {
  public:
    bool accepts_text_input() const override {
        return true;
    }
};

struct Harness {
    Harness()
        : typing(
              [this](const midi::MidiEvent& event) {
                  events.push_back(event);
                  return !((reject_note_on && event.is_note_on()) ||
                           (reject_note_off && event.is_note_off()) ||
                           (reject_pitch && event.is_pitch_bend()) ||
                           (reject_sustain_off && event.is_cc() && event.cc_number() == 64 &&
                            event.cc_value() == 0));
              },
              [this](view::View&, const view::WindowOptions& options) {
                  created_options = options;
                  auto host = std::make_unique<TestWindowHost>();
                  host->gpu_backed = host_gpu_backed;
                  window = host.get();
                  return host;
              }) {
        typing.register_command(registry);
        typing.install_key_route(root);
    }

    view::KeyEvent key(view::KeyCode code, bool down = true,
                       std::uint16_t modifiers = view::kModNone, bool repeat = false) {
        return {
            .key = code,
            .modifiers = modifiers,
            .is_down = down,
            .is_repeat = repeat,
        };
    }

    std::vector<midi::MidiEvent> events;
    bool reject_note_off = false;
    bool reject_note_on = false;
    bool reject_pitch = false;
    bool reject_sustain_off = false;
    bool host_gpu_backed = true;
    TestWindowHost* window = nullptr;
    view::WindowOptions created_options;
    view::View root;
    view::CommandRegistry registry;
    format::detail::StandaloneMusicalTyping typing;
};

} // namespace

TEST_CASE("standalone musical typing is opt-in", "[format][standalone][musical-typing]") {
    CHECK_FALSE(format::StandaloneConfig{}.enable_musical_typing_keyboard);
}

TEST_CASE("standalone musical typing exposes one discoverable toggle command",
          "[format][standalone][musical-typing]") {
    Harness harness;
    view::WindowOptions options;
    harness.typing.add_menu_command(options);

    REQUIRE(options.menu_commands.size() == 1);
    const auto& menu = options.menu_commands.front();
    CHECK(menu.menu == "Window");
    CHECK(menu.title == "Musical Typing Keyboard");
    CHECK(menu.key == view::KeyCode::k);
    CHECK(view::is_main_modifier(menu.modifiers));

    menu.action();
    REQUIRE(harness.typing.is_visible());
    REQUIRE(harness.window != nullptr);
    CHECK(harness.created_options.secondary_window);
    CHECK(harness.created_options.use_gpu);

    menu.action();
    CHECK_FALSE(harness.typing.is_visible());
}

TEST_CASE("standalone musical typing declines a non-GPU window host",
          "[format][standalone][musical-typing]") {
    Harness harness;
    harness.host_gpu_backed = false;

    CHECK_FALSE(harness.typing.show());
    CHECK_FALSE(harness.typing.is_visible());
    CHECK(harness.typing.keyboard_for_test() == nullptr);
}

TEST_CASE("standalone musical typing primary+K toggles without repeat or bare-key leakage",
          "[format][standalone][musical-typing]") {
    Harness harness;
    const auto primary = view::is_main_modifier(view::kModCmd) ? view::kModCmd : view::kModCtrl;
    const auto secondary = primary == view::kModCmd ? view::kModCtrl : view::kModCmd;

    CHECK_FALSE(harness.root.on_global_key(harness.key(view::KeyCode::k)));
    CHECK_FALSE(harness.typing.is_visible());

    REQUIRE(harness.root.on_global_key(harness.key(view::KeyCode::k, true, primary)));
    REQUIRE(harness.typing.is_visible());

    CHECK_FALSE(harness.root.on_global_key(harness.key(view::KeyCode::k, true, primary, true)));
    REQUIRE(harness.typing.is_visible());

    CHECK_FALSE(harness.root.on_global_key(harness.key(view::KeyCode::k, true, secondary)));
    CHECK_FALSE(
        harness.root.on_global_key(harness.key(view::KeyCode::k, true, primary | view::kModShift)));
    REQUIRE(harness.typing.is_visible());

    REQUIRE(harness.root.on_global_key(harness.key(view::KeyCode::k, true, primary)));
    CHECK_FALSE(harness.typing.is_visible());
}

TEST_CASE("standalone musical typing restores routes and invalidates stale menu actions",
          "[format][standalone][musical-typing]") {
    view::View root;
    view::CommandRegistry registry;
    int delegated = 0;
    root.on_global_key = [&delegated](const view::KeyEvent&) {
        ++delegated;
        return true;
    };
    std::function<void()> stale_menu_action;

    {
        format::detail::StandaloneMusicalTyping first([](const midi::MidiEvent&) { return true; });
        first.register_command(registry);
        first.install_key_route(root);
        view::WindowOptions options;
        first.add_menu_command(options);
        stale_menu_action = options.menu_commands.front().action;
        CHECK(root.on_global_key({.key = view::KeyCode::a, .is_down = true}));
        first.shutdown();
    }

    REQUIRE(root.on_global_key({.key = view::KeyCode::a, .is_down = true}));
    stale_menu_action();

    {
        format::detail::StandaloneMusicalTyping second([](const midi::MidiEvent&) { return true; });
        second.register_command(registry);
        second.install_key_route(root);
        CHECK(root.on_global_key({.key = view::KeyCode::a, .is_down = true}));
        second.shutdown();
    }

    REQUIRE(root.on_global_key({.key = view::KeyCode::a, .is_down = true}));
    CHECK(delegated == 4);
}

TEST_CASE("standalone musical typing routes note edges through the MIDI sink",
          "[format][standalone][musical-typing]") {
    Harness harness;
    REQUIRE(harness.typing.show());

    REQUIRE(harness.typing.route_app_key_for_test(harness.key(view::KeyCode::a)));
    REQUIRE(harness.events.size() == 1);
    CHECK(harness.events[0].is_note_on());
    CHECK(harness.events[0].note() == 48);

    REQUIRE(harness.typing.route_app_key_for_test(harness.key(view::KeyCode::a, false)));
    REQUIRE(harness.events.size() == 2);
    CHECK(harness.events[1].is_note_off());
    CHECK(harness.events[1].note() == 48);
}

TEST_CASE("standalone musical typing hide releases held notes and controls",
          "[format][standalone][musical-typing]") {
    Harness harness;
    REQUIRE(harness.typing.show());
    REQUIRE(harness.typing.route_app_key_for_test(harness.key(view::KeyCode::a)));

    harness.typing.hide();

    REQUIRE(harness.events.size() == 2);
    CHECK(harness.events[0].is_note_on());
    CHECK(harness.events[1].is_note_off());
    CHECK_FALSE(harness.typing.is_visible());

    midi::MidiBuffer recovery;
    harness.typing.drain_recovery_into(recovery, 64);
    CHECK(recovery.empty());
}

TEST_CASE("standalone musical typing focus loss emits note off",
          "[format][standalone][musical-typing]") {
    Harness harness;
    REQUIRE(harness.typing.show());
    REQUIRE(harness.typing.route_app_key_for_test(harness.key(view::KeyCode::a)));

    REQUIRE(harness.typing.keyboard_for_test() != nullptr);
    harness.typing.keyboard_for_test()->on_focus_changed(false);

    REQUIRE(harness.events.size() == 2);
    CHECK(harness.events[1].is_note_off());
    CHECK(harness.events[1].note() == 48);
}

TEST_CASE("standalone musical typing focus loss releases momentary controls",
          "[format][standalone][musical-typing]") {
    Harness harness;
    REQUIRE(harness.typing.show());
    REQUIRE(harness.typing.route_app_key_for_test(harness.key(view::KeyCode::num1)));
    REQUIRE(harness.typing.route_app_key_for_test(harness.key(view::KeyCode::tab)));

    REQUIRE(harness.typing.keyboard_for_test() != nullptr);
    harness.typing.keyboard_for_test()->on_focus_changed(false);

    REQUIRE(harness.events.size() == 4);
    CHECK(harness.events[0].is_pitch_bend());
    CHECK(harness.events[1].is_cc());
    CHECK(harness.events[1].cc_number() == 64);
    CHECK(harness.events[1].cc_value() == 127);
    CHECK(harness.events[2].is_pitch_bend());
    CHECK(harness.events[3].is_cc());
    CHECK(harness.events[3].cc_number() == 64);
    CHECK(harness.events[3].cc_value() == 0);
}

TEST_CASE("standalone musical typing releases held keys after modifiers change",
          "[format][standalone][musical-typing]") {
    Harness harness;
    REQUIRE(harness.typing.show());
    REQUIRE(harness.typing.route_app_key_for_test(harness.key(view::KeyCode::a)));

    REQUIRE(
        harness.typing.route_app_key_for_test(harness.key(view::KeyCode::a, false, view::kModCmd)));

    REQUIRE(harness.events.size() == 2);
    CHECK(harness.events[1].is_note_off());
    CHECK(harness.events[1].note() == 48);
}

TEST_CASE("standalone musical typing releases held keys after text focus changes",
          "[format][standalone][musical-typing]") {
    Harness harness;
    TestTextInput text_input;
    REQUIRE(harness.typing.show());
    REQUIRE(harness.typing.route_app_key_for_test(harness.key(view::KeyCode::a)));

    view::View::focused_input_ = &text_input;
    REQUIRE(harness.typing.route_app_key_for_test(harness.key(view::KeyCode::a, false)));
    view::View::focused_input_ = nullptr;

    REQUIRE(harness.events.size() == 2);
    CHECK(harness.events[1].is_note_off());
    CHECK(harness.events[1].note() == 48);
}

TEST_CASE("standalone musical typing preserves failed note offs for recovery",
          "[format][standalone][musical-typing]") {
    Harness harness;
    REQUIRE(harness.typing.show());
    REQUIRE(harness.typing.route_app_key_for_test(harness.key(view::KeyCode::a)));
    harness.reject_note_off = true;

    harness.typing.keyboard_for_test()->on_focus_changed(false);

    midi::MidiBuffer recovery;
    auto queued_note_on = midi::MidiEvent::note_on(0, 48, 98);
    queued_note_on.sample_offset = 0;
    REQUIRE(recovery.add(queued_note_on));
    harness.typing.drain_recovery_into(recovery, 64);
    recovery.sort();

    REQUIRE(recovery.size() == 2);
    CHECK(recovery[0].is_note_on());
    CHECK(recovery[1].is_note_off());
    CHECK(recovery[1].note() == 48);
    CHECK(recovery[1].sample_offset == 63);
}

TEST_CASE("standalone musical typing releases a note after its final overlapping hold",
          "[format][standalone][musical-typing]") {
    Harness harness;
    REQUIRE(harness.typing.show());
    auto* keyboard = harness.typing.keyboard_for_test();
    REQUIRE(keyboard != nullptr);

    keyboard->on_note_on(60, 0.75f);
    keyboard->on_note_on(60, 0.75f);
    keyboard->on_note_off(60);
    REQUIRE(harness.events.size() == 2);
    CHECK(harness.events[0].is_note_on());
    CHECK(harness.events[1].is_note_on());

    keyboard->on_note_off(60);
    REQUIRE(harness.events.size() == 3);
    CHECK(harness.events[2].is_note_off());
    CHECK(harness.events[2].note() == 60);
}

TEST_CASE("standalone musical typing preserves holds whose note-on enqueue failed",
          "[format][standalone][musical-typing]") {
    Harness harness;
    REQUIRE(harness.typing.show());
    auto* keyboard = harness.typing.keyboard_for_test();
    REQUIRE(keyboard != nullptr);

    keyboard->on_note_on(60, 0.75f);
    harness.reject_note_on = true;
    keyboard->on_note_on(60, 0.75f);
    keyboard->on_note_off(60);
    REQUIRE(harness.events.size() == 2);

    keyboard->on_note_off(60);
    REQUIRE(harness.events.size() == 3);
    CHECK(harness.events[2].is_note_off());
}

TEST_CASE("standalone musical typing drops a stale note-off recovery after retrigger",
          "[format][standalone][musical-typing]") {
    Harness harness;
    REQUIRE(harness.typing.show());
    auto* keyboard = harness.typing.keyboard_for_test();
    REQUIRE(keyboard != nullptr);

    keyboard->on_note_on(60, 0.75f);
    harness.reject_note_off = true;
    keyboard->on_note_off(60);
    harness.reject_note_off = false;
    keyboard->on_note_on(60, 0.75f);

    midi::MidiBuffer recovery;
    harness.typing.drain_recovery_into(recovery, 64);
    CHECK(recovery.empty());
}

TEST_CASE("standalone musical typing preserves a failed pitch reset",
          "[format][standalone][musical-typing]") {
    Harness harness;
    REQUIRE(harness.typing.show());
    harness.typing.keyboard_for_test()->on_pitch_bend(1.0f);
    harness.reject_pitch = true;

    harness.typing.hide();

    midi::MidiBuffer recovery;
    harness.typing.drain_recovery_into(recovery, 64);
    REQUIRE(recovery.size() == 1);
    CHECK(recovery[0].is_pitch_bend());
    CHECK(recovery[0].sample_offset == 63);
}

TEST_CASE("standalone musical typing recovers failed momentary control releases",
          "[format][standalone][musical-typing]") {
    Harness harness;
    REQUIRE(harness.typing.show());
    REQUIRE(harness.typing.keyboard_for_test() != nullptr);

    harness.typing.keyboard_for_test()->on_sustain(true);
    harness.reject_sustain_off = true;
    harness.typing.keyboard_for_test()->on_sustain(false);

    harness.typing.keyboard_for_test()->on_pitch_bend(1.0f);
    harness.reject_pitch = true;
    harness.typing.keyboard_for_test()->on_pitch_bend(0.0f);

    midi::MidiBuffer recovery;
    harness.typing.drain_recovery_into(recovery, 64);
    REQUIRE(recovery.size() == 2);
    CHECK(recovery[0].is_cc());
    CHECK(recovery[0].cc_number() == 64);
    CHECK(recovery[0].cc_value() == 0);
    CHECK(recovery[0].sample_offset == 63);
    CHECK(recovery[1].is_pitch_bend());
    CHECK(recovery[1].sample_offset == 63);
}

TEST_CASE("standalone musical typing drops stale momentary-control recovery",
          "[format][standalone][musical-typing]") {
    Harness harness;
    REQUIRE(harness.typing.show());
    auto* keyboard = harness.typing.keyboard_for_test();
    REQUIRE(keyboard != nullptr);

    keyboard->on_sustain(true);
    harness.reject_sustain_off = true;
    keyboard->on_sustain(false);
    harness.reject_sustain_off = false;
    keyboard->on_sustain(true);

    keyboard->on_pitch_bend(1.0f);
    harness.reject_pitch = true;
    keyboard->on_pitch_bend(0.0f);
    harness.reject_pitch = false;
    keyboard->on_pitch_bend(-1.0f);

    midi::MidiBuffer recovery;
    harness.typing.drain_recovery_into(recovery, 64);
    CHECK(recovery.empty());
}
