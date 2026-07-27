#include "test_signal_fet_compressor_support.hpp"

TEST_CASE("the divider is calibrated exactly at both endpoints",
          "[fet-compressor][divider]") {
    Comp c = probe(FetRatio::r8_1, 0.0);

    // c = 0 is exactly unity — the correct rest state for a feedback compressor
    // with nothing to reduce. c = 1 is exactly the design's attenuation ceiling.
    REQUIRE_THAT(c.divider_small_signal_gain(0.0), WithinAbs(1.0, 1e-12));
    REQUIRE_THAT(units::linear_to_db(c.divider_small_signal_gain(1.0)),
                 WithinAbs(-Comp::kGainReductionMaxDb, 1e-9));

    // Monotone attenuation in between: gain reduction rises with the control
    // voltage, which is the entire premise of the topology.
    double previous = c.divider_small_signal_gain(0.0);
    for (int i = 1; i <= 200; ++i) {
        const double g = c.divider_small_signal_gain(i / 200.0);
        REQUIRE(g < previous);
        previous = g;
    }
}

TEST_CASE("the control-voltage inverse round-trips against the divider law",
          "[fet-compressor][divider]") {
    // `control_for_reduction_db` is the closed form that makes the loop
    // unity-compensated. If it is not the exact inverse of the divider law, the
    // loop gain is not 1 and the stability argument evaporates — so it is
    // checked over the whole span, not at a point.
    Comp c = probe(FetRatio::r8_1, 0.0);
    for (int i = 0; i <= 400; ++i) {
        const double db = Comp::kGainReductionMaxDb * i / 400.0;
        const double control = c.control_for_reduction_db(db);
        const double realised = -units::linear_to_db(c.divider_small_signal_gain(control));
        REQUIRE_THAT(realised, WithinAbs(db, 1e-9));
    }
    // Saturating, not wrapping, past the ceiling.
    REQUIRE(c.control_for_reduction_db(Comp::kGainReductionMaxDb * 2.0) == 1.0);
    REQUIRE(c.control_for_reduction_db(-5.0) == 0.0);
}

TEST_CASE("the closed-loop pole is inside the unit circle everywhere",
          "[fet-compressor][loop]") {
    // The bound the file doc block states: |p| ≤ max(α, 1 − 1/R_max) < 1, for
    // every attack, every release, every ratio, every sample rate.
    for (double sr : {44100.0, 48000.0, 96000.0, 192000.0}) {
        for (auto ratio : {FetRatio::r4_1, FetRatio::r8_1, FetRatio::r12_1, FetRatio::r20_1,
                           FetRatio::all_buttons_in}) {
            for (double us : {Comp::kAttackUsMin, 100.0, Comp::kAttackUsMax}) {
                for (double ms : {Comp::kReleaseMsMin, Comp::kReleaseMsMax}) {
                    Comp c;
                    c.prepare(sr);
                    c.set_ratio(ratio);
                    c.set_attack_us(us);
                    c.set_release_ms(ms);
                    const double worst =
                        std::max(std::abs(closed_loop_pole(c, true)),
                                 std::abs(closed_loop_pole(c, false)));
                    REQUIRE(worst < 1.0);
                    // ...and it is bounded by the closed form, not merely
                    // observed to be small.
                    REQUIRE(worst <= std::max({c.attack_coefficient(), c.release_coefficient(),
                                               c.loop_slope()}) + 1e-12);
                }
            }
        }
    }
}

TEST_CASE("the naive control-voltage normalisation would be unstable",
          "[fet-compressor][loop]") {
    // The control. The source spec normalises the control voltage as
    // c = y_L / GR_max_db, which assumes the divider's dB attenuation is linear
    // in c. Measured from the SHIPPED divider law, dΓ/dD near c = 0 is ~21.5, so
    // that loop's pole at the fastest documented attack lands far outside the
    // unit circle. This test exists so "we inverted the divider law" is a
    // measured necessity rather than a stylistic preference — and so that a
    // future re-tune of R_on/R_off/GR_max that made the naive law viable would
    // show up as a failure here rather than as silence.
    Comp c = probe(FetRatio::r8_1, 0.0, Comp::kAttackUsMin);

    const auto attenuation_db = [&](double control) {
        return -units::linear_to_db(c.divider_small_signal_gain(control));
    };
    constexpr double kStep = 1e-6;
    const double naive_loop_gain =
        (attenuation_db(kStep) - attenuation_db(0.0)) / kStep / Comp::kGainReductionMaxDb;
    REQUIRE(naive_loop_gain > 20.0);  // ≈ 21.5 at the shipped constants

    const double alpha = c.attack_coefficient();
    const double naive_pole = alpha - (1.0 - alpha) * naive_loop_gain * c.loop_slope();
    REQUIRE(std::abs(naive_pole) > 1.0);

    // The shipped law, same operating point, same arithmetic.
    REQUIRE(std::abs(closed_loop_pole(c, true)) < 1.0);
}

TEST_CASE("the loop settles rather than ringing at the fastest attack",
          "[fet-compressor][loop]") {
    // Fastest attack, hardest ratio, heaviest drive — the corner the naive
    // control law diverges in. The detector must reach a steady value and stay
    // there: an unstable loop shows up as a growing or persistent oscillation,
    // both of which this catches.
    Comp c = probe(FetRatio::all_buttons_in, 20.0, Comp::kAttackUsMin, Comp::kReleaseMsMin, 0.0);
    for (int i = 0; i < static_cast<int>(kSr); ++i) c.process(1.0);

    double lowest = 1e9;
    double highest = -1e9;
    for (int i = 0; i < static_cast<int>(kSr * 0.1); ++i) {
        const double y = c.process(1.0);
        REQUIRE(std::isfinite(y));
        lowest = std::min(lowest, c.gain_reduction_db());
        highest = std::max(highest, c.gain_reduction_db());
    }
    REQUIRE(highest - lowest < 0.01);
    REQUIRE(highest > 1.0);  // not vacuous: it really is compressing hard
}

TEST_CASE("41 the open-loop gain computer matches the published closed form",
          "[fet-compressor][curve]") {
    // The §3.1 characteristic itself, in the detector's own domain, checked
    // against an independent transcription of the paper's equation.
    for (auto ratio : {FetRatio::r4_1, FetRatio::r8_1, FetRatio::r12_1, FetRatio::r20_1}) {
        for (double knee : {0.0, 1.0, 6.0}) {
            Comp c = probe(ratio, 0.0, 200.0, 300.0, knee);
            for (double x : {-40.0, -24.0, -20.0, -18.0, -16.0, -12.0, -6.0, 0.0, 12.0}) {
                REQUIRE_THAT(c.static_curve_db(x),
                             WithinAbs(reference_static_curve(x, Comp::kThresholdDbfs,
                                                              c.nominal_ratio(),
                                                              c.effective_knee_db()),
                                       1e-9));
            }
        }
    }
}

TEST_CASE("41 the spec's worked example reproduces exactly", "[fet-compressor][curve]") {
    // §3.1: T = −18 dBFS, R = 8, W = 1 dB, detector input at −10 dB.
    // 2(x−T) = 16 > W, so the linear branch: y_G = −18 + 8/8 = −17, x_G = 7 dB.
    Comp c = probe(FetRatio::r8_1, 0.0, 200.0, 300.0, 1.0);
    REQUIRE_THAT(c.static_curve_db(-10.0), WithinAbs(-17.0, 1e-9));
    REQUIRE_THAT(c.gain_computer_db(-10.0), WithinAbs(7.0, 1e-9));
}

TEST_CASE("41 the closed-form loop solution agrees with a bisection",
          "[fet-compressor][curve]") {
    // The shipped `measured_static_curve_db` is a closed form. A closed form
    // that agrees with a bisection over the whole curve, on every branch and
    // every ratio, is a closed form that is right.
    for (auto ratio : {FetRatio::r4_1, FetRatio::r8_1, FetRatio::r12_1, FetRatio::r20_1,
                       FetRatio::all_buttons_in}) {
        for (double knee : {0.0, 1.0, 6.0}) {
            Comp c = probe(ratio, 0.0, 200.0, 300.0, knee);
            for (int i = 0; i <= 120; ++i) {
                const double x = -40.0 + 0.5 * i;
                REQUIRE_THAT(c.measured_static_curve_db(x),
                             WithinAbs(reference_closed_loop_output_db(
                                           x, Comp::kThresholdDbfs, c.nominal_ratio(),
                                           c.effective_knee_db(), c.bias_shift_db()),
                                       1e-6));
            }
        }
    }
}

TEST_CASE("41 measured gain reduction matches the closed-loop curve",
          "[fet-compressor][curve]") {
    // The spec's recipe: steady 1 kHz sine, 4096-point Hann FFT, ±0.2 dB, over
    // {4:1, 8:1, 12:1, 20:1} × {−12, −6, 0, +6, +12} dBFS with input gain
    // pinned at +6 dB so every level crosses the fixed reference.
    //
    // ADJUDICATION A-1. The spec asks this to equal `x_G(x)` from §3.1. It
    // cannot: `x_G` is the open-loop gain computer and this topology closes the
    // loop around it. The two are asserted here to be genuinely different, so
    // the adjudication is proven rather than asserted, and the closed-loop
    // fixed point is what the measurement is held to.
    constexpr double kInputGainDb = 6.0;
    for (auto ratio : {FetRatio::r4_1, FetRatio::r8_1, FetRatio::r12_1, FetRatio::r20_1}) {
        for (double level_db : {-12.0, -6.0, 0.0, 6.0, 12.0}) {
            Comp c = probe(ratio, kInputGainDb);
            const auto rendered = steady_render(c, units::db_to_linear(level_db), 1000.0, 4096);
            const double measured_out_db = component_db(rendered, 1000.0);

            const double driven_db = level_db + kInputGainDb;
            const double expected_out_db = c.measured_static_curve_db(driven_db);
            REQUIRE_THAT(measured_out_db, WithinAbs(expected_out_db, 0.2));

            // The detector agrees with the audio, tightly.
            REQUIRE_THAT(c.gain_reduction_db(),
                         WithinAbs(c.measured_gain_reduction_db(driven_db), 0.05));

            // ...and the open-loop criterion the spec states is nowhere near.
            const double open_loop = c.gain_computer_db(driven_db);
            REQUIRE(open_loop > c.measured_gain_reduction_db(driven_db) * 1.5);
        }
    }
}

TEST_CASE("41 the measured ratio and knee are the loop-mapped ones",
          "[fet-compressor][curve]") {
    // The headline consequence of feedback: the knob is not the measurement.
    for (auto ratio : {FetRatio::r4_1, FetRatio::r8_1, FetRatio::r12_1, FetRatio::r20_1}) {
        Comp c = probe(ratio, 0.0, 200.0, 300.0, 0.0);
        REQUIRE_THAT(c.measured_ratio(), WithinAbs(2.0 - 1.0 / c.nominal_ratio(), 1e-12));
        REQUIRE(c.measured_ratio() < 2.0);

        // Measured directly off the curve, far above the knee, rather than
        // taken on faith from the accessor.
        const double a = c.measured_static_curve_db(0.0);
        const double b = c.measured_static_curve_db(20.0);
        REQUIRE_THAT(20.0 / (b - a), WithinRel(c.measured_ratio(), 1e-9));
    }

    // The knee a user sets is narrower than the one they measure, by exactly
    // the loop's own mapping.
    Comp k = probe(FetRatio::r20_1, 0.0, 200.0, 300.0, 6.0);
    REQUIRE_THAT(k.measured_knee_db(),
                 WithinRel(k.effective_knee_db() * (2.0 + k.loop_slope()) * 0.5, 1e-12));
    REQUIRE(k.measured_knee_db() > k.effective_knee_db());

    // The knee's two ends, located on the curve: reduction starts at
    // T − W/2 and the curve is linear from T + W(1+m)/2 upward.
    const double m = k.loop_slope();
    const double w = k.effective_knee_db();
    REQUIRE_THAT(k.measured_gain_reduction_db(Comp::kThresholdDbfs - 0.5 * w),
                 WithinAbs(0.0, 1e-9));
    REQUIRE(k.measured_gain_reduction_db(Comp::kThresholdDbfs - 0.5 * w + 0.5) > 0.0);
    const double knee_top = Comp::kThresholdDbfs + 0.5 * w * (1.0 + m);
    REQUIRE_THAT(k.measured_static_curve_db(knee_top),
                 WithinAbs(Comp::kThresholdDbfs + 0.5 * w, 1e-9));
}

TEST_CASE("42 attack follows the closed-loop pole, not the open-loop one",
          "[fet-compressor][detector]") {
    // ADJUDICATION A-2. The spec asks for `2.2·τ_A` within ±5 %. The loop
    // divides the effective time constant by roughly 1 + B, so at the 200 µs
    // default and 8:1 the true figure is 232 µs against the spec's 439 µs. The
    // gap is asserted below so the adjudication is proven, and the criterion is
    // the closed-loop prediction computed from the shipped α and B.
    const double ln9 = std::log(9.0);
    for (double us : {200.0, 400.0, Comp::kAttackUsMax}) {
        Comp c = probe(FetRatio::r8_1, 6.0, us, 300.0, 0.0);
        const int samples = dc_rise_samples(c, -40.0, -6.0);
        REQUIRE(samples > 0);

        const double measured_us = samples * 1e6 / kSr;
        const double expected_us = closed_loop_tau(c, true) * 1e6 * ln9;
        // ±5 % or ±1 sample, whichever is larger — the spec's own tolerance.
        const double tolerance = std::max(0.05 * expected_us, 1e6 / kSr);
        REQUIRE_THAT(measured_us, WithinAbs(expected_us, tolerance));

        // The open-loop figure the spec states is outside that window by a wide
        // margin — this is the defect, quantified.
        const double open_loop_us = us * ln9;
        REQUIRE(open_loop_us > expected_us * 1.5);
    }
}

TEST_CASE("42 the fastest documented attack is reachable", "[fet-compressor][detector]") {
    // 20 µs is faster than one base-rate sample at 48 kHz (20.8 µs), which is
    // exactly why the ballistics run oversampled. The rise must complete inside
    // a couple of base-rate samples; a detector running at 1× could not.
    Comp c = probe(FetRatio::r8_1, 6.0, Comp::kAttackUsMin, 300.0, 0.0);
    const int samples = dc_rise_samples(c, -40.0, -6.0);
    REQUIRE(samples >= 0);
    REQUIRE(samples <= 2);
    const double expected_us = closed_loop_tau(c, true) * 1e6 * std::log(9.0);
    REQUIRE(expected_us < 1e6 / kSr);
}

TEST_CASE("42 the detector attacks toward more reduction, not less",
          "[fet-compressor][detector]") {
    // The sign-convention guard. `gain_computer_db()` returns a POSITIVE
    // magnitude and the branch is taken when MORE reduction is wanted. Running
    // the same branch on the negative gain-computer output swaps attack and
    // release — a compressor that still sounds like one with its two most
    // important controls exchanged.
    //
    // In this lineage the two constants differ by three orders of magnitude
    // (µs against ms), so the discriminator is unambiguous: a correct detector
    // rises in microseconds and falls in milliseconds, and a swapped one does
    // the exact opposite. The measurement is the RATIO of the two, so it does
    // not depend on the absolute values at all.
    Comp c = probe(FetRatio::r8_1, 6.0, Comp::kAttackUsMax, Comp::kReleaseMsMin, 0.0);
    const int rise = dc_rise_samples(c, -40.0, -6.0);
    REQUIRE(rise > 0);

    const double lo = units::db_to_linear(-40.0);
    const double start = c.gain_reduction_db();
    REQUIRE(start > 1.0);
    int at_high = -1;
    int fall = -1;
    for (int i = 0; i < static_cast<int>(kSr * 4.0); ++i) {
        c.process(lo);
        const double gr = c.gain_reduction_db();
        if (at_high < 0 && gr <= start * 0.9) at_high = i;
        if (at_high >= 0 && gr <= start * 0.1) {
            fall = i - at_high;
            break;
        }
    }
    REQUIRE(fall > 0);

    // The measured rise:fall ratio must track the shipped time constants, which
    // it cannot if the branches are swapped. The attack is loop-accelerated and
    // the release is not (see test 43), so the prediction uses the closed-loop
    // attack constant and the open-loop release one.
    const double predicted = closed_loop_tau(c, true) / (Comp::kReleaseMsMin * 1e-3);
    REQUIRE_THAT(static_cast<double>(rise) / fall, WithinRel(predicted, 0.10));
    REQUIRE(rise * 50 < fall);
}

TEST_CASE("43 release to silence runs at its own open-loop time constant",
          "[fet-compressor][detector]") {
    // The spec's criterion, asserted exactly as stated — and it holds, which is
    // the interesting part. Releasing toward a level below the knee drives
    // B → 0, so the loop OPENS and the release is not accelerated the way the
    // attack is. The asymmetry is a direct consequence of the topology and is
    // asserted here rather than described.
    const double ln9 = std::log(9.0);
    for (double release_ms : {Comp::kReleaseMsMin, 300.0}) {
        Comp c = probe(FetRatio::r8_1, 6.0, 200.0, release_ms, 0.0);
        const double hi = units::db_to_linear(-6.0);
        const double lo = units::db_to_linear(-40.0);
        for (int i = 0; i < static_cast<int>(kSr); ++i) c.process(hi);

        const double start = c.gain_reduction_db();
        REQUIRE(start > 1.0);
        const double high = start * 0.9;  // 10 % of the way down
        const double low = start * 0.1;   // 90 % of the way down
        int at_high = -1;
        int fall_samples = -1;
        for (int i = 0; i < static_cast<int>(kSr * 8.0); ++i) {
            c.process(lo);
            const double gr = c.gain_reduction_db();
            if (at_high < 0 && gr <= high) at_high = i;
            if (at_high >= 0 && gr <= low) {
                fall_samples = i - at_high;
                break;
            }
        }
        REQUIRE(fall_samples > 0);
        REQUIRE_THAT(units::samples_to_ms(static_cast<double>(fall_samples), kSr),
                     WithinRel(release_ms * ln9, 0.05));

        // And it really is the open-loop constant, not the accelerated one.
        const double accelerated_ms = closed_loop_tau(c, false) * 1e3 * ln9;
        REQUIRE(accelerated_ms < release_ms * ln9 * 0.75);
    }
}

TEST_CASE("44 all-buttons-in reduces harder than the 20 to 1 button",
          "[fet-compressor][abi]") {
    // ADJUDICATION A-3. The spec asks for at least `bias_shift_db − 0.3` dB of
    // extra reduction. The loop divides the bias shift by 1 + B, so 1.5 dB of
    // detector bias buys 0.77 dB of reduction — the spec's criterion is not
    // reachable by any correct implementation. The closed-form loop-attenuated
    // value is what is asserted, and the spec's figure is shown to be out of
    // reach so the adjudication carries its own evidence.
    Comp abi = probe(FetRatio::all_buttons_in, 0.0, 200.0, 300.0, 0.0);
    Comp r20 = probe(FetRatio::r20_1, 0.0, 200.0, 300.0, 0.0);
    for (int i = 0; i < static_cast<int>(kSr * 2.0); ++i) {
        abi.process(1.0);
        r20.process(1.0);
    }

    const double delta = abi.gain_reduction_db() - r20.gain_reduction_db();
    const double expected = Comp::kAbiBiasShiftDb / (1.0 + abi.loop_slope());
    REQUIRE_THAT(delta, WithinAbs(expected, 0.05));
    REQUIRE(delta > 0.0);
    REQUIRE(expected < Comp::kAbiBiasShiftDb - 0.3);  // the spec's criterion, out of reach
}

TEST_CASE("44 all-buttons-in reacts faster by exactly its documented scale",
          "[fet-compressor][abi]") {
    // Measured at the longest attack so the base-rate sampling of the detector
    // is not what limits the comparison: at 20 µs the whole rise is one sample
    // and a ±5 % window would be measuring quantisation.
    Comp abi = probe(FetRatio::all_buttons_in, 6.0, Comp::kAttackUsMax, 300.0, 0.0);
    Comp r20 = probe(FetRatio::r20_1, 6.0, Comp::kAttackUsMax, 300.0, 0.0);
    const int abi_samples = dc_rise_samples(abi, -40.0, -6.0);
    const int r20_samples = dc_rise_samples(r20, -40.0, -6.0);
    REQUIRE(abi_samples > 0);
    REQUIRE(r20_samples > 0);

    const double ratio = static_cast<double>(abi_samples) / r20_samples;
    // ±5 %, widened by the ±1-sample quantisation on both measurements.
    const double quantisation = 1.0 / r20_samples;
    REQUIRE_THAT(ratio, WithinAbs(Comp::kAbiAttackScale,
                                  0.05 * Comp::kAbiAttackScale + quantisation));
}

TEST_CASE("44 all-buttons-in widens the knee and deepens the coloration",
          "[fet-compressor][abi]") {
    Comp abi = probe(FetRatio::all_buttons_in, 0.0, 200.0, 300.0, 2.0);
    Comp r20 = probe(FetRatio::r20_1, 0.0, 200.0, 300.0, 2.0);

    REQUIRE_THAT(abi.effective_knee_db(),
                 WithinRel(r20.effective_knee_db() * Comp::kAbiKneeWidenMult, 1e-12));
    REQUIRE_THAT(abi.coloration_depth(),
                 WithinRel(std::min(Comp::kColorationAlphaMax * Comp::kAbiAlphaExtraMult,
                                    Comp::kColorationAlphaCeiling),
                           1e-12));
    REQUIRE(abi.coloration_depth() > r20.coloration_depth());
    // The operating default is the multiplied value, not the hard ceiling.
    REQUIRE(abi.coloration_depth() < Comp::kColorationAlphaCeiling);

    // ABI reuses the 20:1 static curve rather than inventing a fifth ratio.
    REQUIRE_THAT(abi.nominal_ratio(), WithinAbs(Comp::kAbiRatio, 1e-12));

    // Release is scaled too, which shows up in the coefficient directly.
    Comp abi_release = probe(FetRatio::all_buttons_in, 0.0, 200.0, 400.0, 0.0);
    Comp r20_release = probe(FetRatio::r20_1, 0.0, 200.0,
                             400.0 * Comp::kAbiReleaseScale, 0.0);
    REQUIRE_THAT(abi_release.release_coefficient(),
                 WithinRel(r20_release.release_coefficient(), 1e-12));
}

TEST_CASE("44 all-buttons-in distorts more at matched drive", "[fet-compressor][abi]") {
    const auto measure = [](FetRatio ratio) {
        Comp c = probe(ratio, 12.0, Comp::kAttackUsMax, 300.0, 1.0);
        return thd(steady_render(c, units::db_to_linear(-6.0), 1000.0, 16384), 1000.0);
    };
    const double abi = measure(FetRatio::all_buttons_in);
    const double r20 = measure(FetRatio::r20_1);
    REQUIRE(abi > r20);
    // Not merely different: the coloration depth ratio is the reason.
    REQUIRE(abi > r20 * 1.2);
}

TEST_CASE("45 the coloration multiplier stays inside its closed-form bound",
          "[fet-compressor][gain]") {
    // The spec's own invariant, over its own grid: c ∈ {0, .25, .5, .75, 1} ×
    // v ∈ {−1.2 … 1.2} × {normal, all-buttons-in}. Swept finer than the spec
    // asks, because the bound is closed form and a finer grid costs nothing.
    for (auto ratio : {FetRatio::r20_1, FetRatio::all_buttons_in}) {
        Comp c = probe(ratio, 0.0);
        const double bound = c.coloration_multiplier_bound();
        REQUIRE_THAT(bound,
                     WithinRel(1.0 + c.coloration_depth() * Comp::kColorationHeadroomClamp, 1e-12));
        for (int ci = 0; ci <= 200; ++ci) {
            for (int vi = -300; vi <= 300; ++vi) {
                const double multiplier = c.coloration_multiplier(ci / 200.0, vi / 100.0);
                REQUIRE(multiplier <= bound + 1e-12);
                REQUIRE(multiplier >= 2.0 - bound - 1e-12);
            }
        }
    }
    // The spec's stated figures, reproduced from the shipped constants.
    REQUIRE_THAT(probe(FetRatio::r20_1, 0.0).coloration_multiplier_bound(),
                 WithinAbs(1.0 + 0.08 * 1.2, 1e-12));
    REQUIRE_THAT(probe(FetRatio::all_buttons_in, 0.0).coloration_multiplier_bound(),
                 WithinAbs(1.0 + 0.08 * 1.4 * 1.2, 1e-12));
}

TEST_CASE("45 the divider's supremum gain is exactly one", "[fet-compressor][gain]") {
    // The bound that actually bounds the GAIN — the coloration multiplier above
    // bounds the conductance, and a higher conductance means a lower gain, so
    // it bounds this from below. The divider is a passive attenuator: its
    // supremum is 1, attained only at rest.
    for (auto ratio : {FetRatio::r4_1, FetRatio::r20_1, FetRatio::all_buttons_in}) {
        Comp c = probe(ratio, 0.0);
        REQUIRE(c.divider_supremum_is_provable());
        double worst = 0.0;
        for (int ci = 0; ci <= 400; ++ci) {
            for (int vi = -300; vi <= 300; ++vi)
                worst = std::max(worst, c.divider_gain(ci / 400.0, vi / 100.0));
        }
        REQUIRE(worst <= Comp::kDividerGainSupremum);
        REQUIRE_THAT(worst, WithinAbs(Comp::kDividerGainSupremum, 1e-12));
        // Attained at rest and nowhere else.
        REQUIRE_THAT(c.divider_gain(0.0, 0.0), WithinAbs(1.0, 1e-12));
        REQUIRE(c.divider_gain(0.02, 0.0) < 1.0);
    }
}

TEST_CASE("45 realised gain never exceeds the reported worst case",
          "[fet-compressor][gain]") {
    // The registry's number, exercised. The bound is the product of the shipped
    // gain stages and the resampling pair's ℓ1 norm, so the signal chosen here
    // is deliberately hostile — full-scale square-ish transitions, which is what
    // drives a linear-phase FIR toward its ℓ∞ worst case.
    for (double output_db : {0.0, Comp::kOutputGainDbMax}) {
        for (double mix : {0.0, 0.5, 1.0}) {
            Comp c = probe(FetRatio::r4_1, 12.0, Comp::kAttackUsMin, Comp::kReleaseMsMin);
            c.set_output_gain_db(output_db);
            c.set_transformer_amount(1.0);
            c.set_mix(mix);

            const double bound = c.worst_case_gain();
            Xorshift32 rng{20260725u};
            double worst = 0.0;
            for (int i = 0; i < 200000; ++i) {
                const double x = rng.next_bipolar<double>() > 0.0 ? 1.0 : -1.0;
                worst = std::max(worst, std::abs(c.process(x)));
            }
            REQUIRE(worst <= bound);
            REQUIRE(std::isfinite(bound));
        }
    }
}

TEST_CASE("45 the worst-case bound is assembled from the shipped stages",
          "[fet-compressor][gain]") {
    Comp c = probe(FetRatio::r8_1, 6.0);
    c.set_output_gain_db(3.0);
    c.set_mix(1.0);
    REQUIRE_THAT(c.worst_case_gain(),
                 WithinRel(units::db_to_linear(6.0) * Comp::kDividerGainSupremum *
                               c.resampler_peak_gain_bound() * units::db_to_linear(3.0) *
                               Comp::kTransformerGainSupremum,
                           1e-12));
    // A fully dry blend cannot exceed unity whatever the gains are set to.
    c.set_mix(0.0);
    REQUIRE_THAT(c.worst_case_gain(), WithinAbs(1.0, 1e-12));
    // The resampler bound is a real ℓ1 product, not a placeholder 1.
    REQUIRE(c.resampler_peak_gain_bound() > 1.0);
}

TEST_CASE("46 coloration THD matches the closed-form prediction",
          "[fet-compressor][coloration]") {
    // The spec's Taylor estimate is `α(c)·v_peak/4`. That omits the divider's
    // own sensitivity: gain is `1/(1 + R_s·g)`, so a relative conductance
    // change of `α·v` becomes a relative GAIN change of `β·α·v` with
    // `β = R_s·g/(1 + R_s·g)`, and the second-harmonic amplitude of
    // `x·(1 − β·α·x)` is `β·α·v_peak/2`. Both are asserted: the spec's estimate
    // within its stated factor of 2, and the corrected form much more tightly.
    for (auto ratio : {FetRatio::r20_1, FetRatio::all_buttons_in}) {
        Comp c = probe(ratio, 12.0, Comp::kAttackUsMax, 300.0, 1.0);
        const double measured =
            thd(steady_render(c, units::db_to_linear(-6.0), 1000.0, 16384), 1000.0);

        const double control = c.control_voltage();
        REQUIRE(control > 0.0);
        const double v_peak =
            std::min(units::db_to_linear(-6.0 + 12.0), Comp::kColorationHeadroomClamp);
        // β from the shipped divider law: g_small = 1/gain − 1 in units of R_s·g.
        const double rs_g = 1.0 / c.divider_small_signal_gain(control) - 1.0;
        const double beta = rs_g / (1.0 + rs_g);
        const double alpha = c.coloration_depth() * control;

        const double corrected = beta * alpha * v_peak * 0.5;
        REQUIRE_THAT(measured, WithinRel(corrected, 0.25));

        const double spec_estimate = alpha * v_peak * 0.25;
        REQUIRE(measured < spec_estimate * 2.0);
        REQUIRE(measured > spec_estimate * 0.5);
    }
}

TEST_CASE("46 coloration vanishes at rest and grows with drive",
          "[fet-compressor][coloration]") {
    // α(c) = α_max·c, so the small-signal gain at v → 0 is exactly the divider
    // law and the distortion scales with how hard the FET is driven (series
    // law 1: the documented small-signal gain is exact, independent of drive).
    Comp c = probe(FetRatio::r20_1, 0.0);
    for (int ci = 0; ci <= 20; ++ci)
        REQUIRE_THAT(c.divider_gain(ci / 20.0, 0.0),
                     WithinAbs(c.divider_small_signal_gain(ci / 20.0), 1e-15));
    REQUIRE_THAT(c.coloration_multiplier(0.0, 1.2), WithinAbs(1.0, 1e-15));

    double previous = 0.0;
    for (int ci = 1; ci <= 20; ++ci) {
        const double deviation =
            std::abs(c.divider_gain(ci / 20.0, 1.0) / c.divider_small_signal_gain(ci / 20.0) - 1.0);
        REQUIRE(deviation > previous);
        previous = deviation;
    }
}

TEST_CASE("47 aliasing stays 60 dB below the fundamental", "[fet-compressor][aliasing]") {
    // The spec's recipe: full-scale 15 kHz sine at maximum gain reduction,
    // 32768-point Hann FFT, largest image below fs/2 at least 60 dB down.
    // 15 kHz is the right probe frequency precisely because every harmonic of
    // it lands above Nyquist, so anything measured in band IS an alias rather
    // than legitimate harmonic distortion.
    for (auto ratio : {FetRatio::r20_1, FetRatio::all_buttons_in}) {
        for (double attack_us : {Comp::kAttackUsMin, 200.0}) {
            Comp c = probe(ratio, Comp::kInputGainDbMax / 2.0, attack_us, Comp::kReleaseMsMin);
            const auto mag = spectrum(steady_render(c, 1.0, 15000.0, 32768, 0.5));
            REQUIRE(c.gain_reduction_db() > 10.0);  // genuinely at heavy reduction

            const auto fundamental_bin =
                static_cast<std::size_t>(std::llround(15000.0 * 2.0 *
                                                      static_cast<double>(mag.size()) / kSr));
            const double fundamental = std::pow(10.0, bin_level_db(mag, 15000.0) / 20.0);
            double worst = 0.0;
            for (std::size_t k = 8; k < mag.size(); ++k) {
                if (k + 12 >= fundamental_bin && k <= fundamental_bin + 12) continue;
                worst = std::max(worst, mag[k]);
            }
            REQUIRE(units::linear_to_db(worst / fundamental) <= -60.0);
        }
    }
}

TEST_CASE("47 the resampler is flat through the audio band and steep above it",
          "[fet-compressor][aliasing]") {
    // The round trip's own response, measured rather than asserted from the
    // design equations — this is the number the doc block quotes, and the
    // reason the 60 dB criterion above is met at all.
    const auto round_trip_db = [](double freq_hz) {
        Comp c = probe(FetRatio::r4_1, 0.0, Comp::kAttackUsMax, Comp::kReleaseMsMax);
        const auto rendered = steady_render(c, 0.02, freq_hz, 16384, 0.2);
        return component_db(rendered, freq_hz) - units::linear_to_db(0.02);
    };
    // Below the reference so nothing compresses: this measures the filters.
    for (double f : {1000.0, 5000.0, 10000.0, 15000.0})
        REQUIRE_THAT(round_trip_db(f), WithinAbs(0.0, 0.05));
    // Documented, honest passband cost of a 65-tap pair at 4×.
    REQUIRE(round_trip_db(18000.0) > -0.6);
    REQUIRE(round_trip_db(19200.0) > -1.6);
}

TEST_CASE("48 render reset re-render is bit-identical", "[fet-compressor][determinism]") {
    const auto render = [](Comp& c, int n) {
        Xorshift32 rng{20260725u};
        std::vector<double> out;
        out.reserve(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) out.push_back(c.process(0.5 * rng.next_bipolar<double>()));
        return out;
    };

    for (auto ratio : {FetRatio::r4_1, FetRatio::r20_1, FetRatio::all_buttons_in}) {
        Comp c = probe(ratio, 12.0, 200.0, 300.0, 1.0);
        c.set_transformer_amount(0.6);
        c.set_output_gain_db(3.0);
        c.set_mix(0.7);
        c.reset();

        const auto first = render(c, static_cast<int>(kSr * 2));
        c.reset();
        const auto second = render(c, static_cast<int>(kSr * 2));

        // A third pass on a freshly constructed instance: (a), (b), (c) of the
        // spec's recipe, all three bit-identical.
        Comp fresh = probe(ratio, 12.0, 200.0, 300.0, 1.0);
        fresh.set_transformer_amount(0.6);
        fresh.set_output_gain_db(3.0);
        fresh.set_mix(0.7);
        fresh.reset();
        const auto third = render(fresh, static_cast<int>(kSr * 2));

        REQUIRE(first.size() == second.size());
        REQUIRE(first.size() == third.size());
        for (std::size_t i = 0; i < first.size(); ++i) {
            REQUIRE(first[i] == second[i]);
            REQUIRE(first[i] == third[i]);
        }
    }
}

TEST_CASE("48 reset returns every stage to a silence-equivalent state",
          "[fet-compressor][determinism]") {
    Comp swept = probe(FetRatio::all_buttons_in, 20.0);
    swept.set_transformer_amount(1.0);
    const double w = kTwoPi * 1000.0 / kSr;
    for (int i = 0; i < static_cast<int>(kSr); ++i) swept.process(std::sin(w * i));
    swept.reset();

    Comp fresh = probe(FetRatio::all_buttons_in, 20.0);
    fresh.set_transformer_amount(1.0);
    fresh.reset();

    for (int i = 0; i < 4800; ++i) REQUIRE(swept.process(0.0) == fresh.process(0.0));
    REQUIRE(swept.gain_reduction_db() == fresh.gain_reduction_db());
    REQUIRE(swept.control_voltage() == fresh.control_voltage());
}

TEST_CASE("410 latency is exactly the resampler pair's group delay",
          "[fet-compressor][latency]") {
    // Computed from the shipped geometry, not restated: two linear-phase
    // filters of `kResamplerTaps` taps, each contributing `(N−1)/2` samples at
    // the oversampled rate, folded through the factor.
    REQUIRE(Comp::kLatencySamples ==
            2 * ((Comp::kResamplerTaps - 1) / 2) / Comp::kOversamplingFactor);
    REQUIRE((Comp::kResamplerTaps - 1) / 2 % Comp::kOversamplingFactor == 0);

    // Parameter independence: latency is oversampling-filter-determined only.
    for (auto ratio : {FetRatio::r4_1, FetRatio::r20_1, FetRatio::all_buttons_in}) {
        for (double us : {Comp::kAttackUsMin, Comp::kAttackUsMax}) {
            for (double ms : {Comp::kReleaseMsMin, Comp::kReleaseMsMax}) {
                Comp c = probe(ratio, 0.0, us, ms);
                REQUIRE(c.latency_samples() == Comp::kLatencySamples);
            }
        }
    }
}

TEST_CASE("410 an impulse arrives at exactly the reported latency",
          "[fet-compressor][latency]") {
    // Input gain low enough that nothing engages, so this measures the audio
    // path alone.
    for (auto ratio : {FetRatio::r4_1, FetRatio::r20_1}) {
        Comp c = probe(ratio, 0.0, 200.0, 300.0);
        int peak_index = -1;
        double peak = 0.0;
        for (int i = 0; i < Comp::kLatencySamples + 96; ++i) {
            const double y = c.process(i == 0 ? 0.01 : 0.0);
            if (std::abs(y) > peak) {
                peak = std::abs(y);
                peak_index = i;
            }
        }
        REQUIRE(peak_index == c.latency_samples());
        REQUIRE(c.gain_reduction_db() < 0.01);  // nothing compressed
    }
}

TEST_CASE("410 the composite response is symmetric about the reported delay",
          "[fet-compressor][latency]") {
    // The stronger form of the latency claim, and the one that catches a
    // FRACTIONAL error. Both filters are linear phase, so the composite
    // base-rate impulse response must be symmetric about sample
    // `kLatencySamples` exactly. Decimating on the wrong phase shifts the
    // composite by a quarter of a base sample — too little to move the impulse
    // PEAK off sample 16, so the peak-index test above cannot see it, but it
    // destroys the symmetry immediately and would make the reported latency
    // wrong by a quarter sample for host delay compensation.
    Comp c = probe(FetRatio::r4_1, 0.0, Comp::kAttackUsMax, Comp::kReleaseMsMax);
    // There are only `kLatencySamples` samples on the left side of the centre.
    // Extending this past the delay indexed the vector with a negative value
    // converted to size_t, making the proof undefined and seed/order flaky.
    constexpr int kTail = Comp::kLatencySamples;
    std::vector<double> response;
    for (int i = 0; i < Comp::kLatencySamples + kTail + 1; ++i)
        response.push_back(c.process(i == 0 ? 1e-4 : 0.0));
    REQUIRE(c.gain_reduction_db() < 1e-9);  // nothing engaged; this is the filters alone

    const double peak = std::abs(response[static_cast<std::size_t>(Comp::kLatencySamples)]);
    REQUIRE(peak > 0.0);
    for (int k = 1; k <= kTail; ++k) {
        const double before = response[static_cast<std::size_t>(Comp::kLatencySamples - k)];
        const double after = response[static_cast<std::size_t>(Comp::kLatencySamples + k)];
        REQUIRE_THAT(after, WithinAbs(before, peak * 1e-9));
    }
}

TEST_CASE("410 the dry blend path carries the same delay", "[fet-compressor][latency]") {
    // A fully dry blend must be a pure delay of exactly `latency_samples()`,
    // or the parallel path is a comb filter. Bit-exact, because it is a ring
    // buffer and nothing else.
    Comp c = probe(FetRatio::all_buttons_in, Comp::kInputGainDbMax);
    c.set_mix(0.0);
    c.set_transformer_amount(1.0);
    c.set_output_gain_db(Comp::kOutputGainDbMax);

    Xorshift32 rng{99u};
    std::vector<double> input;
    for (int i = 0; i < 2000; ++i) input.push_back(0.5 * rng.next_bipolar<double>());
    for (int i = 0; i < 2000; ++i) {
        const double y = c.process(input[static_cast<std::size_t>(i)]);
        if (i >= c.latency_samples())
            REQUIRE(y == input[static_cast<std::size_t>(i - c.latency_samples())]);
    }
}

TEST_CASE("the transformer tilts the low end down and nothing up",
          "[fet-compressor][transformer]") {
    const auto response_db = [](double amount, double freq_hz) {
        Comp c = probe(FetRatio::r4_1, 0.0, Comp::kAttackUsMax, Comp::kReleaseMsMax);
        c.set_transformer_amount(amount);
        const auto rendered = steady_render(c, 0.02, freq_hz, 16384, 0.3);
        return component_db(rendered, freq_hz) - units::linear_to_db(0.02);
    };

    // At zero depth the stage is out of the way.
    REQUIRE_THAT(response_db(0.0, 30.0), WithinAbs(0.0, 0.02));
    // At full depth the low end is cut by the design's tilt and the midband is
    // untouched — a cut-only shelf, which is what keeps the gain bound at 1.
    REQUIRE(response_db(1.0, 30.0) < -Comp::kTransformerTiltDbMax * 0.5);
    REQUIRE(response_db(1.0, 30.0) > -Comp::kTransformerTiltDbMax - 0.1);
    REQUIRE_THAT(response_db(1.0, 1000.0), WithinAbs(0.0, 0.05));
    REQUIRE_THAT(response_db(1.0, 8000.0), WithinAbs(0.0, 0.05));
    // Depth is monotone in the control.
    REQUIRE(response_db(0.6, 30.0) > response_db(1.0, 30.0));
    REQUIRE(response_db(0.6, 30.0) < response_db(0.0, 30.0));
}

TEST_CASE("input gain is the only route into gain reduction",
          "[fet-compressor][controls]") {
    // The design has no threshold control. Two settings that put the same level
    // into the fixed reference must produce the same reduction, whichever way
    // the level got there.
    Comp hot = probe(FetRatio::r8_1, 12.0);
    Comp cold = probe(FetRatio::r8_1, 0.0);
    const double a = settled_reduction_db(hot, -12.0);
    const double b = settled_reduction_db(cold, 0.0);
    REQUIRE_THAT(a, WithinAbs(b, 0.05));
    REQUIRE(a > 1.0);

    // And below the reference, nothing happens however long it runs.
    Comp quiet = probe(FetRatio::r20_1, 0.0, 200.0, 300.0, 0.0);
    REQUIRE_THAT(settled_reduction_db(quiet, -30.0), WithinAbs(0.0, 1e-6));
}
