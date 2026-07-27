// Tier 0 mod-utilities toolkit — vca, vactrol, trigger.
//
// Split out of the original single-file suite: shared includes and
// helpers live in test_mod_utilities_support.hpp so each file states
// only the contracts it asserts.

#include "test_mod_utilities_support.hpp"


TEST_CASE("OU walk clamps theta to its declared design range",
          "[rng][mod-utilities][contract]") {
    OuWalk64 walk;
    walk.set_theta(-1.0);
    REQUIRE(walk.theta() == OuWalk64::kMinTheta);
    walk.set_theta(1.0);
    REQUIRE(walk.theta() == OuWalk64::kMaxTheta);
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
