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
        auto ratio = [](float v) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.0f%%", v * 100.0f);
            return std::string(buf);
        };
        // A knob standing in for a menu: it reads its own value back as a name.
        // The index is clamped, never wrapped, so a value a host hands back out of
        // range shows the same mode the DSP selects — `enum_from_param` clamps
        // too, and a label that disagreed with the audio would be worse than none.
        auto named = [](const char* const* names, int count) {
            return [names, count](float v) {
                int i = static_cast<int>(std::lround(v));
                i = std::clamp(i, 0, count - 1);
                return std::string(names[i]);
            };
        };
        // "24pp", not "24": the PPQN knob next door also reads 24 by default, and
        // two adjacent knobs showing the same number invite the reading that one
        // of them is doing nothing.
        static const char* const kClockNames[] = {"Off", "24pp", "48pp", "Cust"};
        static const char* const kRunNames[] = {"Run", "Start", "St/Sp", "Stop"};
        static const char* const kUnitNames[] = {"1/1", "1/2", "1/4", "1/8", "1/16"};
        // Zero beats is not a zero-length interval, it is the off switch.
        auto beats_or_off = [integer](float v) {
            return v < 0.5f ? std::string("Off") : integer(v);
        };
        // The clock's width knob has the same convention: zero selects the 50%
        // duty square wave rather than a zero-width pulse.
        auto width_or_square = [millis](float v) {
            return v <= 0.0f ? std::string("50%") : millis(v);
        };

        auto rate = ui::row(ui::kRowGap, ui::kKnobHeight);
        auto timing = ui::row(ui::kRowGap, ui::kKnobHeight);
        auto run = ui::row(ui::kRowGap, ui::kKnobHeight);
        auto out = ui::row(ui::kRowGap, ui::kKnobHeight);
        auto add_knob = [&](vw_row& into, state::ParamID id, const char* label,
                            std::function<std::string(float)> fmt) {
            auto k = ui::param_knob(store_, id, label, std::move(fmt));
            ui::knob_size(*k);
            into->add_child(std::move(k));
        };
        add_knob(rate, SyncProcessor::kClockType, "Type",
                 named(kClockNames, kClockTypeCount));
        add_knob(rate, SyncProcessor::kPulsesPerBeat, "PPQN", integer);
        add_knob(rate, SyncProcessor::kMultiplier, "Mult", integer);
        add_knob(rate, SyncProcessor::kDivisor, "Div", integer);
        add_knob(rate, SyncProcessor::kTriggerLengthMs, "Width", width_or_square);

        add_knob(timing, SyncProcessor::kFirstDelayMs, "1st Dly", millis);
        add_knob(timing, SyncProcessor::kOffsetMs, "Offset", signed_millis);
        add_knob(timing, SyncProcessor::kSwingPercent, "Swing", percent);

        add_knob(run, SyncProcessor::kRunType, "Run Sig",
                 named(kRunNames, kRunSignalCount));
        add_knob(run, SyncProcessor::kRunLevel, "Level", ratio);
        add_knob(run, SyncProcessor::kRunPulseMs, "Pulse", millis);
        add_knob(run, SyncProcessor::kResetBeats, "Reset", beats_or_off);
        add_knob(run, SyncProcessor::kResetUnit, "Unit",
                 named(kUnitNames, kNoteUnitCount));

        add_knob(out, SyncProcessor::kOutputScale, "Out", ratio);

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
