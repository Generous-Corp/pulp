#include "pulp_delay_editor.hpp"

#include "delay_time_model.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace pulp::examples::delay::ui {

namespace {

void text(canvas::Canvas& c, const std::string& value,
          float x, float baseline, float size, canvas::Color colour,
          canvas::TextAlign align = canvas::TextAlign::left,
          int weight = 500, float spacing = 0.0f) {
    c.set_font_full("Roboto Mono", size, weight, 0, spacing);
    c.set_text_align(align);
    c.set_fill_color(colour);
    c.fill_text(value, x, baseline);
}

canvas::Color with_alpha(canvas::Color value, float alpha) {
    value.a = std::clamp(alpha, 0.0f, 1.0f);
    return value;
}

class HeaderView final : public view::View {
  public:
    void layout_children() override {}

    void paint(canvas::Canvas& c) override {
        const auto b = local_bounds();
        c.set_fill_color(color::header);
        c.fill_rect(0, 0, b.width, b.height);
        c.set_fill_color(color::lime);
        c.fill_rect(0, b.height - 2.0f, b.width, 2.0f);

        text(c, "PULP", 20.0f, 31.0f, 22.0f, color::lime,
             canvas::TextAlign::left, 800, 2.4f);
        text(c, "DELAY", 20.0f, 49.0f, 10.0f, color::ink,
             canvas::TextAlign::left, 650, 4.2f);

        c.set_fill_color(color::panel);
        c.fill_rounded_rect(202.0f, 12.0f, 306.0f, 40.0f, 5.0f);
        c.set_stroke_color(color::border);
        c.set_line_width(1.0f);
        c.stroke_rounded_rect(202.5f, 12.5f, 305.0f, 39.0f, 5.0f);
        text(c, "01", 218.0f, 36.0f, 11.0f, color::lime,
             canvas::TextAlign::left, 700);
        text(c, "DEFAULT SPACE", 255.0f, 36.0f, 11.0f, color::ink,
             canvas::TextAlign::left, 600, 0.65f);
        text(c, "PRESET", 490.0f, 35.0f, 7.5f, color::muted,
             canvas::TextAlign::right, 650, 0.8f);

        text(c, "48.0 kHz", b.width - 258.0f, 29.0f, 8.5f, color::muted,
             canvas::TextAlign::left, 550, 0.35f);
        text(c, "STEREO", b.width - 166.0f, 29.0f, 8.5f, color::ink,
             canvas::TextAlign::left, 650, 0.8f);
        text(c, "DSP  2.1%", b.width - 92.0f, 29.0f, 8.5f, color::lime,
             canvas::TextAlign::left, 650);
        c.set_fill_color(color::lime);
        c.fill_circle(b.width - 250.0f, 44.0f, 2.0f);
        text(c, "ENGINE ONLINE", b.width - 241.0f, 47.0f, 7.5f,
             color::muted, canvas::TextAlign::left, 550, 0.5f);
    }
};

class ToneResponseView final : public view::View {
  public:
    explicit ToneResponseView(state::StateStore& store) : store_(&store) {
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
        c.set_fill_color(with_alpha(color::lime_dim, 0.60f));
        c.fill_rounded_rect(pass_left, 12.0f, pass_right - pass_left,
                            b.height - 24.0f, 3.0f);
        c.set_stroke_color(color::lime);
        c.set_line_width(2.0f);
        c.stroke_line(low_x, b.height - 10.0f, low_x, 10.0f);
        c.stroke_line(high_x, 10.0f, high_x, b.height - 10.0f);
        c.stroke_line(low_x, 10.0f, high_x, 10.0f);
        text(c, "PASSBAND", b.width * 0.5f, b.height - 7.0f, 7.5f,
             color::muted, canvas::TextAlign::center, 650, 0.8f);
    }

  private:
    state::StateStore* store_ = nullptr;
    std::vector<state::ListenerToken> listeners_;
};

class OutputMeterView final : public view::View {
  public:
    explicit OutputMeterView(state::StateStore& store) : store_(&store) {
        set_hit_testable(false);
        listeners_.push_back(store.add_listener(
            [this](state::ParamID changed, float) {
                if (changed == kMix || changed == kFeedback)
                    request_repaint();
            },
            state::ListenerThread::Main));
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
        text(c, "OUTPUT", 16.0f, 20.0f, 8.5f, color::muted,
             canvas::TextAlign::left, 650, 0.9f);
        text(c, "L", 16.0f, 40.0f, 8.0f, color::ink);
        text(c, "R", 16.0f, 57.0f, 8.0f, color::ink);

        const float mix = store_->get_normalized(kMix);
        const float feedback = store_->get_normalized(kFeedback);
        const float left_level = std::clamp(0.22f + mix * 0.70f, 0.0f, 1.0f);
        const float right_level = std::clamp(
            0.18f + mix * 0.54f + feedback * 0.18f, 0.0f, 1.0f);
        const float meter_x = 34.0f;
        const float meter_w = b.width - meter_x - 16.0f;
        for (int channel = 0; channel < 2; ++channel) {
            const float y = 34.0f + static_cast<float>(channel) * 17.0f;
            const float level = channel == 0 ? left_level : right_level;
            c.set_fill_color(color::track);
            c.fill_rounded_rect(meter_x, y, meter_w, 7.0f, 3.5f);
            c.set_fill_color(color::lime);
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
    std::vector<state::ListenerToken> listeners_;
};

} // namespace

class PulpDelayEditor::Panel final : public view::View {
  public:
    Panel(int index, std::string title)
        : index_(index), title_(std::move(title)) {
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
             color::lime, canvas::TextAlign::left, 750, 0.8f);
        text(c, title_, 31.0f, 23.0f, 10.0f, color::ink,
             canvas::TextAlign::left, 700, 1.35f);
        c.set_stroke_color(color::border);
        c.set_line_width(1.0f);
        c.stroke_line(12.0f, 31.0f, b.width - 12.0f, 31.0f);
    }

  private:
    int index_ = 0;
    std::string title_;
};

PulpDelayEditor::PulpDelayEditor(state::StateStore& store) : store_(&store) {
    set_id("pulp-delay-editor");
    set_access_role(view::View::AccessRole::group);
    set_access_label("Pulp Delay editor");
    set_bounds({0, 0, metric::editor_width, metric::editor_height});
    bindings_.reserve(kParameterCount);
    build();
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
    // StateStore::deserialize intentionally restores atomics without dispatching
    // listeners. A native editor therefore reconciles once at frame paint so a
    // host preset/session recall is visible on the very next frame. The silent
    // setters below never write back into the store or open host gestures.
    sync_from_store();
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

void PulpDelayEditor::sync_from_store() {
    for (auto* control : controls_) {
        if (!control)
            continue;
        const auto id = control->parameter_id();
        if (auto* knob = dynamic_cast<DelayKnob*>(control)) {
            knob->set_value(store_->get_normalized(id));
        } else if (auto* fader = dynamic_cast<DelayFader*>(control)) {
            fader->set_value(store_->get_normalized(id));
        } else if (auto* action = dynamic_cast<DelayActionCard*>(control)) {
            action->set_on(store_->get_value(id) >= 0.5f);
        }
        // DelayChoice reads StateStore directly in paint/normalized_value.
    }
}

PulpDelayEditor::Panel& PulpDelayEditor::add_panel(
    view::Rect bounds, int index, std::string title) {
    auto panel = std::make_unique<Panel>(index, std::move(title));
    auto* result = panel.get();
    panel->set_bounds(bounds);
    add_child(std::move(panel));
    return *result;
}

void PulpDelayEditor::register_control(DelayParameterControl& control) {
    const auto id = control.parameter_id();
    if (id >= kTime && id <= kReverse)
        controls_[static_cast<std::size_t>(id - kTime)] = &control;
}

DelayKnob& PulpDelayEditor::add_knob(
    view::View& parent, view::Rect bounds, state::ParamID id,
    std::string caption) {
    auto control = std::make_unique<DelayKnob>(*store_, id, std::move(caption));
    auto* result = control.get();
    control->set_bounds(bounds);
    bindings_.push_back(view::bind_parameter(*result, *store_, id));
    register_control(*result);
    parent.add_child(std::move(control));
    return *result;
}

DelayFader& PulpDelayEditor::add_fader(
    view::View& parent, view::Rect bounds, state::ParamID id,
    std::string caption) {
    auto control = std::make_unique<DelayFader>(*store_, id, std::move(caption));
    auto* result = control.get();
    control->set_bounds(bounds);
    bindings_.push_back(view::bind_parameter(*result, *store_, id));
    register_control(*result);
    parent.add_child(std::move(control));
    return *result;
}

DelayTapField& PulpDelayEditor::add_tap_field(
    view::View& parent, view::Rect bounds) {
    auto control = std::make_unique<DelayTapField>(
        *store_, kTime, kFeedback);
    auto* result = control.get();
    control->set_bounds(bounds);
    bindings_.push_back(view::bind_parameter(
        static_cast<view::Fader&>(*result), *store_, kTime));
    register_control(*result);
    parent.add_child(std::move(control));
    return *result;
}

DelayChoice& PulpDelayEditor::add_choice(
    view::View& parent, view::Rect bounds, state::ParamID id,
    std::string caption, std::vector<std::string> labels) {
    auto control = std::make_unique<DelayChoice>(
        *store_, id, std::move(caption), std::move(labels));
    auto* result = control.get();
    control->set_bounds(bounds);
    register_control(*result);
    parent.add_child(std::move(control));
    return *result;
}

DelayActionCard& PulpDelayEditor::add_action(
    view::View& parent, view::Rect bounds, state::ParamID id,
    std::string caption, std::string subtitle, std::string glyph,
    bool compact) {
    auto control = std::make_unique<DelayActionCard>(
        id, std::move(caption), std::move(subtitle),
        std::move(glyph), compact);
    auto* result = control.get();
    control->set_bounds(bounds);
    bindings_.push_back(view::bind_parameter(
        static_cast<view::ToggleButton&>(*result), *store_, id));
    register_control(*result);
    parent.add_child(std::move(control));
    return *result;
}

void PulpDelayEditor::build() {
    auto header = std::make_unique<HeaderView>();
    header->set_bounds({0, 0, metric::editor_width, 65.0f});
    add_child(std::move(header));

    auto& circulation = add_panel({19, 79, 700, 322}, 1, "CIRCULATION");
    add_tap_field(circulation, {12, 40, 676, 270});

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
    add_choice(stereo, {187, 99, 178, 31}, kOffsetMode, "OFFSET MODE",
               {"RATIO", "MS"});
    add_choice(stereo, {12, 134, 353, 30}, kDivisionRight, "RIGHT DIVISION",
               std::vector<std::string>(
                   kDivisionLabels.begin(), kDivisionLabels.end()));
    add_fader(stereo, {12, 169, 109, 38}, kTimeOffset, "RATIO");
    add_fader(stereo, {129, 169, 109, 38}, kOffsetMs, "OFFSET MS");

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
    add_fader(energy, {190, 178, 160, 50}, kDuck, "DUCK");

    auto& tone = add_panel({735, 339, 366, 296}, 5, "TONE + MOVEMENT");
    auto response = std::make_unique<ToneResponseView>(*store_);
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
               "REVERSE", "TURN THE ECHO AROUND", "<-");

    auto output = std::make_unique<OutputMeterView>(*store_);
    output->set_bounds({817, 649, 284, 72});
    add_child(std::move(output));
}

std::unique_ptr<view::View> build_pulp_delay_editor(
    state::StateStore& store) {
    return std::make_unique<PulpDelayEditor>(store);
}

} // namespace pulp::examples::delay::ui
