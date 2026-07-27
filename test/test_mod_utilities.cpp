// Tier 0 mod-utilities toolkit — the shared modulation infrastructure the DSP
// series composes (see planning/2026-07-25-dsp-series-round2.md, adjudication
// A-1).
//
// The suite asserts the CONTRACTS other modules' specs cite by name — a
// triangle whose 0.5 phase offset is an exact inversion, an LFO rate accurate
// enough for the chorus spec's ±0.01 % zero-crossing test, a constant-time slew
// that arrives exactly, seeded randomness that is bit-reproducible. Expected
// values are computed from the shipped constants rather than restated, so a
// constant that changes fails the test that documents it instead of silently
// disagreeing with it.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/envelope.hpp>
#include <pulp/signal/lfo.hpp>
#include <pulp/signal/lowpass_gate.hpp>
#include <pulp/signal/mod_matrix.hpp>
#include <pulp/signal/rng.hpp>
#include <pulp/signal/slew_limiter.hpp>
#include <pulp/signal/units.hpp>
#include <pulp/signal/vactrol.hpp>
#include <pulp/signal/vca.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <vector>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

constexpr double kSr = 48000.0;

template <typename Fn>
void require_allocates_no_memory(Fn&& fn) {
    pulp::test::RtAllocationProbe probe;
    fn();
    REQUIRE(probe.allocation_count() == 0);
}

/// Measures a rendered signal's frequency from its upward zero crossings — the
/// measurement the chorus/flanger specs use to assert LFO rate accuracy end to
/// end.
///
/// Measures between the FIRST and LAST crossing rather than dividing a crossing
/// count by the render length. Counting over a fixed window is off by up to one
/// crossing depending on where the window boundaries fall relative to the
/// waveform — at 3 Hz over 100 s that is a 0.33 % measurement error, which
/// would swamp the 0.01 % accuracy being tested. Between two crossings the
/// span is exactly `count − 1` periods with no boundary term at all.
double measure_rate_hz(const std::vector<double>& x, double sample_rate) {
    std::size_t first = 0, last = 0;
    int count = 0;
    for (std::size_t i = 1; i < x.size(); ++i) {
        if (x[i - 1] < 0.0 && x[i] >= 0.0) {
            if (count == 0) first = i;
            last = i;
            ++count;
        }
    }
    if (count < 2) return 0.0;
    const double periods = static_cast<double>(count - 1);
    const double span_samples = static_cast<double>(last - first);
    return periods * sample_rate / span_samples;
}

}  // namespace

// ── units ─────────────────────────────────────────────────────────────────

TEST_CASE("units dB conversion round-trips and matches the half-amplitude point",
          "[units][mod-utilities]") {
    // −6.0205999... dB is exactly half amplitude; computed, not restated.
    const double half_db = 20.0 * std::log10(0.5);
    REQUIRE_THAT(units::db_to_linear(half_db), WithinRel(0.5, 1e-12));
    REQUIRE_THAT(units::linear_to_db(0.5), WithinRel(half_db, 1e-12));

    for (double db : {-96.0, -24.0, -6.0, 0.0, 6.0, 24.0})
        REQUIRE_THAT(units::linear_to_db(units::db_to_linear(db)), WithinAbs(db, 1e-10));
}

TEST_CASE("units dB floor is finite for silence and uses the shipped constant",
          "[units][mod-utilities]") {
    const double expected = 20.0 * std::log10(units::kDbFloorAmplitude);
    REQUIRE_THAT(units::linear_to_db(0.0), WithinRel(expected, 1e-12));
    REQUIRE(std::isfinite(units::linear_to_db(0.0)));
    // Magnitude, not signed: a negative gain reports its level.
    REQUIRE_THAT(units::linear_to_db(-0.5), WithinRel(units::linear_to_db(0.5), 1e-12));
}

TEST_CASE("units pitch conversions agree with 12-TET", "[units][mod-utilities]") {
    REQUIRE_THAT(units::midi_to_hz(units::kMidiA4), WithinRel(units::kA4Hz, 1e-12));
    REQUIRE_THAT(units::midi_to_hz(units::kMidiA4 + units::kSemitonesPerOctave),
                 WithinRel(2.0 * units::kA4Hz, 1e-12));
    REQUIRE_THAT(units::hz_to_midi(units::kA4Hz), WithinRel(units::kMidiA4, 1e-12));

    REQUIRE_THAT(units::semitones_to_ratio(12.0), WithinRel(2.0, 1e-12));
    REQUIRE_THAT(units::cents_to_ratio(1200.0), WithinRel(2.0, 1e-12));
    // One semitone is exactly 100 cents.
    REQUIRE_THAT(units::semitones_to_ratio(1.0), WithinRel(units::cents_to_ratio(100.0), 1e-12));
    REQUIRE_THAT(units::ratio_to_cents(units::cents_to_ratio(37.0)), WithinAbs(37.0, 1e-9));

    // 1 V/octave: one volt is one octave.
    REQUIRE_THAT(units::semitones_to_ratio(units::volts_to_semitones(1.0)),
                 WithinRel(2.0, 1e-12));
}

TEST_CASE("units one-pole coefficient matches its documented time constant",
          "[units][mod-utilities]") {
    const double tau_ms = 10.0;
    const double a = units::ms_to_onepole_coef(tau_ms, kSr);

    // A step response driven by this coefficient must reach 1 − 1/e after
    // exactly tau. That IS the definition the doc block states.
    double y = 0.0;
    const int n = static_cast<int>(std::llround(units::ms_to_samples(tau_ms, kSr)));
    for (int i = 0; i < n; ++i) y += a * (1.0 - y);
    REQUIRE_THAT(y, WithinAbs(1.0 - std::exp(-1.0), 1e-4));

    // The two other readings the doc block converts between.
    REQUIRE_THAT(units::t60_ms_to_tau_ms(std::log(1000.0) * tau_ms), WithinRel(tau_ms, 1e-12));

    // Degenerate inputs are "instant", never a division by zero.
    REQUIRE(units::ms_to_onepole_coef(0.0, kSr) == 1.0);
    REQUIRE(units::ms_to_onepole_coef(10.0, 0.0) == 1.0);
}

TEST_CASE("units log taper is geometric and round-trips", "[units][mod-utilities]") {
    const double lo = 20.0, hi = 20000.0;
    // Half travel lands on the geometric mean, not the arithmetic midpoint.
    REQUIRE_THAT(units::taper_log(0.5, lo, hi), WithinRel(std::sqrt(lo * hi), 1e-12));
    REQUIRE_THAT(units::taper_log(0.0, lo, hi), WithinRel(lo, 1e-12));
    REQUIRE_THAT(units::taper_log(1.0, lo, hi), WithinRel(hi, 1e-12));
    for (double u : {0.0, 0.13, 0.5, 0.87, 1.0})
        REQUIRE_THAT(units::untaper_log(units::taper_log(u, lo, hi), lo, hi), WithinAbs(u, 1e-12));
    // A taper through zero has no geometric midpoint; it degrades, not NaNs.
    REQUIRE(std::isfinite(units::taper_log(0.5, 0.0, hi)));
}

// ── rng ───────────────────────────────────────────────────────────────────

TEST_CASE("Xorshift32 is deterministic, reset-repeatable and never zero-locked",
          "[rng][mod-utilities]") {
    Xorshift32 a{12345u};
    Xorshift32 b{12345u};
    for (int i = 0; i < 1000; ++i) REQUIRE(a.next_uint() == b.next_uint());

    a.reset();
    b.reset();
    REQUIRE(a.next_uint() == b.next_uint());

    // Zero is the generator's absorbing state; seeding with it must not stick.
    Xorshift32 z{0u};
    REQUIRE(z.seed() == Xorshift32::kDefaultSeed);
    bool any_nonzero = false;
    for (int i = 0; i < 100; ++i) any_nonzero = any_nonzero || z.next_uint() != 0u;
    REQUIRE(any_nonzero);
}

TEST_CASE("Xorshift32 output ranges hold", "[rng][mod-utilities]") {
    Xorshift32 rng{777u};
    double mean = 0.0;
    constexpr int n = 200000;
    for (int i = 0; i < n; ++i) {
        const double v = rng.next_bipolar<double>();
        REQUIRE(v >= -1.0);
        REQUIRE(v < 1.0);
        mean += v;
    }
    mean /= n;
    // Uniform on [−1, 1): the mean of 2e5 draws sits well inside 0.01 of zero.
    REQUIRE_THAT(mean, WithinAbs(0.0, 0.01));

    Xorshift32 u{888u};
    for (int i = 0; i < 10000; ++i) {
        const double v = u.next_unit<double>();
        REQUIRE(v >= 0.0);
        REQUIRE(v < 1.0);
    }
}

TEST_CASE("mix64 is a stateless keyed draw independent of call order",
          "[rng][mod-utilities]") {
    // The property the granular spec depends on: the same (seed, index, field)
    // gives the same answer regardless of when it is asked for.
    const std::uint64_t seed = 0xC0FFEEull;
    const std::uint64_t forward = mix64(seed, 42, 3);
    for (std::uint64_t i = 0; i < 100; ++i) (void)mix64(seed, i, 0);
    REQUIRE(mix64(seed, 42, 3) == forward);

    // Different fields at the same index must not be correlated — the whole
    // reason `field` exists.
    REQUIRE(mix64(seed, 42, 0) != mix64(seed, 42, 1));
    REQUIRE(mix64(seed, 42, 0) != mix64(seed + 1, 42, 0));

    for (std::uint64_t i = 0; i < 1000; ++i) {
        const double v = unit_from<double>(mix64(seed, i, 0));
        REQUIRE(v >= 0.0);
        REQUIRE(v < 1.0);
        const double b = bipolar_from<double>(mix64(seed, i, 1));
        REQUIRE(b >= -1.0);
        REQUIRE(b < 1.0);
    }
}

TEST_CASE("OuWalk hits the stationary deviation solved for in its update()",
          "[rng][mod-utilities]") {
    // The class solves the step size from θ so that σ is the STATIONARY
    // standard deviation. This asserts that solve, which is the whole reason
    // callers get to state a depth in real units.
    OuWalk64 walk;
    walk.set_theta(OuWalk64::kDefaultTheta);
    walk.set_sigma(0.25);
    walk.set_seed(4242u);
    walk.reset();

    constexpr int burn_in = 20000;
    constexpr int n = 400000;
    for (int i = 0; i < burn_in; ++i) walk.next();
    double sum = 0.0, sum_sq = 0.0;
    for (int i = 0; i < n; ++i) {
        const double v = walk.next();
        sum += v;
        sum_sq += v * v;
    }
    const double mean = sum / n;
    const double sd = std::sqrt(sum_sq / n - mean * mean);
    REQUIRE_THAT(mean, WithinAbs(0.0, 0.02));
    REQUIRE_THAT(sd, WithinRel(0.25, 0.05));
}

TEST_CASE("OU walk clamps theta to its declared design range",
          "[rng][mod-utilities][contract]") {
    OuWalk64 walk;
    walk.set_theta(-1.0);
    REQUIRE(walk.theta() == OuWalk64::kMinTheta);
    walk.set_theta(1.0);
    REQUIRE(walk.theta() == OuWalk64::kMaxTheta);
}

TEST_CASE("OuWalk and Drift are bit-reproducible across reset", "[rng][mod-utilities]") {
    Drift64 drift;
    drift.prepare(kSr);
    drift.set_depth_percent(0.5);
    drift.set_seed(99u);

    drift.reset();
    std::vector<double> first;
    for (int i = 0; i < 5000; ++i) first.push_back(drift.next());

    drift.reset();
    for (int i = 0; i < 5000; ++i) REQUIRE(drift.next() == first[static_cast<std::size_t>(i)]);

    // A drift multiplier is near 1 and strictly positive — the invariant that
    // lets callers multiply a delay time by it without guarding.
    for (double v : first) {
        REQUIRE(v > 0.0);
        REQUIRE_THAT(v, WithinAbs(1.0, 0.1));
    }
}

// ── LFO ───────────────────────────────────────────────────────────────────

TEST_CASE("LFO rate is accurate to far better than the ±0.01 % specs assert",
          "[lfo][mod-utilities]") {
    static_assert(std::is_same_v<decltype(EffectLfo64{}.wave()),
                                 LfoWave>);
    // The chorus spec's test 2, run directly: count zero crossings over a long
    // render and compare against the configured rate.
    constexpr double rate_hz = 3.0;
    constexpr double seconds = 100.0;
    EffectLfo64 lfo;
    lfo.prepare(kSr);
    lfo.set_rate_hz(rate_hz);
    lfo.set_wave(LfoWave::sine);
    lfo.reset();

    std::vector<double> out;
    out.reserve(static_cast<std::size_t>(kSr * seconds));
    for (int i = 0; i < static_cast<int>(kSr * seconds); ++i) out.push_back(lfo.next());

    REQUIRE_THAT(measure_rate_hz(out, kSr), WithinRel(rate_hz, 1e-4));
}

TEST_CASE("LFO shapes stay bipolar and hit their documented landmarks",
          "[lfo][mod-utilities]") {
    for (auto wave : {LfoWave::sine, LfoWave::triangle, LfoWave::saw_up, LfoWave::saw_down,
                      LfoWave::square, LfoWave::sample_hold, LfoWave::smooth_random}) {
        EffectLfo64 lfo;
        lfo.prepare(kSr);
        lfo.set_rate_hz(7.0);
        lfo.set_wave(wave);
        lfo.reset();
        for (int i = 0; i < 100000; ++i) {
            const double v = lfo.next();
            REQUIRE(v >= -1.0);
            REQUIRE(v <= 1.0);
        }
    }

    // The triangle's landmarks: 0 at φ=0, +1 at 0.25, 0 at 0.5, −1 at 0.75.
    // Driven by phase offset so no accumulation is involved.
    EffectLfo64 tri;
    tri.prepare(kSr);
    tri.set_rate_hz(0.0);  // frozen: phase stays at the offset
    tri.set_wave(LfoWave::triangle);
    struct { double phase, expected; } landmarks[] = {
        {0.0, 0.0}, {0.25, 1.0}, {0.5, 0.0}, {0.75, -1.0}};
    for (const auto& lm : landmarks) {
        tri.set_phase_offset(lm.phase);
        tri.reset();
        REQUIRE_THAT(tri.next(), WithinAbs(lm.expected, 1e-12));
    }
}

TEST_CASE("LFO phase offset of half a cycle is exact inversion", "[lfo][mod-utilities]") {
    // The property the Juno/Dimension D voicings assert: two otherwise
    // identical LFOs half a cycle apart are exact negatives of each other, for
    // every odd-symmetric shape.
    for (auto wave : {LfoWave::sine, LfoWave::triangle, LfoWave::square}) {
        EffectLfo64 a, b;
        for (auto* l : {&a, &b}) {
            l->prepare(kSr);
            l->set_rate_hz(2.0);
            l->set_wave(wave);
        }
        b.set_stereo_offset(0.5);
        a.reset();
        b.reset();
        for (int i = 0; i < 200000; ++i) REQUIRE_THAT(a.next() + b.next(), WithinAbs(0.0, 1e-9));
    }
}

TEST_CASE("LFO half-cycle offset is NOT inversion for the saw shapes",
          "[lfo][mod-utilities]") {
    // The negative half of the case above, pinned deliberately.
    //
    // The header used to claim half-cycle inversion for "triangle, sine, saw"
    // while the test above iterated {sine, triangle, square} — the set had been
    // narrowed to the shapes that pass without the claim being corrected, so
    // the false half was load-bearing documentation that nothing measured.
    //
    // A sawtooth's discontinuity makes this inherent: a half-cycle shift is a
    // shift, not a negation. saw(φ + 0.5) = −saw(φ) holds only at φ = 0.5, and
    // everywhere else the two differ by a full unit. Asserting that here means
    // a future change that "fixes" saw to invert has to come here and explain
    // itself, rather than silently altering what anti-phase means for the
    // chorus/flanger/phaser pair constructions built on set_phase_offset(0.5).
    for (auto wave : {LfoWave::saw_up, LfoWave::saw_down}) {
        EffectLfo64 a, b;
        for (auto* l : {&a, &b}) {
            l->prepare(kSr);
            l->set_rate_hz(2.0);
            l->set_wave(wave);
        }
        b.set_stereo_offset(0.5);
        a.reset();
        b.reset();

        double worst = 0.0;
        for (int i = 0; i < 20000; ++i) worst = std::max(worst, std::abs(a.next() + b.next()));
        // Not merely "imperfect" — the sum reaches a full unit.
        REQUIRE_THAT(worst, WithinAbs(1.0, 1e-9));
    }
}

TEST_CASE("LFO N-voice spacing is even", "[lfo][mod-utilities]") {
    // TriChorus: three voices at 120°. Their instantaneous sine values must sum
    // to zero at every sample, which is the algebraic form of even spacing.
    constexpr int n = 3;
    EffectLfo64 voices[n];
    for (int k = 0; k < n; ++k) {
        voices[k].prepare(kSr);
        voices[k].set_rate_hz(1.5);
        voices[k].set_wave(LfoWave::sine);
        voices[k].set_phase_offset(static_cast<double>(k) / n);
        voices[k].reset();
    }
    for (int i = 0; i < 100000; ++i) {
        double sum = 0.0;
        for (auto& v : voices) sum += v.next();
        REQUIRE_THAT(sum, WithinAbs(0.0, 1e-9));
    }
}

TEST_CASE("LFO quadrature is exact and drift-free over a long render",
          "[lfo][mod-utilities]") {
    // The frequency-shifter spec forbids a recursive resonator here precisely
    // because its amplitude drifts. Assert the invariant that forbids it:
    // sin² + cos² == 1 forever, not just at the start.
    EffectLfo64 lfo;
    lfo.prepare(kSr);
    lfo.set_rate_hz(200.0);
    lfo.reset();
    double s = 0.0, c = 0.0;
    for (int i = 0; i < 2000000; ++i) {
        lfo.next_quadrature(s, c);
        REQUIRE_THAT(s * s + c * c, WithinAbs(1.0, 1e-12));
    }
}

TEST_CASE("LFO random shapes are seeded and reset-repeatable", "[lfo][mod-utilities]") {
    EffectLfo64 lfo;
    lfo.prepare(kSr);
    lfo.set_rate_hz(20.0);
    lfo.set_wave(LfoWave::sample_hold);
    lfo.set_seed(31337u);

    lfo.reset();
    std::vector<double> first;
    for (int i = 0; i < 20000; ++i) first.push_back(lfo.next());
    lfo.reset();
    for (int i = 0; i < 20000; ++i) REQUIRE(lfo.next() == first[static_cast<std::size_t>(i)]);

    // sample_hold is piecewise constant; smooth_random is not. Both must
    // actually move.
    bool moved = false;
    for (std::size_t i = 1; i < first.size(); ++i) moved = moved || first[i] != first[i - 1];
    REQUIRE(moved);
}

TEST_CASE("LFO rate is clamped to its documented ceiling", "[lfo][mod-utilities]") {
    EffectLfo64 lfo;
    lfo.set_rate_hz(1e6);
    REQUIRE(lfo.rate_hz() == EffectLfoT<double>::kMaxRateHz);
    lfo.set_rate_hz(-5.0);
    REQUIRE(lfo.rate_hz() == 0.0);
}

// ── slew limiter / sample & hold ──────────────────────────────────────────

TEST_CASE("SlewLimiter linear mode takes the same time regardless of distance",
          "[slew][mod-utilities]") {
    // The TB-303-lineage constant-time portamento the stage sequencer cites:
    // one semitone and one octave take the same 30 ms.
    constexpr double slide_ms = 30.0;
    const int expected = static_cast<int>(std::llround(units::ms_to_samples(slide_ms, kSr)));

    for (double distance : {1.0, 12.0, 0.25}) {
        ConstantTimeSlewLimiter64 slew;
        slew.prepare(kSr);
        slew.set_mode(SlewMode::linear);
        slew.set_time_ms(slide_ms);
        slew.set_immediate(0.0);

        // `settled()` is true before the first `process()` — the limiter has
        // not been shown a target yet — so the count has to be a do-while.
        int samples = 0;
        do {
            slew.process(distance);
            ++samples;
        } while (!slew.settled() && samples < expected * 4);
        REQUIRE(slew.settled());
        // Exact arrival, in the stated time, within one sample of rounding.
        REQUIRE(std::abs(samples - expected) <= 1);
        REQUIRE_THAT(slew.value(), WithinAbs(distance, 1e-12));
    }
}

TEST_CASE("SlewLimiter exponential mode matches the units time constant",
          "[slew][mod-utilities]") {
    constexpr double tau_ms = 20.0;
    ConstantTimeSlewLimiter64 slew;
    slew.prepare(kSr);
    slew.set_mode(SlewMode::exponential);
    slew.set_time_ms(tau_ms);
    slew.set_immediate(0.0);

    const int n = static_cast<int>(std::llround(units::ms_to_samples(tau_ms, kSr)));
    for (int i = 0; i < n; ++i) slew.process(1.0);
    REQUIRE_THAT(slew.value(), WithinAbs(1.0 - std::exp(-1.0), 1e-4));
}

TEST_CASE("SlewLimiter rise and fall are independent", "[slew][mod-utilities]") {
    ConstantTimeSlewLimiter64 slew;
    slew.prepare(kSr);
    slew.set_mode(SlewMode::linear);
    slew.set_rise_ms(10.0);
    slew.set_fall_ms(100.0);
    slew.set_immediate(0.0);

    int up = 0;
    do { slew.process(1.0); ++up; } while (!slew.settled() && up < 100000);
    int down = 0;
    do { slew.process(0.0); ++down; } while (!slew.settled() && down < 1000000);

    // A 10× longer fall time takes 10× as many samples, within rounding.
    REQUIRE_THAT(static_cast<double>(down) / up, WithinRel(10.0, 0.02));
}

TEST_CASE("SlewLimiter with zero time is a pass-through", "[slew][mod-utilities]") {
    for (auto mode : {SlewMode::linear, SlewMode::exponential}) {
        ConstantTimeSlewLimiter64 slew;
        slew.prepare(kSr);
        slew.set_mode(mode);
        slew.set_time_ms(0.0);
        slew.reset();
        REQUIRE_THAT(slew.process(0.7), WithinAbs(0.7, 1e-12));
        REQUIRE_THAT(slew.process(-0.3), WithinAbs(-0.3, 1e-12));
    }
}

TEST_CASE("SlewLimiter recovers after a non-finite control sample",
          "[slew][mod-utilities][nan-recovery]") {
    for (auto mode : {SlewMode::linear, SlewMode::exponential}) {
        ConstantTimeSlewLimiter64 slew;
        slew.prepare(kSr);
        slew.set_mode(mode);
        slew.set_time_ms(20.0);
        for (int i = 0; i < 256; ++i) (void)slew.process(1.0);

        REQUIRE(slew.process(std::numeric_limits<double>::quiet_NaN()) == 0.0);
        for (int i = 0; i < 64; ++i) {
            REQUIRE(std::isfinite(slew.process(0.25)));
            REQUIRE(std::isfinite(slew.value()));
        }
    }
}

TEST_CASE("SampleHold latches on the rising edge only", "[slew][mod-utilities]") {
    SampleHold64 sh;
    sh.reset();

    REQUIRE_THAT(sh.process_signal(0.5, 0.0), WithinAbs(0.0, 1e-12));  // no edge yet
    REQUIRE_THAT(sh.process_signal(0.5, 1.0), WithinAbs(0.5, 1e-12));  // rising: latch
    REQUIRE_THAT(sh.process_signal(0.9, 1.0), WithinAbs(0.5, 1e-12));  // still high: hold
    REQUIRE_THAT(sh.process_signal(0.9, 0.0), WithinAbs(0.5, 1e-12));  // falling: hold
    REQUIRE_THAT(sh.process_signal(0.9, 1.0), WithinAbs(0.9, 1e-12));  // rising again: latch
}

TEST_CASE("SignalComparator preserves the Round-2 level-valued gate API",
          "[trigger][mod-utilities]") {
    SignalComparator64 comparator;
    comparator.set_thresholds(0.6, 0.4);
    comparator.set_levels(-2.0, 5.0);

    REQUIRE(comparator.process(0.5) == -2.0);
    REQUIRE(comparator.process(0.7) == 5.0);
    REQUIRE(comparator.process(0.5) == 5.0);
    REQUIRE(comparator.process(0.3) == -2.0);
}

// ── envelopes ─────────────────────────────────────────────────────────────

TEST_CASE("Envelope segments take exactly their stated time at every curve",
          "[envelope][mod-utilities]") {
    // The normalisation claim in the doc block: a 200 ms decay is 200 ms
    // whether the curve is linear or strongly exponential.
    constexpr double attack_ms = 50.0;
    const int expected = static_cast<int>(std::llround(units::ms_to_samples(attack_ms, kSr)));

    for (double curve : {0.0, 0.5, 1.0}) {
        Ar env;
        env.prepare(kSr);
        env.set_attack_ms(attack_ms);
        env.set_release_ms(1000.0);
        env.set_curve(curve);
        env.reset();
        env.gate_on();

        int samples = 0;
        while (env.stage() == EnvelopeStage::attack && samples < expected * 4) {
            env.next();
            ++samples;
        }
        REQUIRE(std::abs(samples - expected) <= 2);
        REQUIRE_THAT(static_cast<double>(env.value()), WithinAbs(1.0, 1e-6));
    }
}

TEST_CASE("Envelope curve 0 is exactly linear", "[envelope][mod-utilities]") {
    Ar env;
    env.prepare(kSr);
    env.set_attack_ms(10.0);
    env.set_curve(0.0);
    env.reset();
    env.gate_on();

    const double n = units::ms_to_samples(10.0, kSr);
    for (int i = 0; i < static_cast<int>(n) / 2; ++i) {
        const double v = env.next();
        REQUIRE_THAT(v, WithinAbs(i / n, 1e-6));
    }
}

TEST_CASE("Ar uses level-before-advance ordering through release",
          "[envelope][mod-utilities][contract]") {
    Ar env;
    env.prepare(kSr);
    env.set_attack_ms(0.0);
    env.set_release_ms(10.0);
    env.set_curve(0.0);
    env.reset();
    env.gate_on();
    REQUIRE_THAT(env.next(), WithinAbs(1.0, 1e-12));

    env.gate_off();
    const double release_samples = units::ms_to_samples(10.0, kSr);
    REQUIRE_THAT(env.next(), WithinAbs(1.0, 1e-12));
    REQUIRE_THAT(env.next(), WithinAbs(1.0 - 1.0 / release_samples, 1e-6));
}

TEST_CASE("Envelope releases from any stage without hanging", "[envelope][mod-utilities]") {
    // A staccato note on a 2-second attack must not hang for 2 seconds.
    Dahdsr env;
    env.prepare(kSr);
    env.set_delay_ms(500.0);
    env.set_attack_ms(2000.0);
    env.set_release_ms(10.0);
    env.reset();

    env.gate_on();
    for (int i = 0; i < 100; ++i) env.next();
    REQUIRE(env.stage() == EnvelopeStage::delay);
    env.gate_off();
    REQUIRE(env.stage() == EnvelopeStage::release);

    const int release_samples = static_cast<int>(std::llround(units::ms_to_samples(10.0, kSr)));
    int n = 0;
    while (env.active() && n < release_samples * 4) { env.next(); ++n; }
    REQUIRE_FALSE(env.active());
    REQUIRE(n <= release_samples + 2);
}

TEST_CASE("Envelope retrigger continues from the current level", "[envelope][mod-utilities]") {
    Ar env;
    env.prepare(kSr);
    env.set_attack_ms(100.0);
    env.set_release_ms(100.0);
    env.reset();

    env.gate_on();
    for (int i = 0; i < 2000; ++i) env.next();
    env.gate_off();
    for (int i = 0; i < 500; ++i) env.next();
    const double before = env.value();
    REQUIRE(before > 0.0);

    env.gate_on();
    const double after = env.next();
    // No jump to zero — the retrigger is continuous, which is what makes it
    // click-free.
    REQUIRE_THAT(after, WithinAbs(before, 0.02));
}

TEST_CASE("Retrigger is click-free on the delay-capable shapes too",
          "[envelope][mod-utilities]") {
    // The case above uses `Ar` — the ONE shape with the delay stage disabled —
    // so it could not see this. Every delay-capable shape punched a full-scale
    // one-sample notch to zero on retrigger, because `stage_samples()` floors
    // every stage at `kMinStageMs`: `delay_samples_` was nonzero even when the
    // caller asked for no delay, so `gate_on()` always routed through the delay
    // stage, which forces the level to zero.
    //
    // Measured against the signal's own slew rather than an absolute epsilon,
    // because "no click" means "no step the surrounding waveform would not
    // itself have produced".
    Dahdsr env;
    env.prepare(kSr);
    env.set_delay_ms(0.0);
    env.set_attack_ms(5.0);
    env.set_hold_ms(1.0);
    env.set_decay_ms(5.0);
    env.set_sustain(0.5);
    env.set_release_ms(10.0);
    env.reset();
    env.gate_on();
    for (int i = 0; i < 2000; ++i) env.next();

    double previous = env.next();
    double worst_drop = 0.0;
    for (int r = 0; r < 5; ++r) {
        env.gate_on();
        for (int i = 0; i < 64; ++i) {
            const double v = env.next();
            worst_drop = std::max(worst_drop, previous - v);
            previous = v;
        }
    }
    // The attack from sustain is a RISE; a retrigger must never step downward
    // at all. Before the fix this was a drop of the full sustain level.
    REQUIRE(worst_drop < 1e-6);
}

TEST_CASE("A delay-capable shape still delays when a delay is asked for",
          "[envelope][mod-utilities]") {
    // The guard on the fix above: making `delay_ms = 0` mean "no delay" must
    // not turn a real delay into no delay either.
    Dahdsr env;
    env.prepare(kSr);
    env.set_delay_ms(10.0);
    env.set_attack_ms(5.0);
    env.reset();
    env.gate_on();

    int first_nonzero = -1;
    for (int i = 0; i < 4000 && first_nonzero < 0; ++i)
        if (env.next() > 1e-9) first_nonzero = i;

    // 10 ms at the suite's rate, within a sample of the accumulator's rounding.
    const int expected = static_cast<int>(0.010 * kSr);
    REQUIRE(first_nonzero >= expected - 1);
    REQUIRE(first_nonzero <= expected + 1);
}

TEST_CASE("Envelope shapes enable the stages their names promise",
          "[envelope][mod-utilities]") {
    // AhdT holds at peak; ArT does not have a hold stage at all.
    Ahd ahd;
    ahd.prepare(kSr);
    ahd.set_attack_ms(1.0);
    ahd.set_hold_ms(50.0);
    ahd.set_decay_ms(50.0);
    ahd.reset();
    ahd.gate_on();
    bool saw_hold = false;
    for (int i = 0; i < 20000; ++i) {
        ahd.next();
        saw_hold = saw_hold || ahd.stage() == EnvelopeStage::hold;
    }
    REQUIRE(saw_hold);

    Ar ar;
    ar.prepare(kSr);
    ar.set_attack_ms(1.0);
    ar.reset();
    ar.gate_on();
    double peak = 0.0;
    double previous = 0.0;
    double worst_drop = 0.0;
    for (int i = 0; i < 20000; ++i) {
        const double v = ar.next();
        REQUIRE(ar.stage() != EnvelopeStage::hold);
        peak = std::max(peak, v);
        if (i > 0) worst_drop = std::max(worst_drop, previous - v);
        previous = v;
    }

    // Reading the LEVEL, not just the stage. Asserting only `stage() != hold`
    // passes for ANY level the shape settles at, which is how this went unseen:
    // with a held gate `ArT` parked at the unrelated `sustain_` member (default
    // 0.7) and stepped down 3.1 dB in a single sample the moment attack
    // finished. `additive_bank.hpp` had already met this and worked around it
    // locally by setting sustain to exactly 1, leaving the trap armed for every
    // other caller.
    //
    // The shape has no decay and no sustain segment, so a held gate holds at
    // the peak — and holding means no downward step at all.
    REQUIRE_THAT(peak, WithinAbs(1.0, 1e-9));
    REQUIRE_THAT(previous, WithinAbs(1.0, 1e-9));
    REQUIRE(worst_drop < 1e-9);
}

TEST_CASE("ModEnv rests at exactly zero and peaks at its depth",
          "[envelope][mod-utilities]") {
    // The trap this type exists to avoid: an attenuverted unipolar envelope
    // would offset its destination while idle.
    ModEnv env;
    env.prepare(kSr);
    env.set_attack_ms(1.0);
    env.set_decay_ms(1.0);
    env.set_sustain(1.0);
    env.set_depth(-0.5);
    env.reset();

    REQUIRE_THAT(static_cast<double>(env.next()), WithinAbs(0.0, 1e-12));
    env.gate_on();
    double extreme = 0.0;
    for (int i = 0; i < 5000; ++i) {
        const double v = env.next();
        if (std::abs(v) > std::abs(extreme)) extreme = v;
    }
    REQUIRE_THAT(extreme, WithinAbs(-0.5, 1e-3));
}

// ── VCA / attenuverter ────────────────────────────────────────────────────

TEST_CASE("VCA laws both peak at exactly unity and close to exactly zero",
          "[vca][mod-utilities]") {
    for (auto response : {VcaResponse::linear, VcaResponse::exponential}) {
        Vca64 vca;
        vca.set_response(response);
        REQUIRE_THAT(static_cast<double>(vca.gain_for(1.0)), WithinAbs(1.0, 1e-12));
        REQUIRE(vca.gain_for(0.0) == 0.0);
        // Never amplifies, whatever the caller passes.
        REQUIRE(vca.gain_for(5.0) <= 1.0);
        REQUIRE(vca.gain_for(-5.0) >= 0.0);
    }
}

TEST_CASE("VCA exponential law is equal-dB per equal control travel",
          "[vca][mod-utilities]") {
    Vca64 vca;
    vca.set_response(VcaResponse::exponential);
    vca.set_range_db(60.0);

    // Half travel is half the range, in dB — the definition of the law.
    REQUIRE_THAT(units::linear_to_db(static_cast<double>(vca.gain_for(0.5))),
                 WithinAbs(-30.0, 1e-9));
    REQUIRE_THAT(units::linear_to_db(static_cast<double>(vca.gain_for(0.25))),
                 WithinAbs(-45.0, 1e-9));
}

TEST_CASE("VCA range setter enforces its declared design range",
          "[vca][mod-utilities][contract]") {
    Vca64 vca;
    vca.set_range_db(-1.0);
    REQUIRE(vca.range_db() == Vca64::kMinRangeDb);
    vca.set_range_db(1000.0);
    REQUIRE(vca.range_db() == Vca64::kMaxRangeDb);
}

TEST_CASE("Attenuverter inverts and its settings bound the output", "[vca][mod-utilities]") {
    Attenuverter64 att;
    att.set_gain(-1.0);
    att.set_offset(0.0);
    REQUIRE_THAT(static_cast<double>(att.process(0.7)), WithinAbs(-0.7, 1e-12));

    att.set_gain(0.5);
    att.set_offset(0.5);
    REQUIRE_THAT(static_cast<double>(att.process(-1.0)), WithinAbs(0.0, 1e-12));
    REQUIRE_THAT(static_cast<double>(att.process(1.0)), WithinAbs(1.0, 1e-12));
    REQUIRE_THAT(std::abs(static_cast<double>(att.gain())) +
                     std::abs(static_cast<double>(att.offset())),
                 WithinAbs(1.0, 1e-12));

    // The bound actually bounds, for unit-bounded input.
    att.set_gain(-0.3);
    att.set_offset(0.2);
    const double worst_case = std::abs(static_cast<double>(att.gain())) +
                              std::abs(static_cast<double>(att.offset()));
    for (double x = -1.0; x <= 1.0; x += 0.01)
        REQUIRE(std::abs(static_cast<double>(att.process(x))) <= worst_case + 1e-12);
}

// ── vactrol ───────────────────────────────────────────────────────────────

TEST_CASE("Vactrol rise is fast and fall is slow, as the component is",
          "[vactrol][mod-utilities]") {
    VactrolConditioner64 v;
    v.prepare(kSr);
    v.set_rise_ms(2.0);
    v.set_fall_ms(200.0);
    v.reset();

    // Rise: samples to cover 1 − 1/e of the way from 0 to 1. That is one τ by
    // definition, so it must equal the configured rise time in samples.
    const double tau_fraction = 1.0 - std::exp(-1.0);
    int up = 0;
    while (v.control() < tau_fraction && up < 1000000) { v.process(1.0); ++up; }
    REQUIRE_THAT(static_cast<double>(up), WithinRel(units::ms_to_samples(2.0, kSr), 0.05));

    // Fall must be measured from a SATURATED state, not from wherever the rise
    // measurement stopped — decaying from 0.632 rather than 1.0 covers only
    // 0.54 τ before crossing 1/e, which reads as a fall time nearly half what
    // was configured.
    for (int i = 0; i < 1000000 && v.control() < 0.9999; ++i) v.process(1.0);
    int down = 0;
    while (v.control() > std::exp(-1.0) && down < 1000000) { v.process(0.0); ++down; }
    REQUIRE_THAT(static_cast<double>(down), WithinRel(units::ms_to_samples(200.0, kSr), 0.05));

    REQUIRE(down > up * 50);
}

TEST_CASE("Vactrol time setters enforce their declared design ranges",
          "[vactrol][mod-utilities][contract]") {
    VactrolConditioner64 v;
    v.set_rise_ms(-1.0);
    REQUIRE(v.rise_ms() == VactrolConditioner64::kMinRiseMs);
    v.set_rise_ms(1000.0);
    REQUIRE(v.rise_ms() == VactrolConditioner64::kMaxRiseMs);
    v.set_fall_ms(-1.0);
    REQUIRE(v.fall_ms() == VactrolConditioner64::kMinFallMs);
    v.set_fall_ms(10000.0);
    REQUIRE(v.fall_ms() == VactrolConditioner64::kMaxFallMs);
}

TEST_CASE("Vactrol conditioner recovers after a non-finite control sample",
          "[vactrol][mod-utilities][nan-recovery]") {
    VactrolConditioner64 v;
    v.prepare(kSr);
    for (int i = 0; i < 256; ++i) (void)v.process(1.0);

    REQUIRE(v.process(std::numeric_limits<double>::quiet_NaN()) == 0.0);
    for (int i = 0; i < 64; ++i) {
        REQUIRE(std::isfinite(v.process(0.25)));
        REQUIRE(std::isfinite(v.control()));
    }
}

TEST_CASE("LowpassGate still behaves identically after composing the conditioner",
          "[vactrol][mod-utilities]") {
    // The extraction refactor must be behaviour-preserving. Reproduce the
    // conditioner's recurrence independently and require the gate's exposed
    // control to match it sample for sample.
    LowpassGate64 gate;
    gate.set_sample_rate(kSr);
    gate.set_rise_ms(3.0);
    gate.set_fall_ms(150.0);
    gate.reset();

    const double rise_a = 1.0 - std::exp(-1.0 / std::max(0.001 * 3.0 * kSr, 1e-9));
    const double fall_a = 1.0 - std::exp(-1.0 / std::max(0.001 * 150.0 * kSr, 1e-9));
    double reference = 0.0;

    for (int i = 0; i < 20000; ++i) {
        const double target = i < 8000 ? 1.0 : 0.0;
        gate.process(0.5, target);
        reference += (target > reference ? rise_a : fall_a) * (target - reference);
        REQUIRE_THAT(gate.control(), WithinAbs(reference, 1e-12));
    }
}

// ── RT allocation probe roster ────────────────────────────────────────────

TEST_CASE("mod-utilities allocate nothing on the audio thread",
          "[mod-utilities][rt-safety]") {
    Lfo lfo;
    SlewLimiter slew;
    SampleHold sh;
    Dahdsr env;
    ModEnv mod_env;
    Vca vca;
    Attenuverter att;
    VactrolConditioner vactrol;
    LowpassGate lpg;
    Drift drift;
    OuWalk walk;
    Xorshift32 rng{7u};
    DenseModMatrixT<8, 8, float> matrix;

    // prepare() may allocate by contract; do it outside the probe.
    lfo.prepare(kSr);
    slew.prepare(kSr);
    env.prepare(kSr);
    mod_env.prepare(kSr);
    vactrol.prepare(kSr);
    lpg.set_sample_rate(kSr);
    drift.prepare(kSr);
    matrix.add_route(0, 0, 0.5f);

    require_allocates_no_memory([&] {
        for (int i = 0; i < 512; ++i) {
            const float x = static_cast<float>(i % 7) * 0.1f;
            const float clock = (i % 64) == 0 ? 1.0f : 0.0f;

            lfo.set_rate_hz(1.0 + 0.001 * i);
            (void)lfo.next();
            float s = 0.0f, c = 0.0f;
            lfo.next_quadrature(s, c);

            slew.set_time_ms(5.0);
            (void)slew.process(x);
            (void)sh.process(x, clock);

            if (i == 0) env.gate_on();
            if (i == 256) env.gate_off();
            (void)env.next();
            (void)mod_env.next();

            (void)vca.process(x, 0.5f);
            (void)att.process(x);
            (void)vactrol.process(0.5);
            (void)lpg.process(x, 0.5);

            (void)drift.next();
            (void)walk.next();
            (void)rng.next_bipolar<float>();
            (void)mix64(1, static_cast<std::uint64_t>(i), 2);

            matrix.set_source(0, x);
            matrix.process();
            (void)matrix.get(0);
        }
        lfo.reset();
        slew.reset();
        env.reset();
        matrix.reset();
    });
}
