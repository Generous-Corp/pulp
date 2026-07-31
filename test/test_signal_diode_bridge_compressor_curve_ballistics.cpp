#include "test_signal_diode_bridge_compressor_support.hpp"

#include <numbers>

TEST_CASE("A1 the divider realises its requested gain reduction exactly",
          "[diode-bridge][gain-law]") {
    // At −40 dBFS the shaper's curvature is utterly negligible — with the
    // default character the internal amplitude is ~1.1e-3, so the fundamental's
    // compression factor `1 − β·s²/4` differs from 1 by 1.5e-7 — which is what
    // makes this a measurement of the DIVIDER rather than of the colour.
    //
    // ADAA is off here for the same reason it is on everywhere else: the
    // first-order scheme is exactly a two-tap average, whose magnitude at the
    // probe frequency is `cos(ω/2)` and whose half-sample delay moves the peak
    // off the sampling grid. Together those cost 0.037 dB at 1 kHz — 75 % of
    // the ±0.05 dB budget, for a reason that has nothing to do with the gain
    // law. The next case asserts that offset instead of hiding it.
    for (double x : {0.413, 1.0, 2.981, 9.0}) {
        Bridge bridge;
        bridge.prepare(kSr);
        bridge.set_character(0.35);
        bridge.set_adaa(false);
        bridge.reset();

        const double amplitude = units::db_to_linear(-40.0);
        double peak = 0.0;
        for (int n = 0; n < 9600; ++n) {
            const double y =
                bridge.process(amplitude * std::sin(2.0 * std::numbers::pi * kToneHz * n / kSr), x);
            if (n > 4800) peak = std::max(peak, std::abs(y));
        }
        const double expected_db = units::linear_to_db(Bridge::gain_for_control_drive(x));
        REQUIRE_THAT(units::linear_to_db(peak / amplitude), WithinAbs(expected_db, 0.05));
    }

    // The worked table: x = 0.413 / 0.995 / 2.981 / 9.0 are the drives for
    // −3 / −6 / −12 / −20 dB, and the conversion is its own exact inverse.
    for (double gr_db : {-3.0, -6.0, -12.0, -20.0, -30.0}) {
        const double x = Bridge::control_drive_for_gain_db(gr_db);
        REQUIRE_THAT(units::linear_to_db(Bridge::gain_for_control_drive(x)), WithinAbs(gr_db, 1e-12));
    }
}

TEST_CASE("A1 the ADAA offset is the two-tap average, not a gain error",
          "[diode-bridge][gain-law][adaa]") {
    // First-order ADAA of a LINEAR function is `(s[n] + s[n−1])/2` identically,
    // so its effect on a sine is fully determined: magnitude `cos(ω/2)`, delay
    // half a sample. Sampling the peak of a half-sample-delayed sine costs a
    // second factor of `cos(ω/2)` at this probe frequency, where a sample lands
    // exactly on the crest. The measured deviation is therefore `cos²(ω/2)`
    // — asserted, so that a future change to the ADAA path cannot pass this
    // suite by quietly turning into a gain stage.
    const double w = 2.0 * std::numbers::pi * kToneHz / kSr;
    const double predicted_db = units::linear_to_db(std::cos(0.5 * w) * std::cos(0.5 * w));

    Bridge bridge;
    bridge.prepare(kSr);
    bridge.set_character(0.35);
    bridge.set_adaa(true);
    bridge.reset();

    const double amplitude = units::db_to_linear(-40.0);
    double peak = 0.0;
    for (int n = 0; n < 9600; ++n) {
        const double y = bridge.process(amplitude * std::sin(w * n), 0.0);
        if (n > 4800) peak = std::max(peak, std::abs(y));
    }
    REQUIRE_THAT(units::linear_to_db(peak / amplitude), WithinAbs(predicted_db, 0.002));
    // ...and it is a LOSS, so it cannot mask a gain the bound would have to
    // account for.
    REQUIRE(predicted_db < 0.0);
}

TEST_CASE("A1 the gain element can never boost", "[diode-bridge][gain-law]") {
    // The structural fact the whole worst-case bound rests on. Asserted over
    // the declared drive range and past it.
    for (double x = 0.0; x <= 2.0 * Bridge::kMaxControlDrive; x += 0.01)
        REQUIRE(Bridge::gain_for_control_drive(x) <= 1.0);
    REQUIRE(Bridge::gain_for_control_drive(0.0) == 1.0);
    // A negative drive is a caller error, not a boost request.
    REQUIRE(Bridge::gain_for_control_drive(-5.0) == 1.0);
}

TEST_CASE("the control law comes from the shared junction, not a second exponential",
          "[diode-bridge][junction]") {
    // `junction.hpp` owns the thermal voltage and the exponential. This asserts
    // that routing the drive law through `conductance(knee_voltage(I))`
    // reproduces the two textbook closed forms EXACTLY — `r_d = n·V_T/(I + Is)`
    // and `x = Rs·I/(n·V_T)` — which is the evidence that the composition is
    // load-bearing rather than decorative. If someone later inlines an `exp`
    // here, `theta` below stops matching and this fails.
    Bridge bridge;
    const double theta = Bridge::kIdeality * junction::kThermalVoltage;

    for (double amperes : {1e-6, 1e-5, 1e-4, 1e-3, 1e-2}) {
        REQUIRE_THAT(bridge.dynamic_resistance(amperes),
                     WithinRel(theta / (amperes + Bridge::kSaturationCurrent), 1e-9));
        REQUIRE_THAT(bridge.control_drive_for_current(amperes),
                     WithinRel(Bridge::kSeriesResistance * amperes / theta, 1e-6));
    }

    // No current means no reduction, as an identity rather than an
    // approximation: the zero-bias conductance is subtracted out.
    REQUIRE_THAT(bridge.control_drive_for_current(0.0), WithinAbs(0.0, 1e-15));
    REQUIRE(Bridge::gain_for_control_drive(bridge.control_drive_for_current(0.0)) == 1.0);

    // Resistance really is inversely proportional to bias current — the claim
    // the entire topology rests on. A decade of current is a decade of
    // resistance.
    REQUIRE_THAT(bridge.dynamic_resistance(1e-5) / bridge.dynamic_resistance(1e-4),
                 WithinRel(10.0, 1e-6));
}

TEST_CASE("the shaper's antiderivative differentiates back to the shaper",
          "[diode-bridge][adaa]") {
    // The ADAA path is only as good as this identity, in both blocks. A sign
    // error here does not crash — it produces a waveform that is wrong on half
    // of each cycle and still reads as the colour working.
    for (double x : {0.0, 1.0, 9.0, Bridge::kMaxControlDrive}) {
        const double beta = Bridge::curvature(x);
        for (double s = -0.3; s <= 0.3; s += 0.005) {
            const double h = 1e-6;
            const double numerical = (Bridge::shape_antiderivative(s + h, beta) -
                                      Bridge::shape_antiderivative(s - h, beta)) /
                                     (2.0 * h);
            REQUIRE_THAT(numerical, WithinAbs(Bridge::shape(s, beta), 1e-8));
        }
    }

    for (double character : {0.0, 0.35, 1.0}) {
        Bracket bracket;
        bracket.prepare(kSr);
        bracket.set_character(character);
        // Straddles zero, where the piecewise split lives: the two branches
        // must agree in value AND slope or the quotient is wrong every time a
        // waveform crosses the axis.
        for (double u = -1.5; u <= 1.5; u += 0.01) {
            const double h = 1e-6;
            const double numerical = (bracket.saturate_antiderivative(u + h) -
                                      bracket.saturate_antiderivative(u - h)) /
                                     (2.0 * h);
            REQUIRE_THAT(numerical, WithinAbs(bracket.saturate(u), 1e-8));
        }
    }
}

TEST_CASE("the shaper stays monotonic across its whole operating range",
          "[diode-bridge][gain-law]") {
    // The cubic folds back beyond `1/√β`; the clamp is what makes that
    // unreachable. Asserted at the deepest curvature the drive range admits.
    const double beta = Bridge::curvature(Bridge::kMaxControlDrive);
    const double limit = Bridge::max_operating_amplitude(beta);
    REQUIRE(limit < 1.0 / std::sqrt(beta));

    double previous = Bridge::shape(-limit, beta);
    for (double s = -limit + 1e-4; s <= limit; s += 1e-4) {
        const double value = Bridge::shape(s, beta);
        REQUIRE(value > previous);
        previous = value;
    }

    // Full-scale audio at maximum character lands inside that clamp, so the
    // guard is a guard rather than a shaping stage.
    Bridge bridge;
    bridge.set_character(1.0);
    REQUIRE(bridge.drive() <= Bridge::max_operating_amplitude(beta));
    REQUIRE(bridge.drive() == Bridge::kDriveBridgeMax);
}

TEST_CASE("A2 the realised curve follows the gain computer", "[diode-bridge][static-curve]") {
    // Feed-forward isolates the computer from the loop softening A8 covers.
    //
    // TWO DEVIATIONS FROM THE SPEC'S RECIPE, both stated rather than absorbed:
    //
    // (1) Gain reduction is measured RELATIVE to the small-signal gain rather
    //     than absolutely. The colour stages have a fixed −0.063 dB insertion
    //     loss at 1 kHz even at `character = 0` — 0.054 dB of ADAA two-tap
    //     averaging plus 0.009 dB of bracket filter roll-off — which is a
    //     level-independent constant, not a gain-computer error. Charging it to
    //     the computer would spend 63 % of a ±0.1 dB budget before the first
    //     comparison.
    //
    // (2) The release is set to the top of its range. A one-pole PEAK follower
    //     decays between the peaks of a tone, so it reads a sine slightly under
    //     its true peak, and the resulting error is entirely a function of how
    //     much it decays per cycle: at 400 / 1000 / 2000 ms the worst
    //     hard-region error is 0.193 / 0.113 / 0.085 dB. A static-curve
    //     measurement wants peak-hold, which the spec's recipe does not pin.
    Comp c = make_compressor();
    c.set_release_ms(Comp::kReleaseMsMax);

    const double reference_db =
        units::linear_to_db(settled_peak(c, units::db_to_linear(-40.0), 5.0)) + 40.0;

    for (int db = -40; db <= 6; ++db) {
        c.reset();
        const double amplitude = units::db_to_linear(static_cast<double>(db));
        const double measured =
            units::linear_to_db(settled_peak(c, amplitude, 5.0)) - db - reference_db;
        const double expected = c.static_curve_db(static_cast<double>(db));
        const bool in_knee = std::abs(static_cast<double>(db) - (-12.0)) <= 0.5 * 6.0;
        REQUIRE_THAT(measured, WithinAbs(expected, in_knee ? 0.3 : 0.1));
    }
}

TEST_CASE("A2 the limit region engages at the top of the ratio control",
          "[diode-bridge][static-curve]") {
    // `kLimitRatio` must be reachable from the parameter table, or the
    // brickwall position is unreachable — the specific failure the spec's
    // "must be ≥ the ratio parameter's max" note guards against.
    REQUIRE(Comp::kLimitRatio <= Comp::kRatioMax);

    Comp c = make_compressor();
    c.set_knee_db(0.0);
    c.set_ratio(Comp::kRatioMax);
    // Above the threshold the curve pins the output AT the threshold: every dB
    // of input past it becomes a dB of reduction.
    for (double level : {-6.0, 0.0, 6.0})
        REQUIRE_THAT(c.static_curve_db(level), WithinAbs(-(level + 12.0), 1e-12));
}

TEST_CASE("A3 the bridge's third harmonic matches the closed form",
          "[diode-bridge][colour]") {
    // SPEC DEFECT. §3.4's worked example computes the third-harmonic ratio as
    // `β·s²/3`, taking the cubic term's amplitude `β·s³/3` over the fundamental
    // `s`. That drops the `sin³θ = (3·sinθ − sin3θ)/4` expansion: only a
    // QUARTER of the cubic term lands on the third harmonic, and the other
    // three quarters subtract from the fundamental. The correct ratio is
    //
    //     THD3 = (β·s³/12) / (s·(1 − β·s²/4)) = β·s²/12 / (1 − β·s²/4)
    //
    // — four times smaller. At the spec's own worked point (−6 dBFS,
    // character 0.35, at rest) that is 0.0124 %, not 0.0497 %, and A3's
    // "≈0.05 %, tol ±0.02 %-absolute" band of [0.03 %, 0.07 %] excludes the
    // right answer by a factor of 2.4. Asserted against the shipped closed form
    // `Bridge::third_harmonic_ratio()`, which is derived above rather than
    // restated here.
    const auto measure = [](double control_drive) {
        Bridge bridge;
        bridge.prepare(kSr);
        bridge.set_character(0.35);
        bridge.reset();
        const double amplitude = units::db_to_linear(-6.0);
        std::vector<double> out;
        out.reserve(kTonePeriod * 500);
        for (int n = 0; n < 4800 + kTonePeriod * 500; ++n) {
            const double y = bridge.process(
                amplitude * std::sin(2.0 * std::numbers::pi * kToneHz * n / kSr), control_drive);
            if (n >= 4800) out.push_back(y);
        }
        return harmonic_magnitude(out, kToneHz, 3) / harmonic_magnitude(out, kToneHz, 1);
    };

    Bridge reference;
    reference.set_character(0.35);
    const double s = units::db_to_linear(-6.0) * reference.drive();

    const double at_rest = measure(0.0);
    const double predicted_rest = Bridge::third_harmonic_ratio(s, Bridge::curvature(0.0));
    REQUIRE_THAT(at_rest, WithinRel(predicted_rest, 0.05));
    // Small enough to be the documented "low distortion by design" regime, and
    // large enough to be a colour rather than a rounding error.
    REQUIRE(at_rest > 1e-4);
    REQUIRE(at_rest < 1e-3);

    // The spec's figure, shown to be the one that is wrong.
    REQUIRE(std::abs(at_rest - Bridge::curvature(0.0) * s * s / 3.0) > 2e-4);
}

TEST_CASE("A3 curvature grows with control drive — the colour comes from the gain element",
          "[diode-bridge][colour]") {
    // THE mechanism that separates this lineage from a VCA or FET design: the
    // gain element itself generates more harmonics the harder it is
    // compressing. Measured with the INPUT HELD CONSTANT and only the control
    // drive changed, so the effect cannot be confused with "a louder input
    // distorts more" — the spec's own A3 recipe raises the input past the
    // threshold, which conflates the two.
    //
    // The prediction is exact: `β(x)/β(0) = 1 + κ·x`, and THD3 is proportional
    // to β to first order.
    const auto thd3 = [](double control_drive) {
        Bridge bridge;
        bridge.prepare(kSr);
        bridge.set_character(0.35);
        bridge.reset();
        const double amplitude = units::db_to_linear(-6.0);
        std::vector<double> out;
        out.reserve(kTonePeriod * 500);
        for (int n = 0; n < 4800 + kTonePeriod * 500; ++n) {
            const double y = bridge.process(
                amplitude * std::sin(2.0 * std::numbers::pi * kToneHz * n / kSr), control_drive);
            if (n >= 4800) out.push_back(y);
        }
        return harmonic_magnitude(out, kToneHz, 3) / harmonic_magnitude(out, kToneHz, 1);
    };

    const double drive_12db = Bridge::control_drive_for_gain_db(-12.0);
    const double at_rest = thd3(0.0);
    const double compressing = thd3(drive_12db);

    REQUIRE(compressing > at_rest);
    REQUIRE_THAT(compressing / at_rest,
                 WithinRel(1.0 + Bridge::kDriveCurvature * drive_12db, 0.02));

    // ...and it is odd-symmetric: the balanced bridge produces no even
    // harmonics, which is the physical claim the four-diode topology makes.
    Bridge bridge;
    bridge.prepare(kSr);
    bridge.set_character(1.0);
    bridge.set_adaa(false);
    bridge.reset();
    std::vector<double> out;
    for (int n = 0; n < 4800 + kTonePeriod * 500; ++n) {
        const double y =
            bridge.process(0.9 * std::sin(2.0 * std::numbers::pi * kToneHz * n / kSr), 3.0);
        if (n >= 4800) out.push_back(y);
    }
    const double h1 = harmonic_magnitude(out, kToneHz, 1);
    REQUIRE(harmonic_magnitude(out, kToneHz, 3) / h1 > 1e-3);
    for (int k : {2, 4, 6}) REQUIRE(harmonic_magnitude(out, kToneHz, k) / h1 < 1e-9);
}

TEST_CASE("A3 the node's colour deepens as it compresses", "[diode-bridge][colour]") {
    // The same claim end to end, through the full device, which is where a
    // listener meets it.
    const auto node_thd3 = [](double input_db) {
        Comp c = make_compressor();
        c.set_character(0.6);
        c.set_release_ms(Comp::kReleaseMsMax);
        c.reset();
        const double amplitude = units::db_to_linear(input_db);
        std::vector<double> out;
        out.reserve(kTonePeriod * 500);
        for (int n = 0; n < static_cast<int>(kSr) + kTonePeriod * 500; ++n) {
            const double y =
                c.process(amplitude * std::sin(2.0 * std::numbers::pi * kToneHz * n / kSr));
            if (n >= static_cast<int>(kSr)) out.push_back(y);
        }
        return harmonic_magnitude(out, kToneHz, 3) / harmonic_magnitude(out, kToneHz, 1);
    };

    REQUIRE(node_thd3(0.0) > node_thd3(-30.0));
}

TEST_CASE("A4 ADAA suppresses the folded harmonic", "[diode-bridge][adaa][aliasing]") {
    // SPEC DEFECT. A4 probes at 8 kHz / 48 kHz. The shaper is a pure cubic, so
    // its ONLY generated harmonic is the third — at exactly 24 kHz, which is
    // Nyquist. `sin(3ωn) = sin(πn) = 0` for every integer n, so the naive
    // shaper's aliased energy at that probe is identically zero (measured
    // −200 dBFS, i.e. the double-precision floor). ADAA is not a pointwise map
    // and does produce a component there (−56.7 dBFS), so at 8 kHz the spec's
    // comparison reports ADAA as 143 dB WORSE than naive. The criterion is not
    // merely hard, it is inverted.
    //
    // The probe is moved to a frequency whose third harmonic genuinely folds
    // and lands on a distinguishable bin: bin 947 of a 4096-point window is
    // 11097.66 Hz, whose third harmonic at 33.29 kHz folds to bin 1255
    // (14.7 kHz). Everything else about the recipe — full scale, maximum
    // character, comparison against the same shaper with ADAA off — is the
    // spec's. The window is 4096 rather than 65536 because a bin-aligned probe
    // needs no window function and the DFT here is direct.
    constexpr int kWindow = 4096;
    constexpr int kProbeBin = 947;

    const auto render = [](bool adaa) {
        Bridge bridge;
        bridge.prepare(kSr);
        bridge.set_character(1.0);
        bridge.set_adaa(adaa);
        bridge.reset();
        std::vector<double> out;
        out.reserve(kWindow);
        for (int n = 0; n < kWindow + kWindow; ++n) {
            const double y = bridge.process(
                std::sin(2.0 * std::numbers::pi * kProbeBin * n / double(kWindow)), 0.0);
            if (n >= kWindow) out.push_back(y);
        }
        return out;
    };

    // Every bin except the fundamental: for a cubic the only real content is
    // the folded third harmonic, so summing the rest is a conservative
    // accounting of aliased energy plus the numerical floor.
    const auto analyse = [](const std::vector<double>& x, double* fundamental, double* worst) {
        double energy = 0.0;
        *worst = 0.0;
        for (int bin = 1; bin < kWindow / 2; ++bin) {
            double re = 0.0, im = 0.0;
            for (int n = 0; n < kWindow; ++n) {
                const double w = 2.0 * std::numbers::pi * bin * n / double(kWindow);
                re += x[static_cast<std::size_t>(n)] * std::cos(w);
                im += x[static_cast<std::size_t>(n)] * std::sin(w);
            }
            const double magnitude = 2.0 * std::hypot(re, im) / kWindow;
            if (bin == kProbeBin) {
                *fundamental = magnitude;
                continue;
            }
            energy += magnitude * magnitude;
            *worst = std::max(*worst, magnitude);
        }
        return std::sqrt(energy);
    };

    double naive_fundamental = 0.0, naive_worst = 0.0;
    double adaa_fundamental = 0.0, adaa_worst = 0.0;
    const double naive_alias = analyse(render(false), &naive_fundamental, &naive_worst);
    const double adaa_alias = analyse(render(true), &adaa_fundamental, &adaa_worst);

    // The spec's criterion, absolute.
    REQUIRE(units::linear_to_db(naive_alias / adaa_alias) >= 18.0);
    // ...and normalised by the fundamental, so the first-order scheme's own
    // `cos(ω/2)` roll-off cannot be mistaken for alias suppression. This is the
    // stricter reading and it also clears 18 dB at this probe.
    REQUIRE(units::linear_to_db((naive_alias / naive_fundamental) /
                                (adaa_alias / adaa_fundamental)) >= 18.0);
    // No aliased component above −60 dBFS on the shipped path.
    REQUIRE(units::linear_to_db(adaa_worst) < -60.0);
    // The naive path is over that line, so the criterion is discriminating
    // rather than vacuous.
    REQUIRE(units::linear_to_db(naive_worst) > -60.0);
}

TEST_CASE("A5 render, reset, re-render is bit-identical", "[diode-bridge][determinism]") {
    // There is no randomness anywhere in the module, so this checks that no
    // uninitialised or carried-over state leaks between renders.
    const auto render = [](Comp& c, int samples) {
        Xorshift32 noise(0xB12DE5u);
        std::vector<double> out;
        out.reserve(static_cast<std::size_t>(samples));
        // Pink-ish: a one-pole-integrated white source, deterministic by seed.
        double state = 0.0;
        for (int n = 0; n < samples; ++n) {
            state = 0.98 * state + 0.02 * noise.next_bipolar<double>();
            out.push_back(c.process(0.7 * (state * 6.0)));
        }
        return out;
    };

    Comp c;
    c.prepare(kSr);
    c.set_threshold_db(-15.0);
    c.set_ratio(6.0);
    c.set_knee_db(9.0);
    c.set_attack_ms(7.0);
    c.set_release_ms(250.0);
    c.set_character(0.7);
    c.set_makeup_db(9.0);
    c.set_mix_percent(65.0);
    c.set_sc_hpf_hz(160.0);
    c.set_auto_release(true);
    c.set_feedback(true);
    c.reset();

    const auto first = render(c, static_cast<int>(kSr * 5));
    c.reset();
    const auto second = render(c, static_cast<int>(kSr * 5));

    REQUIRE(first.size() == second.size());
    for (std::size_t i = 0; i < first.size(); ++i) REQUIRE(first[i] == second[i]);
}

TEST_CASE("A6 the node reports and measures zero latency", "[diode-bridge][latency]") {
    Comp c = make_compressor();
    c.set_character(1.0);
    c.set_feedback(true);
    REQUIRE(c.latency_samples() == 0);

    c.reset();
    const double first = c.process(1.0);
    REQUIRE(std::abs(first) > 0.0);

    // ...and the rest of the impulse response is finite and decays, so "nonzero
    // at n = 0" is not being bought with an unstable path.
    double previous = std::abs(first);
    for (int n = 1; n < 4096; ++n) {
        const double y = c.process(0.0);
        REQUIRE(std::isfinite(y));
        if (n > 512) REQUIRE(std::abs(y) <= previous + 1e-6);
        previous = std::max(previous, std::abs(y));
    }
}

TEST_CASE("A7 the worst-case gain bound is the one the registry cites",
          "[diode-bridge][gain][worst-case]") {
    // Series law 8: a tested invariant, not an estimate.
    REQUIRE_THAT(Comp::worst_case_gain(),
                 WithinRel(units::db_to_linear(Comp::kMakeupDbMax), 1e-12));
    REQUIRE_THAT(Comp::worst_case_gain(), WithinAbs(15.8489319, 1e-6));

    // SPEC DEFECT. A7 asks for "instantaneous |output| never exceeds |input| ×
    // 15.85". No signal path containing a filter can satisfy that, and the
    // counterexample is one line long: send a single impulse and the input is
    // zero from the next sample on while the output is still ringing, so the
    // instantaneous ratio is unbounded by inspection. What is bounded — and
    // what the registry constant actually means — is the ratio of output PEAK
    // to input PEAK over a render, which is what is asserted.
    //
    // The measurement is taken after the brackets' cold-start transient. That
    // is not a convenience: a linear filter's peak-to-peak gain on a transient
    // is its impulse response's L1 norm, which is strictly larger than the
    // supremum of its magnitude response, so a cold start legitimately exceeds
    // a frequency-domain bound. The excess is measured and accounted for
    // below rather than waved at.
    const auto sweep = [](double seconds, bool skip_transient) {
        double worst = 0.0;
        for (double db = -60.0; db <= 12.001; db += 3.0) {
            Comp c;
            c.prepare(kSr);
            c.set_makeup_db(Comp::kMakeupDbMax);
            c.set_character(1.0);
            c.set_ratio(Comp::kRatioMin);
            c.set_threshold_db(Comp::kThresholdDbMax);
            c.reset();
            const double amplitude = units::db_to_linear(db);
            const int total = static_cast<int>(kSr * seconds);
            const int start = skip_transient ? static_cast<int>(kSr * 0.1) : 0;
            double in_peak = 0.0, out_peak = 0.0;
            for (int n = 0; n < total; ++n) {
                const double x = amplitude * std::sin(2.0 * std::numbers::pi * kToneHz * n / kSr);
                const double y = c.process(x);
                if (n < start) continue;
                in_peak = std::max(in_peak, std::abs(x));
                out_peak = std::max(out_peak, std::abs(y));
            }
            worst = std::max(worst, out_peak / in_peak);
        }
        return worst;
    };

    // THE INVARIANT: the settled peak ratio never exceeds the registry bound,
    // at any level, at the least-reducing setting with makeup wide open.
    REQUIRE(sweep(0.5, true) <= Comp::worst_case_gain());
    // ...and it gets close, so the bound is reached rather than merely
    // respected — a bound nothing approaches proves nothing.
    REQUIRE(sweep(0.5, true) > 0.98 * Comp::worst_case_gain());

    // The cold-start excess, accounted for from a COMPONENT measurement rather
    // than a fudge factor: it is exactly the two brackets' own small-signal
    // cold-start peak gain, squared.
    Bracket bracket;
    bracket.prepare(kSr);
    bracket.set_character(1.0);
    bracket.reset();
    double bracket_in = 0.0, bracket_out = 0.0;
    for (int n = 0; n < static_cast<int>(kSr * 0.1); ++n) {
        const double x = 1e-3 * std::sin(2.0 * std::numbers::pi * kToneHz * n / kSr);
        bracket_in = std::max(bracket_in, std::abs(x));
        bracket_out = std::max(bracket_out, std::abs(bracket.process(x)));
    }
    const double bracket_transient = bracket_out / bracket_in;
    REQUIRE(bracket_transient > 1.0);   // a real, if tiny, transient overshoot
    REQUIRE(bracket_transient < 1.02);  // and a small one
    REQUIRE(sweep(0.5, false) <=
            Comp::worst_case_gain() * bracket_transient * bracket_transient * 1.005);
}

TEST_CASE("A7 the bound holds for noise and impulses too", "[diode-bridge][gain][worst-case]") {
    Comp c;
    c.prepare(kSr);
    c.set_makeup_db(Comp::kMakeupDbMax);
    c.set_character(1.0);
    c.set_ratio(Comp::kRatioMin);
    c.set_threshold_db(Comp::kThresholdDbMax);
    c.reset();

    Xorshift32 noise(0x5EED1234u);
    double in_peak = 0.0, out_peak = 0.0;
    for (int n = 0; n < static_cast<int>(kSr * 2); ++n) {
        double x = noise.next_bipolar<double>();
        if (n % 7919 == 0) x = units::db_to_linear(12.0);  // a full-scale-plus spike
        in_peak = std::max(in_peak, std::abs(x));
        out_peak = std::max(out_peak, std::abs(c.process(x)));
    }
    REQUIRE(out_peak / in_peak <= Comp::worst_case_gain());
}

TEST_CASE("A7 the dry path cannot push the mix past the bound",
          "[diode-bridge][gain][worst-case]") {
    // Makeup applies to the wet path only, so a parallel blend interpolates
    // between unity and the wet gain and can never exceed the larger of the
    // two. Asserted because the alternative wiring — makeup after the sum —
    // would put the dry signal through the makeup stage and is an easy thing
    // to "simplify" into later.
    for (double mix : {0.0, 25.0, 50.0, 75.0, 100.0}) {
        Comp c;
        c.prepare(kSr);
        c.set_makeup_db(Comp::kMakeupDbMax);
        c.set_character(1.0);
        c.set_ratio(Comp::kRatioMin);
        c.set_threshold_db(Comp::kThresholdDbMax);
        c.set_mix_percent(mix);
        c.reset();
        double in_peak = 0.0, out_peak = 0.0;
        const int total = static_cast<int>(kSr * 0.5);
        for (int n = 0; n < total; ++n) {
            const double x = 0.01 * std::sin(2.0 * std::numbers::pi * kToneHz * n / kSr);
            const double y = c.process(x);
            if (n < static_cast<int>(kSr * 0.1)) continue;
            in_peak = std::max(in_peak, std::abs(x));
            out_peak = std::max(out_peak, std::abs(y));
        }
        REQUIRE(out_peak / in_peak <= Comp::worst_case_gain());
    }
}

TEST_CASE("A8 the follower's nominal time is a 10-90 percent rise time",
          "[diode-bridge][ballistics]") {
    // SPEC DEFECT, and its independent ground truth. A8 asks for the 63 % point
    // to land "within ±15 % of attack_ms / release_ms". `BallisticsFilterT`'s
    // coefficient is `1 − exp(−2.2/(ms·fs))`, and 2.2 is `ln 9` — the
    // 10 %-to-90 % rise-time convention `units.hpp` documents explicitly. Its
    // 63 % point is therefore at `ms/ln 9` = 45.5 % of nominal, so the spec's
    // criterion is off by a factor of 2.2 before the gain computer's dB-domain
    // mapping is even considered. Measured directly here so the adjudication
    // rests on the shipped follower rather than on reading its source.
    for (double ms : {3.0, 10.0, 400.0}) {
        BallisticsFilter64 follower;
        follower.prepare(kSr);
        follower.set_attack_ms(ms);
        follower.set_release_ms(ms);
        follower.reset();
        int n = 0;
        while (follower.process(1.0) < 0.63212) ++n;
        REQUIRE_THAT((1000.0 * n / kSr) / ms, WithinRel(1.0 / std::log(9.0), 0.015));
    }
}

TEST_CASE("A8 the release lands where the follower's convention predicts",
          "[diode-bridge][ballistics]") {
    // With the reference value corrected, the spec's ±15 % tolerance is kept.
    //
    // The prediction is computed here from the SHIPPED pieces: the `ln 9`
    // convention proved above, and the gain computer's own inverse. The
    // envelope decays as `e(n) = lo + (hi − lo)·exp(−n·ln9/N)`; the reduction
    // has fallen 63.2 % when `e` reaches the level whose static-curve output is
    // 36.8 % of the loud value; solving for n gives the fraction of nominal
    // below. For a −20 → 0 dBFS step at threshold −12, ratio 4, that is 0.474.
    constexpr double kQuietDb = -20.0;
    constexpr double kLoudDb = 0.0;
    const double quiet = units::db_to_linear(kQuietDb);
    const double loud = units::db_to_linear(kLoudDb);

    Comp curve = make_compressor();
    curve.set_knee_db(0.0);
    const double loud_reduction = -curve.static_curve_db(kLoudDb);
    const double target_reduction = 0.368 * loud_reduction;
    // Invert the hard-region characteristic to the envelope that produces it.
    const double target_db = -12.0 + target_reduction / (1.0 - 1.0 / 4.0);
    const double target_envelope = units::db_to_linear(target_db);
    const double predicted_fraction =
        -std::log((target_envelope - quiet) / (loud - quiet)) / std::log(9.0);

    for (double release : {100.0, 400.0, 1600.0}) {
        const auto step = measure_step(3.0, release, false);
        REQUIRE_THAT(step.release_ms / release, WithinRel(predicted_fraction, 0.15));
    }
}

TEST_CASE("A8 the ballistics controls are calibrated in their stated units",
          "[diode-bridge][ballistics]") {
    // The release is proportional to `release_ms` to well inside 5 %: the
    // follower decays continuously, so nothing gates it.
    const double a = measure_step(3.0, 100.0, false).release_ms / 100.0;
    const double b = measure_step(3.0, 400.0, false).release_ms / 400.0;
    const double c = measure_step(3.0, 1600.0, false).release_ms / 1600.0;
    REQUIRE_THAT(b, WithinRel(a, 0.05));
    REQUIRE_THAT(c, WithinRel(a, 0.05));

    // The ATTACK is not proportional, and the reason is structural rather than
    // a calibration error: a peak follower can only rise while `|x|` exceeds
    // its state, so a tonal stimulus gates the attack for most of each cycle
    // while nothing gates the release. The measured 63 % point runs from 1.18×
    // `attack_ms` at 3 ms down to 0.72× at 30 ms. What IS assertable — and what
    // a user needs — is that the control is monotonic and lands within a factor
    // well under two of its stated value.
    const double fast = measure_step(3.0, 400.0, false).attack_ms;
    const double medium = measure_step(10.0, 400.0, false).attack_ms;
    const double slow = measure_step(30.0, 400.0, false).attack_ms;
    REQUIRE(fast < medium);
    REQUIRE(medium < slow);
    REQUIRE(fast > 0.4 * 3.0);
    REQUIRE(fast < 1.3 * 3.0);
    REQUIRE(slow > 0.4 * 30.0);
    REQUIRE(slow < 1.3 * 30.0);

    // And the attack is far faster than the release at the defaults, which is
    // the ordering that makes the device a compressor rather than a gate.
    const auto defaults = measure_step(3.0, 400.0, false);
    REQUIRE(defaults.attack_ms * 20.0 < defaults.release_ms);
}

TEST_CASE("A8 feedback detection softens the realised ratio", "[diode-bridge][ballistics]") {
    // The documented signature of this lineage, asserted as the strict
    // inequality the spec asks for — plus the closed-form fixed point, which is
    // stronger and which the module ships as `static_curve_feedback_db()`.
    const auto forward = measure_step(3.0, 400.0, false);
    const auto looped = measure_step(3.0, 400.0, true);

    REQUIRE(looped.steady_reduction_db < forward.steady_reduction_db);

    Comp c = make_compressor();
    c.set_knee_db(0.0);
    // Feed-forward at 0 dBFS with threshold −12 and ratio 4: 12 dB over,
    // reduction 9 dB. Feedback: 9/1.75 = 5.14 dB, an effective 1.75:1.
    REQUIRE_THAT(-c.static_curve_db(0.0), WithinAbs(9.0, 1e-9));
    REQUIRE_THAT(-c.static_curve_feedback_db(0.0), WithinAbs(9.0 / 1.75, 1e-9));

    // The measurements track those closed forms once the detector's own
    // under-read of a tone is allowed for.
    REQUIRE_THAT(forward.steady_reduction_db, WithinAbs(9.0, 0.3));
    REQUIRE_THAT(looped.steady_reduction_db, WithinAbs(9.0 / 1.75, 0.3));

    // The effective ratio really is gentler than the knob says.
    const double effective_ratio = 12.0 / (12.0 - looped.steady_reduction_db);
    REQUIRE(effective_ratio < 4.0);
    REQUIRE(effective_ratio > 1.0);
}

TEST_CASE("A8 makeup gain stays outside the feedback loop", "[diode-bridge][ballistics]") {
    // The spec pins the tap at `output_pre_makeup`, and the reason is not
    // cosmetic: makeup inside the loop would make the makeup knob a second,
    // hidden threshold control, so raising the output level by 12 dB would
    // silently add ~7 dB of reduction and the compressor would fight its own
    // gain staging. The invariant is that gain reduction is INDEPENDENT of
    // makeup — which is also the property no other case in this suite covers,
    // because every other feedback measurement runs at 0 dB makeup.
    const auto reduction_with_makeup = [](double makeup_db) {
        Comp c = make_compressor();
        c.set_knee_db(0.0);
        c.set_feedback(true);
        c.set_release_ms(Comp::kReleaseMsMax);
        c.set_makeup_db(makeup_db);
        c.reset();
        for (int n = 0; n < static_cast<int>(kSr * 3.0); ++n)
            c.process(0.5 * std::sin(2.0 * std::numbers::pi * kToneHz * n / kSr));
        return c.gain_reduction_db();
    };

    const double baseline = reduction_with_makeup(0.0);
    REQUIRE(baseline < -1.0);  // it really is compressing, so the check has teeth
    for (double makeup : {6.0, 12.0, Comp::kMakeupDbMax})
        REQUIRE_THAT(reduction_with_makeup(makeup), WithinAbs(baseline, 1e-9));
}

TEST_CASE("A8 the feedback fixed point converges across the whole ratio range",
          "[diode-bridge][ballistics]") {
    // The solve is averaged specifically because the plain iteration `gr ←
    // f(gr)` has slope `1/ρ − 1`, which is exactly −1 in the limit region: it
    // oscillates between two values forever without narrowing, and returns
    // whichever one the iteration cap lands on. This asserts the residual is
    // driven to double precision at every ratio, including the top of the
    // control where the plain form fails outright.
    Comp c = make_compressor();
    c.set_knee_db(0.0);
    for (double ratio : {Comp::kRatioMin, 2.0, 4.0, 10.0, Comp::kLimitRatio, Comp::kRatioMax}) {
        c.set_ratio(ratio);
        for (double level : {-6.0, 0.0, 6.0, 18.0}) {
            const double gr = c.static_curve_feedback_db(level);
            REQUIRE_THAT(c.static_curve_db(level + gr), WithinAbs(gr, 1e-12));
        }
    }

    // The hard-region closed form, which is what the doc block's worked example
    // quotes: GR = (L − thr)·(1/ρ − 1)/(2 − 1/ρ).
    c.set_ratio(4.0);
    const double over = 12.0;
    const double slope = 1.0 / 4.0 - 1.0;
    REQUIRE_THAT(c.static_curve_feedback_db(0.0),
                 WithinAbs(over * slope / (1.0 - slope), 1e-12));
}
