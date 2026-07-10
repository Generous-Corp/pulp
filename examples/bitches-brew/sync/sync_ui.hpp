#pragma once

// Sync's editor: the clock settings, and two lamps proving the clock is running.
//
// The lamps are not decoration. Sync makes no sound, drives no meter, and its
// output is a pulse train a user cannot see. When a patch does not clock, the
// first question is always "is the plug-in even running?" — and without the
// lamps the honest answer from inside the DAW is "no idea". They read real DSP
// state, published once per block from the audio thread.

#include "sync_processor.hpp"

#include <brew/ui/panel.hpp>

#include <cstdio>
#include <functional>
#include <string>
#include <utility>

namespace pulp::examples::brew {

class SyncUi : public ui::BrewPanel {
    using vw_row = std::unique_ptr<view::View>;

public:
    SyncUi(state::StateStore& store, const SyncProcessor& proc)
        : ui::BrewPanel("Sync", "a clock and a run gate, locked to the host"),
          store_(store),
          proc_(proc) {
        auto integer = [](float v) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.0f", v);
            return std::string(buf);
        };
        auto millis = [](float v) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.1f ms", v);
            return std::string(buf);
        };
        // The offset is bipolar, and its sign is the whole point: negative pulls
        // the pulses ahead of the beat to compensate hardware latency.
        auto signed_millis = [](float v) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%+.1f ms", v);
            return std::string(buf);
        };

        auto percent = [](float v) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.0f%%", v);
            return std::string(buf);
        };

        auto rate = ui::row(ui::kRowGap, ui::kKnobHeight);
        auto timing = ui::row(ui::kRowGap, ui::kKnobHeight);
        auto add_knob = [&](vw_row& into, state::ParamID id, const char* label,
                            std::function<std::string(float)> fmt) {
            auto k = ui::param_knob(store_, id, label, std::move(fmt));
            ui::knob_size(*k);
            into->add_child(std::move(k));
        };
        add_knob(rate, SyncProcessor::kPulsesPerBeat, "PPQN", integer);
        add_knob(rate, SyncProcessor::kMultiplier, "Mult", integer);
        add_knob(rate, SyncProcessor::kDivisor, "Div", integer);
        add_knob(rate, SyncProcessor::kTriggerLengthMs, "Width", millis);
        add_knob(rate, SyncProcessor::kFirstDelayMs, "1st Dly", millis);
        add_knob(timing, SyncProcessor::kOffsetMs, "Offset", signed_millis);
        add_knob(timing, SyncProcessor::kSwingPercent, "Swing", percent);

        // Which note the swing moves. Off swings the eighth, on the sixteenth.
        auto sixteenths =
            ui::param_toggle(store_, SyncProcessor::kSwingUnit, "16ths");
        ui::toggle_size(*sixteenths);
        timing->add_child(std::move(sixteenths));

        auto bottom = ui::row(ui::kRowGap, ui::kToggleHeight);
        auto skip = ui::param_toggle(store_, SyncProcessor::kSkipFirst, "Skip 1st");
        auto wait = ui::param_toggle(store_, SyncProcessor::kWaitForBar, "Wait Bar");
        // Toggle draws its label at the bottom of its own box, so the box must be
        // tall enough for both the switch and the text.
        ui::toggle_size(*skip);
        ui::toggle_size(*wait);

        auto clock_lamp = std::make_unique<ui::Lamp>(
            "CLOCK", [this] { return proc_.clock_lamp(); });
        auto run_lamp =
            std::make_unique<ui::Lamp>("RUN", [this] { return proc_.run_lamp(); });
        ui::fixed_size(*clock_lamp, 46.0f, ui::kToggleHeight);
        ui::fixed_size(*run_lamp, 46.0f, ui::kToggleHeight);

        bottom->add_child(std::move(skip));
        bottom->add_child(std::move(wait));
        bottom->add_child(std::move(clock_lamp));
        bottom->add_child(std::move(run_lamp));

        add_child(std::move(rate));
        add_child(std::move(timing));
        add_child(std::move(bottom));
        add_child(ui::caption_label(
            "24 PPQN is DIN sync. 50% swing is straight, and exactly so."));
    }

private:
    state::StateStore& store_;
    const SyncProcessor& proc_;
};

}  // namespace pulp::examples::brew
