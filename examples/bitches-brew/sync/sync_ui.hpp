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

#include <algorithm>
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
        auto rate = ui::row(ui::kRowGap, ui::kKnobHeight);
        auto timing = ui::row(ui::kRowGap, ui::kKnobHeight);
        auto run = ui::row(ui::kRowGap, ui::kKnobHeight);
        auto out = ui::row(ui::kRowGap, ui::kKnobHeight);
        // Every knob reads its value through the parameter's own formatter, so a
        // readout here and the host's automation lane are the same string.
        auto add_knob = [&](vw_row& into, state::ParamID id, const char* label) {
            auto k = ui::param_knob(store_, id, label);
            ui::knob_size(*k);
            into->add_child(std::move(k));
        };
        add_knob(rate, SyncProcessor::kClockType, "Type");
        add_knob(rate, SyncProcessor::kPulsesPerBeat, "PPQN");
        add_knob(rate, SyncProcessor::kMultiplier, "Mult");
        add_knob(rate, SyncProcessor::kDivisor, "Div");
        add_knob(rate, SyncProcessor::kTriggerLengthMs, "Width");

        add_knob(timing, SyncProcessor::kFirstDelayMs, "1st Dly");
        add_knob(timing, SyncProcessor::kOffsetMs, "Offset");
        add_knob(timing, SyncProcessor::kSwingPercent, "Swing");

        add_knob(run, SyncProcessor::kRunType, "Run Sig");
        add_knob(run, SyncProcessor::kRunLevel, "Level");
        add_knob(run, SyncProcessor::kRunPulseMs, "Pulse");
        add_knob(run, SyncProcessor::kResetBeats, "Reset");
        add_knob(run, SyncProcessor::kResetUnit, "Unit");

        add_knob(out, SyncProcessor::kOutputScale, "Out");

        // Which note the swing moves. Off swings the eighth, on the sixteenth.
        auto sixteenths =
            ui::param_toggle(store_, SyncProcessor::kSwingUnit, "16ths");
        ui::toggle_size(*sixteenths);
        timing->add_child(std::move(sixteenths));

        auto invert = ui::param_toggle(store_, SyncProcessor::kInvert, "Invert");
        auto skip = ui::param_toggle(store_, SyncProcessor::kSkipFirst, "Skip 1st");
        auto wait = ui::param_toggle(store_, SyncProcessor::kWaitForBar, "Wait Bar");
        // Toggle draws its label at the bottom of its own box, so the box must be
        // tall enough for both the switch and the text.
        ui::toggle_size(*invert);
        ui::toggle_size(*skip);
        ui::toggle_size(*wait);
        out->add_child(std::move(invert));
        out->add_child(std::move(skip));
        out->add_child(std::move(wait));

        auto lamps = ui::row(ui::kRowGap, ui::kToggleHeight);
        auto clock_lamp = std::make_unique<ui::Lamp>(
            "CLOCK", [this] { return proc_.clock_lamp(); });
        auto run_lamp =
            std::make_unique<ui::Lamp>("RUN", [this] { return proc_.run_lamp(); });
        ui::fixed_size(*clock_lamp, 46.0f, ui::kToggleHeight);
        ui::fixed_size(*run_lamp, 46.0f, ui::kToggleHeight);
        lamps->add_child(std::move(clock_lamp));
        lamps->add_child(std::move(run_lamp));

        add_child(std::move(rate));
        add_child(std::move(timing));
        add_child(std::move(run));
        add_child(std::move(out));
        add_child(std::move(lamps));
        add_child(ui::caption_label(
            "24 PPQN is DIN sync. Reset needs a pulsed Run Sig."));
    }

private:
    state::StateStore& store_;
    const SyncProcessor& proc_;
};

}  // namespace pulp::examples::brew
