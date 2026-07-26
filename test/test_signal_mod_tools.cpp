// Modulation toolkit control tools and unit conversions.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/signal/mod_tools.hpp>
#include <pulp/signal/rng.hpp>
#include <pulp/signal/units.hpp>

#include <cmath>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;

// ── small tools ──────────────────────────────────────────────────────────────

TEST_CASE("SlewLimiter linear mode takes exactly the set ramp time", "[signal][mod][tools]") {
    SlewLimiter slew;
    slew.prepare(48000.0f);
    slew.set_mode(SlewLimiter::Mode::linear);
    slew.set_times_ms(10.0f, 20.0f); // 480 up, 960 down
    slew.reset(0.0f);

    for (int i = 0; i < 479; ++i)
        (void)slew.process(1.0f);
    REQUIRE(slew.current() < 1.0f);
    REQUIRE_THAT(slew.process(1.0f), WithinAbs(1.0f, 1e-5f));

    for (int i = 0; i < 959; ++i)
        (void)slew.process(0.0f);
    REQUIRE(slew.current() > 0.0f);
    REQUIRE_THAT(slew.process(0.0f), WithinAbs(0.0f, 1e-5f));
}

TEST_CASE("SlewLimiter exponential mode reaches one time constant", "[signal][mod][tools]") {
    SlewLimiter slew;
    slew.prepare(48000.0f);
    slew.set_mode(SlewLimiter::Mode::exponential);
    slew.set_times_ms(10.0f, 10.0f);
    slew.reset(0.0f);

    for (int i = 0; i < 480; ++i)
        (void)slew.process(1.0f);
    REQUIRE_THAT(slew.current(), WithinAbs(0.632f, 0.005f));
}

TEST_CASE("SlewLimiter keeps moving when the step is below float precision",
          "[signal][mod][tools]") {
    // A two-minute linear slew at 384 kHz steps ~2.2e-8 per sample — less
    // than half an ulp of 0.5f, which a float accumulator absorbs, stalling
    // the value forever. The limiter accumulates in double, so the ramp
    // keeps its documented travel time at any rate and length.
    SlewLimiter slew;
    slew.prepare(384000.0f);
    slew.set_mode(SlewLimiter::Mode::linear);
    slew.set_times_ms(120000.0f, 120000.0f);
    slew.reset(0.5f);
    for (int i = 0; i < 1000000; ++i)
        (void)slew.process(1.0f);
    // One million samples of a 46.08-million-sample full-span ramp.
    REQUIRE_THAT(slew.current(), WithinAbs(0.5f + 1.0e6f / 46.08e6f, 2.0e-4f));
}

TEST_CASE("SampleHold latches on the clock's rising edge", "[signal][mod][tools]") {
    SampleHold hold;
    hold.prepare(48000.0f);
    hold.reset(0.0f);

    REQUIRE_THAT(hold.process(0.5f, true), WithinAbs(0.5f, 1e-6f));
    REQUIRE_THAT(hold.process(0.9f, true), WithinAbs(0.5f, 1e-6f)); // level, not edge
    REQUIRE_THAT(hold.process(0.9f, false), WithinAbs(0.5f, 1e-6f));
    REQUIRE_THAT(hold.process(0.9f, true), WithinAbs(0.9f, 1e-6f)); // new edge
}

TEST_CASE("SampleHold glide ramps to the latched value", "[signal][mod][tools]") {
    SampleHold hold;
    hold.prepare(48000.0f);
    hold.set_glide_ms(10.0f); // 480 samples
    hold.reset(0.0f);

    const float first = hold.process(1.0f, true);
    REQUIRE(first < 1.0f);
    REQUIRE(hold.held() == 1.0f);
    for (int i = 0; i < 480; ++i)
        (void)hold.process(1.0f, false);
    REQUIRE_THAT(hold.process(1.0f, false), WithinAbs(1.0f, 1e-5f));
}

TEST_CASE("Attenuverter inverts and re-centres", "[signal][mod][tools]") {
    Attenuverter attenuverter;
    attenuverter.set_gain(-0.8f);
    attenuverter.set_offset(1.0f);
    REQUIRE_THAT(attenuverter.process(0.0f), WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(attenuverter.process(1.0f), WithinAbs(0.2f, 1e-6f));

    attenuverter.set_gain(10.0f); // clamped
    REQUIRE_THAT(attenuverter.gain(), WithinAbs(Attenuverter::kMaxGain, 1e-6f));
}

TEST_CASE("Bipolar and unipolar helpers round-trip", "[signal][mod][tools]") {
    for (float x = -1.0f; x <= 1.0f; x += 0.05f)
        REQUIRE_THAT(uni_to_bi(bi_to_uni(x)), WithinAbs(x, 1e-6f));
}

TEST_CASE("Rectifier folds or gates the negative half", "[signal][mod][tools]") {
    Rectifier rectifier;
    rectifier.set_mode(Rectifier::Mode::full_wave);
    REQUIRE_THAT(rectifier.process(-0.7f), WithinAbs(0.7f, 1e-6f));
    rectifier.set_mode(Rectifier::Mode::half_wave);
    REQUIRE_THAT(rectifier.process(-0.7f), WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(rectifier.process(0.7f), WithinAbs(0.7f, 1e-6f));
}

TEST_CASE("Comparator hysteresis suppresses chatter", "[signal][mod][tools]") {
    Comparator comparator;
    comparator.set_threshold(0.5f); // default hysteresis 0.025
    REQUIRE_THAT(comparator.hysteresis(), WithinAbs(0.025f, 1e-6f));

    REQUIRE_FALSE(comparator.process(0.51f)); // inside the dead band
    REQUIRE(comparator.process(0.53f));
    REQUIRE(comparator.process(0.49f)); // still inside, stays high
    REQUIRE_FALSE(comparator.process(0.47f));

    comparator.set_hysteresis(0.0f);
    REQUIRE(comparator.process(0.51f));
}

TEST_CASE("Comparator keeps a dead band at the default threshold of zero", "[signal][mod][tools]") {
    // A bipolar signal's natural threshold is 0, where a proportional-only
    // auto hysteresis would derive a dead band of 0 — the exact chatter the
    // class exists to prevent.
    Comparator comparator;
    REQUIRE(comparator.hysteresis() > 0.0f);

    Xorshift32 rng(7u);
    int transitions = 0;
    bool prev = false;
    for (int i = 0; i < 480000; ++i) {
        // 0.5 Hz sine plus a sliver of noise, as any real mod signal carries.
        const float x = std::sin(static_cast<float>(2.0 * 3.14159265358979 * 0.5 * i / 48000.0)) +
                        2.0e-4f * rng.next_bipolar();
        const bool gate = comparator.process(x);
        if (gate != prev)
            ++transitions;
        prev = gate;
    }
    REQUIRE(transitions == 10); // two clean crossings per cycle, five cycles
}

TEST_CASE("Quantizer snaps to N equally spaced levels", "[signal][mod][tools]") {
    Quantizer quantizer;
    quantizer.set_range(0.0f, 1.0f);
    quantizer.set_steps(5); // 0, 0.25, 0.5, 0.75, 1

    REQUIRE_THAT(quantizer.process(0.0f), WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(quantizer.process(0.3f), WithinAbs(0.25f, 1e-6f));
    REQUIRE_THAT(quantizer.process(0.4f), WithinAbs(0.5f, 1e-6f));
    REQUIRE_THAT(quantizer.process(0.375f), WithinAbs(0.5f, 1e-6f)); // ties round up
    REQUIRE_THAT(quantizer.process(1.0f), WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(quantizer.process(5.0f), WithinAbs(1.0f, 1e-6f)); // clamps
    REQUIRE_THAT(quantizer.process(-5.0f), WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("Stage curve law matches its closed form", "[signal][mod][tools]") {
    for (float curve = -1.0f; curve <= 1.0f; curve += 0.25f) {
        REQUIRE_THAT(stage_curve(0.0f, curve), WithinAbs(0.0f, 1e-6f));
        REQUIRE_THAT(stage_curve(1.0f, curve), WithinAbs(1.0f, 1e-6f));
    }
    // Linear limit.
    for (float p = 0.0f; p <= 1.0f; p += 0.1f)
        REQUIRE_THAT(stage_curve(p, 0.0f), WithinAbs(p, 1e-6f));

    // (1 - e^-4) / (1 - e^-8) at p = 0.5, curve = +1.
    const float expected = (1.0f - std::exp(-4.0f)) / (1.0f - std::exp(-8.0f));
    REQUIRE_THAT(stage_curve(0.5f, 1.0f), WithinAbs(expected, 1e-6f));
}

TEST_CASE("Rise and fall curves both call plus one exponential", "[signal][mod][tools]") {
    // An exponential attack is slow to leave zero...
    REQUIRE(curve_rise(0.5f, 1.0f) < 0.5f);
    // ...and an exponential decay drops fast and then tails.
    REQUIRE(curve_fall(0.5f, 1.0f) < 0.5f);
    // The logarithmic sign is the mirror of each.
    REQUIRE(curve_rise(0.5f, -1.0f) > 0.5f);
    REQUIRE(curve_fall(0.5f, -1.0f) > 0.5f);

    // Endpoints are exact for every curve.
    for (float c = -1.0f; c <= 1.0f; c += 0.5f) {
        REQUIRE_THAT(curve_rise(0.0f, c), WithinAbs(0.0f, 1e-6f));
        REQUIRE_THAT(curve_rise(1.0f, c), WithinAbs(1.0f, 1e-6f));
        REQUIRE_THAT(curve_fall(0.0f, c), WithinAbs(1.0f, 1e-6f));
        REQUIRE_THAT(curve_fall(1.0f, c), WithinAbs(0.0f, 1e-6f));
    }
}

TEST_CASE("Curve shaper follows the rise law and smoothstep", "[signal][mod][tools]") {
    Curve curve;
    curve.set_shape(Curve::Shape::stage_curve);
    curve.set_curve(1.0f);
    REQUIRE_THAT(curve.process(0.5f), WithinAbs(curve_rise(0.5f, 1.0f), 1e-6f));

    curve.set_shape(Curve::Shape::smoothstep);
    REQUIRE_THAT(curve.process(0.5f), WithinAbs(0.5f, 1e-6f));
    REQUIRE_THAT(curve.process(0.25f), WithinAbs(smoothstep(0.25f), 1e-6f));
    REQUIRE_THAT(curve.process(0.0f), WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(curve.process(1.0f), WithinAbs(1.0f, 1e-6f));
}

// ── units ────────────────────────────────────────────────────────────────────

TEST_CASE("Unit conversions round-trip", "[signal][mod][units]") {
    using namespace pulp::signal::units;

    for (float db = -60.0f; db <= 12.0f; db += 6.0f)
        REQUIRE_THAT(linear_to_db(db_to_linear(db)), WithinAbs(db, 1e-3f));

    for (float note = 21.0f; note <= 108.0f; note += 12.0f)
        REQUIRE_THAT(hz_to_midi(midi_to_hz(note)), WithinAbs(note, 1e-3f));

    REQUIRE_THAT(midi_to_hz(69.0f), WithinAbs(440.0f, 1e-3f));
    REQUIRE_THAT(semitones_to_ratio(12.0f), WithinAbs(2.0f, 1e-6f));
    REQUIRE_THAT(cents_to_ratio(1200.0f), WithinAbs(2.0f, 1e-6f));
    REQUIRE_THAT(ratio_to_cents(cents_to_ratio(37.0f)), WithinAbs(37.0f, 1e-3f));

    for (float t = 0.0f; t <= 1.0f; t += 0.125f)
        REQUIRE_THAT(taper_log_inverse(taper_log(t, 0.01f, 20.0f), 0.01f, 20.0f),
                     WithinAbs(t, 1e-5f));

    REQUIRE_THAT(beats_to_seconds(4.0f, 120.0f), WithinAbs(2.0f, 1e-6f));
    REQUIRE_THAT(seconds_to_beats(2.0f, 120.0f), WithinAbs(4.0f, 1e-6f));
}

TEST_CASE("One-pole and T60 helpers match their definitions", "[signal][mod][units]") {
    using namespace pulp::signal::units;

    // A one-pole driven with its own coefficient reaches 63.2% in one time
    // constant.
    const float coefficient = ms_to_onepole_coef(10.0f, 48000.0f);
    float y = 0.0f;
    for (int i = 0; i < 480; ++i)
        y += coefficient * (1.0f - y);
    REQUIRE_THAT(y, WithinAbs(0.632f, 0.005f));

    // A feedback path at the T60 gain is 60 dB down after T60 seconds.
    const float gain = t60_to_per_sample_gain(0.5f, 48000.0f);
    float amplitude = 1.0f;
    for (int i = 0; i < 24000; ++i)
        amplitude *= gain;
    REQUIRE_THAT(linear_to_db(amplitude), WithinAbs(-60.0f, 0.1f));
}

TEST_CASE("Musical division table is order-locked and correct", "[signal][mod][units]") {
    using namespace pulp::signal::units;

    REQUIRE_THAT(division_to_beats(Division::whole), WithinAbs(4.0f, 1e-6f));
    REQUIRE_THAT(division_to_beats(Division::half), WithinAbs(2.0f, 1e-6f));
    REQUIRE_THAT(division_to_beats(Division::quarter), WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(division_to_beats(Division::eighth), WithinAbs(0.5f, 1e-6f));
    REQUIRE_THAT(division_to_beats(Division::sixteenth), WithinAbs(0.25f, 1e-6f));

    REQUIRE_THAT(division_to_beats(Division::quarter_dotted), WithinAbs(1.5f, 1e-6f));
    REQUIRE_THAT(division_to_beats(Division::eighth_triplet), WithinAbs(1.0f / 3.0f, 1e-6f));
    // Three triplet eighths fill one beat: the property the shared table exists
    // to make every synced object agree on.
    REQUIRE_THAT(3.0f * division_to_beats(Division::eighth_triplet), WithinAbs(1.0f, 1e-5f));

    // Index overload addresses the same rows.
    for (int i = 0; i < kDivisionCount; ++i)
        REQUIRE(division_to_beats(i) == division_to_beats(static_cast<Division>(i)));

    // Monotone decreasing across straight note values.
    REQUIRE(division_to_beats(Division::sixty_fourth) < division_to_beats(Division::thirty_second));

    // A synced LFO period lands where the tempo says it should.
    REQUIRE_THAT(division_to_samples(Division::quarter, 120.0f, 48000.0), WithinAbs(24000.0, 1e-3));
}
