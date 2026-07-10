#pragma once

// LFO shapes, phase, and the eight ways a modulation source can be told what time
// it is.
//
// The default is the same decision as the clock grid (brew/clock.hpp), for the
// same reason: the phase is a pure function of `position_beats`, never an
// accumulator advanced one block at a time. An accumulator drifts against the host
// over a long session, has to be re-synced on every locate, and lands somewhere
// arbitrary when the user hits play at bar 57. Deriving it means bar 57 always
// sounds like bar 57, and bouncing the same project twice lands the modulation on
// the same samples.
//
// Two of the eight sync modes cannot work that way, and the header says so rather
// than pretending. `Free` and `Tempo` keep oscillating while the transport is
// parked, which is not a function of a position that is not moving. Those two run
// off an accumulator and are the only modes in this plug-in that do not bounce
// bit-identically. Everything else — including `Free3`, which is what a
// position-derived "free run" actually is — stays pure.
//
// Everything here returns bipolar [-1, +1]. The unipolar conversion is one
// multiply at the output stage, and keeping the shapes bipolar means `invert`
// and `output_scale` compose the same way they do for every other plug-in.

#include <brew/clock.hpp>   // Swing, swing_unwarp
#include <brew/random.hpp>
#include <brew/sync.hpp>    // NoteUnit, note_unit_beats, enum_from_param

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pulp::examples::brew {

/// The shapes. Ordered so the enum's integer value can be a parameter, and so
/// inserting a shape later would renumber a saved preset — hence: append only.
enum class Waveform : int {
    sine = 0,
    triangle = 1,
    saw_up = 2,
    saw_down = 3,
    square = 4,
};

inline constexpr int kWaveformCount = 5;

/// Coerce a parameter float to a Waveform, clamping rather than wrapping. A host
/// handing back an out-of-range value must not silently select a different shape.
[[nodiscard]] inline Waveform waveform_from_param(float v) noexcept {
    const int i = static_cast<int>(std::lround(v));
    if (i < 0) return Waveform::sine;
    if (i >= kWaveformCount) return static_cast<Waveform>(kWaveformCount - 1);
    return static_cast<Waveform>(i);
}

/// Wrap a phase into [0, 1). `std::fmod` keeps the sign of its argument, so a
/// negative phase offset would otherwise index the wrong half of the cycle.
[[nodiscard]] inline double wrap_phase(double p) noexcept {
    p = std::fmod(p, 1.0);
    return p < 0.0 ? p + 1.0 : p;
}

/// The LFO's phase at a position on the host timeline.
///
/// `beats_per_cycle` is the musical rate: 1.0 is one cycle per quarter note, 4.0
/// is one per bar of 4/4. Returns 0 for a degenerate rate rather than dividing.
[[nodiscard]] inline double lfo_phase(double position_beats,
                                      double beats_per_cycle,
                                      double phase_offset = 0.0) noexcept {
    if (!(beats_per_cycle > 0.0)) return 0.0;
    return wrap_phase(position_beats / beats_per_cycle + phase_offset);
}

/// A full turn, in radians. `M_PI` is a POSIX extension: MSVC's `<cmath>` omits it
/// unless `_USE_MATH_DEFINES` is defined before every include of it, which a header
/// cannot guarantee for its includers. Spelling the constant is portable and exact.
inline constexpr double kTau = 6.283185307179586476925286766559;

/// Evaluate a shape at a phase, bipolar in [-1, +1].
///
/// Sine and both saws cross zero rising at phase 0; triangle starts at its
/// trough and peaks at half a cycle; square starts high. These are the
/// conventional phase relationships, and they are what the quadrature output
/// below assumes.
[[nodiscard]] inline float lfo_shape(Waveform w, double phase) noexcept {
    const double p = wrap_phase(phase);
    switch (w) {
        case Waveform::sine:
            return static_cast<float>(std::sin(kTau * p));
        case Waveform::triangle:
            return static_cast<float>(1.0 - 4.0 * std::abs(p - 0.5));
        case Waveform::saw_up:
            return static_cast<float>(2.0 * p - 1.0);
        case Waveform::saw_down:
            return static_cast<float>(1.0 - 2.0 * p);
        case Waveform::square:
            return p < 0.5 ? 1.0f : -1.0f;
    }
    return 0.0f;
}

/// A quarter cycle. The offset one channel takes when it is locked to the other:
/// patched into two CV inputs the pair traces a circle, which is how a two-axis
/// modulation is driven from a single oscillator.
inline constexpr double kQuadratureOffset = 0.25;

/// Bend the time axis so the waveform's centre falls at `centre` rather than at
/// half a cycle. This is a pulse-width control generalized to every shape: at
/// 0.25 a triangle becomes a fast-rise slow-fall ramp, a sine leans, and a square
/// spends a quarter of its cycle high.
///
/// Continuous and monotone in `phase`, so it never introduces a discontinuity a
/// clean waveform did not already have. Degenerate centres are refused rather
/// than divided by.
[[nodiscard]] inline double warp_phase(double phase, double centre) noexcept {
    const double p = wrap_phase(phase);
    const double c = std::clamp(centre, 1e-4, 1.0 - 1e-4);
    return p < c ? 0.5 * p / c : 0.5 + 0.5 * (p - c) / (1.0 - c);
}

/// A square with an explicit duty cycle. `pulse_width` is the fraction of the
/// cycle spent high.
[[nodiscard]] inline float square_shape(double phase, double pulse_width) noexcept {
    return wrap_phase(phase) < std::clamp(pulse_width, 0.0, 1.0) ? 1.0f : -1.0f;
}

// ── The mix ──────────────────────────────────────────────────────────────────

/// Separates the noise hash from the sample-and-hold hash. Without it a cycle
/// index and a sample index that happened to collide would hand both generators
/// the same number, and the two would move together.
inline constexpr std::uint32_t kNoiseSalt = 0x9E3779B9u;

/// How much of each shape is summed into the output.
///
/// A mixer, not a selector. Four depths subsume the five-way enum this replaced
/// (set one to 1.0 and the rest to 0) and reach everything between. Each depth is
/// bipolar, so a shape can be subtracted as easily as added.
///
/// `random` is a sample-and-hold: one level per cycle, held flat across it, in the
/// manner of a noise source feeding a hardware S&H. `noise` is the ungated source
/// itself: a new value every sample. Both are hashes of an index rather than
/// generators, so they render identically every time — see brew/random.hpp. A
/// white-noise LFO that could not be bounced twice would be a strange thing to put
/// in a suite built around bounce determinism.
struct LfoMix {
    float sine = 1.0f;
    float triangle = 0.0f;
    float saw = 0.0f;
    float square = 0.0f;
    float random = 0.0f;
    float noise = 0.0f;
    float pulse_width = 0.5f;
    float asymmetry = 0.5f;
    float offset = 0.0f;
    std::uint32_t seed = 0;
};

/// The mixed waveform at a phase, before the output stage.
///
/// The sum is *not* clamped here. Five depths at full can reach 5.0, and clamping
/// mid-chain would silently flatten a mix the user asked for before `offset` and
/// the output scale have had their say. `resolve_output` clamps once, at the jack.
///
/// `cycle` keys the sample-and-hold; `sample` keys the noise. They are different
/// indices because the two are different things: one changes once per cycle, the
/// other once per sample.
[[nodiscard]] inline float lfo_mix_value(const LfoMix& m, double phase,
                                         std::int64_t cycle,
                                         std::int64_t sample = 0) noexcept {
    const double p = warp_phase(phase, static_cast<double>(m.asymmetry));
    float v = 0.0f;
    if (m.sine != 0.0f) v += m.sine * lfo_shape(Waveform::sine, p);
    if (m.triangle != 0.0f) v += m.triangle * lfo_shape(Waveform::triangle, p);
    if (m.saw != 0.0f) v += m.saw * lfo_shape(Waveform::saw_up, p);
    if (m.square != 0.0f) v += m.square * square_shape(p, static_cast<double>(m.pulse_width));
    // Held across the whole cycle, so it is keyed on the cycle, not the phase.
    if (m.random != 0.0f) v += m.random * hash_bipolar(cycle, m.seed);
    if (m.noise != 0.0f) v += m.noise * hash_bipolar(sample, m.seed ^ kNoiseSalt);
    return v + m.offset;
}

// ── Sync modes ───────────────────────────────────────────────────────────────

/// How the LFO is told what time it is. Parameter values are persisted: the order
/// is the reference taxonomy's, and it is append-only.
///
/// Two independent questions, and eight of the answers have names:
///
///   frequency source     — hertz, or beats of the host's tempo
///   transport behaviour  — free-run always / free-run then lock while playing /
///                          lock while playing and hold when stopped /
///                          ...and hold, resetting the phase on the play edge
///
/// | mode         | frequency | while stopped | while playing        |
/// |--------------|-----------|---------------|----------------------|
/// | free         | hertz     | keeps running | keeps running        |
/// | tempo        | beats     | keeps running | keeps running        |
/// | transport    | beats     | keeps running | locked to position   |
/// | quadrature   | (other's) | (other's)     | (other's) + Phase    |
/// | start_stop   | —         | low           | high                 |
/// | transport2   | beats     | holds         | locked to position   |
/// | free2        | hertz     | holds         | from the play edge   |
/// | free3        | hertz     | holds         | from the timeline    |
///
/// `free` and `tempo` are the two that keep running against wall-clock time while
/// the playhead is parked, so they are the two that do not bounce bit-identically.
/// `free3` is what a position-derived "free run" actually is, and it is exact.
enum class SyncMode : int {
    free = 0,
    tempo = 1,
    transport = 2,
    quadrature = 3,
    start_stop = 4,
    transport2 = 5,
    free2 = 6,
    free3 = 7,
};

inline constexpr int kSyncModeCount = 8;

/// The rate knob a mode reads: `Speed` × `Multiplier`, or `Beats` × `Divisor`.
[[nodiscard]] inline constexpr bool sync_uses_hertz(SyncMode m) noexcept {
    return m == SyncMode::free || m == SyncMode::free2 || m == SyncMode::free3;
}

[[nodiscard]] inline constexpr bool sync_uses_beats(SyncMode m) noexcept {
    return m == SyncMode::tempo || m == SyncMode::transport ||
           m == SyncMode::transport2;
}

/// Whether the mode keeps oscillating with the transport parked. The two that do
/// are the two that cannot be a pure function of a position that is not moving.
[[nodiscard]] inline constexpr bool sync_runs_when_stopped(SyncMode m) noexcept {
    return m == SyncMode::free || m == SyncMode::tempo || m == SyncMode::transport;
}

/// Whether the mode is bit-identical across renders. The honest answer, exposed
/// so the editor can say it rather than the README alone.
[[nodiscard]] inline constexpr bool sync_is_deterministic(SyncMode m) noexcept {
    return m != SyncMode::free && m != SyncMode::tempo;
}

/// Whether the play edge snaps the phase back to `Phase`.
[[nodiscard]] inline constexpr bool sync_resets_on_play(SyncMode m) noexcept {
    return m == SyncMode::free2;
}

// ── Frequency ────────────────────────────────────────────────────────────────

/// The decade switch beside `Speed`. Persisted as an index: append only.
enum class Multiplier : int {
    tenth = 0,
    one = 1,
    ten = 2,
    hundred = 3,
    thousand = 4,
};

inline constexpr int kMultiplierCount = 5;

[[nodiscard]] inline constexpr double multiplier_value(Multiplier m) noexcept {
    switch (m) {
        case Multiplier::tenth: return 0.1;
        case Multiplier::one: return 1.0;
        case Multiplier::ten: return 10.0;
        case Multiplier::hundred: return 100.0;
        case Multiplier::thousand: return 1000.0;
    }
    return 1.0;
}

/// Slow enough to sweep a filter over a couple of minutes. The ceiling is well
/// into the audio band, because `Speed` × 1000 asks for it — but the shapes here
/// are naive, not band-limited, so a saw or a square up there will alias. An LFO
/// is not an oscillator; the range exists so the decade switch means something,
/// not as a claim about spectral purity.
inline constexpr double kMinFreeHz = 0.001;
inline constexpr double kMaxFreeHz = 1000.0;

/// `Speed` × `Multiplier`, clamped. A rate of zero is a phase that never advances
/// and a rate of a million is a stepped voltage pretending to be a waveform.
[[nodiscard]] inline double free_hz(double speed, Multiplier m) noexcept {
    return std::clamp(speed * multiplier_value(m), kMinFreeHz, kMaxFreeHz);
}

/// How long one cycle lasts, in beats: `Beats` of the note `Divisor` names, and a
/// third off if `Triplet`.
///
/// `Divisor` 1/8 with `Beats` 3 is three eighth notes, which is the reference
/// manual's own example. `Beats` is deliberately fractional, because automating it
/// through a non-integer value is a legitimate way to sweep the rate.
[[nodiscard]] inline double cycle_beats(double beats, NoteUnit divisor,
                                        bool triplet) noexcept {
    const double length = beats * note_unit_beats(divisor);
    return triplet ? length * (2.0 / 3.0) : length;
}

// ── Elapsed cycles ───────────────────────────────────────────────────────────

/// Cycles elapsed at a beat position, relative to an origin.
///
/// Swing warps the beat timeline, so it is applied to the *position* before the
/// position becomes a phase — not to the phase afterwards. `swing_unwarp` maps a
/// sounding beat back to the straight beat it stands for, which is exactly the
/// coordinate the cycle count is measured in.
[[nodiscard]] inline double cycles_from_beats(double position_beats,
                                              double origin_beats,
                                              double beats_per_cycle,
                                              const Swing& swing = {}) noexcept {
    if (!(beats_per_cycle > 0.0)) return 0.0;
    return (swing_unwarp(position_beats, swing) - swing_unwarp(origin_beats, swing)) /
           beats_per_cycle;
}

/// Cycles elapsed at a time position, relative to an origin.
///
/// Swing has no meaning here. It is a subdivision of a beat, and a plug-in running
/// in hertz has no beats; a shuffled hertz rate would just be a wrong hertz rate.
[[nodiscard]] inline double cycles_from_seconds(double position_seconds,
                                                double origin_seconds,
                                                double hz) noexcept {
    return (position_seconds - origin_seconds) * hz;
}

/// The phase within the current cycle, given elapsed cycles and an offset.
[[nodiscard]] inline double phase_at(double cycles, double offset) noexcept {
    return wrap_phase(cycles + offset);
}

/// Which cycle of the whole timeline the offset position falls in. Signed:
/// cycle -1 precedes the project's origin, and a host will ask for it.
///
/// It takes the *same* offset as `phase_at`, deliberately. Keying the
/// sample-and-hold on an un-offset cycle index while the waveform's phase is
/// offset makes the held value step in the middle of the visible cycle rather
/// than where the shape wraps.
[[nodiscard]] inline std::int64_t cycle_at(double cycles, double offset) noexcept {
    return static_cast<std::int64_t>(std::floor(cycles + offset));
}

// ── Start/Stop ───────────────────────────────────────────────────────────────

/// `Start/Stop` mode's whole waveform: the transport, as a square wave whose
/// period is the length of the session.
///
/// Not really an LFO. It exists so a patch can know the transport is running
/// without a clock cable, and it is exactly as useful as `Smooth` makes it — set
/// a long smoothing time and the play edge becomes a fade-in.
[[nodiscard]] inline constexpr float start_stop_value(bool playing) noexcept {
    return playing ? 1.0f : -1.0f;
}

// ── Input ────────────────────────────────────────────────────────────────────

/// What the plug-in does with the voltage on its input bus.
///
/// `off` is the default, and for the same reason a bypassed generator is silent:
/// the output ends at a patch cable, and whatever a DAW happens to have on the
/// track — a drum loop, at full scale — would otherwise arrive at a VCO's pitch
/// input. A modulation source that reads its input by default is a modulation
/// source that screams the first time it is dropped on an audio track.
enum class InputMode : int {
    off = 0,
    add = 1,
    multiply = 2,
    combine = 3,
};

inline constexpr int kInputModeCount = 4;

/// `combine` is the sum of what `add` and `multiply` would each have produced —
/// ring modulation riding on the sum, which is what the reference documents.
[[nodiscard]] inline constexpr float apply_input(InputMode mode, float lfo,
                                                 float in) noexcept {
    switch (mode) {
        case InputMode::off: return lfo;
        case InputMode::add: return lfo + in;
        case InputMode::multiply: return lfo * in;
        case InputMode::combine: return (lfo + in) + (lfo * in);
    }
    return lfo;
}

/// Map bipolar [-1, +1] to unipolar [0, 1]. Some CV inputs (a VCA, an envelope
/// depth) want only positive voltage; feeding them a bipolar LFO wastes half the
/// travel and, on a VCA, silences half the cycle.
[[nodiscard]] inline constexpr float to_unipolar(float bipolar) noexcept {
    return (bipolar + 1.0f) * 0.5f;
}

}  // namespace pulp::examples::brew
