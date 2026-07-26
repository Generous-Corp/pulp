#include "test_signal_diode_bridge_compressor_support.hpp"

TEST_CASE("A8 auto release is program-dependent, not just slow",
          "[diode-bridge][ballistics]") {
    // SPEC AMENDMENT, documented in the header: `max(fast, slow)` is
    // identically the slow follower, because during release both decay from the
    // same value and the slow one is always the larger. That would make "auto"
    // a synonym for "six times the release time" with nothing program-dependent
    // about it. The shipped blend is asserted to sit strictly BETWEEN the two
    // manual settings — which is the observable difference between a blend and
    // a maximum.
    const double manual_fast = measure_step(3.0, 200.0, false).release_ms;
    const double manual_slow =
        measure_step(3.0, 200.0 * Comp::kAutoSlowFactor, false).release_ms;
    REQUIRE(manual_slow > manual_fast);

    Comp c = make_compressor();
    c.set_knee_db(0.0);
    c.set_release_ms(200.0);
    c.set_auto_release(true);
    c.reset();

    const double quiet = units::db_to_linear(-20.0);
    const int pre = static_cast<int>(kSr * 1.0);
    const int hold = static_cast<int>(kSr * 2.0);
    const int post = static_cast<int>(kSr * 6.0);
    std::vector<double> reduction;
    reduction.reserve(static_cast<std::size_t>(pre + hold + post));
    for (int n = 0; n < pre + hold + post; ++n) {
        const double amplitude = (n < pre) ? quiet : (n < pre + hold ? 1.0 : quiet);
        c.process(amplitude * std::sin(2.0 * M_PI * kToneHz * n / kSr));
        reduction.push_back(-c.gain_reduction_db());
    }
    const double loud = reduction[static_cast<std::size_t>(pre + hold - 1)];
    const double recovered = reduction.back();
    const double target = loud + 0.63212 * (recovered - loud);
    int index = pre + hold;
    while (index < static_cast<int>(reduction.size()) &&
           reduction[static_cast<std::size_t>(index)] > target)
        ++index;
    const double automatic = 1000.0 * (index - pre - hold) / kSr;

    REQUIRE(automatic > manual_fast);
    REQUIRE(automatic <= manual_slow);
}

TEST_CASE("A9 the sidechain high-pass de-sensitises the low end",
          "[diode-bridge][sidechain]") {
    // SPEC DEFECT in the rationale, though the criterion survives at a stated
    // ratio. A9 asserts "the 60 Hz tone produces ≥ 4 dB less GR than the 1 kHz
    // tone", and justifies it with the high-pass's ~5.7 dB of relative
    // attenuation at 60 Hz. Those are different quantities: a level change
    // reaches the REDUCTION multiplied by `(1 − 1/ratio)`, so at the ratio
    // control's minimum of 1.5 the same 5.7 dB of rejection is only 1.9 dB of
    // reduction difference and the criterion fails. A9 does not pin the ratio;
    // it is pinned here, and the derived lower bound is asserted alongside the
    // spec's round number so the mechanism is what is being tested.
    const auto reduction_at = [](double tone_hz, double ratio) {
        Comp c = make_compressor();
        c.set_threshold_db(-20.0);
        c.set_ratio(ratio);
        c.set_knee_db(0.0);
        c.set_sc_hpf_hz(100.0);
        c.set_release_ms(Comp::kReleaseMsMax);
        c.reset();
        double sum = 0.0;
        int count = 0;
        const int total = static_cast<int>(kSr * 3.0);
        for (int n = 0; n < total; ++n) {
            c.process(0.5 * std::sin(2.0 * M_PI * tone_hz * n / kSr));
            if (n > total - static_cast<int>(kSr)) {
                sum += -c.gain_reduction_db();
                ++count;
            }
        }
        return sum / count;
    };

    constexpr double kCorner = 100.0;
    constexpr double kRatio = 4.0;
    // A one-pole high-pass at `kCorner` passes `f/√(f² + fc²)`; the derived
    // reduction difference is that relative attenuation times the gain
    // computer's slope.
    const double relative_attenuation_db =
        units::linear_to_db(1000.0 / std::hypot(1000.0, kCorner)) -
        units::linear_to_db(60.0 / std::hypot(60.0, kCorner));
    const double derived = relative_attenuation_db * (1.0 - 1.0 / kRatio);

    const double low = reduction_at(60.0, kRatio);
    const double mid = reduction_at(1000.0, kRatio);
    REQUIRE(mid - low >= 4.0);            // the spec's criterion, at a stated ratio
    REQUIRE(mid - low >= 0.95 * derived);  // ...and the mechanism that produces it

    // The ratio dependence, asserted rather than described: at the control's
    // minimum the same high-pass cannot deliver 4 dB, which is why the ratio
    // has to be pinned.
    REQUIRE(relative_attenuation_db * (1.0 - 1.0 / Comp::kRatioMin) < 4.0);

    // Opening the corner right down makes the compressor respond to the low end
    // again — the "does it duck to the kick?" knob doing its job.
    Comp wide = make_compressor();
    wide.set_threshold_db(-20.0);
    wide.set_ratio(kRatio);
    wide.set_knee_db(0.0);
    wide.set_sc_hpf_hz(Comp::kScHpfHzMin);
    wide.set_release_ms(Comp::kReleaseMsMax);
    wide.reset();
    double sum = 0.0;
    int count = 0;
    const int total = static_cast<int>(kSr * 3.0);
    for (int n = 0; n < total; ++n) {
        wide.process(0.5 * std::sin(2.0 * M_PI * 60.0 * n / kSr));
        if (n > total - static_cast<int>(kSr)) {
            sum += -wide.gain_reduction_db();
            ++count;
        }
    }
    REQUIRE(sum / count > low + 2.0);
}

TEST_CASE("the bracket generates even harmonics, which needs an asymmetry",
          "[diode-bridge][transformer]") {
    // SPEC DEFECT. §5 prescribes `sat(u) = u − (a/2)·u·|u|` and describes it as
    // adding a second harmonic. It cannot: `u·|u|` is an ODD function, so that
    // shaper is odd-symmetric and produces odd harmonics only — physically the
    // right answer for a SYMMETRIC magnetic core, and the wrong one for the
    // even-harmonic weight §0 and §5 both attribute to the brackets. Even
    // harmonics in a transformer come from asymmetry, so `kEvenAsymmetry`
    // supplies it and the shipped curve is
    //
    //     sat(u) = u − (a/2)·u·|u| − (a·ε/2)·u²
    //
    // which is the spec's formula exactly at ε = 0.
    //
    // Asserted structurally first: the even part of the curve is EXACTLY the
    // added term, so the amendment is measurable rather than asserted.
    Bracket bracket;
    bracket.prepare(kSr);
    bracket.set_character(1.0);
    const double a = Bracket::kSaturationDepth;
    for (double u = 0.05; u <= 1.5; u += 0.05) {
        const double even_part = 0.5 * (bracket.saturate(u) + bracket.saturate(-u));
        REQUIRE_THAT(even_part, WithinAbs(-0.5 * a * Bracket::kEvenAsymmetry * u * u, 1e-12));
    }
    // Slope exactly 1 at the origin, at every depth — series law 1. Asserted
    // against the closed form of the central difference rather than against 1
    // with a fudged tolerance: `(sat(h) − sat(−h))/2h = 1 − a·h/2` exactly, so
    // the residual is the difference scheme's own truncation and vanishes
    // linearly in h. Asserting `≈ 1` would be asserting that truncation is
    // small, which is a weaker claim about a different thing.
    for (double character : {0.0, 0.35, 1.0}) {
        bracket.set_character(character);
        const double depth = Bracket::kSaturationDepth * character;
        for (double h : {1e-4, 1e-6, 1e-8}) {
            REQUIRE_THAT((bracket.saturate(h) - bracket.saturate(-h)) / (2.0 * h),
                         WithinAbs(1.0 - 0.5 * depth * h, 1e-12));
        }
    }

    // ...and it shows up in the spectrum, even-forward.
    const auto render = [](double character) {
        Bracket b;
        b.prepare(kSr);
        b.set_character(character);
        b.reset();
        std::vector<double> out;
        out.reserve(kTonePeriod * 500);
        for (int n = 0; n < 4800 + kTonePeriod * 500; ++n) {
            const double y = b.process(0.5 * std::sin(2.0 * M_PI * kToneHz * n / kSr));
            if (n >= 4800) out.push_back(y);
        }
        return out;
    };

    const auto coloured = render(1.0);
    const double h1 = harmonic_magnitude(coloured, kToneHz, 1);
    const double h2 = harmonic_magnitude(coloured, kToneHz, 2);
    const double h3 = harmonic_magnitude(coloured, kToneHz, 3);
    REQUIRE(h2 > h3);  // even-forward, the documented transformer character

    // The second harmonic's amplitude is closed-form: the added term is
    // `−(a·ε/2)·u²`, and `sin²θ = (1 − cos2θ)/2`, so `h2 = a·ε·A²/4`.
    const double amplitude = 0.5;
    REQUIRE_THAT(h2 / h1,
                 WithinRel(a * Bracket::kEvenAsymmetry * amplitude / 4.0, 0.06));

    // At zero character the nonlinearity is exactly off: the bracket is a pure
    // band-limit, not a shaper that happens to be shallow.
    const auto clean = render(0.0);
    const double clean_h1 = harmonic_magnitude(clean, kToneHz, 1);
    for (int k : {2, 3, 4, 5})
        REQUIRE(harmonic_magnitude(clean, kToneHz, k) / clean_h1 < 1e-9);
}

TEST_CASE("the bracket's peak gain is exactly one", "[diode-bridge][transformer][gain]") {
    // The `Tpeak = 1.0` the worst-case bound multiplies by, asserted as the
    // construction it claims to be: every stage has magnitude ≤ 1, so no
    // amplitude and no frequency can produce gain.
    Bracket bracket;
    bracket.prepare(kSr);
    for (double character : {0.0, 0.5, 1.0}) {
        bracket.set_character(character);
        // The memoryless stage, over its whole guarded range and both signs.
        for (double u = -4.0; u <= 4.0; u += 0.01) {
            if (std::abs(u) < 1e-9) continue;
            REQUIRE(std::abs(bracket.saturate(u) / u) <= 1.0 + 1e-12);
        }
        // And the whole stage, settled, across the audio band.
        for (double hz : {20.0, 100.0, 1000.0, 10000.0, 20000.0}) {
            bracket.reset();
            double in_peak = 0.0, out_peak = 0.0;
            const int total = static_cast<int>(kSr * 0.3);
            for (int n = 0; n < total; ++n) {
                const double x = 0.5 * std::sin(2.0 * M_PI * hz * n / kSr);
                const double y = bracket.process(x);
                if (n < total / 2) continue;
                in_peak = std::max(in_peak, std::abs(x));
                out_peak = std::max(out_peak, std::abs(y));
            }
            REQUIRE(out_peak / in_peak <= Bracket::kPeakGain);
        }
    }
}

TEST_CASE("the bracket blocks DC that its own saturation generates",
          "[diode-bridge][transformer]") {
    // The even-harmonic term rectifies, so it produces a program-dependent DC
    // offset. Placing the high-pass AFTER the saturator is what keeps that
    // offset out of the output sum and out of the feedback detector, which
    // would otherwise read it as signal and hold gain down through a silent
    // passage.
    Bracket bracket;
    bracket.prepare(kSr);
    bracket.set_character(1.0);
    bracket.reset();

    double mean = 0.0;
    int count = 0;
    const int total = static_cast<int>(kSr * 2.0);
    for (int n = 0; n < total; ++n) {
        const double y = bracket.process(0.8 * std::sin(2.0 * M_PI * 200.0 * n / kSr));
        if (n > total / 2) {
            mean += y;
            ++count;
        }
    }
    REQUIRE_THAT(mean / count, WithinAbs(0.0, 1e-4));
}

TEST_CASE("the winding corner is clamped below Nyquist at base rates",
          "[diode-bridge][transformer]") {
    // Worth stating rather than discovering: the declared 28 kHz corner is
    // ABOVE Nyquist at 44.1 and 48 kHz, so the house TPT filter clamps it and
    // the bracket is effectively full-bandwidth there. The declared value is
    // only realised from 88.2 kHz up. This is a property of the sample rate,
    // not a defect, and it is asserted so a future change to the corner cannot
    // silently become a change to the audio-band response at 48 kHz.
    TptFilter64 winding;
    winding.prepare(kSr);
    winding.set_cutoff(Bracket::kHighCornerHz);
    REQUIRE(winding.cutoff() < Bracket::kHighCornerHz);
    REQUIRE(winding.cutoff() > 0.4 * kSr);

    TptFilter64 high_rate;
    high_rate.prepare(96000.0);
    high_rate.set_cutoff(Bracket::kHighCornerHz);
    REQUIRE_THAT(high_rate.cutoff(), WithinRel(Bracket::kHighCornerHz, 1e-12));
}

TEST_CASE("A10 the module allocates nothing on the audio thread",
          "[diode-bridge][rt-safety]") {
    DiodeBridgeGainT<float> bridge;
    TransformerBracketT<float> bracket;
    DiodeBridgeCompressorT<float> node;
    bridge.prepare(kSr);
    bracket.prepare(kSr);
    node.prepare(kSr);

    require_allocates_no_memory([&] {
        // Ten seconds, with every control moving, because a setter that
        // allocates only on a value change is the failure mode a static
        // configuration would miss.
        for (int n = 0; n < static_cast<int>(kSr * 10.0); ++n) {
            const auto phase = 0.0001f * static_cast<float>(n);
            const float character = 0.5f + 0.5f * std::sin(phase);
            bridge.set_character(character);
            bridge.set_adaa((n % 2) == 0);
            bracket.set_character(character);
            bracket.set_adaa((n % 3) == 0);
            node.set_character(character);
            node.set_threshold_db(-20.0 + 10.0 * std::sin(phase));
            node.set_ratio(2.0 + 8.0 * (0.5 + 0.5 * std::sin(phase)));
            node.set_knee_db(9.0 * (0.5 + 0.5 * std::sin(phase)));
            node.set_attack_ms(1.0 + 20.0 * (0.5 + 0.5 * std::sin(phase)));
            node.set_release_ms(100.0 + 500.0 * (0.5 + 0.5 * std::sin(phase)));
            node.set_makeup_db(12.0 * (0.5 + 0.5 * std::sin(phase)));
            node.set_mix_percent(100.0 * (0.5 + 0.5 * std::sin(phase)));
            node.set_sc_hpf_hz(50.0 + 200.0 * (0.5 + 0.5 * std::sin(phase)));
            node.set_auto_release((n % 4096) < 2048);
            node.set_feedback((n % 8192) < 4096);

            const float x = 0.5f * std::sin(0.05f * static_cast<float>(n));
            (void)bridge.process(x, 1.0);
            (void)bracket.process(x);
            (void)node.process(x);
        }
        bridge.reset();
        bracket.reset();
        node.reset();
    });
}

TEST_CASE("A11 the float and double instantiations agree on the gain law",
          "[diode-bridge][precision]") {
    DiodeBridgeGainT<float> single;
    DiodeBridgeGainT<double> dual;
    single.prepare(kSr);
    dual.prepare(kSr);
    single.set_character(0.35);
    dual.set_character(0.35);
    single.set_adaa(false);
    dual.set_adaa(false);
    single.reset();
    dual.reset();

    for (double x : {0.0, 0.413, 2.981, 9.0}) {
        for (int n = 0; n < 4800; ++n) {
            const double input = 0.25 * std::sin(2.0 * M_PI * 440.0 * n / kSr);
            const double a = static_cast<double>(single.process(static_cast<float>(input), x));
            const double b = dual.process(input, x);
            REQUIRE_THAT(a, WithinAbs(b, 1e-6 * (0.25 + std::abs(b))));
        }
    }

    // The whole node too, to a tolerance the float path can actually hold: the
    // detector is a recursive one-pole, so single precision accumulates rather
    // than merely rounds.
    DiodeBridgeCompressorT<float> node_f;
    DiodeBridgeCompressorT<double> node_d;
    node_f.prepare(kSr);
    node_d.prepare(kSr);
    node_f.set_character(0.5);
    node_d.set_character(0.5);
    node_f.reset();
    node_d.reset();
    for (int n = 0; n < 24000; ++n) {
        const double input = 0.6 * std::sin(2.0 * M_PI * 220.0 * n / kSr);
        REQUIRE_THAT(static_cast<double>(node_f.process(static_cast<float>(input))),
                     WithinAbs(node_d.process(input), 1e-4));
    }
}

TEST_CASE("zero-init is a valid fresh instance", "[diode-bridge][lifecycle]") {
    // POD state, zero-init = fresh (series contract §6). A default-constructed,
    // never-prepared instance must not produce NaN or run away.
    Comp c;
    for (int n = 0; n < 1024; ++n) {
        const double y = c.process(0.5 * std::sin(2.0 * M_PI * kToneHz * n / kSr));
        REQUIRE(std::isfinite(y));
    }

    Bridge bridge;
    Bracket bracket;
    for (int n = 0; n < 1024; ++n) {
        const double x = 0.5 * std::sin(2.0 * M_PI * kToneHz * n / kSr);
        REQUIRE(std::isfinite(bridge.process(x, 2.0)));
        REQUIRE(std::isfinite(bracket.process(x)));
    }
}

TEST_CASE("controls clamp to their declared ranges", "[diode-bridge][lifecycle]") {
    // The catalog table's ranges are the module's contract; a setter that lets
    // a host push past them is a way for an automation curve to reach a state
    // no test covers.
    Comp c;
    c.prepare(kSr);
    c.set_knee_db(0.0);

    c.set_ratio(1000.0);
    c.set_threshold_db(-1000.0);
    // Ratio pinned at the maximum is the limit region, so a 6 dB overshoot of
    // the lowest threshold is 6 dB of reduction exactly.
    REQUIRE_THAT(c.static_curve_db(Comp::kThresholdDbMin + 6.0), WithinAbs(-6.0, 1e-9));

    c.set_makeup_db(1000.0);
    Xorshift32 noise(7u);
    double in_peak = 0.0, out_peak = 0.0;
    c.set_mix_percent(1000.0);
    c.set_sc_hpf_hz(-1000.0);
    c.set_attack_ms(-1.0);
    c.set_release_ms(1e9);
    c.reset();
    for (int n = 0; n < static_cast<int>(kSr); ++n) {
        const double x = 0.01 * noise.next_bipolar<double>();
        in_peak = std::max(in_peak, std::abs(x));
        out_peak = std::max(out_peak, std::abs(c.process(x)));
    }
    REQUIRE(out_peak / in_peak <= Comp::worst_case_gain());
}

TEST_CASE("a NaN sample cannot latch the diode-bridge feedback detector",
          "[diode-bridge][nan-recovery]") {
    for (double sample_rate : {8000.0, 192000.0}) {
        Comp c;
        c.prepare(sample_rate);
        c.set_threshold_db(-30.0);
        c.set_feedback(true);
        c.set_auto_release(true);
        for (int i = 0; i < static_cast<int>(sample_rate * 0.1); ++i) c.process(0.5);

        c.process(std::numeric_limits<double>::quiet_NaN());
        for (int i = 0; i < 64; ++i) {
            REQUIRE(std::isfinite(c.process(0.25)));
            REQUIRE(std::isfinite(c.gain_reduction_db()));
            REQUIRE(std::isfinite(c.control_drive()));
        }
    }
}

TEST_CASE("non-finite diode-bridge controls retain the last valid configuration",
          "[diode-bridge][nan-recovery]") {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    const double invalid[] = {nan, inf, -inf};

    SECTION("the complete compressor retains a non-default configuration") {
        Comp actual;
        Comp reference;
        for (Comp* c : {&actual, &reference}) {
            c->prepare(kSr);
            c->set_threshold_db(-27.0);
            c->set_ratio(7.0);
            c->set_knee_db(4.0);
            c->set_attack_ms(2.5);
            c->set_release_ms(173.0);
            c->set_makeup_db(3.0);
            c->set_character(0.73);
            c->set_mix_percent(61.0);
            c->set_sc_hpf_hz(137.0);
        }

        int sample_index = 0;
        const auto continue_exactly = [&](int count) {
            for (int i = 0; i < count; ++i, ++sample_index) {
                const double input =
                    0.35 * std::sin(2.0 * M_PI * 997.0 * sample_index / kSr);
                REQUIRE(actual.process(input) == reference.process(input));
                REQUIRE(actual.gain_reduction_db() == reference.gain_reduction_db());
            }
        };

        // Invalid automation arrives after every recursive stage is live. A
        // setter that resets detector, feedback, bracket, or follower state can
        // no longer hide behind both instances starting fresh.
        continue_exactly(512);
        for (double value : invalid) {
            actual.set_threshold_db(value);
            actual.set_ratio(value);
            actual.set_knee_db(value);
            actual.set_attack_ms(value);
            actual.set_release_ms(value);
            actual.set_makeup_db(value);
            actual.set_character(value);
            actual.set_mix_percent(value);
            actual.set_sc_hpf_hz(value);
            continue_exactly(256);
        }
    }

    SECTION("the diode bridge gain component retains its character") {
        Bridge actual;
        Bridge reference;
        for (Bridge* bridge : {&actual, &reference}) {
            bridge->prepare(kSr);
            bridge->set_character(0.73);
        }

        int sample_index = 0;
        const auto continue_exactly = [&](int count) {
            for (int i = 0; i < count; ++i, ++sample_index) {
                const double input = 0.7 * std::sin(2.0 * M_PI * 613.0 * sample_index / kSr);
                const double drive = 1.0 + 0.75 * std::sin(2.0 * M_PI * 7.0 * sample_index / kSr);
                REQUIRE(actual.process(input, drive) == reference.process(input, drive));
            }
        };

        continue_exactly(257);
        for (double value : invalid) {
            actual.set_character(value);
            REQUIRE_THAT(actual.drive(), WithinAbs(reference.drive(), 1e-15));
            continue_exactly(127);
        }
    }

    SECTION("the transformer bracket component retains its character") {
        Bracket actual;
        Bracket reference;
        for (Bracket* bracket : {&actual, &reference}) {
            bracket->prepare(kSr);
            bracket->set_character(0.73);
        }

        int sample_index = 0;
        const auto continue_exactly = [&](int count) {
            for (int i = 0; i < count; ++i, ++sample_index) {
                // Deliberately cross the character-dependent clamp so the
                // comparison covers limit_ as well as saturation coefficients.
                const double input = 8.0 * std::sin(2.0 * M_PI * 431.0 * sample_index / kSr);
                REQUIRE(actual.process(input) == reference.process(input));
            }
        };

        continue_exactly(257);
        for (double value : invalid) {
            actual.set_character(value);
            REQUIRE_THAT(actual.saturate(0.8),
                         WithinAbs(reference.saturate(0.8), 1e-15));
            REQUIRE_THAT(actual.saturate(-0.8),
                         WithinAbs(reference.saturate(-0.8), 1e-15));
            continue_exactly(127);
        }
    }
}

TEST_CASE("public diode colour components recover exactly after non-finite audio",
          "[diode-bridge][nan-recovery][rt-safety]") {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();

    for (double invalid : {nan, inf, -inf}) {
        CAPTURE(invalid);

        SECTION("diode bridge gain") {
            Bridge poisoned;
            Bridge fresh;
            for (Bridge* bridge : {&poisoned, &fresh}) {
                bridge->prepare(kSr);
                bridge->set_character(0.73);
            }
            for (int i = 0; i < 257; ++i) {
                const double input = 0.7 * std::sin(2.0 * M_PI * 613.0 * i / kSr);
                (void)poisoned.process(input, 2.0);
            }

            REQUIRE(poisoned.process(invalid, 2.0) == 0.0);
            REQUIRE(poisoned.process(0.5, invalid) == 0.0);
            fresh.reset();
            for (int i = 0; i < 1024; ++i) {
                const double input = 0.7 * std::sin(2.0 * M_PI * 613.0 * i / kSr);
                REQUIRE(poisoned.process(input, 2.0) == fresh.process(input, 2.0));
            }
        }

        SECTION("transformer bracket") {
            Bracket poisoned;
            Bracket fresh;
            for (Bracket* bracket : {&poisoned, &fresh}) {
                bracket->prepare(kSr);
                bracket->set_character(0.73);
            }
            for (int i = 0; i < 257; ++i) {
                (void)poisoned.process(0.8 * std::sin(2.0 * M_PI * 431.0 * i / kSr));
            }

            REQUIRE(poisoned.process(invalid) == 0.0);
            fresh.reset();
            for (int i = 0; i < 1024; ++i) {
                const double input = 0.8 * std::sin(2.0 * M_PI * 431.0 * i / kSr);
                REQUIRE(poisoned.process(input) == fresh.process(input));
            }
        }
    }
}

TEST_CASE("enabling auto release cannot blend against a stale slow follower",
          "[diode-bridge][ballistics]") {
    Comp automatic;
    Comp manual;
    automatic.prepare(kSr);
    manual.prepare(kSr);
    for (Comp* c : {&automatic, &manual}) {
        c->set_feedback(false);
        c->set_threshold_db(-30.0);
        c->set_ratio(4.0);
        c->set_knee_db(0.0);
        c->set_attack_ms(3.0);
        c->set_release_ms(400.0);
        c->set_auto_release(false);
    }

    for (int i = 0; i < static_cast<int>(kSr); ++i) {
        automatic.process(0.5);
        manual.process(0.5);
    }
    REQUIRE_THAT(automatic.gain_reduction_db(), WithinAbs(manual.gain_reduction_db(), 1e-12));

    automatic.set_auto_release(true);
    double maximum_premature_recovery = 0.0;
    for (int i = 0; i < 256; ++i) {
        automatic.process(0.0);
        manual.process(0.0);
        maximum_premature_recovery =
            std::max(maximum_premature_recovery,
                     automatic.gain_reduction_db() - manual.gain_reduction_db());
    }

    // A correctly synchronized slow follower is never below the fast follower
    // during release, so Auto can hold MORE reduction than manual but must not
    // recover prematurely. A frozen-at-zero slow state reverses that ordering.
    REQUIRE(maximum_premature_recovery < 1e-9);
}
