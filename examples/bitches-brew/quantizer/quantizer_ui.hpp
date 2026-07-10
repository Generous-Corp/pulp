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

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <string>
#include <utility>

namespace pulp::examples::brew {

/// Plots output against input across the full bipolar square. Evaluates the
/// processor's own transfer function, so the picture cannot drift from the signal.
class StaircaseGraph : public view::View {
public:
    StaircaseGraph(const QuantizerProcessor& proc, std::size_t channel)
        : proc_(proc), channel_(channel) {}

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

        const QuantizeSettings settings = proc_.settings(channel_);

        // Sample densely. The treads are flat and the risers are vertical, so a
        // coarse sweep would round the corners off a shape whose corners are the
        // entire point.
        c.set_stroke_color(ui::palette::accent);
        c.set_line_width(2.0f * s);
        ui::plot(c, 512, x_of(-1.0), x_of(1.0), [&](float t) {
            const auto in = static_cast<float>(-1.0 + 2.0 * t);
            return y_of(quantize_transfer(in, settings));
        });

        const float in = proc_.display_input(channel_);
        const float out = proc_.display_output(channel_);
        c.set_fill_color(ui::palette::text);
        c.fill_circle(x_of(in), y_of(out), 3.5f * s);

        request_repaint();
    }

private:
    const QuantizerProcessor& proc_;
    std::size_t channel_;
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

        // Knobs standing in for menus, reading their own value back as a name.
        // The index is clamped, never wrapped, so a label can never name a mode
        // other than the one the DSP is running.
        auto named = [](const char* const* names, int count) {
            return [names, count](float v) {
                int i = static_cast<int>(std::lround(v));
                i = std::clamp(i, 0, count - 1);
                return std::string(names[i]);
            };
        };
        static const char* const kModeNames[] = {"Man", "Scale"};
        static const char* const kScaleNames[] = {"chrom", "maj",   "min",   "harm",
                                                  "pent+", "pent-", "blues", "whole",
                                                  "dor",   "mixo"};
        static const char* const kKeyNames[] = {"C",  "C#", "D",  "D#", "E",  "F",
                                                "F#", "G",  "G#", "A",  "A#", "B"};
        // Sign carries the mode: positive slews, negative low-passes.
        auto millis = [](float v) {
            char buf[20];
            std::snprintf(buf, sizeof(buf), "%+.0f ms", v);
            return std::string(buf);
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
            add_child(ui::channel_label(ch == 0 ? "LEFT" : "RIGHT"));

            auto graph = std::make_unique<StaircaseGraph>(proc, ch);
            graph->flex().preferred_height = 104.0f;
            graph->flex().align_self = view::FlexAlign::stretch;

            auto top = ui::row(ui::kRowGap, ui::kKnobHeight);
            add(*top, QuantizerProcessor::kMode, ch, "Mode", named(kModeNames, 2));
            add(*top, QuantizerProcessor::kSteps, ch, "Steps", whole);
            add(*top, QuantizerProcessor::kFine, ch, "Fine", number);
            add(*top, QuantizerProcessor::kOffset, ch, "Offset", number);
            add(*top, QuantizerProcessor::kTranspose, ch, "Transp", signed_whole);

            auto mid = ui::row(ui::kRowGap, ui::kKnobHeight);
            add(*mid, QuantizerProcessor::kScale, ch, "Scale",
                named(kScaleNames, kScaleCount));
            add(*mid, QuantizerProcessor::kKey, ch, "Key", named(kKeyNames, 12));
            add(*mid, QuantizerProcessor::kKeyOffset, ch, "Key Off", signed_whole);
            add(*mid, QuantizerProcessor::kSmoothMs, ch, "Smooth", millis);
            add(*mid, QuantizerProcessor::kOutputScale, ch, "Out", number);

            auto bottom = ui::row(ui::kRowGap, ui::kToggleHeight);
            auto en = ui::param_toggle(
                store_,
                static_cast<state::ParamID>(param_for(QuantizerProcessor::kEnable, ch)),
                "Enable");
            auto inv = ui::param_toggle(
                store_,
                static_cast<state::ParamID>(param_for(QuantizerProcessor::kInvert, ch)),
                "Invert");
            ui::toggle_size(*en);
            ui::toggle_size(*inv);
            bottom->add_child(std::move(en));
            bottom->add_child(std::move(inv));

            add_child(std::move(graph));
            add_child(std::move(top));
            add_child(std::move(mid));
            add_child(std::move(bottom));
        }

        // Steps divide full scale, not an octave. Scale mode needs no rail voltage
        // — set Steps so twelve of them span an octave of your oscillator and the
        // lattice is chromatic. Calibrated mode, which would do that for you,
        // needs a number nothing here has measured.
        add_child(ui::caption_label(
            "scale mode assumes 12 steps make an octave — set Steps to suit"));
    }

private:
    state::StateStore& store_;
};

}  // namespace pulp::examples::brew
