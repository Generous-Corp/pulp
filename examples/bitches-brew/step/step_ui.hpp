#pragma once

// Step LFO's editor: the pattern, drawn as bars you can drag, and the register,
// drawn as lamps you cannot.
//
// Eight knobs would be the cheap way to expose eight levels, and it would be
// unusable — a pattern is a shape, and a shape has to be seen at once. So the
// bars are the control: drag one to set its level, and the bar that is currently
// playing lights up. Steps outside the window are drawn recessed, because their
// values are still in the preset and moving `Start` brings them straight back.
//
// The lamps below the bars are the shift register's bits, bit zero on the left,
// with the DAC's window outlined. Bit zero is the DAC's most significant and the
// one a `Rotate` of zero starts from; the bit about to fall off the end and be fed
// back is the rightmost. When `Register` is on the lamps are what the plug-in is
// playing and the bars are not — so the lamps are the only honest readout, and
// drawing them is not decoration.

#include "step_processor.hpp"

#include <brew/ui/panel.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace pulp::examples::brew {

/// Eight bars from a shared zero line, bipolar. Dragging sets a level; the
/// playing step is tinted.
class StepBars : public view::View {
public:
    StepBars(state::StateStore& store, const StepProcessor& proc)
        : store_(store), proc_(proc), edit_(store) {}

    void paint(canvas::Canvas& c) override {
        const float w = local_bounds().width, h = local_bounds().height;
        const float s = scale();
        const float mid = h * 0.5f;

        c.set_fill_color(ui::palette::rail);
        c.fill_rounded_rect(0, 0, w, h, 6.0f);

        const int start = proc_.start_step();
        const int window = proc_.window();
        const int playing = proc_.display_step();
        const bool from_register = proc_.register_on();
        const float slot = w / static_cast<float>(kMaxSequencerSteps);
        const float pad = 3.0f * s;

        for (int i = 0; i < kMaxSequencerSteps; ++i) {
            const float x = slot * static_cast<float>(i);

            // Membership of the window wraps: Start 7 with Length 3 plays steps
            // 7, 0, 1. `wrap_index` is the same arithmetic the DSP uses.
            const bool live = static_cast<int>(wrap_index(i - start, kMaxSequencerSteps)) <
                              window;
            const float v = store_.get_value(StepProcessor::step_param(i));
            const float top = mid - v * (mid - pad);

            // A tinted bar under the register means "this is not what is playing".
            const canvas::Color live_col = from_register ? ui::palette::negative
                                                         : ui::palette::accent_dim;
            c.set_fill_color(live ? (i == playing && !from_register ? ui::palette::accent
                                                                    : live_col)
                                  : ui::palette::border);
            const float y0 = std::min(top, mid), y1 = std::max(top, mid);
            c.fill_rect(x + pad, y0, slot - 2.0f * pad, std::max(y1 - y0, 1.0f * s));
        }

        c.set_stroke_color(ui::palette::border);
        c.set_line_width(1.0f * s);
        c.stroke_line(0, mid, w, mid);

        request_repaint();
    }

    void on_mouse_down(view::Point p) override {
        dragging_ = step_under(p);
        if (dragging_ >= 0) {
            edit_.begin(StepProcessor::step_param(dragging_));
            write(p);
        }
    }

    void on_mouse_drag(view::Point p) override {
        // Keep writing the step the gesture *started* on. Re-picking under the
        // cursor would let a sloppy vertical drag rewrite its neighbours, and the
        // gesture already belongs to one parameter for undo grouping.
        if (dragging_ >= 0) write(p);
    }

    void on_mouse_up(view::Point) override {
        if (dragging_ >= 0) edit_.finish();
        dragging_ = -1;
    }

private:
    [[nodiscard]] int step_under(view::Point p) const noexcept {
        const float w = local_bounds().width;
        if (w <= 0.0f || p.x < 0.0f || p.x >= w) return -1;
        const int i = static_cast<int>(p.x / (w / static_cast<float>(kMaxSequencerSteps)));
        return std::clamp(i, 0, kMaxSequencerSteps - 1);
    }

    void write(view::Point p) {
        const float h = local_bounds().height;
        if (h <= 0.0f) return;
        const float v = std::clamp(1.0f - 2.0f * (p.y / h), -1.0f, 1.0f);
        edit_.set(StepProcessor::step_param(dragging_), v);
    }

    state::StateStore& store_;
    const StepProcessor& proc_;
    state::ParameterEdit edit_;
    int dragging_ = -1;
};

/// The register's bits, bit zero on the left, with the DAC's window outlined.
/// Dark when `Register` is off, because then it is driving nothing.
class RegisterLamps : public view::View {
public:
    explicit RegisterLamps(const StepProcessor& proc) : proc_(proc) {}

    void paint(canvas::Canvas& c) override {
        const float w = local_bounds().width, h = local_bounds().height;
        const float s = scale();

        c.set_fill_color(ui::palette::rail);
        c.fill_rounded_rect(0, 0, w, h, 4.0f);

        const auto rs = proc_.register_settings();
        const int n = clamp_register_length(rs.length);
        const std::uint64_t bits = proc_.display_bits();
        const bool live = proc_.register_on();

        const float slot = w / static_cast<float>(n);
        const float pad = std::min(1.5f * s, slot * 0.25f);

        // Bit 0 leftmost: it is the DAC's most significant, and where a `Rotate`
        // of zero starts reading. The bit about to fall off the end is rightmost.
        for (int bit = 0; bit < n; ++bit) {
            const bool on = live && ((bits >> bit) & 1ULL) != 0ULL;
            c.set_fill_color(on ? ui::palette::accent : ui::palette::lamp_off);
            c.fill_rect(slot * static_cast<float>(bit) + pad, pad, slot - 2.0f * pad,
                        h - 2.0f * pad);
        }

        // The window the DAC reads, from `Rotate` upward. It wraps, so it is drawn
        // bit by bit rather than as one rectangle.
        const auto dac = proc_.dac_settings();
        c.set_stroke_color(live ? ui::palette::text : ui::palette::text_dim);
        c.set_line_width(1.0f * s);
        for (int k = 0; k < std::clamp(dac.bits, 1, kMaxDacBits) && k < n; ++k) {
            const float x = slot * static_cast<float>(wrap_bit(dac.rotate + k, n));
            c.stroke_rect(x + pad * 0.5f, pad * 0.5f, slot - pad, h - pad);
        }

        request_repaint();
    }

private:
    const StepProcessor& proc_;
};

class StepUi : public ui::BrewPanel {
public:
    StepUi(state::StateStore& store, const StepProcessor& proc)
        : ui::BrewPanel("Step LFO", "an eight-step pattern on the LFO's clock"),
          store_(store) {
        // Every knob reads its value through the parameter's own formatter, so a
        // readout here and the host's automation lane are the same string.
        auto add = [&](view::View& row, state::ParamID id, const char* label) {
            auto k = ui::param_knob(store_, id, label);
            ui::knob_size(*k);
            row.add_child(std::move(k));
        };
        auto toggle = [&](view::View& row, state::ParamID id, const char* label) {
            auto t = ui::param_toggle(store_, id, label);
            ui::toggle_size(*t);
            row.add_child(std::move(t));
        };

        auto bars = std::make_unique<StepBars>(store_, proc);
        bars->flex().preferred_height = 110.0f;
        bars->flex().align_self = view::FlexAlign::stretch;

        auto lamps = std::make_unique<RegisterLamps>(proc);
        lamps->flex().preferred_height = 18.0f;
        lamps->flex().align_self = view::FlexAlign::stretch;

        // Rate. Speed/Mult feed the hertz modes, Beats/Div the beat modes; both are
        // always shown, so changing Sync never makes a knob appear under the mouse.
        auto rate = ui::row(ui::kRowGap, ui::kKnobHeight);
        add(*rate, StepProcessor::kSyncMode, "Sync");
        add(*rate, StepProcessor::kSpeedHz, "Speed");
        add(*rate, StepProcessor::kMultiplier, "Mult");
        add(*rate, StepProcessor::kBeats, "Beats");
        add(*rate, StepProcessor::kDivisor, "Div");

        // The window. `Bounds` chooses which two of Start/Length/End are read.
        auto bounds = ui::row(ui::kRowGap, ui::kKnobHeight);
        add(*bounds, StepProcessor::kStart, "Start");
        add(*bounds, StepProcessor::kLength, "Length");
        add(*bounds, StepProcessor::kEnd, "End");
        add(*bounds, StepProcessor::kLengthMode, "Bounds");
        add(*bounds, StepProcessor::kInterpolation, "Interp");

        auto shape = ui::row(ui::kRowGap, ui::kKnobHeight);
        add(*shape, StepProcessor::kGlide, "Glide");
        add(*shape, StepProcessor::kGate, "Gate");
        add(*shape, StepProcessor::kPhaseDegrees, "Phase");
        add(*shape, StepProcessor::kSwingPercent, "Swing");
        add(*shape, StepProcessor::kAsymmetry, "Asym");

        auto output = ui::row(ui::kRowGap, ui::kKnobHeight);
        add(*output, StepProcessor::kLevelOffset, "Offset");
        add(*output, StepProcessor::kSmoothMs, "Smooth");
        add(*output, StepProcessor::kRandom, "Random");
        add(*output, StepProcessor::kSeed, "Seed");
        add(*output, StepProcessor::kOutputScale, "Out");

        // The register. `Rand` is signed: both ends lock a pattern, the middle
        // never repeats.
        auto reg = ui::row(ui::kRowGap, ui::kKnobHeight);
        add(*reg, StepProcessor::kRegisterLength, "Bits");
        add(*reg, StepProcessor::kDacBits, "DAC");
        add(*reg, StepProcessor::kRandomness, "Rand");
        add(*reg, StepProcessor::kRotate, "Rotate");
        add(*reg, StepProcessor::kDacScale, "Scale");

        // The DAC's ladder, most significant first.
        auto weights_hi = ui::row(ui::kRowGap, ui::kKnobHeight);
        auto weights_lo = ui::row(ui::kRowGap, ui::kKnobHeight);
        static const char* const kWeightLabels[] = {"W1", "W2", "W3", "W4",
                                                    "W5", "W6", "W7", "W8"};
        for (int i = 0; i < kMaxDacBits; ++i)
            add(i < 4 ? *weights_hi : *weights_lo, StepProcessor::weight_param(i), kWeightLabels[i]);

        auto inputs = ui::row(ui::kRowGap, ui::kKnobHeight);
        add(*inputs, StepProcessor::kInputLeft, "In L");
        add(*inputs, StepProcessor::kInputRight, "In R");
        add(*inputs, StepProcessor::kSignalMode, "Signal");

        auto switches_a = ui::row(ui::kRowGap, ui::kToggleHeight);
        toggle(*switches_a, StepProcessor::kTriplet, "Triplet");
        toggle(*switches_a, StepProcessor::kSwingUnit, "16ths");
        toggle(*switches_a, StepProcessor::kSpeedMode, "Per Step");
        toggle(*switches_a, StepProcessor::kInvert, "Invert");

        auto switches_b = ui::row(ui::kRowGap, ui::kToggleHeight);
        toggle(*switches_b, StepProcessor::kRegisterOn, "Register");
        toggle(*switches_b, StepProcessor::kSetNext, "Set Next");
        toggle(*switches_b, StepProcessor::kAutoScale, "Auto");
        toggle(*switches_b, StepProcessor::kRange, "Unipolar");

        add_child(std::move(bars));
        add_child(std::move(lamps));
        add_child(std::move(rate));
        add_child(std::move(bounds));
        add_child(std::move(shape));
        add_child(std::move(output));
        add_child(std::move(reg));
        add_child(std::move(weights_hi));
        add_child(std::move(weights_lo));
        add_child(std::move(inputs));
        add_child(std::move(switches_a));
        add_child(std::move(switches_b));

        add_child(ui::caption_label(
            "Per Step makes one cycle one step; off, one cycle"));
        add_child(ui::caption_label("is the whole window"));
        add_child(ui::caption_label("Register drives the steps: Rand +1 and -1 both"));
        add_child(ui::caption_label("lock a pattern, 0 never repeats"));
    }

private:
    state::StateStore& store_;
};

}  // namespace pulp::examples::brew
