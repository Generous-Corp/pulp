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

        auto percent = [](float v) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.1f%%", v);
            return std::string(buf);
        };

        auto millis = [](float v) {
            char buf[24];
            if (v == 0.0f) return std::string("off");
            std::snprintf(buf, sizeof(buf), "%s %.0f ms", v > 0.0f ? "slew" : "lpf",
                          std::abs(v));
            return std::string(buf);
        };

        auto hertz = [](float v) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.4g Hz", v);
            return std::string(buf);
        };

        auto add = [&](view::View& row, state::ParamID id, const char* label,
                       std::function<std::string(float)> fmt) {
            auto k = ui::param_knob(store_, id, label, std::move(fmt));
            ui::knob_size(*k);
            row.add_child(std::move(k));
        };

        // The four depths are summed, not selected. Sine at full and the rest at
        // zero is the single-shape behaviour this replaced.
        auto mix = ui::row(ui::kRowGap, ui::kKnobHeight);
        add(*mix, LfoProcessor::kSine, "Sine", depth);
        add(*mix, LfoProcessor::kTriangle, "Tri", depth);
        add(*mix, LfoProcessor::kSaw, "Saw", depth);
        add(*mix, LfoProcessor::kSquare, "Sqr", depth);

        // Rate and Free are never both live. Both are always shown, so flipping
        // Free Run never makes a knob appear where the mouse already is.
        auto timing = ui::row(ui::kRowGap, ui::kKnobHeight);
        add(*timing, LfoProcessor::kBeatsPerCycle, "Rate", beats);
        add(*timing, LfoProcessor::kFreeHz, "Free", hertz);
        add(*timing, LfoProcessor::kPhaseDegrees, "Phase", degrees);
        add(*timing, LfoProcessor::kAsymmetry, "Asym", depth);
        add(*timing, LfoProcessor::kPulseWidth, "PW", depth);

        // Swing warps the beat timeline; Smooth is the only stateful control.
        auto shaping = ui::row(ui::kRowGap, ui::kKnobHeight);
        add(*shaping, LfoProcessor::kRandom, "Random", depth);
        add(*shaping, LfoProcessor::kOffset, "Offset", depth);
        add(*shaping, LfoProcessor::kOutputScale, "Out", depth);
        add(*shaping, LfoProcessor::kSwingPercent, "Swing", percent);
        add(*shaping, LfoProcessor::kSmoothMs, "Smooth", millis);

        auto bottom = ui::row(ui::kRowGap, ui::kToggleHeight);
        auto free_run = ui::param_toggle(store_, LfoProcessor::kRateMode, "Free Run");
        auto sixteenths = ui::param_toggle(store_, LfoProcessor::kSwingUnit, "16ths");
        auto inv = ui::param_toggle(store_, LfoProcessor::kInvert, "Invert");
        ui::toggle_size(*free_run);
        ui::toggle_size(*sixteenths);
        ui::toggle_size(*inv);
        bottom->add_child(std::move(free_run));
        bottom->add_child(std::move(sixteenths));
        bottom->add_child(std::move(inv));

        add_child(std::move(scope));
        add_child(std::move(mix));
        add_child(std::move(timing));
        add_child(std::move(shaping));
        add_child(std::move(bottom));
        add_child(ui::caption_label(
            "orange is the output, blue is the quadrature a quarter cycle ahead"));
        add_child(ui::caption_label(
            "Free Run ignores the tempo and Swing; 50% swing is straight"));
        add_child(ui::caption_label(
            "the scope draws the unsmoothed shape"));
    }

private:
    state::StateStore& store_;
};

}  // namespace pulp::examples::brew
