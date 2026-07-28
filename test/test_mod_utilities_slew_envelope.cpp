// Tier 0 mod-utilities toolkit — slew, envelope.
//
// Split out of the original single-file suite: shared includes and
// helpers live in test_mod_utilities_support.hpp so each file states
// only the contracts it asserts.

#include "test_mod_utilities_support.hpp"


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


TEST_CASE("SampleHold latches on the rising edge only", "[slew][mod-utilities]") {
    SampleHold64 sh;
    sh.reset();

    REQUIRE_THAT(sh.process_signal(0.5, 0.0), WithinAbs(0.0, 1e-12));  // no edge yet
    REQUIRE_THAT(sh.process_signal(0.5, 1.0), WithinAbs(0.5, 1e-12));  // rising: latch
    REQUIRE_THAT(sh.process_signal(0.9, 1.0), WithinAbs(0.5, 1e-12));  // still high: hold
    REQUIRE_THAT(sh.process_signal(0.9, 0.0), WithinAbs(0.5, 1e-12));  // falling: hold
    REQUIRE_THAT(sh.process_signal(0.9, 1.0), WithinAbs(0.9, 1e-12));  // rising again: latch
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
