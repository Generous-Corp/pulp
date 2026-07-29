#include "pulp_delay_editor.hpp"

#include "delay_time_model.hpp"
#include "pulp_delay_paint_helpers.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

namespace pulp::examples::delay::ui {

namespace {

using paint_detail::text;
using paint_detail::with_alpha;

class HeaderView final : public view::View {
  public:
    explicit HeaderView(const CharacterPalette& palette) : palette_(&palette) {}

    void layout_children() override {}

    void paint(canvas::Canvas& c) override {
        const auto b = local_bounds();
        c.set_fill_color(color::header);
        c.fill_rect(0, 0, b.width, b.height);
        c.set_fill_color(palette_->accent());
        c.fill_rect(0, b.height - 2.0f, b.width, 2.0f);

        text(c, "PULP", 20.0f, 31.0f, 22.0f, palette_->accent(),
             canvas::TextAlign::left, 800, 2.4f);
        text(c, "DELAY", 20.0f, 49.0f, 10.0f, color::ink,
             canvas::TextAlign::left, 650, 4.2f);

        c.set_fill_color(color::panel);
        c.fill_rounded_rect(202.0f, 12.0f, 306.0f, 40.0f, 5.0f);
        c.set_stroke_color(color::border);
        c.set_line_width(1.0f);
        c.stroke_rounded_rect(202.5f, 12.5f, 305.0f, 39.0f, 5.0f);
        const auto labels = PulpDelayEditor::truthful_header_labels();
        text(c, std::string(labels[0]), 218.0f, 31.0f, 9.0f, palette_->accent(),
             canvas::TextAlign::left, 600, 0.65f);
        text(c, std::string(labels[1]), 218.0f, 44.0f, 7.5f, color::muted,
             canvas::TextAlign::left, 550, 0.5f);
        text(c, std::string(labels[2]), b.width - 258.0f, 35.0f, 8.5f, color::muted,
             canvas::TextAlign::left, 550, 0.35f);
        text(c, std::string(labels[3]), b.width - 166.0f, 35.0f, 8.5f, color::ink,
             canvas::TextAlign::left, 650, 0.8f);
        text(c, std::string(labels[4]), b.width - 58.0f, 35.0f, 8.5f,
             palette_->accent(),
             canvas::TextAlign::right, 650, 0.8f);
    }

  private:
    const CharacterPalette* palette_ = nullptr;
};

class ToneResponseView final : public view::View {
  public:
    ToneResponseView(state::StateStore& store, const CharacterPalette& palette)
        : store_(&store), palette_(&palette) {
        set_hit_testable(false);
        listeners_.push_back(store.add_listener(
            [this](state::ParamID changed, float) {
                if (changed == kLowCut || changed == kHighCut)
                    request_repaint();
            },
            state::ListenerThread::Main));
    }

    void layout_children() override {}

    void paint(canvas::Canvas& c) override {
        const auto b = local_bounds();
        const float low = store_->get_normalized(kLowCut);
        const float high = store_->get_normalized(kHighCut);
        c.set_fill_color(color::panel_deep);
        c.fill_rounded_rect(0, 0, b.width, b.height, 5.0f);
        c.set_stroke_color(color::border);
        c.set_line_width(1.0f);
        c.stroke_rounded_rect(0.5f, 0.5f, b.width - 1.0f,
                              b.height - 1.0f, 5.0f);
        for (int i = 1; i < 8; ++i) {
            const float x = b.width * static_cast<float>(i) / 8.0f;
            c.set_stroke_color(with_alpha(color::border, 0.58f));
            c.stroke_line(x, 7.0f, x, b.height - 7.0f);
        }
        const float low_x = 12.0f + low * b.width * 0.36f;
        const float high_x = b.width * 0.54f + high * b.width * 0.42f;
        const float pass_left = std::min(low_x, high_x - 6.0f);
        const float pass_right = std::max(high_x, pass_left + 6.0f);
        c.set_fill_color(with_alpha(palette_->dim(), 0.60f));
        c.fill_rounded_rect(pass_left, 12.0f, pass_right - pass_left,
                            b.height - 24.0f, 3.0f);
        c.set_stroke_color(palette_->accent());
        c.set_line_width(2.0f);
        c.stroke_line(low_x, b.height - 10.0f, low_x, 10.0f);
        c.stroke_line(high_x, 10.0f, high_x, b.height - 10.0f);
        c.stroke_line(low_x, 10.0f, high_x, 10.0f);
        text(c, "PASSBAND", b.width * 0.5f, b.height - 7.0f, 7.5f,
             color::muted, canvas::TextAlign::center, 650, 0.8f);
    }

  private:
    state::StateStore* store_ = nullptr;
    const CharacterPalette* palette_ = nullptr;
    std::vector<state::ListenerToken> listeners_;
};

class ControlStateView final : public view::View {
  public:
    ControlStateView(state::StateStore& store, const CharacterPalette& palette)
        : store_(&store), palette_(&palette) {
        set_hit_testable(false);
        listeners_.push_back(store.add_listener(
            [this](state::ParamID changed, float) {
                if (changed == kMix || changed == kFeedback)
                    request_repaint();
            },
            state::ListenerThread::Main));
    }

    static float level(const state::StateStore& store, state::ParamID id) noexcept {
        return id == kMix || id == kFeedback ? store.get_normalized(id) : 0.0f;
    }

    void layout_children() override {}

    void paint(canvas::Canvas& c) override {
        const auto b = local_bounds();
        c.set_fill_color(color::panel);
        c.fill_rounded_rect(0, 0, b.width, b.height, metric::panel_radius);
        c.set_stroke_color(color::border);
        c.set_line_width(1.0f);
        c.stroke_rounded_rect(0.5f, 0.5f, b.width - 1.0f,
                              b.height - 1.0f, metric::panel_radius);
        text(c, "CONTROL STATE", 16.0f, 20.0f, 8.5f, color::muted,
             canvas::TextAlign::left, 650, 0.9f);
        text(c, "MIX", 16.0f, 40.0f, 8.0f, color::ink);
        text(c, "FB", 16.0f, 57.0f, 8.0f, color::ink);

        const float mix = level(*store_, kMix);
        const float feedback = level(*store_, kFeedback);
        const std::array levels{mix, feedback};
        const float meter_x = 34.0f;
        const float meter_w = b.width - meter_x - 16.0f;
        for (int channel = 0; channel < 2; ++channel) {
            const float y = 34.0f + static_cast<float>(channel) * 17.0f;
            const float level = levels[static_cast<std::size_t>(channel)];
            c.set_fill_color(color::track);
            c.fill_rounded_rect(meter_x, y, meter_w, 7.0f, 3.5f);
            c.set_fill_color(palette_->accent());
            c.fill_rounded_rect(meter_x, y, meter_w * level, 7.0f, 3.5f);
            for (int tick = 1; tick < 12; ++tick) {
                const float x = meter_x + meter_w * static_cast<float>(tick) / 12.0f;
                c.set_stroke_color(color::panel);
                c.set_line_width(1.0f);
                c.stroke_line(x, y, x, y + 7.0f);
            }
        }
    }

  private:
    state::StateStore* store_ = nullptr;
    const CharacterPalette* palette_ = nullptr;
    std::vector<state::ListenerToken> listeners_;
};

class CrossfeedOverrideView final : public view::View {
  public:
    static constexpr std::string_view display_text = "100% · PING PONG";

    explicit CrossfeedOverrideView(const CharacterPalette& palette)
        : palette_(&palette) {
        set_hit_testable(false);
        set_access_role(view::View::AccessRole::label);
        set_access_label("Crossfeed overridden by Ping Pong");
        set_access_value("100 percent");
    }

    void layout_children() override {}

    void paint(canvas::Canvas& c) override {
        const auto b = local_bounds();
        text(c, "CROSSFEED", 2.0f, 11.0f, 8.5f, color::muted,
             canvas::TextAlign::left, 600, 0.65f);
        text(c, std::string(display_text), b.width - 2.0f, 11.0f, 9.5f,
             palette_->accent(), canvas::TextAlign::right, 650);
        c.set_stroke_color(palette_->accent());
        c.set_line_width(4.0f);
        c.set_line_cap(canvas::LineCap::round);
        c.stroke_line(2.0f, b.height - 8.0f, b.width - 2.0f, b.height - 8.0f);
        text(c, "ROUTING OVERRIDE", 2.0f, b.height - 15.0f, 7.2f, color::muted,
             canvas::TextAlign::left, 600, 0.5f);
    }

  private:
    const CharacterPalette* palette_ = nullptr;
};

} // namespace

class PulpDelayEditor::Panel final : public view::View {
  public:
    Panel(const CharacterPalette& palette, int index, std::string title)
        : palette_(&palette), index_(index), title_(std::move(title)) {
        set_access_role(view::View::AccessRole::group);
        set_access_label(title_);
    }

    void layout_children() override {}

    void paint(canvas::Canvas& c) override {
        const auto b = local_bounds();
        c.set_fill_color(color::panel);
        c.fill_rounded_rect(0, 0, b.width, b.height, metric::panel_radius);
        c.set_stroke_color(color::border);
        c.set_line_width(1.0f);
        c.stroke_rounded_rect(0.5f, 0.5f, b.width - 1.0f,
                              b.height - 1.0f, metric::panel_radius);
        text(c, std::to_string(index_), 13.0f, 23.0f, 9.0f,
             palette_->accent(), canvas::TextAlign::left, 750, 0.8f);
        text(c, title_, 31.0f, 23.0f, 10.0f, color::ink,
             canvas::TextAlign::left, 700, 1.35f);
        c.set_stroke_color(color::border);
        c.set_line_width(1.0f);
        c.stroke_line(12.0f, 31.0f, b.width - 12.0f, 31.0f);
    }

  private:
    const CharacterPalette* palette_ = nullptr;
    int index_ = 0;
    std::string title_;
};

class PulpDelayEditor::EffectiveRightTimeView final : public view::View {
  public:
    EffectiveRightTimeView(state::StateStore& store,
                           const CharacterPalette& palette)
        : store_(&store), palette_(&palette) {
        set_hit_testable(false);
        set_access_role(view::View::AccessRole::label);
        set_access_label("Effective right delay time");
    }

    std::string display_text() const {
        const auto inputs = delay_time_inputs_from_store(
            *store_, DelayTimeModel::kFallbackTempoBpm);
        if (inputs.routing == Routing::ping_pong) {
            if (inputs.sync)
                return "RIGHT = LEFT · HOST SYNC";
            char buffer[64]{};
            std::snprintf(buffer, sizeof(buffer), "RIGHT = LEFT · %.0f ms",
                          DelayTimeModel::derive(inputs).right_ms);
            return buffer;
        }
        if (inputs.sync) {
            const auto id =
                inputs.offset_mode == OffsetMode::ratio ? kTimeOffset : kOffsetMs;
            const auto operation =
                inputs.offset_mode == OffsetMode::ratio ? " × " : " + ";
            return "RIGHT · HOST SYNC" + std::string(operation)
                + format_parameter_value(*store_, id, store_->get_normalized(id));
        }
        char buffer[64]{};
        std::snprintf(buffer, sizeof(buffer), "RIGHT EFFECTIVE · %.0f ms",
                      DelayTimeModel::derive(inputs).right_ms);
        return buffer;
    }

    void layout_children() override {}

    void paint(canvas::Canvas& c) override {
        const auto b = local_bounds();
        c.set_fill_color(color::panel_deep);
        c.fill_rounded_rect(0, 0, b.width, b.height, metric::control_radius);
        c.set_stroke_color(color::border);
        c.set_line_width(1.0f);
        c.stroke_rounded_rect(0.5f, 0.5f, b.width - 1.0f, b.height - 1.0f,
                              metric::control_radius);
        text(c, display_text(), 8.0f, b.height * 0.63f, 8.3f,
             palette_->accent(),
             canvas::TextAlign::left, 650, 0.15f);
    }

  private:
    state::StateStore* store_ = nullptr;
    const CharacterPalette* palette_ = nullptr;
};

PulpDelayEditor::PulpDelayEditor(state::StateStore& store)
    : store_(&store), palette_(store) {
    set_id("pulp-delay-editor");
    set_access_role(view::View::AccessRole::group);
    set_access_label("Pulp Delay editor");
    set_bounds({0, 0, metric::editor_width, metric::editor_height});
    bindings_.reserve(kParameterCount);
    build();
    presentation_listeners_.push_back(store.add_listener(
        [this](state::ParamID changed, float) {
            if (changed == kSync || changed == kLink || changed == kOffsetMode
                || changed == kRouting || changed == kTime || changed == kTimeOffset
                || changed == kOffsetMs) {
                update_timing_presentation();
            }
        },
        state::ListenerThread::Main));
    update_timing_presentation();
}

DelayParameterControl* PulpDelayEditor::control_for(
    state::ParamID id) const noexcept {
    if (id < kTime || id > kReverse)
        return nullptr;
    return controls_[static_cast<std::size_t>(id - kTime)];
}

std::size_t PulpDelayEditor::bound_parameter_count() const noexcept {
    return static_cast<std::size_t>(std::count_if(
        controls_.begin(), controls_.end(),
        [](const auto* control) { return control != nullptr; }));
}

void PulpDelayEditor::paint(canvas::Canvas& c) {
    const auto b = local_bounds();
    c.set_fill_color(color::background);
    c.fill_rect(0, 0, b.width, b.height);
    c.set_stroke_color(with_alpha(color::border, 0.32f));
    c.set_line_width(1.0f);
    for (float x = 0.0f; x < b.width; x += 28.0f)
        c.stroke_line(x, 65.0f, x, b.height);
    for (float y = 65.0f; y < b.height; y += 28.0f)
        c.stroke_line(0.0f, y, b.width, y);
}

float PulpDelayEditor::control_state_level(state::ParamID id) const noexcept {
    return ControlStateView::level(*store_, id);
}

std::string PulpDelayEditor::effective_right_time_text() const {
    return effective_right_time_ ? effective_right_time_->display_text() : std::string{};
}

bool PulpDelayEditor::effective_right_time_visible() const noexcept {
    return effective_right_time_ && effective_right_time_->visible();
}

bool PulpDelayEditor::crossfeed_override_visible() const noexcept {
    return crossfeed_override_ && crossfeed_override_->visible();
}

std::string PulpDelayEditor::crossfeed_override_text() const {
    return crossfeed_override_visible() ? std::string(CrossfeedOverrideView::display_text)
                                        : std::string{};
}

std::string PulpDelayEditor::left_time_display_text() const {
    return tap_field_ ? tap_field_->timing_display_text() : std::string{};
}

PulpDelayEditor::Panel& PulpDelayEditor::add_panel(
    view::Rect bounds, int index, std::string title) {
    auto panel = std::make_unique<Panel>(palette_, index, std::move(title));
    auto* result = panel.get();
    panel->set_bounds(bounds);
    add_child(std::move(panel));
    return *result;
}

void PulpDelayEditor::register_control(DelayParameterControl& control,
                                       view::View& control_view) {
    const auto id = control.parameter_id();
    if (id >= kTime && id <= kReverse) {
        controls_[static_cast<std::size_t>(id - kTime)] = &control;
        control_views_[static_cast<std::size_t>(id - kTime)] = &control_view;
    }
}

DelayKnob& PulpDelayEditor::add_knob(
    view::View& parent, view::Rect bounds, state::ParamID id,
    std::string caption) {
    auto control =
        std::make_unique<DelayKnob>(*store_, palette_, id, std::move(caption));
    auto* result = control.get();
    control->set_bounds(bounds);
    bindings_.push_back(view::bind_parameter(*result, *store_, id));
    register_control(*result, *result);
    parent.add_child(std::move(control));
    return *result;
}

DelayFader& PulpDelayEditor::add_fader(
    view::View& parent, view::Rect bounds, state::ParamID id,
    std::string caption) {
    auto control =
        std::make_unique<DelayFader>(*store_, palette_, id, std::move(caption));
    auto* result = control.get();
    control->set_bounds(bounds);
    bindings_.push_back(view::bind_parameter(*result, *store_, id));
    register_control(*result, *result);
    parent.add_child(std::move(control));
    return *result;
}

DelayTapField& PulpDelayEditor::add_tap_field(
    view::View& parent, view::Rect bounds) {
    auto control = std::make_unique<DelayTapField>(
        *store_, palette_, kTime, kFeedback, kSync, kDivision);
    auto* result = control.get();
    control->set_bounds(bounds);
    bindings_.push_back(view::bind_parameter(
        static_cast<view::Fader&>(*result), *store_, kTime));
    register_control(*result, *result);
    parent.add_child(std::move(control));
    return *result;
}

DelayChoice& PulpDelayEditor::add_choice(
    view::View& parent, view::Rect bounds, state::ParamID id,
    std::string caption, std::vector<std::string> labels) {
    auto control = std::make_unique<DelayChoice>(
        *store_, palette_, id, std::move(caption), std::move(labels));
    auto* result = control.get();
    control->set_bounds(bounds);
    register_control(*result, *result);
    parent.add_child(std::move(control));
    return *result;
}

DelayActionCard& PulpDelayEditor::add_action(
    view::View& parent, view::Rect bounds, state::ParamID id,
    std::string caption, std::string subtitle, std::string glyph,
    bool compact, bool warning) {
    auto control = std::make_unique<DelayActionCard>(
        palette_, id, std::move(caption), std::move(subtitle),
        std::move(glyph), compact, warning);
    auto* result = control.get();
    control->set_bounds(bounds);
    bindings_.push_back(view::bind_parameter(
        static_cast<view::ToggleButton&>(*result), *store_, id));
    register_control(*result, *result);
    parent.add_child(std::move(control));
    return *result;
}

void PulpDelayEditor::build() {
    auto header = std::make_unique<HeaderView>(palette_);
    header->set_bounds({0, 0, metric::editor_width, 65.0f});
    add_child(std::move(header));

    auto& circulation = add_panel({19, 79, 700, 322}, 1, "CIRCULATION");
    tap_field_ = &add_tap_field(circulation, {12, 40, 676, 270});

    auto& stereo = add_panel({19, 413, 378, 222}, 2, "STEREO FIELD");
    add_action(stereo, {12, 36, 58, 24}, kSync,
               "SYNC", {}, {}, true);
    add_action(stereo, {76, 36, 58, 24}, kLink,
               "LINK", {}, {}, true);
    add_choice(stereo, {141, 36, 224, 24}, kRouting, {},
               {"MONO", "STEREO", "PING PONG"});
    add_choice(stereo, {12, 64, 353, 30}, kDivision, "LEFT DIVISION",
               std::vector<std::string>(
                   kDivisionLabels.begin(), kDivisionLabels.end()));
    add_fader(stereo, {12, 99, 166, 31}, kTimeRight, "RIGHT TIME");
    auto effective_right =
        std::make_unique<EffectiveRightTimeView>(*store_, palette_);
    effective_right_time_ = effective_right.get();
    effective_right->set_bounds({12, 99, 166, 31});
    stereo.add_child(std::move(effective_right));
    add_choice(stereo, {187, 99, 178, 31}, kOffsetMode, "OFFSET MODE",
               {"RATIO", "MS"});
    add_choice(stereo, {12, 134, 353, 30}, kDivisionRight, "RIGHT DIVISION",
               std::vector<std::string>(
                   kDivisionLabels.begin(), kDivisionLabels.end()));
    add_fader(stereo, {12, 169, 226, 38}, kTimeOffset, "RATIO");
    add_fader(stereo, {12, 169, 226, 38}, kOffsetMs, "OFFSET MS");

    auto& character = add_panel({409, 413, 310, 222}, 3, "CHARACTER");
    add_choice(character, {12, 38, 286, 30}, kCharacter, {},
               {"CLEAN", "VINT", "TAPE", "BBD"});
    add_knob(character, {18, 75, 126, 134},
             kCharacterAmount, "AMOUNT");
    add_knob(character, {166, 75, 126, 134},
             kDiffusion, "DIFFUSION");

    auto& energy = add_panel({735, 79, 366, 247}, 4, "ENERGY");
    add_knob(energy, {22, 38, 135, 128}, kFeedback, "FEEDBACK");
    add_knob(energy, {209, 38, 135, 128}, kMix, "MIX");
    add_fader(energy, {16, 178, 160, 50}, kCrossfeed, "CROSSFEED");
    auto crossfeed_override = std::make_unique<CrossfeedOverrideView>(palette_);
    crossfeed_override_ = crossfeed_override.get();
    crossfeed_override->set_bounds({16, 178, 160, 50});
    energy.add_child(std::move(crossfeed_override));
    add_fader(energy, {190, 178, 160, 50}, kDuck, "DUCK");

    auto& tone = add_panel({735, 339, 366, 296}, 5, "TONE + MOVEMENT");
    auto response = std::make_unique<ToneResponseView>(*store_, palette_);
    response->set_bounds({12, 38, 342, 53});
    tone.add_child(std::move(response));
    add_fader(tone, {16, 101, 158, 38}, kLowCut, "LOW CUT");
    add_fader(tone, {192, 101, 158, 38}, kHighCut, "HIGH CUT");
    add_fader(tone, {16, 151, 158, 38}, kLowCutResonance, "LOW RES");
    add_fader(tone, {192, 151, 158, 38}, kHighCutResonance, "HIGH RES");
    add_fader(tone, {16, 201, 158, 38}, kModRate, "MOD RATE");
    add_fader(tone, {192, 201, 158, 38}, kModDepth, "MOD DEPTH");

    add_action(*this, {19, 649, 387, 72}, kFreeze,
               "FREEZE", "HOLD THE DELAY BUFFER", "||");
    add_action(*this, {418, 649, 387, 72}, kReverse,
               "REVERSE", "TURN THE ECHO AROUND", "<-", false, true);

    auto control_state = std::make_unique<ControlStateView>(*store_, palette_);
    control_state->set_bounds({817, 649, 284, 72});
    add_child(std::move(control_state));
}

void PulpDelayEditor::set_control_present(state::ParamID id, bool present) {
    if (id < kTime || id > kReverse)
        return;
    if (auto* control = control_views_[static_cast<std::size_t>(id - kTime)]) {
        if (control->visible() != present)
            control->set_visible(present);
        if (control->enabled() != present)
            control->set_enabled(present);
        if (control->hit_testable() != present)
            control->set_hit_testable(present);
    }
}

void PulpDelayEditor::update_timing_presentation() {
    const auto inputs = delay_time_inputs_from_store(
        *store_, DelayTimeModel::kFallbackTempoBpm);
    const auto branch = DelayTimeModel::right_timing_branch(inputs);
    const bool ping_pong = branch == RightTimingBranch::ping_pong;
    const bool linked_ratio = branch == RightTimingBranch::linked_ratio;
    const bool linked_ms = branch == RightTimingBranch::linked_offset_ms;
    const bool synced_independent = branch == RightTimingBranch::synced_independent;
    const bool free_independent = branch == RightTimingBranch::free_independent;

    // The large circulation field remains visible as the primary timing
    // visualization, but host-sync makes its raw Time parameter non-editable
    // and swaps its text/geometry to the selected Division.
    if (auto* time = control_views_[static_cast<std::size_t>(kTime - kTime)]) {
        const bool editable = !inputs.sync;
        if (time->enabled() != editable)
            time->set_enabled(editable);
        if (time->hit_testable() != editable)
            time->set_hit_testable(editable);
    }
    set_control_present(kDivision, inputs.sync);
    set_control_present(kLink, !ping_pong);
    set_control_present(kOffsetMode, linked_ratio || linked_ms);
    set_control_present(kTimeOffset, linked_ratio);
    set_control_present(kOffsetMs, linked_ms);
    set_control_present(kTimeRight, free_independent);
    set_control_present(kDivisionRight, synced_independent);
    set_control_present(kCrossfeed, !ping_pong);

    if (effective_right_time_) {
        const bool present = ping_pong || linked_ratio || linked_ms;
        if (effective_right_time_->visible() != present)
            effective_right_time_->set_visible(present);
        const auto display = effective_right_time_->display_text();
        if (effective_right_time_->access_value() != display)
            effective_right_time_->set_access_value(display);
    }
    if (crossfeed_override_
        && crossfeed_override_->visible() != ping_pong) {
        crossfeed_override_->set_visible(ping_pong);
    }
}

std::unique_ptr<view::View> build_pulp_delay_editor(
    state::StateStore& store) {
    return std::make_unique<PulpDelayEditor>(store);
}

} // namespace pulp::examples::delay::ui
