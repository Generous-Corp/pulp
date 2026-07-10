#pragma once

// DC's editor: the controls, and proof of where their sum went.
//
// The readout is the point. A CV plug-in produces no sound and draws no meter in
// the host, so without a rail the user cannot distinguish "holding +0.5 full
// scale" from "cable unplugged". The rail shows normalized full scale, never
// volts — the plug-in does not know the interface's rail voltage, and printing a
// number it cannot know would be a lie the user wires into a modular.
//
// Both readouts show the sample the DSP last emitted, not the value the knobs
// ask for. Those differ whenever the input bus or the smoother is doing anything,
// and it is the emitted sample that reaches the jack.

#include "dc_processor.hpp"

#include <brew/ui/panel.hpp>

#include <array>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <string>
#include <utility>

namespace pulp::examples::brew {

class DcUi : public ui::BrewPanel {
public:
    DcUi(state::StateStore& store, const DcProcessor& proc)
        : ui::BrewPanel("DC", "a constant control voltage"),
          store_(store), proc_(proc) {
        auto pct = [](float v) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.0f%%", v * 100.0f);
            return std::string(buf);
        };
        auto signed_number = [](float v) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%+.3f", v);
            return std::string(buf);
        };
        auto number = [](float v) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.3f", v);
            return std::string(buf);
        };
        // Sign carries the mode, so it must be shown: positive slews, negative
        // low-passes, and "0.0 ms" means the smoother is out of the circuit.
        auto millis = [](float v) {
            char buf[20];
            std::snprintf(buf, sizeof(buf), "%+.1f ms", v);
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

        // One block per channel. They are two unrelated control voltages, not a
        // stereo pair, so each gets its own controls, its own readout, and its own
        // rail — and the block is labelled, because sending a voltage down the
        // wrong cable is the failure this layout exists to prevent.
        for (std::size_t ch = 0; ch < kChannelCount; ++ch) {
            add_child(ui::channel_label(ch == 0 ? "LEFT" : "RIGHT"));

            // Labels are abbreviated to the knob's width. A Knob centres its label
            // on its own box, so a longer one silently overlaps its neighbours.
            auto levels = ui::row(ui::kRowGap, ui::kKnobHeight);
            add(*levels, DcProcessor::kValue, ch, "Value", signed_number);
            add(*levels, DcProcessor::kUnipolar, ch, "Unipolar", number);
            add(*levels, DcProcessor::kMultiplier, ch, "Mult", signed_number);
            add(*levels, DcProcessor::kSmoothMs, ch, "Smooth", millis);

            auto inputs = ui::row(ui::kRowGap, ui::kKnobHeight);
            add(*inputs, DcProcessor::kInputAdd, ch, "In Add", signed_number);
            add(*inputs, DcProcessor::kInputMul, ch, "In Mul", number);
            add(*inputs, DcProcessor::kOutputScale, ch, "Out", pct);
            auto invert = ui::param_toggle(
                store_, static_cast<state::ParamID>(param_for(DcProcessor::kInvert, ch)),
                "Invert");
            // Toggle draws its label at the bottom of its own box, so the box must
            // be tall enough for both the switch and the text.
            ui::toggle_size(*invert);
            inputs->add_child(std::move(invert));

            auto readout = ui::caption_label("", 13.0f);
            readout->set_text_color(ui::palette::text);
            readout_[ch] = readout.get();

            auto rail = std::make_unique<ui::VoltageRail>(
                [this, ch] { return emitted(ch); });
            rail->flex().preferred_height = 14.0f;
            rail->flex().align_self = view::FlexAlign::stretch;

            add_child(std::move(levels));
            add_child(std::move(inputs));
            add_child(std::move(readout));
            add_child(std::move(rail));
        }

        // Full scale, never volts: the plug-in cannot know the interface's rail.
        add_child(ui::caption_label(
            "-1 . centre 0 . +1     full scale, not volts"));
    }

    void paint(canvas::Canvas& canvas) override {
        // The parent paints before its children, so the readouts' text must be
        // updated here — not drawn here. A caption drawn under a child's bounds
        // is invisible.
        for (std::size_t ch = 0; ch < kChannelCount; ++ch) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%+.3f  full scale", emitted(ch));
            readout_[ch]->set_text(buf);
        }

        ui::BrewPanel::paint(canvas);

        // Parameters can move under host automation, so the editor never learns
        // of a change; repaint continuously rather than guess.
        request_repaint();
    }

    /// The sample the processor last wrote to a channel of the bus.
    float emitted(std::size_t ch = 0) const { return proc_.display_output(ch); }

private:
    state::StateStore& store_;
    const DcProcessor& proc_;
    std::array<view::Label*, kChannelCount> readout_{};
};

}  // namespace pulp::examples::brew
