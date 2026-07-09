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
        auto y_of = [&](int i, double quad) {
            const double beat = rate * static_cast<double>(i) / steps;
            const float v = proc_.value_at(beat, quad);
            return mid - v * (mid - 4.0f * s);
        };

        auto trace = [&](double quad, canvas::Color col, float width) {
            c.set_stroke_color(col);
            c.set_line_width(width * s);
            for (int i = 1; i <= steps; ++i) {
                const float x0 = w * static_cast<float>(i - 1) / steps;
                const float x1 = w * static_cast<float>(i) / steps;
                c.stroke_line(x0, y_of(i - 1, quad), x1, y_of(i, quad));
            }
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
        : ui::BrewPanel("LFO", "a tempo-locked modulation source, plus quadrature"),
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
        auto shape_name = [](float v) {
            switch (waveform_from_param(v)) {
                case Waveform::sine: return std::string("sine");
                case Waveform::triangle: return std::string("tri");
                case Waveform::saw_up: return std::string("saw");
                case Waveform::saw_down: return std::string("ramp");
                case Waveform::square: return std::string("sqr");
            }
            return std::string("?");
        };

        auto scope = std::make_unique<LfoScope>(
            proc, [&proc] { return proc.display_phase(); });
        scope->flex().preferred_height = 88.0f;
        scope->flex().align_self = view::FlexAlign::stretch;

        auto knobs = ui::row(10.0f, 84.0f);
        auto add = [&](state::ParamID id, const char* label,
                       std::function<std::string(float)> fmt) {
            auto k = ui::param_knob(store_, id, label, std::move(fmt));
            ui::fixed_size(*k, 88.0f, 84.0f);
            knobs->add_child(std::move(k));
        };
        add(LfoProcessor::kWaveform, "Shape", shape_name);
        add(LfoProcessor::kBeatsPerCycle, "Rate", beats);
        add(LfoProcessor::kPhaseDegrees, "Phase", degrees);

        auto bottom = ui::row(14.0f, 52.0f);
        auto uni = ui::param_toggle(store_, LfoProcessor::kUnipolar, "Unipolar");
        auto inv = ui::param_toggle(store_, LfoProcessor::kInvert, "Invert");
        ui::fixed_size(*uni, 78.0f, 50.0f);
        ui::fixed_size(*inv, 78.0f, 50.0f);
        bottom->add_child(std::move(uni));
        bottom->add_child(std::move(inv));

        add_child(std::move(scope));
        add_child(std::move(knobs));
        add_child(std::move(bottom));
        add_child(ui::caption_label(
            "orange is the output, blue is the quadrature a quarter cycle ahead"));
    }

private:
    state::StateStore& store_;
};

}  // namespace pulp::examples::brew
