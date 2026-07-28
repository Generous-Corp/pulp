// Tier 0 mod-utilities toolkit — units, rng.
//
// Split out of the original single-file suite: shared includes and
// helpers live in test_mod_utilities_support.hpp so each file states
// only the contracts it asserts.

#include "test_mod_utilities_support.hpp"


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
