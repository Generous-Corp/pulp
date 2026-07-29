#include "delay_params.hpp"
#include "pulp_delay_controls.hpp"
#include "pulp_delay_editor.hpp"
#include "pulp_delay.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/format/headless.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/screenshot_compare.hpp>

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

PulpDelayEditor& as_delay_editor(std::unique_ptr<view::View>& root) {
    auto* editor = dynamic_cast<PulpDelayEditor*>(root.get());
    REQUIRE(editor != nullptr);
    return *editor;
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
    store.pump_listeners();
    editor.sync_from_store();

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
