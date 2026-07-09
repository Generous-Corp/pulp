#pragma once

// The suite's shared editor furniture.
//
// Every Bitches Brew plug-in gets the same panel: a title, a one-line statement
// of what the plug-in does, a row of controls, and — the part that matters for a
// CV plug-in — a readout of what is actually leaving the jack. A CV plug-in is
// invisible and inaudible. Without a readout the user has no way to tell a
// working patch from a dead cable, which is exactly the moment they blame the
// software.
//
// Controls are Pulp's own widgets (`pulp::view::Knob`, `Toggle`), themed from
// the shared preset, rather than hand-painted. That keeps the suite consistent
// with the rest of Pulp's design system and means a widget fix upstream lands
// here for free.

#include <pulp/canvas/canvas.hpp>
#include <pulp/state/parameter_edit.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace pulp::examples::brew::ui {

namespace cv = pulp::canvas;
namespace vw = pulp::view;

/// A dark instrument-panel palette. Voltage is drawn in a warm accent so a live
/// output reads at a glance across a dim studio.
namespace palette {
inline constexpr cv::Color bg = cv::Color::rgba8(18, 18, 24);
inline constexpr cv::Color surface = cv::Color::rgba8(30, 31, 40);
inline constexpr cv::Color border = cv::Color::rgba8(58, 60, 74);
inline constexpr cv::Color text = cv::Color::rgba8(226, 228, 238);
inline constexpr cv::Color text_dim = cv::Color::rgba8(140, 146, 166);
inline constexpr cv::Color accent = cv::Color::rgba8(255, 176, 92);
inline constexpr cv::Color accent_dim = cv::Color::rgba8(255, 176, 92, 60);
inline constexpr cv::Color negative = cv::Color::rgba8(120, 180, 255);
inline constexpr cv::Color rail = cv::Color::rgba8(44, 46, 58);
inline constexpr cv::Color lamp_off = cv::Color::rgba8(48, 40, 34);
}  // namespace palette

/// A flex row. Layout in Pulp is Yoga (flex + grid) — `render_to_png` and the
/// window host both run `layout_children()` after `set_bounds()`, so any bounds a
/// view assigns by hand are overwritten before it ever paints. Widgets must be
/// positioned by flex properties, never by `set_bounds`.
inline std::unique_ptr<vw::View> row(float gap, float height) {
    auto v = std::make_unique<vw::View>();
    v->flex().direction = vw::FlexDirection::row;
    v->flex().align_items = vw::FlexAlign::center;
    v->flex().gap = gap;
    v->flex().preferred_height = height;
    return v;
}

/// Give a widget a fixed box in the flex layout.
inline void fixed_size(vw::View& v, float w, float h) {
    v.flex().preferred_width = w;
    v.flex().preferred_height = h;
    v.flex().flex_grow = 0;
    v.flex().flex_shrink = 0;
}

/// A caption line. Text lives in a Label so Yoga can measure it; drawing captions
/// in the parent's paint puts them underneath children, which paint afterwards.
inline std::unique_ptr<vw::Label> caption_label(std::string text, float size = 10.0f) {
    auto l = std::make_unique<vw::Label>();
    l->set_text(std::move(text));
    l->set_font_size(size);
    l->set_text_color(palette::text_dim);
    l->flex().preferred_height = size + 6.0f;
    return l;
}

/// Wire a Knob to a parameter, with proper host gestures.
///
/// The gesture bracket is not optional: without `begin`/`end` a DAW does not
/// record the automation, and the user's edit silently fails to stick.
inline std::unique_ptr<vw::Knob> param_knob(state::StateStore& store,
                                            state::ParamID id,
                                            std::string label,
                                            std::function<std::string(float)> fmt = {}) {
    auto knob = std::make_unique<vw::Knob>();
    knob->set_label(std::move(label));
    knob->set_value(store.get_normalized(id));

    // The Knob speaks normalized [0,1]; the store owns the real range, the step,
    // and the denormalization. Never duplicate that mapping in the editor.
    auto* raw = knob.get();
    auto edit = std::make_shared<state::ParameterEdit>(store);

    knob->on_gesture_begin = [edit, id] { edit->begin(id); };
    knob->on_gesture_end = [edit] { edit->finish(); };
    knob->on_change = [&store, id](float normalized) {
        store.set_normalized(id, normalized);
    };
    if (fmt) {
        knob->set_format([&store, id, fmt = std::move(fmt)](float) {
            return fmt(store.get_value(id));
        });
    }
    (void)raw;
    return knob;
}

/// Wire a Toggle to a 0/1 parameter.
inline std::unique_ptr<vw::Toggle> param_toggle(state::StateStore& store,
                                                state::ParamID id,
                                                std::string label) {
    auto toggle = std::make_unique<vw::Toggle>();
    toggle->set_label(std::move(label));
    toggle->set_on(store.get_value(id) >= 0.5f, /*animate=*/false);
    toggle->on_toggle = [&store, id](bool on) {
        state::ParameterEdit edit(store);
        edit.begin(id);
        edit.set(id, on ? 1.0f : 0.0f);
        edit.finish();
    };
    return toggle;
}

/// A bipolar voltage rail: full-scale negative at the left, zero at the centre,
/// full-scale positive at the right, with a marker at the current output.
///
/// It shows **normalized full scale**, never volts — the plug-in does not know
/// the interface's rail voltage, and inventing a number here would be a lie the
/// user would wire into a modular.
class VoltageRail : public vw::View {
public:
    explicit VoltageRail(std::function<float()> value) : value_(std::move(value)) {}

    void paint(cv::Canvas& canvas) override {
        const float w = local_bounds().width, h = local_bounds().height;
        const float s = scale();
        const float v = std::clamp(value_ ? value_() : 0.0f, -1.0f, 1.0f);

        canvas.set_fill_color(palette::rail);
        canvas.fill_rounded_rect(0, 0, w, h, h * 0.5f);

        const float mid = w * 0.5f;
        // Zero tick, so "no voltage" is visibly distinct from "not connected".
        canvas.set_stroke_color(palette::border);
        canvas.set_line_width(1.0f * s);
        canvas.stroke_line(mid, 0, mid, h);

        if (v != 0.0f) {
            const float span = mid * std::abs(v);
            const float x = v > 0.0f ? mid : mid - span;
            canvas.set_fill_color(v > 0.0f ? palette::accent : palette::negative);
            canvas.fill_rounded_rect(x, 0, span, h, h * 0.5f);
        }

        // Repaint continuously: the value is driven by parameters the host can
        // automate, so the editor cannot know when it changed.
        request_repaint();
    }

private:
    std::function<float()> value_;
};

/// An indicator lamp. `brightness()` returns 0…1; the lamp is a CV plug-in's
/// only honest "is anything happening" signal.
class Lamp : public vw::View {
public:
    Lamp(std::string label, std::function<float()> brightness)
        : label_(std::move(label)), brightness_(std::move(brightness)) {}

    void paint(cv::Canvas& canvas) override {
        const float w = local_bounds().width, h = local_bounds().height;
        const float s = scale();
        const float b = std::clamp(brightness_ ? brightness_() : 0.0f, 0.0f, 1.0f);
        // Leave a text line clear at the bottom, and keep the glow inside the
        // bulb's own footprint so neighbouring lamps never bleed together.
        const float label_h = 14.0f;
        const float r = std::min(w, h - label_h) * 0.5f * 0.72f;
        const float cx = w * 0.5f;
        const float cy = (h - label_h) * 0.5f;

        if (b > 0.01f) {
            canvas.set_fill_color(palette::accent_dim);
            canvas.fill_circle(cx, cy, r * (1.0f + 0.35f * b));
        }
        canvas.set_fill_color(b > 0.01f ? palette::accent : palette::lamp_off);
        canvas.fill_circle(cx, cy, r);

        canvas.set_fill_color(palette::text_dim);
        canvas.set_font("Inter", 10.0f * s);
        canvas.fill_text_anchored(label_, cx, h - 2.0f,
                                  cv::Canvas::TextAnchor::Baseline);

        request_repaint();
    }

private:
    std::string label_;
    std::function<float()> brightness_;
};

/// The root panel: background, title, tagline. Children paint on top of it —
/// which is why the title is drawn here and never underneath a child's bounds.
class BrewPanel : public vw::View {
public:
    BrewPanel(std::string title, std::string tagline)
        : title_(std::move(title)), tagline_(std::move(tagline)) {
        // Reserve the header strip with padding rather than by positioning
        // children below it: Yoga owns the child boxes.
        flex().direction = vw::FlexDirection::column;
        flex().padding_top = 78.0f;
        flex().padding_left = 20.0f;
        flex().padding_right = 20.0f;
        flex().padding_bottom = 16.0f;
        flex().gap = 14.0f;
    }

    void paint(cv::Canvas& canvas) override {
        const float w = local_bounds().width, h = local_bounds().height;
        const float s = scale();

        canvas.set_fill_color(palette::bg);
        canvas.fill_rect(0, 0, w, h);

        canvas.set_fill_color(palette::text);
        canvas.set_font("Inter", 22.0f * s);
        canvas.fill_text(title_, 20.0f * s, 34.0f * s);

        canvas.set_fill_color(palette::text_dim);
        canvas.set_font("Inter", 11.0f * s);
        canvas.fill_text(tagline_, 20.0f * s, 52.0f * s);

        canvas.set_stroke_color(palette::border);
        canvas.set_line_width(1.0f * s);
        canvas.stroke_line(20.0f * s, 64.0f * s, w - 20.0f * s, 64.0f * s);
    }

private:
    std::string title_;
    std::string tagline_;
};

}  // namespace pulp::examples::brew::ui
