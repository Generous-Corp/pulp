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

        auto rate = ui::row(10.0f, 84.0f);
        auto timing = ui::row(10.0f, 84.0f);
        auto add_knob = [&](vw_row& into, state::ParamID id, const char* label,
                            std::function<std::string(float)> fmt) {
            auto k = ui::param_knob(store_, id, label, std::move(fmt));
            ui::fixed_size(*k, 88.0f, 84.0f);
            into->add_child(std::move(k));
        };
        add_knob(rate, SyncProcessor::kPulsesPerBeat, "PPQN", integer);
        add_knob(rate, SyncProcessor::kMultiplier, "Mult", integer);
        add_knob(rate, SyncProcessor::kDivisor, "Div", integer);
        add_knob(timing, SyncProcessor::kTriggerLengthMs, "Width", millis);
        add_knob(timing, SyncProcessor::kFirstDelayMs, "1st Delay", millis);
        add_knob(timing, SyncProcessor::kOffsetMs, "Offset", signed_millis);

        auto bottom = ui::row(14.0f, 52.0f);
        auto skip = ui::param_toggle(store_, SyncProcessor::kSkipFirst, "Skip 1st");
        auto wait = ui::param_toggle(store_, SyncProcessor::kWaitForBar, "Wait Bar");
        // Toggle draws its label at the bottom of its own box, so the box must be
        // tall enough for both the switch and the text.
        ui::fixed_size(*skip, 78.0f, 50.0f);
        ui::fixed_size(*wait, 78.0f, 50.0f);

        auto clock_lamp = std::make_unique<ui::Lamp>(
            "CLOCK", [this] { return proc_.clock_lamp(); });
        auto run_lamp =
            std::make_unique<ui::Lamp>("RUN", [this] { return proc_.run_lamp(); });
        ui::fixed_size(*clock_lamp, 56.0f, 50.0f);
        ui::fixed_size(*run_lamp, 56.0f, 50.0f);

        bottom->add_child(std::move(skip));
        bottom->add_child(std::move(wait));
        bottom->add_child(std::move(clock_lamp));
        bottom->add_child(std::move(run_lamp));

        add_child(std::move(rate));
        add_child(std::move(timing));
        add_child(std::move(bottom));
        add_child(ui::caption_label(
            "24 PPQN is DIN sync. Width is clamped below half the pulse period."));
    }

private:
    state::StateStore& store_;
    const SyncProcessor& proc_;
};

}  // namespace pulp::examples::brew
