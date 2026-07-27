#pragma once

// HarmonyEngineT — the scale-aware diatonic harmonizer, and the YIN tracker and
// key/scale mapper it composes.
//
// This is the spec's acceptance suite 1–11 plus the characterisation cases its
// own criteria turned out to need. Expected values are COMPUTED from the shipped
// constants — the scale MASKS, the ceil-derived tracker geometry, the glide
// time — never restated as literals, so retuning a design parameter fails the
// case that documents it rather than quietly disagreeing with it.
//
// ── What this module is, in one assertion ────────────────────────────────
//
// A "third" is not a transposition. In C major a third above the tonic is 4
// semitones and a third above the supertonic is 3, and producing that
// difference from the same `+2` setting is the whole distance between a
// harmonizer and a pitch shifter with a knob. `T3` pins that mapping across
// every degree of five scales; `T5` proves it survives all the way to the
// audio output.
//
// ── Measurement recipe ───────────────────────────────────────────────────
//
// fs = 48 kHz. Spectral reads use a Blackman-Harris windowed DFT scanned on a
// fine grid (`peak_near`), never a peak-sample tracker: a discrete sine
// under-reads its own amplitude when no sample lands on the crest, and the
// error looks exactly like a filter fault. BH's −92 dB sidelobes keep the
// crossfade comb's neighbouring lines from contaminating a read.
//
// The tracker needs `W + kHop` to settle and the shifters need their buffers to
// fill, so every audio case discards `3·latency + 8000` samples first.
//
// ── The trap every audio tolerance in this spec walked into ──────────────
//
// The harmony voices are `PitchShifterT` instances, and a dual-tap crossfade
// shifter does not put a pure tone's shifted energy on a single line. It
// produces a COMB at `f0 + k·(1−r)·1000/crossfade_ms`, whose weights depend on
// `q = f0·crossfade_ms/1000` — the half-window tap separation in half-cycles of
// the input. At the shipped 20 ms window the comb step is 46–67 cents, so the
// dominant line can sit tens of cents from `r·f0` even though the shift ratio is
// exact. Measured on this implementation at the spec's own settings: −25.22,
// +51.61 and +5.93 cents for three different input notes.
//
// That breaks two of the spec's audio tolerances outright (test 4's "±1 bin" =
// 3.9 cents, test 5's "±5 cents") — and the spec contains the refutation itself:
// §5.3 computes the splice rate as 13.0 Hz at exactly the settings test 4 then
// asks to resolve to 0.732 Hz. `T4 defect` ships that measurement. `T5` is
// rebuilt on the one configuration in which a pure tone DOES yield a single
// line — an even `q` — where the measured error is 0.00 cents on all seven
// degrees rather than merely inside a loosened tolerance.
//
// ── Spec deviations, each argued at the case that makes it ───────────────
//
//   §6.1  `LogRampedValueT` is the wrong primitive for a cents-domain glide. It
//         ramps MULTIPLICATIVELY and its `set_target` bails to an instant jump
//         when `current_ <= 0 || target_ <= 0` — and cents targets are routinely
//         0 (unison) and negative (any interval below the input). `T6 defect`
//         demonstrates the silent step. The engine uses `SlewLimiterT` in
//         `SlewMode::linear`, which is what test 6's own criterion — linear in
//         cents, arriving exactly at `glide_ms` — actually describes.
//   §3.1  the difference function as written sums `j` over the full window `W`
//         while reading `x[j+τ]`, which reaches `W − 1 + τ_max` — 600 samples
//         past the end of the very window whose length the reported latency is
//         derived from. The integration length is `W − τ_max` here, which makes
//         the last index touched exactly `W − 1`. `T11` asserts the geometry.
//   §7    `latency_samples() == W` is right at the defaults but the architecture
//         behind it is not: the wet legs have their own throughput delay, so
//         delaying only the dry by `W` leaves the two 720 samples apart. `T7`
//         asserts both legs land on the same sample.
//   §6.2  the spec calls `DriftT::pitch_factor()`; the shipped primitive's
//         accessor is `next()`, and the cents conversion is `ratio_to_cents`.
//   §5.6  formant preservation is outside this delay-domain tier and therefore
//         is not exposed. `T8` proves a supported control is audible.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/harmony_engine.hpp>
#include <pulp/signal/log_ramped_value.hpp>
#include <pulp/signal/rng.hpp>
#include <pulp/signal/units.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

using Engine = HarmonyEngine64;
using Tracker = YinTracker64;
using Mapper = DiatonicMap64;

constexpr double kSr = 48000.0;
constexpr double kPi = 3.14159265358979323846;

/// Equal-temperament reference pitches used by the mapping and audio cases,
/// computed rather than tabulated: MIDI 60..71 is C4..B4.
double midi_hz(int note) { return units::midi_to_hz(static_cast<double>(note)); }

double cents_between(double measured, double reference) {
    return 1200.0 * std::log2(measured / reference);
}

/// Blackman-Harris windowed amplitude at `hz`.
double magnitude_bh(const std::vector<double>& x, double hz) {
    const double w = 2.0 * kPi * hz / kSr;
    const double n_max = static_cast<double>(x.size()) - 1.0;
    double re = 0.0, im = 0.0, wsum = 0.0;
    for (std::size_t n = 0; n < x.size(); ++n) {
        const double t = 2.0 * kPi * static_cast<double>(n) / n_max;
        const double win = 0.35875 - 0.48829 * std::cos(t) +
                           0.14128 * std::cos(2.0 * t) - 0.01168 * std::cos(3.0 * t);
        re += x[n] * win * std::cos(w * static_cast<double>(n));
        im += x[n] * win * std::sin(w * static_cast<double>(n));
        wsum += win;
    }
    return 2.0 * std::hypot(re, im) / wsum;
}

/// Coarse scan then a 50x refinement — resolves a clean peak to `step/50`.
double peak_near(const std::vector<double>& x, double guess, double span = 20.0,
                 double step = 0.1) {
    double best = guess, best_mag = -1.0;
    for (double f = guess - span; f <= guess + span; f += step) {
        const double m = magnitude_bh(x, f);
        if (m > best_mag) { best_mag = m; best = f; }
    }
    const double coarse = best;
    for (double f = coarse - step; f <= coarse + step; f += step / 50.0) {
        const double m = magnitude_bh(x, f);
        if (m > best_mag) { best_mag = m; best = f; }
    }
    return best;
}

double peak(const std::vector<double>& x) {
    double m = 0.0;
    for (double v : x) m = std::max(m, std::abs(v));
    return m;
}

/// An engine set up to render ONE harmony voice with the dry pushed to the
/// bottom of its range, so a spectral read sees the wet leg alone.
Engine make_wet_only(int interval_steps, double crossfade_ms,
                     ScaleType scale = ScaleType::major, int key = 0) {
    Engine engine;
    engine.prepare(kSr);
    engine.set_crossfade_ms(crossfade_ms);
    engine.set_key(key);
    engine.set_scale(scale);
    engine.set_voice_interval(0, interval_steps);
    engine.set_voice_enabled(0, true);
    engine.set_voice_enabled(1, false);
    engine.set_glide_ms(0.0);
    engine.set_dry_level_db(Engine::kLevelMinDb);
    engine.set_voice_level_db(0, 0.0);
    engine.reset();
    return engine;
}

std::vector<double> render_sine(Engine& engine, double hz, int length,
                                double amplitude = 0.5) {
    const int settle = 3 * engine.latency_samples() + 8000;
    std::vector<double> out;
    out.reserve(static_cast<std::size_t>(length));
    for (int n = 0; n < settle + length; ++n) {
        const double x =
            amplitude * std::sin(2.0 * kPi * hz * static_cast<double>(n) / kSr);
        const double y = static_cast<double>(engine.process(x));
        if (n >= settle) out.push_back(y);
    }
    return out;
}

/// Integer floor division — `-1/7` must be `-1`. Reimplemented here so the
/// expectation below does not borrow the implementation's helper.
int floor_div(int a, int b) {
    const int q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

/// The expected diatonic shift, derived in the test FROM THE SHIPPED MASK by
/// walking the degree list — the definition of the operation, not a copy of the
/// implementation's answer. Returns `kNotInScale` for an off-scale input so a
/// caller can restrict itself to the degrees.
constexpr int kNotInScale = -999;

int expected_shift(ScaleType scale, int root, int midi_note, int steps) {
    int degrees[12];
    int count = 0;
    const std::uint16_t mask = kScaleTable[static_cast<std::size_t>(scale)];
    for (int k = 0; k < 12; ++k)
        if ((mask >> k) & 1u) degrees[count++] = k;

    const int rel = midi_note - root;
    const int pc = ((rel % 12) + 12) % 12;
    const int octave = (rel - pc) / 12;

    int degree = -1;
    for (int i = 0; i < count; ++i)
        if (degrees[i] == pc) degree = i;
    if (degree < 0) return kNotInScale;

    const int target_degree = degree + steps;
    const int wrapped = floor_div(target_degree, count);
    const int index = target_degree - wrapped * count;
    const int target = root + 12 * (octave + wrapped) + degrees[index];
    const int snapped = root + 12 * octave + degrees[degree];
    return std::clamp(target - snapped,
                      -Mapper::kShiftSemitonesMax, Mapper::kShiftSemitonesMax);
}

}  // namespace

// ── T1 — pitch tracking accuracy ──────────────────────────────────────────



// ── T2 — the voicing gate ─────────────────────────────────────────────────


// ── T3 — diatonic interval mapping ────────────────────────────────────────







// ── T4 — crossfade behaviour ──────────────────────────────────────────────



// ── T5 — shift-ratio accuracy across the degrees ──────────────────────────



// ── T6 — the cents-domain glide ───────────────────────────────────────────





// ── T7 — latency ──────────────────────────────────────────────────────────




// ── T9 — determinism ──────────────────────────────────────────────────────



// ── T10 — RT allocation ───────────────────────────────────────────────────


// ── T11 — derived geometry and physical achievability ─────────────────────



// ── Gain bound, and the rest of the contract ──────────────────────────────
