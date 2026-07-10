#pragma once

// Musical scales, defined on the lattice rather than on volts.
//
// A scale quantizer is usually described as needing calibration: a semitone is a
// fixed voltage (1/12 V into a 1V/oct input), so snapping to one means knowing
// the interface's full-scale voltage. That is true of *calibrated* mode, and this
// suite does not have that number yet.
//
// But it is not true of the scale itself. Once the user has set the step count so
// that twelve lattice steps span an octave of their oscillator — which is exactly
// the calibration-by-ear the reference manual describes, turning a Multiplier
// until the pitch reads an octave above — the lattice *is* a chromatic scale, and
// restricting it to a mode is pure arithmetic on the step index. No volts appear
// anywhere below.
//
// So: an octave is twelve lattice steps, a scale is a twelve-bit mask of the
// pitch classes it admits, and transposing "within the scale" means moving by
// scale degrees rather than by steps. `Transpose` of +7 in a major scale is an
// octave, because a major scale has seven degrees.

#include <array>
#include <cstdint>

namespace pulp::examples::brew {

/// Lattice steps in an octave. Twelve, by the definition above.
inline constexpr int kSemitonesPerOctave = 12;

/// The scales the Quantizer offers. Parameter values are persisted: append only.
enum class Scale : int {
    chromatic = 0,
    major = 1,
    natural_minor = 2,
    harmonic_minor = 3,
    pentatonic_major = 4,
    pentatonic_minor = 5,
    blues = 6,
    whole_tone = 7,
    dorian = 8,
    mixolydian = 9,
};

inline constexpr int kScaleCount = 10;

/// Pitch classes admitted by a scale, as a twelve-bit mask. Bit `n` is the
/// semitone `n` above the root.
[[nodiscard]] inline constexpr std::uint16_t scale_mask(Scale s) noexcept {
    switch (s) {
        case Scale::chromatic:        return 0b111111111111;
        case Scale::major:            return 0b101010110101;
        case Scale::natural_minor:    return 0b010110101101;
        case Scale::harmonic_minor:   return 0b100110101101;
        case Scale::pentatonic_major: return 0b001010010101;
        case Scale::pentatonic_minor: return 0b010010101001;
        case Scale::blues:            return 0b010011101001;
        case Scale::whole_tone:       return 0b010101010101;
        case Scale::dorian:           return 0b011010101101;
        case Scale::mixolydian:       return 0b011010110101;
    }
    return 0b111111111111;
}

/// Euclidean remainder: always in `[0, m)`, unlike `%` for a negative dividend.
/// Step indices run negative below zero volts, so this is not a nicety.
[[nodiscard]] inline constexpr int floor_mod(int n, int m) noexcept {
    const int r = n % m;
    return r < 0 ? r + m : r;
}

[[nodiscard]] inline constexpr bool scale_admits(Scale s, int pitch_class) noexcept {
    return (scale_mask(s) >> floor_mod(pitch_class, kSemitonesPerOctave)) & 1u;
}

/// How many notes the scale has per octave.
[[nodiscard]] inline constexpr int scale_degree_count(Scale s) noexcept {
    int n = 0;
    for (int i = 0; i < kSemitonesPerOctave; ++i)
        if (scale_admits(s, i)) ++n;
    return n;
}

/// The nearest step index at or around `index` whose pitch class is in the scale.
///
/// Ties go to the lower index. A tie is reachable — a whole-tone gap puts a
/// chromatic step exactly between two scale notes — and an unspecified rule there
/// would make the staircase asymmetric in a way that depends on the compiler's
/// rounding rather than on anything musical.
[[nodiscard]] inline constexpr int snap_to_scale(int index, Scale s, int root) noexcept {
    if (scale_admits(s, index - root)) return index;
    // The widest gap in any scale here is three semitones (the blues scale's), so
    // two steps out always finds a note. Six is the furthest any twelve-bit mask
    // with at least one bit set can require.
    for (int d = 1; d <= kSemitonesPerOctave; ++d) {
        if (scale_admits(s, index - d - root)) return index - d;
        if (scale_admits(s, index + d - root)) return index + d;
    }
    return index;
}

/// Move `index` by `degrees` notes *of the scale*, not by semitones.
///
/// This is what the manual means by "in Scale mode, the Transpose control
/// actually transposes up and down within the scale — so for a normal major scale
/// a Transpose value of +7 will transpose up by an octave." `index` need not
/// already be in the scale; it is snapped first.
[[nodiscard]] inline constexpr int transpose_in_scale(int index, Scale s, int root,
                                                      int degrees) noexcept {
    const int n = scale_degree_count(s);
    if (n <= 0) return index;

    const int snapped = snap_to_scale(index, s, root);
    const int rel = snapped - root;

    // Split into (octave, ordinal-within-octave), move the ordinal, recombine.
    const int octave = (rel - floor_mod(rel, kSemitonesPerOctave)) / kSemitonesPerOctave;
    const int pc = floor_mod(rel, kSemitonesPerOctave);

    int ordinal = 0;
    for (int i = 0; i < pc; ++i)
        if (scale_admits(s, i)) ++ordinal;

    const int total = octave * n + ordinal + degrees;
    const int out_octave = (total - floor_mod(total, n)) / n;
    const int out_ordinal = floor_mod(total, n);

    int semitone = 0;
    for (int i = 0, seen = 0; i < kSemitonesPerOctave; ++i) {
        if (!scale_admits(s, i)) continue;
        if (seen == out_ordinal) {
            semitone = i;
            break;
        }
        ++seen;
    }
    return root + out_octave * kSemitonesPerOctave + semitone;
}

}  // namespace pulp::examples::brew
