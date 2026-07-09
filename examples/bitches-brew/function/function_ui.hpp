#pragma once

// Function's editor: the curve, drawn, with the signal's current position on it.
//
// A transfer function is the one thing in this suite that is genuinely hard to
// read from its numbers. "Exponential, amount 3.4, in-scale -1.2, out-offset
// 0.25" describes a shape nobody can picture. Drawing it costs a hundred calls to
// the same `function_transfer` the DSP runs, and answers the question directly.
//
// The dot is the point the plug-in is actually operating at, taken from the last
// sample of the last block. Watching it slide along the curve is how you tell a
// live CV input from a dead cable — which, for a plug-in that makes no sound and
// moves no meter, is otherwise unanswerable from inside a DAW.

#include "function_processor.hpp"

#include <brew/ui/panel.hpp>

#include <cstdio>
#include <functional>
#include <string>
#include <utility>

namespace pulp::examples::brew {

/// Plots output against input over the full [-1, +1] square, and marks where the
/// signal currently sits. Evaluates the processor's own transfer function, so the
/// picture cannot drift from the signal.
class FunctionGraph : public view::View {
public:
    explicit FunctionGraph(const FunctionProcessor& proc) : proc_(proc) {}

    void paint(canvas::Canvas& c) override {
        const float w = local_bounds().width, h = local_bounds().height;
        const float s = scale();
        const float pad = 4.0f * s;

        c.set_fill_color(ui::palette::rail);
        c.fill_rounded_rect(0, 0, w, h, 6.0f);

        // Input runs left-to-right, output bottom-to-top, origin at the centre.
        auto x_of = [&](double in) {
            return static_cast<float>(pad + (in + 1.0) * 0.5 * (w - 2.0f * pad));
        };
        auto y_of = [&](double out) {
            return static_cast<float>(h - pad - (out + 1.0) * 0.5 * (h - 2.0f * pad));
        };

        c.set_stroke_color(ui::palette::border);
        c.set_line_width(1.0f * s);
        c.stroke_line(x_of(-1.0), y_of(0.0), x_of(1.0), y_of(0.0));
        c.stroke_line(x_of(0.0), y_of(-1.0), x_of(0.0), y_of(1.0));

        const FunctionSettings settings = proc_.settings();

        // The identity, faint, so the curve's departure from a wire is visible.
        c.set_stroke_color(ui::palette::border);
        c.stroke_line(x_of(-1.0), y_of(-1.0), x_of(1.0), y_of(1.0));

        c.set_stroke_color(ui::palette::accent);
        c.set_line_width(2.0f * s);
        ui::plot(c, 128, x_of(-1.0), x_of(1.0), [&](float t) {
            const auto in = static_cast<float>(-1.0 + 2.0 * t);
            return y_of(function_transfer(in, settings));
        });

        const float in = proc_.display_input();
        const float out = proc_.display_output();
        const float dot = 3.5f * s;
        c.set_fill_color(ui::palette::text);
        c.fill_circle(x_of(in), y_of(out), dot);

        request_repaint();
    }

private:
    const FunctionProcessor& proc_;
};

class FunctionUi : public ui::BrewPanel {
public:
    explicit FunctionUi(state::StateStore& store, const FunctionProcessor& proc)
        : ui::BrewPanel("Function", "shape an incoming control voltage"),
          store_(store) {
        auto number = [](float v) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.2f", v);
            return std::string(buf);
        };
        auto curve_name = [](float v) {
            switch (curve_from_param(v)) {
                case Curve::linear: return std::string("lin");
                case Curve::exponential: return std::string("exp");
                case Curve::logarithmic: return std::string("log");
                case Curve::absolute: return std::string("abs");
                case Curve::power: return std::string("pow");
            }
            return std::string("?");
        };
        // Amount drives only the power curve. Showing a dash rather than a stale
        // number is how the editor says "this knob does nothing right now".
        auto amount_or_dash = [&store](float v) {
            char buf[16];
            if (!curve_uses_amount(curve_from_param(store.get_value(FunctionProcessor::kCurve))))
                return std::string("—");
            std::snprintf(buf, sizeof(buf), "%.3g", v);
            return std::string(buf);
        };

        auto graph = std::make_unique<FunctionGraph>(proc);
        graph->flex().preferred_height = 120.0f;
        graph->flex().align_self = view::FlexAlign::stretch;

        auto add = [&](view::View& row, state::ParamID id, const char* label,
                       std::function<std::string(float)> fmt) {
            auto k = ui::param_knob(store_, id, label, std::move(fmt));
            ui::knob_size(*k);
            row.add_child(std::move(k));
        };

        auto top = ui::row(ui::kRowGap, ui::kKnobHeight);
        add(*top, FunctionProcessor::kCurve, "Curve", curve_name);
        add(*top, FunctionProcessor::kAmount, "Amount", amount_or_dash);
        add(*top, FunctionProcessor::kInScale, "In Scale", number);

        auto bottom = ui::row(ui::kRowGap, ui::kKnobHeight);
        add(*bottom, FunctionProcessor::kInOffset, "In Off", number);
        add(*bottom, FunctionProcessor::kOutOffset, "Out Off", number);
        add(*bottom, FunctionProcessor::kOutputScale, "Out", number);

        auto toggles = ui::row(ui::kRowGap, ui::kToggleHeight);
        auto inv = ui::param_toggle(store_, FunctionProcessor::kInvert, "Invert");
        ui::toggle_size(*inv);
        toggles->add_child(std::move(inv));

        add_child(std::move(graph));
        add_child(std::move(top));
        add_child(std::move(bottom));
        add_child(std::move(toggles));
        add_child(ui::caption_label(
            "the dot is where the incoming signal is sitting on the curve"));
    }

private:
    state::StateStore& store_;
};

}  // namespace pulp::examples::brew
