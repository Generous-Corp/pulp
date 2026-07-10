#pragma once

// How a parameter reads — to the editor, and to the host.
//
// A `ParamInfo::to_string` is not decoration. It is the only thing standing
// between a user and a bare float in the DAW's own parameter list, its
// automation lane, and its type-in field. A mode knob with no formatter is a
// slider that reads "2" where it should read "Env"; an AU shows a continuous
// slider instead of a menu, because `GetParameterValueStrings` requires the
// formatter before it will offer names at all.
//
// So the formatters live here, next to the DSP, and every plug-in attaches them
// in `define_parameters()`. The editors read them back out of the store rather
// than carrying their own copies, which is why a knob's readout and the host's
// automation lane can never disagree about what a value means.
//
// A formatter sees only its own parameter's value. Anything that needs to
// consult a *second* parameter (Function's `Amount`, which is meaningless for
// three of its five curves) stays an editor-local override — the host has no
// way to express that dependency anyway.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>

namespace pulp::examples::brew::text {

using Formatter = std::function<std::string(float)>;

namespace detail {
inline std::string printf_to_string(const char* fmt, double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), fmt, v);
    return std::string(buf);
}
}  // namespace detail

/// Two decimals. The default for a normalized depth or level.
inline std::string plain(float v) { return detail::printf_to_string("%.2f", v); }
/// Two decimals, always signed — for anything bipolar, where the sign is the point.
inline std::string signed_plain(float v) { return detail::printf_to_string("%+.2f", v); }
/// Three decimals, for the knobs a user nudges by a millivolt.
inline std::string number(float v) { return detail::printf_to_string("%.3f", v); }
inline std::string signed_number(float v) { return detail::printf_to_string("%+.3f", v); }
/// An integer count: steps, notes, bits, pulses.
inline std::string whole(float v) { return detail::printf_to_string("%.0f", v); }
inline std::string signed_whole(float v) { return detail::printf_to_string("%+.0f", v); }
/// Four significant figures, no trailing zeros — for values that span decades
/// (0.01 Hz to 40 Hz, 1/16 beat to 64 beats). The unit rides on `ParamInfo::unit`.
inline std::string compact(float v) { return detail::printf_to_string("%.4g", v); }

inline std::string percent0(float v) { return detail::printf_to_string("%.0f%%", v); }
inline std::string percent1(float v) { return detail::printf_to_string("%.1f%%", v); }
/// A 0..1 fraction shown as a percentage.
inline std::string fraction_percent(float v) {
    return detail::printf_to_string("%.0f%%", v * 100.0f);
}
inline std::string degrees(float v) { return detail::printf_to_string("%.0f°", v); }

inline std::string millis(float v) { return detail::printf_to_string("%.1f ms", v); }
inline std::string signed_millis(float v) { return detail::printf_to_string("%+.1f ms", v); }
inline std::string compact_millis(float v) { return detail::printf_to_string("%.4g ms", v); }
inline std::string seconds(float v) { return detail::printf_to_string("%.4g s", v); }

/// `Smooth` is one control with two behaviors and a dead centre: positive
/// slew-rate-limits, negative low-passes, zero is a wire. A signed millisecond
/// count says none of that. Every plug-in in the suite carries this control, so
/// every one of them reads it the same way.
inline std::string smooth(float v) {
    if (v == 0.0f) return std::string("off");
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%s %.0f", v > 0.0f ? "slew" : "lpf",
                  std::abs(v));
    return std::string(buf);
}

inline std::string on_off(float v) { return v >= 0.5f ? "on" : "off"; }

/// A menu index, read back as a name.
///
/// The index is clamped, never wrapped, so the text can never name a mode other
/// than the one the DSP is running: an out-of-range value and the nearest legal
/// value must read the same, because that is what the DSP does with it.
[[nodiscard]] inline std::string named_at(const char* const* names, int count,
                                          float v) {
    int i = static_cast<int>(std::lround(v));
    i = std::clamp(i, 0, count - 1);
    return std::string(names[i]);
}

/// The same, curried, for a caller that wants a `Formatter` to hand around.
[[nodiscard]] inline Formatter named(const char* const* names, int count) {
    return [names, count](float v) { return named_at(names, count, v); };
}

// ── The menus the suite shares ────────────────────────────────────────────────
//
// Two plug-ins that offer the same choice must offer it under the same name, or
// a patch that reads "Comb" on one and "3" on the other is two patches. Each
// menu gets a plain function, not a `Formatter`, so a plug-in's control table
// can name it and stay `constexpr`.

/// The continuous LFO's eight sync modes, in `SyncMode` order.
inline constexpr const char* kSyncNames[] = {"Free",  "Tempo", "Trans", "Quad",
                                             "St/Sp", "Trans2", "Free2", "Free3"};
/// The stepped LFO's ten: the eight above, plus the two that wait for a trigger.
inline constexpr const char* kStepSyncNames[] = {"Free",  "Tempo", "Trans", "Quad",
                                                 "St/Sp", "Trans2", "Free2", "Free3",
                                                 "TrgFr", "TrgTm"};
/// The free-running speed's decade switch, and the envelope's time scale.
inline constexpr const char* kMultiplierNames[] = {"x0.1", "x1", "x10", "x100", "x1k"};
/// The note length one `Beats` counts.
inline constexpr const char* kDivisorNames[] = {"1/1", "1/2", "1/4", "1/8", "1/16"};
/// How an input bus folds into a generator's output.
inline constexpr const char* kInputModeNames[] = {"Off", "Add", "Mul", "Comb"};

inline std::string sync_name(float v) { return named_at(kSyncNames, 8, v); }
inline std::string step_sync_name(float v) { return named_at(kStepSyncNames, 10, v); }
inline std::string multiplier_name(float v) { return named_at(kMultiplierNames, 5, v); }
inline std::string divisor_name(float v) { return named_at(kDivisorNames, 5, v); }
inline std::string input_mode_name(float v) { return named_at(kInputModeNames, 4, v); }

}  // namespace pulp::examples::brew::text
