#pragma once

// LFO's editor: two shapes, drawn.
//
// Same reasoning as DC's rail and Sync's lamps. The plug-in emits no sound, so the
// only way to know what voltage it is putting out is to draw it. Each channel's
// scope shows one full cycle of its own shape and a marker at the phase the DSP
// last reached — so a stopped transport, a wrong rate, or a bypassed instance are
// all visible at a glance rather than diagnosed by patching a scope to the
// interface.
//
// When a channel is locked to the other with `Quad`, its scope also draws the
// leader faintly behind it. That is the whole content of the mode: two traces a
// quarter cycle apart, which is a circle once they reach two CV inputs.

#include "lfo_processor.hpp"

#include <brew/ui/panel.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace pulp::examples::brew {

/// Draws one cycle of one channel's shape, and a phase marker. Reads the same
/// `value_at_cycles` the DSP uses, so the picture cannot drift from the signal.
class LfoScope : public view::View {
public:
    LfoScope(const LfoProcessor& proc, std::size_t channel)
        : proc_(proc), channel_(channel) {}

    void paint(canvas::Canvas& c) override {
        const float w = local_bounds().width, h = local_bounds().height;
        const float s = scale();
        const float mid = h * 0.5f;

        c.set_fill_color(ui::palette::rail);
        c.fill_rounded_rect(0, 0, w, h, 6.0f);

        c.set_stroke_color(ui::palette::border);
        c.set_line_width(1.0f * s);
        c.stroke_line(0, mid, w, mid);

        // The horizontal axis is one cycle, whatever a cycle currently means. That
        // is the one coordinate every sync mode shares, so the scope needs to know
        // nothing about tempo, hertz, or the transport.
        constexpr int kSteps = 160;
        auto trace = [&](std::size_t ch, canvas::Color col, float width) {
            c.set_stroke_color(col);
            c.set_line_width(width * s);
            ui::plot(c, kSteps, 0.0f, w, [&](float t) {
                const float v = proc_.value_at_cycles(ch, static_cast<double>(t));
                return mid - v * (mid - 4.0f * s);
            });
        };

        // A follower's leader, faintly, so the lock is visible rather than asserted.
        if (channel_ != 0 && proc_.sync_mode(channel_) == SyncMode::quadrature)
            trace(0, ui::palette::negative, 1.0f);
        trace(channel_, ui::palette::accent, 2.0f);

        // A negative phase means the DSP has not run — hide the marker rather than
        // park it at zero and imply the LFO is sitting at the start.
        const float p = proc_.display_phase(channel_);
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
    std::size_t channel_;
};

class LfoUi : public ui::BrewPanel {
public:
    LfoUi(state::StateStore& store, const LfoProcessor& proc)
        : ui::BrewPanel("LFO", "two modulation sources, tempo-locked or free"),
          store_(store) {
        auto beats = [](float v) {
            char buf[20];
            std::snprintf(buf, sizeof(buf), "%.4g", v);
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
        auto whole = [](float v) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.0f", v);
            return std::string(buf);
        };
        auto percent = [](float v) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.1f%%", v);
            return std::string(buf);
        };
        auto millis = [](float v) {
            char buf[24];
            if (v == 0.0f) return std::string("off");
            std::snprintf(buf, sizeof(buf), "%s %.0f", v > 0.0f ? "slew" : "lpf",
                          std::abs(v));
            return std::string(buf);
        };
        auto hertz = [](float v) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.4g", v);
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
        static const char* const kSyncNames[] = {"Free",  "Tempo", "Trans", "Quad",
                                                 "St/Sp", "Trans2", "Free2", "Free3"};
        static const char* const kMultNames[] = {"x0.1", "x1", "x10", "x100", "x1k"};
        static const char* const kDivNames[] = {"1/1", "1/2", "1/4", "1/8", "1/16"};
        static const char* const kInputNames[] = {"Off", "Add", "Mul", "Comb"};

        auto add = [&](view::View& row, state::ParamID id, std::size_t ch,
                       const char* label, std::function<std::string(float)> fmt) {
            auto k = ui::param_knob(store_,
                                    static_cast<state::ParamID>(param_for(id, ch)),
                                    label, std::move(fmt));
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

            auto scope = std::make_unique<LfoScope>(proc, ch);
            scope->flex().preferred_height = 80.0f;
            scope->flex().align_self = view::FlexAlign::stretch;

            // Time. Speed/Mult feed the hertz modes, Beats/Div the beat modes; both
            // are always shown, so changing Sync never makes a knob appear where the
            // mouse already is.
            auto timing = ui::row(ui::kRowGap, ui::kKnobHeight);
            add(*timing, LfoProcessor::kSyncMode, ch, "Sync",
                named(kSyncNames, kSyncModeCount));
            add(*timing, LfoProcessor::kSpeedHz, ch, "Speed", hertz);
            add(*timing, LfoProcessor::kMultiplier, ch, "Mult",
                named(kMultNames, kMultiplierCount));
            add(*timing, LfoProcessor::kBeats, ch, "Beats", beats);
            add(*timing, LfoProcessor::kDivisor, ch, "Div",
                named(kDivNames, kNoteUnitCount));

            // The four depths are summed, not selected. Sine at full and the rest at
            // zero is the single-shape behaviour this replaced.
            auto mix = ui::row(ui::kRowGap, ui::kKnobHeight);
            add(*mix, LfoProcessor::kSine, ch, "Sine", depth);
            add(*mix, LfoProcessor::kTriangle, ch, "Tri", depth);
            add(*mix, LfoProcessor::kSaw, ch, "Saw", depth);
            add(*mix, LfoProcessor::kSquare, ch, "Sqr", depth);
            add(*mix, LfoProcessor::kPulseWidth, ch, "PW", depth);

            // Random steps once a cycle, Noise once a sample; both are hashes, so
            // both survive a bounce. Seed rerolls them.
            auto chaos = ui::row(ui::kRowGap, ui::kKnobHeight);
            add(*chaos, LfoProcessor::kRandom, ch, "Random", depth);
            add(*chaos, LfoProcessor::kNoise, ch, "Noise", depth);
            add(*chaos, LfoProcessor::kSeed, ch, "Seed", whole);
            add(*chaos, LfoProcessor::kAsymmetry, ch, "Asym", depth);
            add(*chaos, LfoProcessor::kOffset, ch, "Offset", depth);

            auto shaping = ui::row(ui::kRowGap, ui::kKnobHeight);
            add(*shaping, LfoProcessor::kPhaseDegrees, ch, "Phase", degrees);
            add(*shaping, LfoProcessor::kSwingPercent, ch, "Swing", percent);
            add(*shaping, LfoProcessor::kSmoothMs, ch, "Smooth", millis);
            add(*shaping, LfoProcessor::kInputMode, ch, "Input",
                named(kInputNames, kInputModeCount));
            add(*shaping, LfoProcessor::kOutputScale, ch, "Out", depth);

            auto switches = ui::row(ui::kRowGap, ui::kToggleHeight);
            toggle(*switches, LfoProcessor::kTriplet, ch, "Triplet");
            toggle(*switches, LfoProcessor::kSwingUnit, ch, "16ths");
            toggle(*switches, LfoProcessor::kResetByNote, ch, "Reset");
            toggle(*switches, LfoProcessor::kInvert, ch, "Invert");

            add_child(std::move(scope));
            add_child(std::move(timing));
            add_child(std::move(mix));
            add_child(std::move(chaos));
            add_child(std::move(shaping));
            add_child(std::move(switches));
        }

        add_child(ui::caption_label(
            "Free and Tempo run on wall-clock time: the two modes"));
        add_child(ui::caption_label("a bounce cannot reproduce"));
        add_child(ui::caption_label("Quad locks the right channel to the left;"));
        add_child(ui::caption_label("90° traces a circle"));
    }

private:
    state::StateStore& store_;
};

}  // namespace pulp::examples::brew
