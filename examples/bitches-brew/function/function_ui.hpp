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

#include <cstddef>
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
    FunctionGraph(const FunctionProcessor& proc, std::size_t channel)
        : proc_(proc), channel_(channel) {}

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

        const FunctionSettings settings = proc_.settings(channel_);

        // The identity, faint, so the curve's departure from a wire is visible.
        c.set_stroke_color(ui::palette::border);
        c.stroke_line(x_of(-1.0), y_of(-1.0), x_of(1.0), y_of(1.0));

        // The curve is drawn with the *output* stage applied and the input stage
        // held at unity. Scaling the input does not change the shape of the
        // function — it changes where on the function the signal lands — so a
        // graph that redrew for the input knobs would be answering a question
        // nobody asked, and would hide the one the indicator answers.
        FunctionSettings plotted = settings;
        plotted.in_scale = 1.0f;
        plotted.in_offset = 0.0f;

        c.set_stroke_color(ui::palette::accent);
        c.set_line_width(2.0f * s);
        ui::plot(c, 128, x_of(-1.0), x_of(1.0), [&](float t) {
            const auto in = static_cast<float>(-1.0 + 2.0 * t);
            return y_of(function_transfer(in, plotted));
        });

        // ...and so the indicator sits at the point the curve is *evaluated* at,
        // which is the incoming signal after the input stage. Turn `In Scale` and
        // the dot slides along a curve that does not move. That is what the input
        // knobs do, drawn honestly.
        const float in = proc_.display_input(channel_);
        const float out = proc_.display_output(channel_);
        const float dot = 3.5f * s;
        c.set_fill_color(ui::palette::text);
        c.fill_circle(x_of(function_input_stage(in, settings)), y_of(out), dot);

        request_repaint();
    }

private:
    const FunctionProcessor& proc_;
    std::size_t channel_;
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
        auto add = [&](view::View& row, state::ParamID id, std::size_t ch,
                       const char* label, std::function<std::string(float)> fmt) {
            auto k = ui::param_knob(store_,
                                    static_cast<state::ParamID>(param_for(id, ch)),
                                    label, std::move(fmt));
            ui::knob_size(*k);
            row.add_child(std::move(k));
        };

        for (std::size_t ch = 0; ch < kChannelCount; ++ch) {
            // Amount drives only the power curve, and only this channel's. Showing
            // a dash rather than a stale number is how the editor says "this knob
            // does nothing right now".
            auto amount_or_dash = [&store, ch](float v) {
                char buf[16];
                const float curve = store.get_value(
                    static_cast<state::ParamID>(param_for(FunctionProcessor::kCurve, ch)));
                if (!curve_uses_amount(curve_from_param(curve)))
                    return std::string("—");
                std::snprintf(buf, sizeof(buf), "%.3g", v);
                return std::string(buf);
            };

            add_child(ui::channel_label(ch == 0 ? "LEFT" : "RIGHT"));

            auto graph = std::make_unique<FunctionGraph>(proc, ch);
            graph->flex().preferred_height = 96.0f;
            graph->flex().align_self = view::FlexAlign::stretch;

            // Input stage, function, output stage — left to right, the order the
            // signal actually travels.
            auto top = ui::row(ui::kRowGap, ui::kKnobHeight);
            add(*top, FunctionProcessor::kInScale, ch, "In Scale", number);
            add(*top, FunctionProcessor::kInOffset, ch, "In Off", number);
            add(*top, FunctionProcessor::kCurve, ch, "Curve", curve_name);
            add(*top, FunctionProcessor::kAmount, ch, "Amount", amount_or_dash);
            add(*top, FunctionProcessor::kOutputScale, ch, "Out", number);

            auto bottom = ui::row(ui::kRowGap, ui::kKnobHeight);
            add(*bottom, FunctionProcessor::kOutOffset, ch, "Out Off", number);
            auto enable = ui::param_toggle(
                store_,
                static_cast<state::ParamID>(param_for(FunctionProcessor::kEnable, ch)),
                "Enable");
            auto inv = ui::param_toggle(
                store_,
                static_cast<state::ParamID>(param_for(FunctionProcessor::kInvert, ch)),
                "Invert");
            ui::toggle_size(*enable);
            ui::toggle_size(*inv);
            bottom->add_child(std::move(enable));
            bottom->add_child(std::move(inv));

            add_child(std::move(graph));
            add_child(std::move(top));
            add_child(std::move(bottom));
        }

        add_child(ui::caption_label(
            "the dot is where the incoming signal is sitting on the curve"));
    }

private:
    state::StateStore& store_;
};

}  // namespace pulp::examples::brew
