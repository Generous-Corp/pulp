#pragma once

// LFO's editor: the shape, drawn.
//
// Same reasoning as DC's rail and Sync's lamps. The plug-in emits no sound, so
// the only way to know what voltage it is putting out is to draw it. The scope
// shows one full cycle of the selected shape, the quadrature partner behind it,
// and a marker at the phase the DSP last reached — so a stopped transport, a
// wrong rate, or a bypassed instance are all visible at a glance rather than
// diagnosed by patching a scope to the interface.

#include "lfo_processor.hpp"

#include <brew/ui/panel.hpp>

#include <cstdio>
#include <functional>
#include <string>
#include <utility>

namespace pulp::examples::brew {

/// Draws one cycle of the LFO's shape, plus its quadrature partner and a phase
/// marker. Reads the same `value_at` the DSP uses, so the picture cannot drift
/// from the signal.
class LfoScope : public view::View {
public:
    LfoScope(const LfoProcessor& proc, std::function<float()> phase)
        : proc_(proc), phase_(std::move(phase)) {}

    void paint(canvas::Canvas& c) override {
        const float w = local_bounds().width, h = local_bounds().height;
        const float s = scale();
        const float mid = h * 0.5f;

        c.set_fill_color(ui::palette::rail);
        c.fill_rounded_rect(0, 0, w, h, 6.0f);

        c.set_stroke_color(ui::palette::border);
        c.set_line_width(1.0f * s);
        c.stroke_line(0, mid, w, mid);

        // Sample the plug-in's own value function across one cycle. `value_at`
        // takes beats, so one cycle is exactly `rate` beats wide.
        const double rate =
            static_cast<double>(proc_.state().get_value(LfoProcessor::kBeatsPerCycle));
        const int steps = 160;
        // `t` is the fraction of the cycle, straight from the plotter. Recovering
        // an integer sample index from it would round the last point back onto the
        // previous one.
        auto y_of = [&](float t, double quad) {
            const float v = proc_.value_at(rate * static_cast<double>(t), quad);
            return mid - v * (mid - 4.0f * s);
        };

        auto trace = [&](double quad, canvas::Color col, float width) {
            c.set_stroke_color(col);
            c.set_line_width(width * s);
            ui::plot(c, steps, 0.0f, w, [&](float t) { return y_of(t, quad); });
        };

        trace(kQuadratureOffset, ui::palette::negative, 1.0f);
        trace(0.0, ui::palette::accent, 2.0f);

        // A negative phase means the DSP has not run — hide the marker rather
        // than park it at zero and imply the LFO is sitting at the start.
        const float p = phase_ ? phase_() : -1.0f;
        if (p >= 0.0f) {
            const float x = w * p;
            c.set_stroke_color(ui::palette::text_dim);
            c.set_line_width(1.0f * s);
            c.stroke_line(x, 0, x, h);
        }

        request_repaint();
    }

private:
    const LfoProcessor& proc_;
    std::function<float()> phase_;
};

class LfoUi : public ui::BrewPanel {
public:
    explicit LfoUi(state::StateStore& store, const LfoProcessor& proc)
        : ui::BrewPanel("LFO", "a modulation source, tempo-locked or free, plus quadrature"),
          store_(store) {
        auto beats = [](float v) {
            char buf[20];
            if (v < 1.0f)
                std::snprintf(buf, sizeof(buf), "1/%.0f", 1.0f / v);
            else
                std::snprintf(buf, sizeof(buf), "%.2g bt", v);
            return std::string(buf);
        };
        auto degrees = [](float v) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.0f°", v);
            return std::string(buf);
        };
        auto depth = [](float v) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%+.2f", v);
            return std::string(buf);
        };

        auto scope = std::make_unique<LfoScope>(
            proc, [&proc] { return proc.display_phase(); });
        scope->flex().preferred_height = 88.0f;
        scope->flex().align_self = view::FlexAlign::stretch;

        auto hertz = [](float v) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.4g Hz", v);
            return std::string(buf);
        };

        auto add = [&](view::View& row, state::ParamID id, const char* label,
                       std::function<std::string(float)> fmt, float w) {
            auto k = ui::param_knob(store_, id, label, std::move(fmt));
            ui::fixed_size(*k, w, 80.0f);
            row.add_child(std::move(k));
        };

        // The four depths are summed, not selected. Sine at full and the rest at
        // zero is the single-shape behaviour this replaced.
        auto mix = ui::row(6.0f, 80.0f);
        add(*mix, LfoProcessor::kSine, "Sine", depth, 76.0f);
        add(*mix, LfoProcessor::kTriangle, "Tri", depth, 76.0f);
        add(*mix, LfoProcessor::kSaw, "Saw", depth, 76.0f);
        add(*mix, LfoProcessor::kSquare, "Sqr", depth, 76.0f);

        auto timing = ui::row(6.0f, 80.0f);
        add(*timing, LfoProcessor::kBeatsPerCycle, "Rate", beats, 76.0f);
        add(*timing, LfoProcessor::kPhaseDegrees, "Phase", degrees, 76.0f);
        add(*timing, LfoProcessor::kAsymmetry, "Asym", depth, 76.0f);
        add(*timing, LfoProcessor::kPulseWidth, "PW", depth, 76.0f);

        auto shaping = ui::row(6.0f, 80.0f);
        add(*shaping, LfoProcessor::kRandom, "Random", depth, 76.0f);
        add(*shaping, LfoProcessor::kOffset, "Offset", depth, 76.0f);
        add(*shaping, LfoProcessor::kOutputScale, "Out", depth, 76.0f);
        // Only one of Rate and Free is live at a time. Both are always shown, so
        // flipping the switch never makes a knob appear where the mouse already is.
        add(*shaping, LfoProcessor::kFreeHz, "Free", hertz, 76.0f);

        auto bottom = ui::row(14.0f, 52.0f);
        auto free_run = ui::param_toggle(store_, LfoProcessor::kRateMode, "Free Run");
        auto inv = ui::param_toggle(store_, LfoProcessor::kInvert, "Invert");
        ui::fixed_size(*free_run, 84.0f, 50.0f);
        ui::fixed_size(*inv, 78.0f, 50.0f);
        bottom->add_child(std::move(free_run));
        bottom->add_child(std::move(inv));

        add_child(std::move(scope));
        add_child(std::move(mix));
        add_child(std::move(timing));
        add_child(std::move(shaping));
        add_child(std::move(bottom));
        add_child(ui::caption_label(
            "orange is the output, blue is the quadrature a quarter cycle ahead"));
        add_child(ui::caption_label(
            "Free Run reads Free in hertz, and ignores the tempo"));
    }

private:
    state::StateStore& store_;
};

}  // namespace pulp::examples::brew
