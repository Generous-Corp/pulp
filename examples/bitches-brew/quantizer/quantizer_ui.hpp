#pragma once

// Quantizer's editor: the staircase, drawn, with the signal's position on it.
//
// "Twelve steps, offset 0.3, transposed up two" is a shape nobody can picture.
// Drawing it costs a couple of hundred calls to the same `quantize_transfer` the
// DSP runs, and answers the question directly. The dot is where the incoming
// voltage is landing — watching it hop between treads is how you tell a live CV
// from a dead cable, which for a plug-in that makes no sound is otherwise
// unanswerable from inside a DAW.

#include "quantizer_processor.hpp"

#include <brew/ui/panel.hpp>

#include <cstdio>
#include <functional>
#include <string>
#include <utility>

namespace pulp::examples::brew {

/// Plots output against input across the full bipolar square. Evaluates the
/// processor's own transfer function, so the picture cannot drift from the signal.
class StaircaseGraph : public view::View {
public:
    explicit StaircaseGraph(const QuantizerProcessor& proc) : proc_(proc) {}

    void paint(canvas::Canvas& c) override {
        const float w = local_bounds().width, h = local_bounds().height;
        const float s = scale();
        const float pad = 4.0f * s;

        c.set_fill_color(ui::palette::rail);
        c.fill_rounded_rect(0, 0, w, h, 6.0f);

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
        // The identity, faint: the line the signal would follow unquantized.
        c.stroke_line(x_of(-1.0), y_of(-1.0), x_of(1.0), y_of(1.0));

        const QuantizeSettings settings = proc_.settings();

        // Sample densely. The treads are flat and the risers are vertical, so a
        // coarse sweep would round the corners off a shape whose corners are the
        // entire point.
        c.set_stroke_color(ui::palette::accent);
        c.set_line_width(2.0f * s);
        ui::plot(c, 512, x_of(-1.0), x_of(1.0), [&](float t) {
            const auto in = static_cast<float>(-1.0 + 2.0 * t);
            return y_of(quantize_transfer(in, settings));
        });

        const float in = proc_.display_input();
        const float out = proc_.display_output();
        c.set_fill_color(ui::palette::text);
        c.fill_circle(x_of(in), y_of(out), 3.5f * s);

        request_repaint();
    }

private:
    const QuantizerProcessor& proc_;
};

class QuantizerUi : public ui::BrewPanel {
public:
    explicit QuantizerUi(state::StateStore& store, const QuantizerProcessor& proc)
        : ui::BrewPanel("Quantizer", "snap a control voltage to discrete steps"),
          store_(store) {
        auto whole = [](float v) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.0f", v);
            return std::string(buf);
        };
        auto signed_whole = [](float v) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%+.0f", v);
            return std::string(buf);
        };
        auto number = [](float v) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%+.2f", v);
            return std::string(buf);
        };

        auto graph = std::make_unique<StaircaseGraph>(proc);
        graph->flex().preferred_height = 130.0f;
        graph->flex().align_self = view::FlexAlign::stretch;

        auto add = [&](view::View& row, state::ParamID id, const char* label,
                       std::function<std::string(float)> fmt) {
            auto k = ui::param_knob(store_, id, label, std::move(fmt));
            ui::knob_size(*k);
            row.add_child(std::move(k));
        };

        auto top = ui::row(ui::kRowGap, ui::kKnobHeight);
        add(*top, QuantizerProcessor::kSteps, "Steps", whole);
        add(*top, QuantizerProcessor::kFine, "Fine", number);
        add(*top, QuantizerProcessor::kOffset, "Offset", number);
        add(*top, QuantizerProcessor::kTranspose, "Transp", signed_whole);

        add(*top, QuantizerProcessor::kOutputScale, "Out", number);

        auto bottom = ui::row(ui::kRowGap, ui::kToggleHeight);
        auto inv = ui::param_toggle(store_, QuantizerProcessor::kInvert, "Invert");
        ui::toggle_size(*inv);
        bottom->add_child(std::move(inv));

        add_child(std::move(graph));
        add_child(std::move(top));
        add_child(std::move(bottom));
        // Steps divide full scale, not an octave. A semitone is a fixed voltage,
        // and no plug-in here knows what full scale is worth in volts.
        add_child(ui::caption_label(
            "steps divide full scale — semitones need a calibrated rail voltage"));
    }

private:
    state::StateStore& store_;
};

}  // namespace pulp::examples::brew
