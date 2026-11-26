// The Vellum standalone typing panel: the standalone's panel interface, the MIDI
// the keyboard emits through the UI queue, recovery of events the queue refused,
// and the keyboard drawn offscreen.

#include <pulp/format/standalone_config.hpp>
#include <pulp/vellum_audio/standalone_typing_panel.hpp>

#include <catch2/catch_test_macros.hpp>

#include <vellum/view/screenshot.hpp>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace pulp;
using pulp::vellum_audio::StandaloneTypingPanel;

namespace vv = ::vellum::view;

namespace {

class TestWindowHost final : public vv::WindowHost {
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
    void set_app_key_monitor(std::function<bool(const vv::KeyEvent&)> handler) override {
        app_key_monitor = std::move(handler);
    }
    void position_beside(vv::WindowHost* other) override {
        positioned_beside = other ? other->native_window_handle() : nullptr;
    }

    bool visible = false;
    bool gpu_backed = true;
    void* positioned_beside = nullptr;
    std::function<void()> close_callback;
    std::function<bool(const vv::KeyEvent&)> app_key_monitor;
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
              [this](vv::View&, const vv::WindowOptions& options) {
                  created_options = options;
                  auto host = std::make_unique<TestWindowHost>();
                  host->gpu_backed = host_gpu_backed;
                  window = host.get();
                  return host;
              },
              [this] { return text_focused; }) {}

    vv::KeyEvent key(vv::KeyCode code, bool down = true, std::uint16_t modifiers = vv::kModNone,
                     bool repeat = false) {
        vv::KeyEvent event;
        event.key = code;
        event.modifiers = modifiers;
        event.is_down = down;
        event.is_repeat = repeat;
        return event;
    }

    std::vector<midi::MidiEvent> events;
    bool reject_note_off = false;
    bool reject_note_on = false;
    bool reject_pitch = false;
    bool reject_sustain_off = false;
    bool host_gpu_backed = true;
    bool text_focused = false;
    TestWindowHost* window = nullptr;
    vv::WindowOptions created_options;
    StandaloneTypingPanel typing;
};

} // namespace

TEST_CASE("vellum typing panel declines a non-GPU window host", "[standalone-typing-panel]") {
    Harness harness;
    harness.host_gpu_backed = false;

    CHECK_FALSE(harness.typing.show());
    CHECK_FALSE(harness.typing.is_visible());
    CHECK(harness.typing.keyboard() == nullptr);
}

TEST_CASE("vellum typing panel routes note edges through the MIDI sink",
          "[standalone-typing-panel]") {
    Harness harness;
    REQUIRE(harness.typing.show());

    REQUIRE(harness.typing.route_app_key(harness.key(vv::KeyCode::a)));
    REQUIRE(harness.events.size() == 1);
    CHECK(harness.events[0].is_note_on());
    CHECK(harness.events[0].note() == 48);

    REQUIRE(harness.typing.route_app_key(harness.key(vv::KeyCode::a, false)));
    REQUIRE(harness.events.size() == 2);
    CHECK(harness.events[1].is_note_off());
    CHECK(harness.events[1].note() == 48);
}

TEST_CASE("vellum typing panel hide releases held notes and controls",
          "[standalone-typing-panel]") {
    Harness harness;
    REQUIRE(harness.typing.show());
    REQUIRE(harness.typing.route_app_key(harness.key(vv::KeyCode::a)));

    harness.typing.hide();

    REQUIRE(harness.events.size() == 2);
    CHECK(harness.events[0].is_note_on());
    CHECK(harness.events[1].is_note_off());
    CHECK_FALSE(harness.typing.is_visible());

    midi::MidiBuffer recovery;
    harness.typing.drain_midi_into(recovery, 64);
    CHECK(recovery.empty());
}

TEST_CASE("vellum typing panel focus loss emits note off", "[standalone-typing-panel]") {
    Harness harness;
    REQUIRE(harness.typing.show());
    REQUIRE(harness.typing.route_app_key(harness.key(vv::KeyCode::a)));

    REQUIRE(harness.typing.keyboard() != nullptr);
    harness.typing.keyboard()->on_focus_changed(false);

    REQUIRE(harness.events.size() == 2);
    CHECK(harness.events[1].is_note_off());
    CHECK(harness.events[1].note() == 48);
}

TEST_CASE("vellum typing panel focus loss releases momentary controls",
          "[standalone-typing-panel]") {
    Harness harness;
    REQUIRE(harness.typing.show());
    REQUIRE(harness.typing.route_app_key(harness.key(vv::KeyCode::num1)));
    REQUIRE(harness.typing.route_app_key(harness.key(vv::KeyCode::tab)));

    REQUIRE(harness.typing.keyboard() != nullptr);
    harness.typing.keyboard()->on_focus_changed(false);

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

TEST_CASE("vellum typing panel releases held keys after modifiers change",
          "[standalone-typing-panel]") {
    Harness harness;
    REQUIRE(harness.typing.show());
    REQUIRE(harness.typing.route_app_key(harness.key(vv::KeyCode::a)));

    REQUIRE(harness.typing.route_app_key(harness.key(vv::KeyCode::a, false, vv::kModCmd)));

    REQUIRE(harness.events.size() == 2);
    CHECK(harness.events[1].is_note_off());
    CHECK(harness.events[1].note() == 48);
}

TEST_CASE("vellum typing panel preserves failed note offs for recovery",
          "[standalone-typing-panel]") {
    Harness harness;
    REQUIRE(harness.typing.show());
    REQUIRE(harness.typing.route_app_key(harness.key(vv::KeyCode::a)));
    harness.reject_note_off = true;

    harness.typing.keyboard()->on_focus_changed(false);

    midi::MidiBuffer recovery;
    auto queued_note_on = midi::MidiEvent::note_on(0, 48, 98);
    queued_note_on.sample_offset = 0;
    REQUIRE(recovery.add(queued_note_on));
    harness.typing.drain_midi_into(recovery, 64);
    recovery.sort();

    REQUIRE(recovery.size() == 2);
    CHECK(recovery[0].is_note_on());
    CHECK(recovery[1].is_note_off());
    CHECK(recovery[1].note() == 48);
    CHECK(recovery[1].sample_offset == 63);
}

// The audio thread drains into the host's real MIDI buffer, which runs with a
// reserved capacity and set_realtime_capacity_limit(true) so it drops instead of
// allocating. Every other test here uses a default-constructed MidiBuffer, which
// grows without limit — so add() never fails and the drain's retry branch never
// executes. Reserve zero events to force that branch.
TEST_CASE("vellum typing panel keeps recovery pending when the drain buffer is full",
          "[standalone-typing-panel][rt-safety]") {
    Harness harness;
    REQUIRE(harness.typing.show());
    REQUIRE(harness.typing.route_app_key(harness.key(vv::KeyCode::a)));
    harness.reject_note_off = true;
    harness.typing.keyboard()->on_focus_changed(false);

    midi::MidiBuffer full;
    full.reserve_events(0);
    full.set_realtime_capacity_limit(true);
    harness.typing.drain_midi_into(full, 64);
    CHECK(full.empty());

    // The recovery must survive a rejected append, not be consumed by it.
    midi::MidiBuffer roomy;
    harness.typing.drain_midi_into(roomy, 64);
    REQUIRE(roomy.size() == 1);
    CHECK(roomy[0].is_note_off());
    CHECK(roomy[0].note() == 48);
    CHECK(roomy[0].sample_offset == 63);

    // ...and is not delivered a second time once it has landed.
    midi::MidiBuffer after;
    harness.typing.drain_midi_into(after, 64);
    CHECK(after.empty());
}

// hide() must clear the KEYBOARD's latched control state, not just emit the
// matching MIDI. Otherwise the sustain pad stays lit across a hide/show cycle
// while the synth's sustain is off.
TEST_CASE("vellum typing panel hide clears latched keyboard controls",
          "[standalone-typing-panel]") {
    Harness harness;
    REQUIRE(harness.typing.show());
    auto* keyboard = harness.typing.keyboard();
    REQUIRE(keyboard != nullptr);

    const int pad = keyboard->element_for_action("sustain");
    REQUIRE(pad >= 0);

    // Hold tab (momentary sustain) and hide while it is still down, so the
    // key-up can never route back to the keyboard.
    REQUIRE(harness.typing.route_app_key(harness.key(vv::KeyCode::tab)));
    REQUIRE(keyboard->element_value(pad) == 1.0f);

    harness.typing.hide();
    CHECK(keyboard->element_value(pad) == 0.0f);
}

TEST_CASE("vellum typing panel releases a note after its final overlapping hold",
          "[standalone-typing-panel]") {
    Harness harness;
    REQUIRE(harness.typing.show());
    auto* keyboard = harness.typing.keyboard();
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

TEST_CASE("vellum typing panel preserves holds whose note-on enqueue failed",
          "[standalone-typing-panel]") {
    Harness harness;
    REQUIRE(harness.typing.show());
    auto* keyboard = harness.typing.keyboard();
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

TEST_CASE("vellum typing panel drops a stale note-off recovery after retrigger",
          "[standalone-typing-panel]") {
    Harness harness;
    REQUIRE(harness.typing.show());
    auto* keyboard = harness.typing.keyboard();
    REQUIRE(keyboard != nullptr);

    keyboard->on_note_on(60, 0.75f);
    harness.reject_note_off = true;
    keyboard->on_note_off(60);
    harness.reject_note_off = false;
    keyboard->on_note_on(60, 0.75f);

    midi::MidiBuffer recovery;
    harness.typing.drain_midi_into(recovery, 64);
    CHECK(recovery.empty());
}

TEST_CASE("vellum typing panel preserves a failed pitch reset", "[standalone-typing-panel]") {
    Harness harness;
    REQUIRE(harness.typing.show());
    harness.typing.keyboard()->on_pitch_bend(1.0f);
    harness.reject_pitch = true;

    harness.typing.hide();

    midi::MidiBuffer recovery;
    harness.typing.drain_midi_into(recovery, 64);
    REQUIRE(recovery.size() == 1);
    CHECK(recovery[0].is_pitch_bend());
    CHECK(recovery[0].sample_offset == 63);
}

TEST_CASE("vellum typing panel recovers failed momentary control releases",
          "[standalone-typing-panel]") {
    Harness harness;
    REQUIRE(harness.typing.show());
    REQUIRE(harness.typing.keyboard() != nullptr);

    harness.typing.keyboard()->on_sustain(true);
    harness.reject_sustain_off = true;
    harness.typing.keyboard()->on_sustain(false);

    harness.typing.keyboard()->on_pitch_bend(1.0f);
    harness.reject_pitch = true;
    harness.typing.keyboard()->on_pitch_bend(0.0f);

    midi::MidiBuffer recovery;
    harness.typing.drain_midi_into(recovery, 64);
    REQUIRE(recovery.size() == 2);
    CHECK(recovery[0].is_cc());
    CHECK(recovery[0].cc_number() == 64);
    CHECK(recovery[0].cc_value() == 0);
    CHECK(recovery[0].sample_offset == 63);
    CHECK(recovery[1].is_pitch_bend());
    CHECK(recovery[1].sample_offset == 63);
}

TEST_CASE("vellum typing panel drops stale momentary-control recovery",
          "[standalone-typing-panel]") {
    Harness harness;
    REQUIRE(harness.typing.show());
    auto* keyboard = harness.typing.keyboard();
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
    harness.typing.drain_midi_into(recovery, 64);
    CHECK(recovery.empty());
}

TEST_CASE("vellum typing panel names itself and toggles on the shortcut contract",
          "[standalone-typing-panel]") {
    Harness harness;
    CHECK(harness.typing.menu_title() == "Musical Typing Keyboard");
    CHECK(harness.typing.shortcut() == 'k');
    CHECK_FALSE(harness.typing.is_visible());

    int primary_window_token = 0;
    harness.typing.set_primary_window(&primary_window_token);
    REQUIRE(harness.typing.toggle());
    REQUIRE(harness.typing.is_visible());
    CHECK(harness.created_options.secondary_window);
    CHECK(harness.created_options.use_gpu);
    CHECK(harness.window->positioned_beside == &primary_window_token);
    CHECK(harness.window->app_key_monitor);

    REQUIRE(harness.typing.toggle());
    CHECK_FALSE(harness.typing.is_visible());
    CHECK_FALSE(harness.window->app_key_monitor);

    // A declined window leaves the panel hidden and reports it.
    Harness cpu_only;
    cpu_only.host_gpu_backed = false;
    CHECK_FALSE(cpu_only.typing.toggle());
}

TEST_CASE("vellum typing panel closes on Escape and the main-modifier chords",
          "[standalone-typing-panel]") {
    Harness harness;
    const auto main_mod = vv::is_main_modifier(vv::kModCmd) ? vv::kModCmd : vv::kModCtrl;
    for (const auto& chord :
         {harness.key(vv::KeyCode::escape), harness.key(vv::KeyCode::k, true, main_mod),
          harness.key(vv::KeyCode::w, true, main_mod)}) {
        REQUIRE(harness.typing.show());
        REQUIRE(harness.typing.keyboard()->on_global_key(chord));
        CHECK_FALSE(harness.typing.is_visible());
    }

    REQUIRE(harness.typing.show());
    CHECK_FALSE(harness.typing.keyboard()->on_global_key(harness.key(vv::KeyCode::k)));
    CHECK_FALSE(harness.typing.keyboard()->on_global_key(
        harness.key(vv::KeyCode::k, true, main_mod, true)));
    CHECK(harness.typing.is_visible());
}

TEST_CASE("vellum typing panel shutdown releases notes once and is repeatable",
          "[standalone-typing-panel]") {
    Harness harness;
    REQUIRE(harness.typing.show());
    REQUIRE(harness.typing.route_app_key(harness.key(vv::KeyCode::a)));
    harness.typing.shutdown();
    REQUIRE(harness.events.size() == 2);
    CHECK(harness.events[1].is_note_off());
    CHECK(harness.typing.keyboard() == nullptr);
    CHECK_FALSE(harness.typing.is_visible());

    harness.typing.shutdown();
    CHECK(harness.events.size() == 2);

    midi::MidiBuffer after;
    harness.typing.drain_midi_into(after, 64);
    CHECK(after.empty());
}

TEST_CASE("the typing panel factory follows the standalone config and installs once",
          "[standalone-typing-panel]") {
    const format::StandaloneConfig disabled;
    format::StandaloneConfig config;
    const format::StandalonePanelContext off{disabled, [](const midi::MidiEvent&) { return true; }};
    CHECK(pulp::vellum_audio::make_standalone_typing_panel(off) == nullptr);

    config.enable_musical_typing_keyboard = true;
    const format::StandalonePanelContext on{config, [](const midi::MidiEvent&) { return true; }};
    auto panel = pulp::vellum_audio::make_standalone_typing_panel(on);
    REQUIRE(panel);
    CHECK(panel->menu_title() == "Musical Typing Keyboard");
    CHECK(panel->shortcut() == 'k');

    format::detail::reset_standalone_panel_factories_for_testing();
    CHECK(pulp::vellum_audio::install_standalone_typing_panel());
    CHECK(pulp::vellum_audio::install_standalone_typing_panel());
    CHECK(format::detail::create_standalone_panels(off).empty());
    CHECK(format::detail::create_standalone_panels(on).size() == 1);
    format::detail::reset_standalone_panel_factories_for_testing();
}

namespace {

struct Rgb {
    int r, g, b;
};

// Render the keyboard and read one pixel. A view that has already painted
// repaints only what changed, so each state is read at the one key it changes.
Rgb pixel_at(vellum_audio::MusicalTypingKeyboard& keyboard, float x, float y) {
    keyboard.set_bounds({0, 0, keyboard.panel_width(), keyboard.panel_height()});
    std::uint32_t w = 0;
    std::uint32_t h = 0;
    const auto px = vv::render_to_rgba(keyboard, 732, 266, 1.0f, &w, &h);
    REQUIRE(w == 732);
    REQUIRE(h == 266);
    const auto o = (static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x)) * 4;
    return {px[o], px[o + 1], px[o + 2]};
}

int note_zero_element(const vellum_audio::MusicalTypingKeyboard& keyboard) {
    for (int i = 0; i < keyboard.element_count(); ++i)
        if (keyboard.element_kind(i) == ::vellum::view::DesignFrameElement::Kind::momentary &&
            keyboard.element_note(i) == 0)
            return i;
    return -1;
}

} // namespace

TEST_CASE("vellum typing panel keyboard lights a pressed key in its own render",
          "[standalone-typing-panel][render]") {
    if (!vv::raw_rgba_render_available())
        SKIP("Vellum CPU raster backend unavailable");
    Harness harness;
    REQUIRE(harness.typing.show());
    auto* keyboard = harness.typing.keyboard();
    REQUIRE(keyboard != nullptr);

    const int key = note_zero_element(*keyboard);
    REQUIRE(key >= 0);
    const auto rect = keyboard->element_rect(key);
    const float x = rect.x + rect.width * 0.5f;
    const float y = rect.y + rect.height - 12.0f;

    // A resting white key is near-white and neutral.
    const auto idle = pixel_at(*keyboard, x, y);
    CHECK(idle.r > 200);
    CHECK(idle.g > 200);
    CHECK(idle.b > 200);

    // Pressed through the panel's key route, it lights teal.
    REQUIRE(harness.typing.route_app_key(harness.key(vv::KeyCode::a)));
    const auto lit = pixel_at(*keyboard, x, y);
    if (const char* dir = std::getenv("PULP_VELLUM_AUDIO_PNG_DIR")) {
        // A fresh panel, pressed before its first paint, for a full-frame review image.
        Harness review;
        REQUIRE(review.typing.show());
        REQUIRE(review.typing.route_app_key(review.key(vv::KeyCode::a)));
        auto* review_keyboard = review.typing.keyboard();
        review_keyboard->set_bounds(
            {0, 0, review_keyboard->panel_width(), review_keyboard->panel_height()});
        CHECK(vv::render_to_file(*review_keyboard, 732, 266,
                                 std::string(dir) + "/typing-panel-key-lit.png", 1.0f,
                                 vv::ScreenshotBackend::skia));
    }
    CHECK(lit.g > 170);
    CHECK(lit.r < 120);

    REQUIRE(harness.typing.route_app_key(harness.key(vv::KeyCode::a, false)));
    const auto released = pixel_at(*keyboard, x, y);
    CHECK(released.r > 200);
    CHECK(released.g > 200);
}

TEST_CASE("vellum typing panel leaves keys to a focused text field", "[standalone-typing-panel]") {
    Harness harness;
    REQUIRE(harness.typing.show());

    harness.text_focused = true;
    CHECK_FALSE(harness.typing.route_app_key(harness.key(vv::KeyCode::a)));
    CHECK(harness.events.empty());

    harness.text_focused = false;
    CHECK(harness.typing.route_app_key(harness.key(vv::KeyCode::a)));
    REQUIRE(harness.events.size() == 1);
    CHECK(harness.events[0].is_note_on());
}
