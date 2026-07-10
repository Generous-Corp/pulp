#pragma once

// Trigger's editor: the envelope, drawn, and the voltage it is putting out.
//
// Same reasoning as DC's rail and the LFO's scope. The plug-in makes no sound, so
// the shape twenty-odd knobs describe has to be visible at once, and the voltage
// actually leaving the jack has to be visible beside it — otherwise there is no way
// to tell a working patch from a silent one, and the user blames the software.
//
// The curve is `envelope_at`, the same pure function the DSP's state machine is
// tested against, so the picture cannot drift from the signal. The rail down the
// right-hand edge is the sample the DSP last emitted, which is a different thing:
// in `Gate`, `Trigger` and `Velocity` modes the curve is only a description of a
// generator nothing is listening to, and the rail says so by moving anyway.

#include "trigger_processor.hpp"

#include <brew/ui/panel.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace pulp::examples::brew {

/// One channel's envelope shape, and a rail carrying its live output voltage.
class EnvelopeScope : public view::View {
public:
    EnvelopeScope(const TriggerProcessor& proc, std::size_t channel)
        : proc_(proc), channel_(channel) {}

    void paint(canvas::Canvas& c) override {
        const float w = local_bounds().width, h = local_bounds().height;
        const float s = scale();
        const float rail_w = 14.0f * s;
        const float plot_w = std::max(w - rail_w - 4.0f * s, 1.0f);

        c.set_fill_color(ui::palette::rail);
        c.fill_rounded_rect(0, 0, w, h, 6.0f);

        const EnvelopeSpec spec = proc_.envelope_spec(channel_);
        const double attack = attack_seconds(spec);
        const double release = release_seconds(spec);
        const double span = attack + kScopeSustainSeconds + release;

        const float pad = 4.0f * s;
        auto y_of = [&](float unipolar) { return h - pad - unipolar * (h - 2.0f * pad); };

        // The note-off instant, so the sustain's length reads as a hold rather than
        // as part of the release.
        if (span > 0.0) {
            const float x = plot_w * static_cast<float>((attack + kScopeSustainSeconds) / span);
            c.set_stroke_color(ui::palette::border);
            c.set_line_width(1.0f * s);
            c.stroke_line(x, 0, x, h);
        }

        // Dim when nothing is listening: in the other three modes the envelope is
        // still running, it is just not what reaches the jack.
        const bool live = signal_uses_envelope(proc_.mode(channel_));
        c.set_stroke_color(live ? ui::palette::accent : ui::palette::text_dim);
        c.set_line_width(2.0f * s);
        // `ui::plot` hands the callback a normalized position along the span, not a
        // pixel — the x coordinate is its own business.
        constexpr int kSteps = 240;
        const double note_off = attack + kScopeSustainSeconds;
        ui::plot(c, kSteps, 0.0f, plot_w, [&](float u) {
            if (!(span > 0.0)) return y_of(spec.sustain);
            const double t = span * static_cast<double>(u);
            const float v = t <= note_off ? envelope_at(spec, t)
                                          : release_at(spec, t - note_off, spec.sustain);
            return y_of(v);
        });

        // The rail: the voltage the DSP last emitted, bipolar, from a centre line.
        const float rx = w - rail_w;
        const float mid = h * 0.5f;
        c.set_fill_color(ui::palette::lamp_off);
        c.fill_rect(rx, pad, rail_w, h - 2.0f * pad);
        const float v = std::clamp(proc_.display_value(channel_), -1.0f, 1.0f);
        const float top = mid - v * (mid - pad);
        c.set_fill_color(proc_.held(channel_) ? ui::palette::accent
                                              : ui::palette::accent_dim);
        c.fill_rect(rx, std::min(top, mid), rail_w,
                    std::max(std::abs(top - mid), 1.0f * s));
        c.set_stroke_color(ui::palette::border);
        c.set_line_width(1.0f * s);
        c.stroke_line(rx, mid, w, mid);

        request_repaint();
    }

private:
    const TriggerProcessor& proc_;
    std::size_t channel_;
};

class TriggerUi : public ui::BrewPanel {
public:
    TriggerUi(state::StateStore& store, const TriggerProcessor& proc)
        : ui::BrewPanel("Trigger", "notes and voltages become gates and envelopes"),
          store_(store) {
        auto plain = [](float v) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.2f", v);
            return std::string(buf);
        };
        auto signed_plain = [](float v) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%+.2f", v);
            return std::string(buf);
        };
        auto whole = [](float v) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.0f", v);
            return std::string(buf);
        };
        auto seconds = [](float v) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.4g s", v);
            return std::string(buf);
        };
        auto millis = [](float v) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.4g ms", v);
            return std::string(buf);
        };
        auto smooth = [](float v) {
            char buf[24];
            if (v == 0.0f) return std::string("off");
            std::snprintf(buf, sizeof(buf), "%s %.0f", v > 0.0f ? "slew" : "lpf",
                          std::abs(v));
            return std::string(buf);
        };

        // Knobs standing in for menus, reading their own value back as a name. The
        // index is clamped, never wrapped, so a label can never name a mode other
        // than the one the DSP is running.
        auto named = [](const char* const* names, int count) {
            return [names, count](float v) {
                int i = static_cast<int>(std::lround(v));
                i = std::clamp(i, 0, count - 1);
                return std::string(names[i]);
            };
        };
        static const char* const kModeNames[] = {"Gate", "Trig", "Env", "Vel"};
        static const char* const kMultNames[] = {"x0.1", "x1", "x10", "x100", "x1k"};

        auto add = [&](view::View& row, state::ParamID id, std::size_t ch,
                       const char* label, std::function<std::string(float)> fmt) {
            auto k = ui::param_knob(store_,
                                    static_cast<state::ParamID>(param_for(id, ch)), label,
                                    std::move(fmt));
            ui::knob_size(*k);
            row.add_child(std::move(k));
        };
        auto toggle = [&](view::View& row, state::ParamID id, std::size_t ch,
                          const char* label) {
            auto t = ui::param_toggle(
                store_, static_cast<state::ParamID>(param_for(id, ch)), label);
            ui::toggle_size(*t);
            row.add_child(std::move(t));
        };

        for (std::size_t ch = 0; ch < kChannelCount; ++ch) {
            add_child(ui::channel_label(ch == 0 ? "LEFT" : "RIGHT"));

            auto scope = std::make_unique<EnvelopeScope>(proc, ch);
            scope->flex().preferred_height = 76.0f;
            scope->flex().align_self = view::FlexAlign::stretch;

            auto source = ui::row(ui::kRowGap, ui::kKnobHeight);
            add(*source, TriggerProcessor::kMode, ch, "Mode",
                named(kModeNames, kTriggerSignalCount));
            add(*source, TriggerProcessor::kNote, ch, "Note", whole);
            add(*source, TriggerProcessor::kVelocityAmount, ch, "Vel", signed_plain);
            add(*source, TriggerProcessor::kSmoothMs, ch, "Smooth", smooth);
            add(*source, TriggerProcessor::kLengthMs, ch, "Length", millis);

            auto jack = ui::row(ui::kRowGap, ui::kKnobHeight);
            add(*jack, TriggerProcessor::kCvThreshold, ch, "Thresh", signed_plain);
            add(*jack, TriggerProcessor::kVoltageMin, ch, "V Min", signed_plain);
            add(*jack, TriggerProcessor::kVoltageMax, ch, "V Max", signed_plain);
            add(*jack, TriggerProcessor::kOverrideValue, ch, "Value", signed_plain);
            add(*jack, TriggerProcessor::kMult, ch, "Mult",
                named(kMultNames, kMultiplierCount));

            // Each stage's level, time, and curve, in the order the envelope walks
            // them. A3 falls to `Sustain` and R2 falls to zero, so neither has a
            // level of its own to show.
            auto stage_a = ui::row(ui::kRowGap, ui::kKnobHeight);
            add(*stage_a, TriggerProcessor::kLevelA1, ch, "Lvl A1", plain);
            add(*stage_a, TriggerProcessor::kTimeA1, ch, "Time A1", seconds);
            add(*stage_a, TriggerProcessor::kCurveA1, ch, "Crv A1", signed_plain);
            add(*stage_a, TriggerProcessor::kLevelA2, ch, "Lvl A2", plain);
            add(*stage_a, TriggerProcessor::kTimeA2, ch, "Time A2", seconds);

            auto stage_b = ui::row(ui::kRowGap, ui::kKnobHeight);
            add(*stage_b, TriggerProcessor::kCurveA2, ch, "Crv A2", signed_plain);
            add(*stage_b, TriggerProcessor::kTimeA3, ch, "Time A3", seconds);
            add(*stage_b, TriggerProcessor::kCurveA3, ch, "Crv A3", signed_plain);
            add(*stage_b, TriggerProcessor::kSustain, ch, "Sustain", plain);
            add(*stage_b, TriggerProcessor::kLevelR1, ch, "Lvl R1", plain);

            auto stage_c = ui::row(ui::kRowGap, ui::kKnobHeight);
            add(*stage_c, TriggerProcessor::kTimeR1, ch, "Time R1", seconds);
            add(*stage_c, TriggerProcessor::kCurveR1, ch, "Crv R1", signed_plain);
            add(*stage_c, TriggerProcessor::kTimeR2, ch, "Time R2", seconds);
            add(*stage_c, TriggerProcessor::kCurveR2, ch, "Crv R2", signed_plain);

            auto switches = ui::row(ui::kRowGap, ui::kToggleHeight);
            toggle(*switches, TriggerProcessor::kAnyNote, ch, "Any Note");
            toggle(*switches, TriggerProcessor::kCvEnable, ch, "CV Trig");
            toggle(*switches, TriggerProcessor::kOverride, ch, "Override");
            toggle(*switches, TriggerProcessor::kExponential, ch, "Exp");
            toggle(*switches, TriggerProcessor::kResetToZero, ch, "RTZ");

            add_child(std::move(scope));
            add_child(std::move(source));
            add_child(std::move(jack));
            add_child(std::move(stage_a));
            add_child(std::move(stage_b));
            add_child(std::move(stage_c));
            add_child(std::move(switches));
        }

        add_child(ui::caption_label("Voltage Min above Max inverts the signal:"));
        add_child(ui::caption_label("there is no Invert here"));
        add_child(ui::caption_label("Level A1 at zero makes Time A1 a delay"));
        add_child(ui::caption_label("before the envelope starts"));
    }

private:
    state::StateStore& store_;
};

}  // namespace pulp::examples::brew
