#pragma once

// Sync's discrete choices: what kind of clock, what kind of run signal, and the
// note unit the periodic reset counts in.
//
// These live outside the processor because the editor draws them, the tests
// assert on them, and the audio callback switches on them. A menu whose labels
// are typed out in three places drifts.
//
// Parameter values are persisted. The integers here are the contract: append
// only, never renumber.

#include <algorithm>
#include <cmath>

namespace pulp::examples::brew {

/// How the clock output's pulse rate is specified.
///
/// `ppqn24` and `ppqn48` are the two standards worth naming: 24 pulses per
/// quarter note is what Roland's DIN-sync hardware expects, 48 is what several
/// Korg boxes expect. `custom` hands the rate to the `Pulses Per Beat` knob.
/// `off` silences the clock output entirely, which is the setting you want when
/// the plug-in is in the patch only for its run signal.
enum class ClockType : int {
    off = 0,
    ppqn24 = 1,
    ppqn48 = 2,
    custom = 3,
};

inline constexpr int kClockTypeCount = 4;

/// What the run output emits.
///
/// `run` is a level, not a pulse: high for the whole time the transport is
/// running. That is what DIN sync requires. The other three are pulses, and
/// `start` — one pulse at the moment the transport starts — is what most
/// hardware step sequencers want on their reset input.
enum class RunSignal : int {
    run = 0,
    start = 1,
    start_stop = 2,
    stop = 3,
};

inline constexpr int kRunSignalCount = 4;

/// Note values the periodic reset interval is counted in, expressed in
/// quarter-note beats — the unit Pulp reports transport position in.
enum class NoteUnit : int {
    whole = 0,
    half = 1,
    quarter = 2,
    eighth = 3,
    sixteenth = 4,
};

inline constexpr int kNoteUnitCount = 5;

/// Coerce a persisted parameter float to an enumerator, clamping rather than
/// wrapping. A host that hands back an out-of-range value must not silently
/// select a different mode than the one the user saved.
template <typename Enum>
[[nodiscard]] inline Enum enum_from_param(float v, int count) noexcept {
    int i = static_cast<int>(std::lround(v));
    i = std::clamp(i, 0, count - 1);
    return static_cast<Enum>(i);
}

[[nodiscard]] inline constexpr double note_unit_beats(NoteUnit u) noexcept {
    switch (u) {
        case NoteUnit::whole: return 4.0;
        case NoteUnit::half: return 2.0;
        case NoteUnit::quarter: return 1.0;
        case NoteUnit::eighth: return 0.5;
        case NoteUnit::sixteenth: return 0.25;
    }
    return 1.0;
}

/// Pulses per quarter note implied by a clock type. Zero means "no clock", and
/// every downstream rate calculation already treats a non-positive rate as a
/// degenerate setting that emits nothing.
[[nodiscard]] inline constexpr double clock_pulses_per_beat(ClockType t,
                                                            double custom) noexcept {
    switch (t) {
        case ClockType::off: return 0.0;
        case ClockType::ppqn24: return 24.0;
        case ClockType::ppqn48: return 48.0;
        case ClockType::custom: return custom > 0.0 ? custom : 0.0;
    }
    return 0.0;
}

/// True when the run output is a pulse rather than a level. The periodic reset
/// only makes sense for these — a reset pulse cannot be distinguished from the
/// run level if the run output *is* the level.
[[nodiscard]] inline constexpr bool run_signal_is_pulsed(RunSignal r) noexcept {
    return r != RunSignal::run;
}

[[nodiscard]] inline constexpr bool run_signal_pulses_on_start(RunSignal r) noexcept {
    return r == RunSignal::start || r == RunSignal::start_stop;
}

[[nodiscard]] inline constexpr bool run_signal_pulses_on_stop(RunSignal r) noexcept {
    return r == RunSignal::stop || r == RunSignal::start_stop;
}

/// The interval between periodic reset pulses, in beats. Zero disables it.
[[nodiscard]] inline constexpr double reset_interval_beats(double beats,
                                                           NoteUnit unit) noexcept {
    const double interval = beats * note_unit_beats(unit);
    return interval > 0.0 ? interval : 0.0;
}

}  // namespace pulp::examples::brew
