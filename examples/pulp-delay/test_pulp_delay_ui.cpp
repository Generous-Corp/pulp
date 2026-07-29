#include "delay_params.hpp"
#include "pulp_delay_controls.hpp"
#include "pulp_delay_editor.hpp"
#include "pulp_delay.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/format/editor_idle_pump.hpp>
#include <pulp/format/headless.hpp>
#include <pulp/format/plugin_state_io.hpp>
#include <pulp/view/window_host.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/screenshot_compare.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using Catch::Matchers::WithinAbs;
using namespace pulp;
using namespace pulp::examples::delay;
using namespace pulp::examples::delay::ui;

namespace {

class CountingWindowHost final : public view::WindowHost {
  public:
    void show() override {}
    void hide() override {}
    bool is_visible() const override { return true; }
    void repaint() override { ++repaint_count; }
    void set_close_callback(std::function<void()>) override {}
    void run_event_loop() override {}

    int repaint_count = 0;
};

PulpDelayEditor& as_delay_editor(std::unique_ptr<view::View>& root) {
    auto* editor = dynamic_cast<PulpDelayEditor*>(root.get());
    REQUIRE(editor != nullptr);
    return *editor;
}

view::View& control_view(PulpDelayEditor& editor, state::ParamID id) {
    auto* control = editor.control_for(id);
    REQUIRE(control != nullptr);
    auto* result = dynamic_cast<view::View*>(control);
    REQUIRE(result != nullptr);
    return *result;
}

view::Point root_center(const view::View& view) {
    float x = view.bounds().x + view.bounds().width * 0.5f;
    float y = view.bounds().y + view.bounds().height * 0.5f;
    for (auto* parent = view.parent(); parent && parent->parent();
         parent = parent->parent()) {
        x += parent->bounds().x;
        y += parent->bounds().y;
    }
    return {x, y};
}

std::uint64_t bytes_hash(const std::vector<std::uint8_t>& bytes) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::size_t count_paint_colour(const canvas::RecordingCanvas& canvas,
                               canvas::Color colour) {
    return static_cast<std::size_t>(std::count_if(
        canvas.commands().begin(), canvas.commands().end(),
        [argb = colour.to_argb32()](const canvas::DrawCommand& command) {
            return (command.type == canvas::DrawCommand::Type::set_fill_color
                    || command.type == canvas::DrawCommand::Type::set_stroke_color)
                && command.color.to_argb32() == argb;
        }));
}

view::Point root_origin(const view::View& view) {
    float x = view.bounds().x;
    float y = view.bounds().y;
    for (auto* parent = view.parent(); parent; parent = parent->parent()) {
        x += parent->bounds().x;
        y += parent->bounds().y;
    }
    return {x, y};
}

} // namespace

TEST_CASE("Pulp Delay native editor binds every stable parameter",
          "[pulp-delay][ui][bindings]") {
    format::HeadlessHost host(create_pulp_delay);
    REQUIRE(host.processor() != nullptr);
    auto root = host.processor()->create_view();
    auto& store = host.state();
    auto& editor = as_delay_editor(root);

    REQUIRE(editor.bounds().width == 1120.0f);
    REQUIRE(editor.bounds().height == 740.0f);
    REQUIRE(editor.bound_parameter_count() == kParameterCount);
    for (state::ParamID id = kTime; id <= kReverse; ++id) {
        INFO("parameter id " << id);
        auto* control = editor.control_for(id);
        REQUIRE(control != nullptr);
        REQUIRE_THAT(control->normalized_value(),
                     WithinAbs(store.get_normalized(id), 1.0e-6));
    }

    for (const auto id : {kFeedback, kMix, kCharacterAmount, kDiffusion}) {
        auto* knob = dynamic_cast<DelayKnob*>(editor.control_for(id));
        REQUIRE(knob != nullptr);
        REQUIRE(knob->render_style() == view::WidgetRenderStyle::standard);
    }
}

TEST_CASE("Pulp Delay UI labels and control-state bars have truthful provenance",
          "[pulp-delay][ui][truth]") {
    state::StateStore store;
    define_delay_parameters(store);
    auto root = build_pulp_delay_editor(store);
    auto& editor = as_delay_editor(root);

    std::string header;
    for (const auto label : PulpDelayEditor::truthful_header_labels()) {
        header.append(label);
        header.push_back(' ');
    }
    for (const auto forbidden : {"kHz", "DSP", "ONLINE", "PRESET", "DEFAULT"})
        REQUIRE(header.find(forbidden) == std::string::npos);

    store.set_normalized(kMix, 0.27f);
    store.set_normalized(kFeedback, 0.81f);
    REQUIRE_THAT(editor.control_state_level(kMix),
                 WithinAbs(store.get_normalized(kMix), 1.0e-6));
    REQUIRE_THAT(editor.control_state_level(kFeedback),
                 WithinAbs(store.get_normalized(kFeedback), 1.0e-6));
    REQUIRE(editor.control_state_level(kTime) == 0.0f);
}

TEST_CASE("Pulp Delay character palette matches the four authored HTML accents",
          "[pulp-delay][ui][palette][mapping]") {
    constexpr std::array expected{
        canvas::Color::rgba8(0x16, 0xDA, 0xC2),
        canvas::Color::rgba8(0xA9, 0x7B, 0xFF),
        canvas::Color::rgba8(0xB8, 0xE6, 0x35),
        canvas::Color::rgba8(0xFF, 0x4F, 0x4F),
    };
    for (std::size_t index = 0; index < expected.size(); ++index) {
        const auto actual =
            CharacterPalette::accent_for(static_cast<Character>(index));
        REQUIRE(actual.to_argb32() == expected[index].to_argb32());
    }
    REQUIRE(CharacterPalette::accent_for(Character::tape).r8() == 0xB8);
    REQUIRE(CharacterPalette::accent_for(Character::tape).g8() == 0xE6);
    REQUIRE(CharacterPalette::accent_for(Character::tape).b8() == 0x35);
}

TEST_CASE("Pulp Delay choices support keyboard selection with host gestures",
          "[pulp-delay][ui][keyboard][accessibility]") {
    state::StateStore store;
    define_delay_parameters(store);
    store.set_value(kCharacter, static_cast<float>(Character::vintage));
    auto root = build_pulp_delay_editor(store);
    auto& editor = as_delay_editor(root);
    auto* choice = dynamic_cast<DelayChoice*>(editor.control_for(kCharacter));
    REQUIRE(choice != nullptr);

    std::vector<std::pair<std::string, state::ParamID>> gestures;
    store.set_gesture_callbacks(
        [&](state::ParamID id) { gestures.emplace_back("begin", id); },
        [&](state::ParamID id) { gestures.emplace_back("end", id); });
    std::vector<float> values;
    const auto value_listener = store.add_listener(
        [&](state::ParamID id, float value) {
            if (id == kCharacter)
                values.push_back(value);
        },
        state::ListenerThread::Main);

    const auto press = [](view::KeyCode key) {
        view::KeyEvent event;
        event.key = key;
        return event;
    };
    REQUIRE(choice->on_key_event(press(view::KeyCode::right)));
    REQUIRE(choice->selected_index() == static_cast<int>(Character::tape));
    REQUIRE(choice->access_value() == "TAPE");
    REQUIRE(choice->on_key_event(press(view::KeyCode::up)));
    REQUIRE(choice->selected_index() == static_cast<int>(Character::vintage));
    REQUIRE(choice->on_key_event(press(view::KeyCode::home)));
    REQUIRE(choice->selected_index() == static_cast<int>(Character::clean));
    REQUIRE_FALSE(choice->on_key_event(press(view::KeyCode::left)));
    REQUIRE(choice->on_key_event(press(view::KeyCode::end_)));
    REQUIRE(choice->selected_index() == static_cast<int>(Character::bbd));
    REQUIRE(choice->access_value() == "BBD");

    REQUIRE_FALSE(choice->on_key_event(press(view::KeyCode::enter)));
    REQUIRE_FALSE(choice->on_key_event(press(view::KeyCode::space)));
    auto release = press(view::KeyCode::left);
    release.is_down = false;
    REQUIRE_FALSE(choice->on_key_event(release));

    REQUIRE(values == std::vector<float>{2.0f, 1.0f, 0.0f, 3.0f});
    REQUIRE(gestures == std::vector<std::pair<std::string, state::ParamID>>{
                            {"begin", kCharacter},
                            {"end", kCharacter},
                            {"begin", kCharacter},
                            {"end", kCharacter},
                            {"begin", kCharacter},
                            {"end", kCharacter},
                            {"begin", kCharacter},
                            {"end", kCharacter}});
    REQUIRE(store.open_gesture_count() == 0);
}

TEST_CASE("Pulp Delay character accent propagates globally without recoloring Reverse",
          "[pulp-delay][ui][palette][propagation]") {
    constexpr std::array characters{
        Character::clean, Character::vintage, Character::tape, Character::bbd};
    constexpr std::array labels{"CLEAN", "VINT", "TAPE", "BBD"};

    state::StateStore store;
    define_delay_parameters(store);
    auto root = build_pulp_delay_editor(store);
    auto& editor = as_delay_editor(root);
    CountingWindowHost host;
    editor.set_window_host(&host);
    canvas::RecordingCanvas canvas;

    for (std::size_t index = 0; index < characters.size(); ++index) {
        CAPTURE(index);
        host.repaint_count = 0;
        store.set_value(kCharacter, static_cast<float>(characters[index]));
        REQUIRE(host.repaint_count == 1);
        canvas.clear();
        editor.paint_all(canvas);
        REQUIRE(count_paint_colour(
                    canvas, CharacterPalette::accent_for(characters[index]))
                >= 20);
        REQUIRE(count_paint_colour(canvas, color::warning) >= 1);
        REQUIRE(control_view(editor, kCharacter).access_value() == labels[index]);
        REQUIRE(host.repaint_count == 1);
    }
}

TEST_CASE("Pulp Delay Stereo Field exposes only the active timing branch",
          "[pulp-delay][ui][timing][branches]") {
    struct Branch {
        bool sync;
        bool link;
        OffsetMode offset_mode;
        std::array<state::ParamID, 3> active;
        std::size_t active_count;
    };
    constexpr std::array branches{
        Branch{false, true, OffsetMode::ratio, {kOffsetMode, kTimeOffset, 0}, 2},
        Branch{false, true, OffsetMode::milliseconds, {kOffsetMode, kOffsetMs, 0}, 2},
        Branch{true, true, OffsetMode::ratio, {kDivision, kOffsetMode, kTimeOffset}, 3},
        Branch{true, true, OffsetMode::milliseconds,
               {kDivision, kOffsetMode, kOffsetMs}, 3},
        Branch{false, false, OffsetMode::ratio, {kTimeRight, 0, 0}, 1},
        Branch{true, false, OffsetMode::ratio, {kDivision, kDivisionRight, 0}, 2},
    };
    constexpr std::array conditional{
        kDivision, kTimeRight, kOffsetMode, kDivisionRight, kTimeOffset, kOffsetMs};

    state::StateStore store;
    define_delay_parameters(store);
    auto root = build_pulp_delay_editor(store);
    auto& editor = as_delay_editor(root);
    for (const auto& branch : branches) {
        CAPTURE(branch.sync, branch.link, branch.offset_mode);
        store.set_value(kRouting, static_cast<float>(Routing::stereo));
        store.set_value(kSync, branch.sync ? 1.0f : 0.0f);
        store.set_value(kLink, branch.link ? 1.0f : 0.0f);
        store.set_value(kOffsetMode, static_cast<float>(branch.offset_mode));
        for (const auto id : conditional) {
            const bool expected =
                std::find(branch.active.begin(),
                          branch.active.begin()
                              + static_cast<std::ptrdiff_t>(branch.active_count),
                          id)
                != branch.active.begin()
                    + static_cast<std::ptrdiff_t>(branch.active_count);
            REQUIRE(control_view(editor, id).visible() == expected);
        }
        REQUIRE(control_view(editor, kTime).enabled() == !branch.sync);
        REQUIRE(control_view(editor, kLink).visible());
        REQUIRE(control_view(editor, kCrossfeed).visible());
        REQUIRE(editor.effective_right_time_visible() == branch.link);
        if (branch.link && !branch.sync) {
            const auto expected = branch.offset_mode == OffsetMode::ratio
                ? "RIGHT EFFECTIVE · 426 ms"
                : "RIGHT EFFECTIVE · 394 ms";
            REQUIRE(editor.effective_right_time_text() == expected);
            REQUIRE(editor.effective_right_time_text().find("620") == std::string::npos);
        } else if (branch.link) {
            REQUIRE(editor.effective_right_time_text().find("HOST SYNC")
                    != std::string::npos);
        }
        if (branch.sync)
            REQUIRE(editor.left_time_display_text().find("SYNC 1/8") == 0);
        else
            REQUIRE(editor.left_time_display_text() == "380 ms");
    }
}

TEST_CASE("Pulp Delay Ping Pong suppresses overridden right and crossfeed controls",
          "[pulp-delay][ui][timing][ping-pong]") {
    state::StateStore store;
    define_delay_parameters(store);
    auto root = build_pulp_delay_editor(store);
    auto& editor = as_delay_editor(root);
    store.set_value(kRouting, static_cast<float>(Routing::ping_pong));

    for (const bool sync : {false, true}) {
        CAPTURE(sync);
        store.set_value(kSync, sync ? 1.0f : 0.0f);
        store.set_value(kLink, sync ? 0.0f : 1.0f);
        store.set_value(kOffsetMode, static_cast<float>(
                                         sync ? OffsetMode::milliseconds : OffsetMode::ratio));
        REQUIRE(control_view(editor, kDivision).visible() == sync);
        REQUIRE(control_view(editor, kTime).enabled() == !sync);
        for (const auto id : {kLink, kTimeRight, kOffsetMode, kDivisionRight,
                              kTimeOffset, kOffsetMs, kCrossfeed})
            REQUIRE_FALSE(control_view(editor, id).visible());
        REQUIRE(editor.effective_right_time_visible());
        REQUIRE(editor.crossfeed_override_visible());
        REQUIRE(editor.crossfeed_override_text() == "100% · PING PONG");
        REQUIRE(editor.effective_right_time_text()
                == (sync ? "RIGHT = LEFT · HOST SYNC"
                         : "RIGHT = LEFT · 380 ms"));
        REQUIRE_THAT(store.get_value(kCrossfeed), WithinAbs(48.0, 1.0e-6));
    }
}

TEST_CASE("Pulp Delay timing presentation follows live state transitions",
          "[pulp-delay][ui][timing][transitions]") {
    state::StateStore store;
    define_delay_parameters(store);
    auto root = build_pulp_delay_editor(store);
    auto& editor = as_delay_editor(root);

    REQUIRE_FALSE(control_view(editor, kDivision).visible());
    REQUIRE(control_view(editor, kTimeOffset).visible());
    REQUIRE(editor.effective_right_time_text() == "RIGHT EFFECTIVE · 426 ms");

    store.set_value(kOffsetMode, static_cast<float>(OffsetMode::milliseconds));
    REQUIRE_FALSE(control_view(editor, kTimeOffset).visible());
    REQUIRE(control_view(editor, kOffsetMs).visible());
    REQUIRE(editor.effective_right_time_text() == "RIGHT EFFECTIVE · 394 ms");

    store.set_value(kLink, 0.0f);
    REQUIRE_FALSE(editor.effective_right_time_visible());
    REQUIRE(control_view(editor, kTimeRight).visible());
    REQUIRE_FALSE(control_view(editor, kOffsetMode).visible());

    store.set_value(kSync, 1.0f);
    REQUIRE_FALSE(control_view(editor, kTime).enabled());
    REQUIRE(control_view(editor, kDivision).visible());
    REQUIRE_FALSE(control_view(editor, kTimeRight).visible());
    REQUIRE(control_view(editor, kDivisionRight).visible());
    REQUIRE(editor.left_time_display_text() == "SYNC 1/8");

    store.set_value(kRouting, static_cast<float>(Routing::ping_pong));
    REQUIRE_FALSE(control_view(editor, kLink).visible());
    REQUIRE_FALSE(control_view(editor, kDivisionRight).visible());
    REQUIRE_FALSE(control_view(editor, kCrossfeed).visible());
    REQUIRE(editor.crossfeed_override_visible());
    REQUIRE(editor.crossfeed_override_text() == "100% · PING PONG");
    REQUIRE(editor.effective_right_time_text() == "RIGHT = LEFT · HOST SYNC");
}

TEST_CASE("Pulp Delay static editor becomes idle and external state repaints once",
          "[pulp-delay][ui][repaint][quiescence]") {
    state::StateStore store;
    define_delay_parameters(store);
    auto root = build_pulp_delay_editor(store);
    auto& editor = as_delay_editor(root);
    CountingWindowHost host;
    editor.set_window_host(&host);
    canvas::RecordingCanvas canvas;

    editor.paint_all(canvas);
    REQUIRE(host.repaint_count == 0);
    editor.paint_all(canvas);
    REQUIRE(host.repaint_count == 0);

    store.set_value(kRouting, static_cast<float>(Routing::ping_pong));
    REQUIRE(host.repaint_count == 1);
    REQUIRE_FALSE(control_view(editor, kCrossfeed).visible());
    REQUIRE(editor.crossfeed_override_visible());
    REQUIRE(editor.crossfeed_override_text() == "100% · PING PONG");

    host.repaint_count = 0;
    editor.paint_all(canvas);
    REQUIRE(host.repaint_count == 0);
}

TEST_CASE("Pulp Delay idle pump reconciles listener-silent preset restore",
          "[pulp-delay][ui][repaint][restore]") {
    format::HeadlessHost plugin_host(create_pulp_delay);
    auto* processor = plugin_host.processor();
    REQUIRE(processor != nullptr);
    auto& store = plugin_host.state();
    format::ViewBridge bridge(*processor, store);
    REQUIRE(bridge.open());
    bridge.notify_attached();
    auto* editor = dynamic_cast<PulpDelayEditor*>(bridge.view());
    REQUIRE(editor != nullptr);
    auto* feedback = dynamic_cast<DelayKnob*>(editor->control_for(kFeedback));
    auto* freeze = dynamic_cast<DelayActionCard*>(editor->control_for(kFreeze));
    REQUIRE(feedback != nullptr);
    REQUIRE(freeze != nullptr);
    CountingWindowHost host;
    editor->set_window_host(&host);
    canvas::RecordingCanvas canvas;

    store.set_normalized(kFeedback, 0.75f);
    store.set_value(kRouting, static_cast<float>(Routing::ping_pong));
    store.set_value(kCharacter, static_cast<float>(Character::bbd));
    store.set_value(kFreeze, 1.0f);
    const auto preset =
        format::plugin_state_io::serialize(store, *processor);

    store.set_normalized(kFeedback, 0.1f);
    store.set_value(kRouting, static_cast<float>(Routing::stereo));
    store.set_value(kCharacter, static_cast<float>(Character::clean));
    store.set_value(kFreeze, 0.0f);
    REQUIRE_THAT(feedback->value(), WithinAbs(0.1, 1.0e-6));
    REQUIRE(control_view(*editor, kCharacter).access_value() == "CLEAN");
    REQUIRE_FALSE(freeze->is_on());
    REQUIRE(control_view(*editor, kCrossfeed).visible());
    REQUIRE_FALSE(editor->crossfeed_override_visible());

    host.repaint_count = 0;
    REQUIRE(format::plugin_state_io::deserialize(
        preset, store, *processor));
    REQUIRE(host.repaint_count == 0);
    REQUIRE_THAT(feedback->value(), WithinAbs(0.1, 1.0e-6));
    REQUIRE(control_view(*editor, kCharacter).access_value() == "CLEAN");
    REQUIRE_FALSE(freeze->is_on());
    REQUIRE(control_view(*editor, kCrossfeed).visible());
    REQUIRE_FALSE(editor->crossfeed_override_visible());

    int store_callbacks = 0;
    const auto callback_probe = store.add_listener(
        [&store_callbacks](state::ParamID, float) { ++store_callbacks; },
        state::ListenerThread::Main);
    int gesture_begins = 0;
    int gesture_ends = 0;
    store.set_gesture_callbacks(
        [&gesture_begins](state::ParamID) { ++gesture_begins; },
        [&gesture_ends](state::ParamID) { ++gesture_ends; });

    auto pump = format::make_editor_idle_pump(bridge);
    pump();
    REQUIRE_THAT(feedback->value(), WithinAbs(0.75, 1.0e-6));
    REQUIRE(control_view(*editor, kCharacter).access_value() == "BBD");
    REQUIRE(freeze->is_on());
    REQUIRE_FALSE(control_view(*editor, kCrossfeed).visible());
    REQUIRE(editor->crossfeed_override_visible());
    REQUIRE(editor->crossfeed_override_text() == "100% · PING PONG");
    REQUIRE(host.repaint_count > 0);
    REQUIRE(store_callbacks == static_cast<int>(kParameterCount));
    REQUIRE(gesture_begins == 0);
    REQUIRE(gesture_ends == 0);
    REQUIRE(store.open_gesture_count() == 0);
    REQUIRE(store.serialize() == preset);

    const int restore_repaint_count = host.repaint_count;
    pump();
    REQUIRE(host.repaint_count == restore_repaint_count);
    REQUIRE(store_callbacks == static_cast<int>(kParameterCount));

    host.repaint_count = 0;
    canvas.clear();
    editor->paint_all(canvas);
    REQUIRE(host.repaint_count == 0);
    REQUIRE(store_callbacks == static_cast<int>(kParameterCount));
    REQUIRE(gesture_begins == 0);
    REQUIRE(gesture_ends == 0);
    REQUIRE(store.open_gesture_count() == 0);
    REQUIRE(store.serialize() == preset);
}

TEST_CASE("Pulp Delay controls follow host automation and preset restore",
          "[pulp-delay][ui][automation][state]") {
    state::StateStore store;
    define_delay_parameters(store);
    auto root = build_pulp_delay_editor(store);
    auto& editor = as_delay_editor(root);
    auto* feedback = dynamic_cast<DelayKnob*>(editor.control_for(kFeedback));
    REQUIRE(feedback != nullptr);

    const float original = feedback->value();
    store.set_normalized_rt(kFeedback, 0.75f);
    REQUIRE_THAT(feedback->value(), WithinAbs(original, 1.0e-6));
    REQUIRE(store.pump_listeners() >= 1);
    REQUIRE_THAT(feedback->value(), WithinAbs(0.75, 1.0e-6));
    REQUIRE_THAT(feedback->pointer_angle(),
                 WithinAbs(view::Knob::start_angle
                           + 0.75 * (view::Knob::end_angle
                                     - view::Knob::start_angle),
                           1.0e-6));

    std::array<float, kParameterCount> expected{};
    for (state::ParamID id = kTime; id <= kReverse; ++id) {
        const float requested = static_cast<float>(id - kTime)
            / static_cast<float>(kParameterCount - 1);
        store.set_normalized(id, requested);
        expected[static_cast<std::size_t>(id - kTime)] =
            store.get_normalized(id);
    }
    const auto preset = store.serialize();
    store.reset_all_to_defaults();
    REQUIRE(store.deserialize(preset));
    REQUIRE(store.reconcile_main_listeners() > 0);

    for (state::ParamID id = kTime; id <= kReverse; ++id) {
        INFO("parameter id " << id);
        REQUIRE_THAT(
            editor.control_for(id)->normalized_value(),
            WithinAbs(expected[static_cast<std::size_t>(id - kTime)],
                      1.0e-6));
    }
}

TEST_CASE("Pulp Delay custom knob drag is gesture-bracketed and audible-state live",
          "[pulp-delay][ui][drag]") {
    state::StateStore store;
    define_delay_parameters(store);
    std::vector<std::pair<std::string, state::ParamID>> gestures;
    store.set_gesture_callbacks(
        [&](state::ParamID id) { gestures.emplace_back("begin", id); },
        [&](state::ParamID id) { gestures.emplace_back("end", id); });
    auto root = build_pulp_delay_editor(store);
    auto& editor = as_delay_editor(root);
    auto* knob = dynamic_cast<DelayKnob*>(editor.control_for(kFeedback));
    REQUIRE(knob != nullptr);

    store.set_normalized(kFeedback, 0.25f);
    const auto center = root_center(*knob);
    editor.simulate_drag(
        {center.x, center.y + 24.0f},
        {center.x, center.y - 36.0f}, 8);

    REQUIRE(store.get_normalized(kFeedback) > 0.25f);
    REQUIRE_THAT(knob->value(),
                 WithinAbs(store.get_normalized(kFeedback), 1.0e-6));
    REQUIRE(gestures == std::vector<std::pair<std::string, state::ParamID>>{
                            {"begin", kFeedback},
                            {"end", kFeedback}});
    REQUIRE(store.open_gesture_count() == 0);
}

TEST_CASE("Pulp Delay feedback knob paints five synchronized normalized states",
          "[pulp-delay][ui][screenshot]") {
    state::StateStore store;
    define_delay_parameters(store);
    auto root = build_pulp_delay_editor(store);
    auto& editor = as_delay_editor(root);
    auto* knob = dynamic_cast<DelayKnob*>(editor.control_for(kFeedback));
    REQUIRE(knob != nullptr);

    constexpr std::array<float, 5> normalized = {
        0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
    std::array<std::uint64_t, normalized.size()> hashes{};
    std::array<std::vector<std::uint8_t>, normalized.size()> knob_crops;
    const auto knob_origin = root_origin(*knob);
    for (std::size_t index = 0; index < normalized.size(); ++index) {
        const float value = normalized[index];
        CAPTURE(value);
        store.set_normalized(kFeedback, value);
        REQUIRE_THAT(knob->value(), WithinAbs(value, 1.0e-6));
        REQUIRE_THAT(knob->pointer_angle(),
                     WithinAbs(view::Knob::start_angle
                               + value * (view::Knob::end_angle
                                          - view::Knob::start_angle),
                               1.0e-6));

        auto capture = view::capture_view(
            editor, 1120, 740, 1.0f, view::ScreenshotBackend::skia);
        INFO(capture.reason);
        REQUIRE(capture.ok);
        REQUIRE(capture.png.size() > 4096);
        hashes[index] = bytes_hash(capture.png);
        knob_crops[index] = view::crop_png(
            capture.png, static_cast<std::uint32_t>(std::lround(knob_origin.x)),
            static_cast<std::uint32_t>(std::lround(knob_origin.y)),
            static_cast<std::uint32_t>(std::lround(knob->bounds().width)),
            static_cast<std::uint32_t>(std::lround(knob->bounds().height)));
        REQUIRE_FALSE(knob_crops[index].empty());
    }
    for (std::size_t index = 1; index < hashes.size(); ++index) {
        REQUIRE(hashes[index] != hashes[index - 1]);
        const auto diff = view::diff_bounds(
            knob_crops[index - 1], knob_crops[index], 16);
        REQUIRE(diff.valid);
        const auto dial = knob->dial_geometry();
        const float angle = view::Knob::start_angle
            + normalized[index] * (view::Knob::end_angle - view::Knob::start_angle);
        const float endpoint_x = dial.center_x + std::cos(angle) * dial.arc_radius;
        const float endpoint_y = dial.center_y + std::sin(angle) * dial.arc_radius;
        constexpr float kAntialiasMargin = 4.0f;
        CAPTURE(index, endpoint_x, endpoint_y, diff.x, diff.y, diff.width, diff.height);
        REQUIRE(endpoint_x >= static_cast<float>(diff.x) - kAntialiasMargin);
        REQUIRE(endpoint_x <= static_cast<float>(diff.x + diff.width) + kAntialiasMargin);
        REQUIRE(endpoint_y >= static_cast<float>(diff.y) - kAntialiasMargin);
        REQUIRE(endpoint_y <= static_cast<float>(diff.y + diff.height) + kAntialiasMargin);
    }

    store.set_normalized(kFeedback, 0.5f);
    knob->on_focus_changed(true);
    auto focused = view::capture_view(
        editor, 1120, 740, 1.0f, view::ScreenshotBackend::skia);
    INFO(focused.reason);
    REQUIRE(focused.ok);
    REQUIRE(bytes_hash(focused.png) != hashes[2]);
    knob->on_focus_changed(false);
}
