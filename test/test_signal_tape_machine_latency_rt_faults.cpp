#include "test_signal_tape_machine_support.hpp"

TEST_CASE("Tape machine: latency is constant, exact, and measurable",
          "[signal][tape-machine][latency]") {
    // R14, re-scoped — see the header note. What is asserted:
    //   1. the reported value is CONSTANT across archetype × curve × speed ×
    //      age with pre-echo off, so it never moves under the audio thread;
    //   2. it equals the sum of the two constant delays, computed from shipped
    //      constants rather than restated;
    //   3. enabling pre-echo raises it by exactly the wrap offset;
    //   4. the MEASURED impulse position moves by exactly that same amount,
    //      which is the part of the claim an impulse can prove.
    const int expected_base =
        TapeMachine::oversampler_latency_samples() +
        static_cast<int>(std::llround(TapeMachine::kInstabilityNominalMs * kSr / 1000.0));

    for (const TapeArchetype archetype : kAllArchetypes) {
        for (const TapeCurve curve : kAllCurves) {
            for (const float age : {0.0f, 0.5f, 1.0f}) {
                TapeMachine machine;
                machine.set_archetype(archetype);
                machine.set_eq_curve(curve);
                machine.prepare(kSr);
                machine.set_age(age);
                const tape::ArchetypePreset preset = tape::archetype_preset(archetype);
                for (int i = 0; i < preset.legal_speed_count; ++i) {
                    machine.set_speed_ips(preset.legal_speeds_ips[static_cast<std::size_t>(i)]);
                    REQUIRE(machine.latency_samples() == expected_base);
                }
            }
        }
    }

    // The oversampler's contribution is the group delay of every half-band
    // stage the magnetic solve is wrapped in. That wrap is `Oversampled-
    // Hysteresis8x`: the 4x pair plus one further house stage, added so the
    // nonlinear solve clears the -60 dBc folded-product contract. Three stages,
    // so the shipped law is `(taps−1)/2 + (taps−1)/4 + (taps−1)/8` — 56 samples
    // at the shipped 65 taps, not the 48 the 4x pair alone would contribute.
    const auto taps = static_cast<int>(chardelay::kHysteresisHalfBandTaps);
    REQUIRE(TapeMachine::oversampler_latency_samples() ==
            (taps - 1) / 2 + (taps - 1) / 4 + (taps - 1) / 8);

    // Pre-echo's cost is exact, and the impulse agrees with the arithmetic.
    constexpr double kOffsetMs = 300.0;
    const int offset_samples = static_cast<int>(std::llround(kOffsetMs * kSr / 1000.0));

    auto impulse_position = [&](bool pre_echo) {
        TapeMachine machine;
        machine.set_archetype(TapeArchetype::studer_a800);
        machine.prepare(kSr);
        quiesce(machine);
        machine.set_print_through(-80.0f, static_cast<float>(kOffsetMs), pre_echo);
        const auto out = render_impulse(machine, static_cast<int>(kSr));
        return std::pair{peak_of(out.left).second, machine.latency_samples()};
    };

    const auto [position_off, latency_off] = impulse_position(false);
    const auto [position_on, latency_on] = impulse_position(true);
    REQUIRE(latency_on - latency_off == offset_samples);
    REQUIRE(position_on - position_off == offset_samples);

    // The measured position is never EARLIER than the reported latency — a
    // module reporting more delay than it has would break a host's
    // compensation just as badly as one reporting less. The excess is the
    // minimum-phase and IIR group delay of the loss and EQ stages, which is
    // colouration and is deliberately not reported, the same convention a
    // biquad gets.
    REQUIRE(position_off >= latency_off);
}

TEST_CASE("Tape machine: drive changes colour, not the level of a quiet signal",
          "[signal][tape-machine][drive]") {
    // Series law 1. The reused hysteresis stage is unity-compensated at its own
    // boundary; this module's drive law is `f(g·x)/g`, which preserves that at
    // every drive setting. If it did not, the drive knob would double as a
    // level knob and every A/B of "more tape" would really be an A/B of "more
    // gain" — the exact defect the law exists to prevent.
    //
    // The tolerance is 1.2 dB, not the 0.1 dB a memoryless shaper would hold
    // to, and the reason is inherited rather than sloppy. The reused stage's
    // makeup gain is MEASURED, by running a 0.02-amplitude probe tone through a
    // scratch Jiles-Atherton solver at eleven age knots — so its compensation
    // is exact at that one operating point and drifts away from it elsewhere.
    // Measured here: monotone from −64.970 dBFS at drive 0 to −64.011 dBFS at
    // drive 1, a spread of 0.96 dB across the full 24 dB drive span. That is
    // 0.96 dB of level for 24 dB of drive — the law holds in the sense that
    // matters, and stating the residual is more useful than a tolerance that
    // pretends it is zero.
    double reference = 0.0;
    for (const float drive : {0.0f, 0.3f, 0.6f, 1.0f}) {
        TapeMachine machine;
        machine.set_archetype(TapeArchetype::studer_a800);
        machine.prepare(kSr);
        quiesce(machine);
        machine.set_drive(drive);
        // −60 dBFS: far enough below the knee that the stage is operating on
        // its small-signal slope, which is what the law is about.
        const auto out = render_tone(machine, 1000.0, 1e-3, static_cast<int>(kSr));
        const double db = 20.0 * std::log10(steady_rms(out.left));
        if (drive == 0.0f)
            reference = db;
        else
            REQUIRE_THAT(db, WithinAbs(reference, 1.2));
    }
}

TEST_CASE("Tape machine: the insert is aligned, not silently lossy",
          "[signal][tape-machine][alignment]") {
    // The reused Wallace model loses 19.7 dB at 1 kHz at cassette speed and mid
    // age — appropriate as delay-loop character, fatal for an insert. The
    // alignment gain undoes it at the reference frequency the same way a
    // calibration tape does on a real machine. Without this the cassette preset
    // measured −24 dB at 1 kHz.
    for (const TapeArchetype archetype : kAllArchetypes) {
        for (const float age : {0.0f, 0.2f, 0.5f}) {
            TapeMachine machine;
            machine.set_archetype(archetype);
            machine.prepare(kSr);
            quiesce(machine);
            machine.set_age(age);
            const auto out = render_tone(machine, tape::kEqReferenceHz, 0.1,
                                         static_cast<int>(kSr) * 3);
            const double db =
                20.0 * std::log10(steady_rms(out.left) / (0.1 / std::sqrt(2.0)));
            REQUIRE(std::abs(db) < 4.0);
        }
    }

    // The alignment gain is bounded, so a pathological configuration cannot
    // turn the reused hiss generator into the loudest thing in the mix.
    TapeMachine worn;
    worn.set_archetype(TapeArchetype::cassette_deck);
    worn.prepare(kSr);
    worn.set_age(1.0f);
    REQUIRE(worn.reproduce_alignment_db() <= TapeMachine::kAlignmentCeilingDb + 1e-9);
    REQUIRE(worn.reproduce_alignment_db() > 0.0);

    // It rises with age, because the loss it is compensating does.
    TapeMachine fresh;
    fresh.set_archetype(TapeArchetype::cassette_deck);
    fresh.prepare(kSr);
    fresh.set_age(0.0f);
    REQUIRE(fresh.reproduce_alignment_db() < worn.reproduce_alignment_db());
}

TEST_CASE("Tape machine: the mix control crossfades against the true dry path",
          "[signal][tape-machine][mix]") {
    TapeMachine machine;
    machine.set_archetype(TapeArchetype::studer_a800);
    machine.prepare(kSr);
    machine.set_age(0.5f);
    machine.set_mix(0.0f);

    const int n = 2048;
    chardelay::Xorshift32 rng(99u);
    std::vector<float> in_l(static_cast<std::size_t>(n)), in_r(static_cast<std::size_t>(n));
    std::vector<float> out_l(static_cast<std::size_t>(n)), out_r(static_cast<std::size_t>(n));
    for (int k = 0; k < n; ++k) {
        in_l[static_cast<std::size_t>(k)] = static_cast<float>(0.5 * rng.bipolar());
        in_r[static_cast<std::size_t>(k)] = static_cast<float>(0.5 * rng.bipolar());
    }
    machine.process(in_l.data(), in_r.data(), out_l.data(), out_r.data(), n);
    for (int k = 0; k < n; ++k)
        REQUIRE_THAT(static_cast<double>(out_l[static_cast<std::size_t>(k)]),
                     WithinAbs(static_cast<double>(in_l[static_cast<std::size_t>(k)]), 1e-6));
}

TEST_CASE("Tape machine: there is no feedback path, and the insertion bound holds",
          "[signal][tape-machine][gain]") {
    // Series law 8 in the shape it takes for a feed-forward design: Forge's
    // `worst_case_gain` does not apply, so what the registry cites instead is
    // this bound, and it has to be a bound the suite asserts rather than an
    // estimate. The largest gain any single stage can present is whichever of
    // the reciprocal EQ pair is boosting, times the bias shelf's under-bias
    // boost.
    for (const TapeArchetype archetype : kAllArchetypes) {
        TapeMachine machine;
        machine.set_archetype(archetype);
        machine.prepare(kSr);
        const double bound = machine.worst_case_insertion_gain();
        REQUIRE(bound > 1.0);
        REQUIRE(std::isfinite(bound));

        for (const TapeCurve curve : kAllCurves) {
            machine.set_eq_curve(curve);
            for (double hz = 20.0; hz < 0.45 * kSr; hz *= 1.05) {
                const double record = machine.record_eq().response_db(hz, kSr);
                const double playback = machine.playback_eq().response_db(hz, kSr);
                REQUIRE(units::db_to_linear(std::max(record, playback)) <= bound);
            }
        }
    }

    // A feed-forward insert cannot run away: a full-scale input at maximum
    // drive and maximum age stays bounded.
    TapeMachine hot;
    hot.set_archetype(TapeArchetype::cassette_deck);
    hot.prepare(kSr);
    hot.set_age(1.0f);
    hot.set_drive(1.0f);
    hot.set_bias(-1.0f);
    hot.set_companding_enabled(true);
    const auto out = render_tone(hot, 100.0, 1.0, static_cast<int>(kSr));
    for (const float v : out.left) {
        REQUIRE(std::isfinite(v));
        REQUIRE(std::abs(v) < 16.0);
    }

    // The consequence the large section bound actually implies, asserted where
    // it lives: the reproduce network's ceiling sits near Nyquist, so what
    // could get amplified by it is the noise injected between the record and
    // playback networks. On silence, at the worst archetype/curve/age the
    // module offers, the output floor stays far below anything audible.
    for (const TapeArchetype archetype : kAllArchetypes) {
        for (const TapeCurve curve : kAllCurves) {
            TapeMachine quiet;
            quiet.set_archetype(archetype);
            quiet.set_eq_curve(curve);
            quiet.prepare(kSr);
            quiet.set_age(1.0f);
            const int n = static_cast<int>(kSr);
            std::vector<float> silence(static_cast<std::size_t>(n), 0.0f);
            std::vector<float> out_l(static_cast<std::size_t>(n)),
                out_r(static_cast<std::size_t>(n));
            quiet.process(silence.data(), silence.data(), out_l.data(), out_r.data(), n);
            const double floor_db = 20.0 * std::log10(std::max(steady_rms(out_l), 1e-30));
            REQUIRE(floor_db < -30.0);
            for (const float v : out_l) REQUIRE(std::isfinite(v));
        }
    }
}

TEST_CASE("Tape machine: a fresh instance and a reset instance agree",
          "[signal][tape-machine][state]") {
    // The zero-init claim, in the form that matters: whatever `reset()` leaves
    // behind must be what a freshly prepared instance starts from, or the first
    // render after a transport stop differs from the first render of the
    // session.
    const int n = 2048;
    chardelay::Xorshift32 rng(7u);
    std::vector<float> in_l(static_cast<std::size_t>(n)), in_r(static_cast<std::size_t>(n));
    for (int k = 0; k < n; ++k) {
        in_l[static_cast<std::size_t>(k)] = static_cast<float>(0.2 * rng.bipolar());
        in_r[static_cast<std::size_t>(k)] = static_cast<float>(0.2 * rng.bipolar());
    }

    TapeMachine fresh;
    fresh.set_archetype(TapeArchetype::ampex_350_440);
    fresh.prepare(kSr);
    std::vector<float> a(static_cast<std::size_t>(n)), b(static_cast<std::size_t>(n));
    fresh.process(in_l.data(), in_r.data(), a.data(), b.data(), n);

    TapeMachine used;
    used.set_archetype(TapeArchetype::ampex_350_440);
    used.prepare(kSr);
    std::vector<float> scratch_l(static_cast<std::size_t>(n)),
        scratch_r(static_cast<std::size_t>(n));
    used.process(in_l.data(), in_r.data(), scratch_l.data(), scratch_r.data(), n);
    used.reset();
    std::vector<float> c(static_cast<std::size_t>(n)), d(static_cast<std::size_t>(n));
    used.process(in_l.data(), in_r.data(), c.data(), d.data(), n);

    for (std::size_t k = 0; k < a.size(); ++k) REQUIRE(a[k] == c[k]);
}

TEST_CASE("Tape machine rejects non-finite controls and audio without latching recursive state",
          "[signal][tape-machine][nan-recovery][rt-safety]") {
    TapeMachine poisoned;
    TapeMachine fresh;
    for (TapeMachine* machine : {&poisoned, &fresh}) {
        machine->set_archetype(TapeArchetype::studer_a800);
        machine->prepare(kSr);
        machine->set_bias(0.2f);
        machine->set_drive(0.4f);
        machine->set_age(0.5f);
        machine->set_crosstalk_db(-35.0f);
        machine->set_print_through(-55.0f, 600.0f, true);
    }

    const double speed = poisoned.speed_ips();
    const double bias = poisoned.effective_bias();
    const double drive = poisoned.drive();
    const double age = poisoned.age();
    const double crosstalk = poisoned.crosstalk_db();
    const double print_db = poisoned.print_through_db();
    const double print_offset = poisoned.print_offset_ms();
    const float nan = std::numeric_limits<float>::quiet_NaN();

    poisoned.set_speed_ips(std::numeric_limits<double>::quiet_NaN());
    poisoned.set_bias(nan);
    poisoned.set_drive(nan);
    poisoned.set_age(nan);
    poisoned.set_crosstalk_db(nan);
    poisoned.set_print_through(nan, nan, true);
    poisoned.set_mix(nan);
    REQUIRE(poisoned.speed_ips() == speed);
    REQUIRE(poisoned.effective_bias() == bias);
    REQUIRE(poisoned.drive() == drive);
    REQUIRE(poisoned.age() == age);
    REQUIRE(poisoned.crosstalk_db() == crosstalk);
    REQUIRE(poisoned.print_through_db() == print_db);
    REQUIRE(poisoned.print_offset_ms() == print_offset);

    float bad_l = nan;
    float bad_r = 0.0f;
    float bad_out_l = 1.0f;
    float bad_out_r = 1.0f;
    {
        pulp::test::RtAllocationProbe probe;
        poisoned.process(&bad_l, &bad_r, &bad_out_l, &bad_out_r, 1);
        REQUIRE(probe.allocation_count() == 0);
    }
    REQUIRE(bad_out_l == 0.0f);
    REQUIRE(bad_out_r == 0.0f);

    constexpr int kSamples = 4096;
    std::vector<float> in_l(kSamples), in_r(kSamples), recovered_l(kSamples),
        recovered_r(kSamples), reference_l(kSamples), reference_r(kSamples);
    for (int i = 0; i < kSamples; ++i) {
        in_l[static_cast<std::size_t>(i)] =
            static_cast<float>(0.2 * std::sin(2.0 * M_PI * 997.0 * i / kSr));
        in_r[static_cast<std::size_t>(i)] =
            static_cast<float>(0.2 * std::sin(2.0 * M_PI * 431.0 * i / kSr));
    }
    poisoned.process(in_l.data(), in_r.data(), recovered_l.data(), recovered_r.data(), kSamples);
    fresh.process(in_l.data(), in_r.data(), reference_l.data(), reference_r.data(), kSamples);
    REQUIRE(recovered_l == reference_l);
    REQUIRE(recovered_r == reference_r);
}

TEST_CASE("Tape machine audio-fault recovery discards populated dynamic histories",
          "[signal][tape-machine][nan-recovery][rt-safety]") {
    TapeMachine poisoned;
    TapeMachine fresh;
    for (TapeMachine* machine : {&poisoned, &fresh}) {
        machine->set_archetype(TapeArchetype::studer_a800);
        machine->prepare(kSr);
        machine->set_bias(0.2f);
        machine->set_drive(0.4f);
        machine->set_age(0.5f);
        machine->set_crosstalk_db(-35.0f);
        machine->set_print_through(-55.0f, 600.0f, true);
    }

    // Fill the entire dynamically sized print-through history. Recovery resets
    // its cursor to zero, so every stale slot must be populated for a missing
    // logical invalidation to be observable immediately at the configured taps.
    const int kWarmupSamples = static_cast<int>(2.0 * pulp::signal::units::ms_to_samples(
                                   TapeMachine::kPrintThroughOffsetMsMax, kSr)) +
                               8;
    std::vector<float> warm_l(kWarmupSamples), warm_r(kWarmupSamples),
        scratch_l(kWarmupSamples), scratch_r(kWarmupSamples);
    for (int i = 0; i < kWarmupSamples; ++i) {
        warm_l[static_cast<std::size_t>(i)] =
            static_cast<float>(0.31 * std::sin(2.0 * M_PI * 613.0 * i / kSr));
        warm_r[static_cast<std::size_t>(i)] =
            static_cast<float>(0.27 * std::sin(2.0 * M_PI * 887.0 * i / kSr));
    }
    poisoned.process(warm_l.data(), warm_r.data(), scratch_l.data(), scratch_r.data(),
                     kWarmupSamples);

    const float nan = std::numeric_limits<float>::quiet_NaN();
    float finite = 0.0f;
    float fault_l = 1.0f;
    float fault_r = 1.0f;
    {
        pulp::test::RtAllocationProbe probe;
        poisoned.process(&nan, &finite, &fault_l, &fault_r, 1);
        REQUIRE(probe.allocation_count() == 0);
    }
    REQUIRE(fault_l == 0.0f);
    REQUIRE(fault_r == 0.0f);

    constexpr int kRecoverySamples = 4096;
    std::vector<float> in_l(kRecoverySamples), in_r(kRecoverySamples),
        recovered_l(kRecoverySamples), recovered_r(kRecoverySamples),
        reference_l(kRecoverySamples), reference_r(kRecoverySamples);
    for (int i = 0; i < kRecoverySamples; ++i) {
        in_l[static_cast<std::size_t>(i)] =
            static_cast<float>(0.2 * std::sin(2.0 * M_PI * 997.0 * i / kSr));
        in_r[static_cast<std::size_t>(i)] =
            static_cast<float>(0.2 * std::sin(2.0 * M_PI * 431.0 * i / kSr));
    }
    poisoned.process(in_l.data(), in_r.data(), recovered_l.data(), recovered_r.data(),
                     kRecoverySamples);
    fresh.process(in_l.data(), in_r.data(), reference_l.data(), reference_r.data(),
                  kRecoverySamples);
    REQUIRE(recovered_l == reference_l);
    REQUIRE(recovered_r == reference_r);
}
