#include "test_signal_nonlin_ambience_support.hpp"

TEST_CASE("Nonlin ambience: the seed selects the realization and nothing else",
          "[signal][nonlin-ambience][determinism]") {
    NonlinAmbience a, b;
    for (auto* engine : {&a, &b}) {
        engine->prepare(kFs, na::kMaxLengthMs);
        engine->set_length_ms(400.0);
    }
    b.set_seed(na::kDefaultSeed ^ 0x1234u);
    a.reset();
    b.reset();

    // Same count and same L1 budget — the seed moves taps, it does not change
    // how many there are or how loud the field is.
    REQUIRE(a.tap_count(0) == b.tap_count(0));
    REQUIRE_THAT(a.tap_norm(0), Catch::Matchers::WithinRel(b.tap_norm(0), 0.05));

    int differing = 0;
    for (int k = 0; k < a.tap_count(0); ++k)
        if (a.tap(0, k).delay != b.tap(0, k).delay) ++differing;
    REQUIRE(differing > a.tap_count(0) / 2);
}

TEST_CASE("Nonlin ambience: the two channels are independent realizations",
          "[signal][nonlin-ambience][stereo]") {
    NonlinAmbience engine;
    engine.prepare(kFs, na::kMaxLengthMs);
    engine.set_length_ms(400.0);
    engine.set_width_pct(100.0);
    engine.reset();

    const Stereo ir = render_impulse(engine, engine.window_samples() + 8000);
    const int start = static_cast<int>(0.1 * engine.window_samples());

    double ll = 0.0, rr = 0.0, lr = 0.0;
    for (std::size_t i = static_cast<std::size_t>(start); i < ir.left.size(); ++i) {
        const double l = ir.left[i], r = ir.right[i];
        ll += l * l;
        rr += r * r;
        lr += l * r;
    }
    const double rho = lr / std::sqrt(ll * rr);
    INFO("inter-channel correlation " << rho);
    // Spec T5's kDecorrThresh. Measured is ~0.01, so this is not a tuned number.
    REQUIRE(std::fabs(rho) <= 0.2);
}

TEST_CASE("Nonlin ambience: width 0 is exactly mono",
          "[signal][nonlin-ambience][stereo]") {
    // The third of the header's shipped-behaviour adjudications: §4.6 describes
    // width as crossfading toward "both channels from seed_L", which would be a
    // rebuild, and §1 classifies width as a continuous parameter that must never
    // trigger one. The mid/side law reaches the same observable — bit-identical
    // channels — without a rebuild.
    NonlinAmbience engine;
    engine.prepare(kFs, na::kMaxLengthMs);
    engine.set_length_ms(400.0);
    engine.set_width_pct(0.0);
    engine.reset();

    const Stereo ir = render_impulse(engine, engine.window_samples() + 4000);
    REQUIRE(ir.left == ir.right);

    double peak = 0.0;
    for (float v : ir.left) peak = std::max(peak, static_cast<double>(std::fabs(v)));
    REQUIRE(peak > 1e-4);  // mono, not silent
}

TEST_CASE("Nonlin ambience: latency is exactly zero and the dry path is a wire",
          "[signal][nonlin-ambience][latency]") {
    REQUIRE(NonlinAmbience::latency_samples() == 0);

    NonlinAmbience engine;
    engine.prepare(kFs, na::kMaxLengthMs);
    engine.set_length_ms(400.0);
    engine.set_mix_pct(0.0);
    engine.reset();

    const Stereo ir = render_impulse(engine, 8000);
    REQUIRE(ir.left[0] == 1.0f);
    REQUIRE(ir.right[0] == 1.0f);

    // Not merely "small": exactly zero. At mix = 0 the dry path is a wire and
    // no wet energy leaks past it at all.
    double leak = 0.0;
    for (std::size_t n = 1; n < ir.left.size(); ++n)
        leak = std::max({leak, std::fabs(static_cast<double>(ir.left[n])),
                         std::fabs(static_cast<double>(ir.right[n]))});
    INFO("largest wet leak at mix = 0: " << leak);
    REQUIRE(leak == 0.0);
}

TEST_CASE("Nonlin ambience: the rendered L1 gain stays under the shipped bound",
          "[signal][nonlin-ambience][gain]") {
    // The registry value is a closed form of the shipped constants, recomputed
    // here rather than restated, and then asserted against the actual response.
    const double bound = na::worst_case_gain();
    REQUIRE_THAT(bound,
                 Catch::Matchers::WithinRel(
                     std::pow(1.0 + 2.0 * na::kDiffusionDefault, na::kNumAllpass) *
                         na::kL1Budget,
                     1e-12));
    REQUIRE_THAT(bound, Catch::Matchers::WithinAbs(23.04, 1e-9));

    // Render length: the window plus the diffuser's own 100 dB ring time, so
    // the sum captures every sample that can contribute to it. Computed from
    // the shipped coefficient rather than picked.
    auto ring_tail = [](const NonlinAmbience& engine) {
        const double repetitions = std::log(1e-5) / std::log(na::kDiffusionDefault);
        return static_cast<int>(std::ceil(
            repetitions * (engine.allpass_length(0) + engine.allpass_length(1))));
    };

    auto check = [&](NonlinProgram program, double length_ms) {
        NonlinAmbience engine;
        engine.prepare(kFs, na::kMaxLengthMs);
        engine.set_program(program);
        engine.set_length_ms(length_ms);
        engine.reset();
        REQUIRE_THAT(engine.worst_case_gain(), Catch::Matchers::WithinRel(bound, 1e-12));

        const Stereo ir =
            render_impulse(engine, engine.window_samples() + ring_tail(engine));
        double l1_left = 0.0, l1_right = 0.0;
        for (std::size_t n = 0; n < ir.left.size(); ++n) {
            l1_left += std::fabs(static_cast<double>(ir.left[n]));
            l1_right += std::fabs(static_cast<double>(ir.right[n]));
        }
        INFO("program " << program_name(program) << " length " << length_ms
                        << " ms: L1 = " << l1_left << " / " << l1_right << ", bound "
                        << bound);
        REQUIRE(l1_left <= bound);
        REQUIRE(l1_right <= bound);
        // The bound is not vacuous either — a bound ten times the truth would
        // be no bound at all.
        REQUIRE(std::max(l1_left, l1_right) > 0.35 * bound);
    };

    for (NonlinProgram program : kAllPrograms)
        for (double length_ms : {50.0, 400.0}) check(program, length_ms);

    // One pass at the parameter maximum, where the tap count is highest. The
    // bound is length-independent by construction (the L1 budget is normalised),
    // so this guards against a length-dependent regression without paying for
    // four full-length renders; N7 exercises the maximum length on every
    // channel and sample rate.
    check(NonlinProgram::ambience, na::kMaxLengthMs);
}

TEST_CASE("Nonlin ambience: the bound tracks diffusion and the converter stage",
          "[signal][nonlin-ambience][gain]") {
    // Defect D4: the DC blocker's L1 gain is exactly 2, so a chain containing
    // one has twice the bound. That is why the default path does not contain
    // one, and why the bound reported with the converter engaged is doubled.
    REQUIRE_THAT(na::worst_case_gain(0.0), Catch::Matchers::WithinRel(na::kL1Budget, 1e-12));
    REQUIRE_THAT(na::worst_case_gain(na::kDiffusionDefault, true),
                 Catch::Matchers::WithinRel(2.0 * na::worst_case_gain(), 1e-12));

    NonlinAmbience engine;
    engine.prepare(kFs, na::kMaxLengthMs);
    engine.set_length_ms(400.0);
    engine.set_diffusion(0.0);
    engine.reset();
    REQUIRE_THAT(engine.worst_case_gain(),
                 Catch::Matchers::WithinRel(na::kL1Budget, 1e-12));

    // With the diffuser bypassed the FIR's L1 gain is exactly the budget, by
    // construction — the taps are the whole response, so the bound is tight.
    const Stereo ir = render_impulse(engine, engine.window_samples() + 8000);
    double l1 = 0.0;
    for (float v : ir.left) l1 += std::fabs(static_cast<double>(v));
    INFO("L1 with diffuser bypassed = " << l1 << ", budget " << na::kL1Budget);
    REQUIRE(l1 <= na::kL1Budget);
    REQUIRE(l1 > 0.75 * na::kL1Budget);
}

TEST_CASE("Nonlin ambience: process and the rebuild-swap path never allocate",
          "[signal][nonlin-ambience][rt]") {
    NonlinAmbience engine;
    engine.prepare(kFs, na::kMaxLengthMs);
    engine.set_length_ms(200.0);
    engine.reset();

    constexpr int kBlock = 256;
    std::vector<float> left(kBlock), right(kBlock);
    const auto stimulus = pink_ish(kBlock, 0xBEEFu);

    {
        pulp::test::RtAllocationProbe probe;
        for (int block = 0; block < 200; ++block) {
            left = stimulus;
            right = stimulus;
            for (int n = 0; n < kBlock; ++n) {
                // Per-sample automation of every continuous parameter, per T8.
                const double u = static_cast<double>(n) / kBlock;
                engine.set_mix_pct(100.0 * u);
                engine.set_tone(2.0 * u - 1.0);
                engine.set_width_pct(100.0 * (1.0 - u));
                engine.set_output_gain_db(6.0 * u - 3.0);
                engine.process_sample(left[static_cast<std::size_t>(n)],
                                      right[static_cast<std::size_t>(n)]);
            }
            // ...and a topology swap partway through, which is the path that
            // regenerates ~1500 taps into the pre-sized back bank.
            if (block == 100) engine.set_program(NonlinProgram::gated);
            if (block == 150) engine.set_length_ms(180.0);
        }
        REQUIRE(probe.allocation_count() == 0);
    }

    // Every topology setter, once each, still inside a probe.
    {
        pulp::test::RtAllocationProbe probe;
        engine.set_program(NonlinProgram::reverse);
        engine.set_length_ms(120.0);
        engine.set_predelay_ms(15.0);
        engine.set_density_pct(85.0);
        engine.set_density_growth(1.0);
        engine.set_gate_hold_pct(50.0);
        engine.set_attack_pct(60.0);
        engine.set_seed(0x1234u);
        engine.set_diffusion(0.4);
        engine.set_converter_amount(0.7);
        engine.reset();
        engine.process(left.data(), right.data(), kBlock);
        REQUIRE(probe.allocation_count() == 0);
    }
}

TEST_CASE("Nonlin ambience: silence after a loud transient flushes to zero",
          "[signal][nonlin-ambience][rt]") {
    NonlinAmbience engine;
    engine.prepare(kFs, na::kMaxLengthMs);
    engine.set_length_ms(400.0);
    engine.reset();

    const int length = static_cast<int>(5.0 * kFs);
    std::vector<float> left(static_cast<std::size_t>(length), 0.0f);
    std::vector<float> right(static_cast<std::size_t>(length), 0.0f);
    left[0] = 4.0f;
    right[0] = 4.0f;
    engine.process(left.data(), right.data(), length);

    int denormals = 0, first_denormal = -1;
    for (int n = 0; n < length; ++n) {
        const bool bad = pulp::signal::is_denormal(left[static_cast<std::size_t>(n)]) ||
                         pulp::signal::is_denormal(right[static_cast<std::size_t>(n)]);
        if (bad) {
            ++denormals;
            if (first_denormal < 0) first_denormal = n;
        }
    }
    INFO(denormals << " denormal samples, first at " << first_denormal);
    REQUIRE(denormals == 0);
    // Every recursive state — both allpasses and all sixteen segment one-poles —
    // snaps through the denormal threshold, so the tail reaches exact zero
    // rather than grinding on subnormals forever.
    REQUIRE(left[static_cast<std::size_t>(length - 1)] == 0.0f);
    REQUIRE(right[static_cast<std::size_t>(length - 1)] == 0.0f);
}

TEST_CASE("Nonlin ambience: a program change crossfades instead of clicking",
          "[signal][nonlin-ambience][swap]") {
    NonlinAmbience engine;
    engine.prepare(kFs, na::kMaxLengthMs);
    engine.set_length_ms(300.0);
    engine.set_program(NonlinProgram::gated);
    engine.reset();

    const int length = static_cast<int>(1.0 * kFs);
    auto stimulus = pink_ish(length, 0x5EEDu);
    std::vector<float> left = stimulus, right = stimulus;

    const int swap_at = length / 2;
    engine.process(left.data(), right.data(), swap_at);
    REQUIRE_FALSE(engine.swap_in_progress());
    engine.set_program(NonlinProgram::reverse);
    REQUIRE(engine.swap_in_progress());
    engine.process(left.data() + swap_at, right.data() + swap_at, length - swap_at);
    REQUIRE_FALSE(engine.swap_in_progress());

    // No sample-to-sample step larger than the largest one the same signal
    // produces while NOT swapping. A hard bank switch shows up here as a
    // discontinuity of the order of the signal itself.
    auto largest_step = [&](int from, int to) {
        double worst = 0.0;
        for (int n = from + 1; n < to; ++n)
            worst = std::max(worst, std::fabs(static_cast<double>(
                                        left[static_cast<std::size_t>(n)] -
                                        left[static_cast<std::size_t>(n - 1)])));
        return worst;
    };
    const double quiescent = largest_step(1000, swap_at - 1000);
    const int fade = static_cast<int>(na::kSwapFadeMs * kFs / 1000.0);
    const double during = largest_step(swap_at - 4, swap_at + fade + 4);
    INFO("largest step while quiescent " << quiescent << ", across the swap " << during);
    REQUIRE(during < quiescent * 1.5);
}

TEST_CASE("Nonlin ambience: the field darkens over time, and tone steers it",
          "[signal][nonlin-ambience][tilt]") {
    // §4.5's spectral evolution. Measured as the high-frequency energy fraction
    // early versus late in the response, which is what "loses highs over time"
    // means without needing a full spectrum.
    auto hf_fraction = [](const std::vector<float>& x, int start, int count) {
        // One-zero difference as the HF probe, one-pole sum as the LF probe.
        double hf = 0.0, total = 0.0;
        for (int n = start + 1; n < start + count; ++n) {
            const double d = static_cast<double>(x[static_cast<std::size_t>(n)]) -
                             static_cast<double>(x[static_cast<std::size_t>(n - 1)]);
            hf += d * d;
            total += static_cast<double>(x[static_cast<std::size_t>(n)]) *
                     static_cast<double>(x[static_cast<std::size_t>(n)]);
        }
        return hf / std::max(total, 1e-30);
    };

    NonlinAmbience engine;
    engine.prepare(kFs, na::kMaxLengthMs);
    engine.set_program(NonlinProgram::gated);  // flat envelope isolates the colour
    engine.set_length_ms(800.0);
    engine.set_diffusion(0.0);
    engine.reset();

    const int window = engine.window_samples();
    const int lag = bypassed_diffuser_delay(engine);
    const int span = static_cast<int>(0.05 * kFs);
    const Stereo ir = render_impulse(engine, window + lag + 4000);

    const double early = hf_fraction(ir.left, lag + static_cast<int>(0.05 * window), span);
    const double late = hf_fraction(ir.left, lag + static_cast<int>(0.60 * window), span);
    INFO("HF fraction early " << early << " late " << late);
    REQUIRE(late < early);

    // tone > 0 keeps highs later; the late field must brighten relative to the
    // neutral setting, and the neutral setting relative to tone < 0.
    auto late_hf_at_tone = [&](double tone) {
        NonlinAmbience e;
        e.prepare(kFs, na::kMaxLengthMs);
        e.set_program(NonlinProgram::gated);
        e.set_length_ms(800.0);
        e.set_diffusion(0.0);
        e.set_tone(tone);
        e.reset();
        const Stereo r = render_impulse(e, e.window_samples() + lag + 4000);
        return hf_fraction(r.left, lag + static_cast<int>(0.60 * e.window_samples()), span);
    };
    const double dark = late_hf_at_tone(-1.0);
    const double bright = late_hf_at_tone(1.0);
    INFO("late HF fraction: dark " << dark << " neutral " << late << " bright " << bright);
    REQUIRE(bright > late);
    REQUIRE(late > dark);
}

TEST_CASE("Nonlin ambience: reverse brightens into its swell",
          "[signal][nonlin-ambience][tilt]") {
    // §4.5's program-specific flag: the Reverse program maps segments backwards
    // so the field gets brighter as the swell approaches its peak, which is the
    // opposite of every other program and of every real room.
    NonlinAmbience engine;
    engine.prepare(kFs, na::kMaxLengthMs);
    engine.set_program(NonlinProgram::reverse);
    engine.set_length_ms(800.0);
    engine.reset();

    // The reversal is visible directly in the tap table: early taps carry high
    // segment indices (the dark end) and late taps carry low ones.
    const int first = engine.tap(0, 0).segment;
    const int last = engine.tap(0, engine.tap_count(0) - 1).segment;
    INFO("first tap segment " << first << ", last tap segment " << last);
    REQUIRE(first == na::kSegments - 1);
    REQUIRE(last == 0);

    NonlinAmbience forward;
    forward.prepare(kFs, na::kMaxLengthMs);
    forward.set_program(NonlinProgram::ambience);
    forward.set_length_ms(800.0);
    forward.reset();
    REQUIRE(forward.tap(0, 0).segment == 0);
    REQUIRE(forward.tap(0, forward.tap_count(0) - 1).segment == na::kSegments - 1);
}

TEST_CASE("Nonlin ambience: the converter stage is off by default and quantizes when on",
          "[signal][nonlin-ambience][converter]") {
    auto render = [](double amount) {
        NonlinAmbience engine;
        engine.prepare(kFs, na::kMaxLengthMs);
        engine.set_length_ms(200.0);
        engine.set_converter_amount(amount);
        engine.reset();
        return render_impulse(engine, engine.window_samples() + 8000);
    };

    const Stereo off = render(0.0);
    const Stereo on = render(1.0);

    // Off is the default, and off means bit-for-bit untouched.
    NonlinAmbience defaulted;
    defaulted.prepare(kFs, na::kMaxLengthMs);
    defaulted.set_length_ms(200.0);
    defaulted.reset();
    const Stereo baseline =
        render_impulse(defaulted, defaulted.window_samples() + 8000);
    REQUIRE(baseline.left == off.left);

    // On is audibly different, and the difference is in the direction the stage
    // says: the DC blocker and the low corner remove sub-`kConverterFcLo`
    // energy, and the quantizer adds a noise floor.
    double difference = 0.0, reference = 0.0;
    for (std::size_t n = 0; n < off.left.size(); ++n) {
        const double d = static_cast<double>(on.left[n]) - off.left[n];
        difference += d * d;
        reference += static_cast<double>(off.left[n]) * off.left[n];
    }
    INFO("converter difference " << 10.0 * std::log10(difference / reference) << " dB");
    REQUIRE(difference > 0.0);

    // The word length follows the shipped formula, so the quantizer's step is a
    // computed quantity rather than an implied one.
    const int bits = na::kConverterBitsMax -
                     static_cast<int>(std::lround(na::kConverterBitSweep * 1.0));
    REQUIRE(bits == 12);
}

TEST_CASE("Nonlin ambience: the five re-scoped spec criteria, proven with numbers",
          "[signal][nonlin-ambience][spec-defects]") {
    // Each block below re-derives, from the shipped constants, why the criterion
    // as written cannot hold. If a shipped constant changes so that it CAN hold,
    // these assertions fail and the re-scoping above must be revisited — which
    // is the point of asserting the arithmetic instead of writing it in a
    // comment.

    SECTION("D1: the segment tilt alone moves a broadband RMS envelope by 3.19 dB") {
        const double bright = onepole_noise_power(spec_segment_pole(0, 0.0, na::kFcDark));
        const double dark = onepole_noise_power(
            spec_segment_pole(na::kSegments - 1, 0.0, na::kFcDark));
        const double tilt_db = 10.0 * std::log10(bright / dark);
        INFO("tilt across the window = " << tilt_db << " dB");
        REQUIRE(tilt_db > 3.0);
        // ...which is more than T1's entire ±1.0 dB Gated flatness budget, and
        // the Gated body covers most of it.
        const double body_fraction = na::kGateHold;
        REQUIRE(tilt_db * body_fraction > 2.0);
    }

    SECTION("D2: the mandated diffuser rings far longer than 2w allows") {
        NonlinAmbience engine;
        engine.prepare(kFs, na::kMaxLengthMs);
        engine.set_length_ms(400.0);
        engine.reset();

        const double repetitions = std::log(1e-3) / std::log(na::kDiffusionDefault);
        const int ring_60db = static_cast<int>(std::ceil(
            repetitions * std::max(engine.allpass_length(0), engine.allpass_length(1))));
        const int allowed =
            static_cast<int>(2.0 * na::kGateFall * engine.window_samples());
        INFO("allpass 60 dB time " << ring_60db << " samples, T1 allows " << allowed);
        REQUIRE(ring_60db > allowed);
        // The window length at which T1's criterion would become achievable —
        // reported so the re-scoping is a bounded statement, not a shrug.
        const double achievable_ms =
            ring_60db * 1000.0 / kFs / (2.0 * na::kGateFall);
        INFO("T1's criterion needs length_ms >= " << achievable_ms);
        REQUIRE(achievable_ms > 400.0);
    }

    SECTION("D3: normalized echo density cannot reach 0.9 at any shipped density") {
        // NED counts samples above the local RMS. A sparse cloud's only such
        // samples are its pulses, so NED <= (Nd/fs)/erfc(1/sqrt2).
        const double ceiling = na::kNdMax / kFs / kErfcHalfRoot2;
        INFO("NED ceiling at kNdMax = " << ceiling << "; T2 demands >= 0.9");
        REQUIRE(ceiling < 0.9);
        // The density that WOULD reach 1, for the record: 0.3173·fs.
        const double required = kErfcHalfRoot2 * kFs;
        INFO("NED = 1 needs " << required << " pulses/s, " << required / na::kNdMax
                              << "x the shipped maximum");
        REQUIRE(required > 3.0 * na::kNdMax);
    }

    SECTION("D4: a DC blocker in the path would double the shipped bound") {
        REQUIRE_THAT(na::kDcBlockerL1Gain, Catch::Matchers::WithinAbs(2.0, 1e-12));
        REQUIRE_THAT(na::worst_case_gain(na::kDiffusionDefault, true),
                     Catch::Matchers::WithinAbs(46.08, 1e-9));
    }

    SECTION("D5: the spec's gain law would tilt the envelope by 7 dB") {
        // Without the sqrt(Td) weight, a window's RMS is E(τ)·sqrt(Nd(τ)), so
        // the measured envelope carries half the density ratio in dB.
        const double early = spec_density(0.0, na::kDensityRefPct, na::kGammaDefault);
        const double late = spec_density(1.0, na::kDensityRefPct, na::kGammaDefault);
        const double tilt_db = 10.0 * std::log10(late / early);
        INFO("uncompensated envelope tilt across the window = " << tilt_db << " dB");
        REQUIRE(tilt_db > 6.0);
        // And across the Gated body specifically, which T1 wants flat to ±1 dB.
        const double body =
            10.0 * std::log10(spec_density(na::kGateHold, na::kDensityRefPct,
                                           na::kGammaDefault) /
                              early);
        INFO("across the gated body = " << body << " dB against a ±1.0 dB budget");
        REQUIRE(body > 4.0);
    }
}

TEST_CASE("Nonlin ambience: prepare and reset leave a usable engine",
          "[signal][nonlin-ambience][lifecycle]") {
    NonlinAmbience engine;

    // Parameter calls before prepare must not crash or leave state that a later
    // prepare cannot repair.
    engine.set_program(NonlinProgram::gated);
    engine.set_length_ms(500.0);
    engine.prepare(kFs, na::kMaxLengthMs);
    engine.reset();
    REQUIRE(engine.tap_count(0) > 0);
    REQUIRE(engine.program() == NonlinProgram::gated);
    REQUIRE_THAT(engine.length_ms(), Catch::Matchers::WithinAbs(500.0, 1e-9));

    // Re-preparing at a different rate rebuilds coherently.
    engine.prepare(96000.0, na::kMaxLengthMs);
    engine.reset();
    REQUIRE_THAT(static_cast<double>(engine.window_samples()),
                 Catch::Matchers::WithinRel(0.5 * 96000.0, 1e-3));
    REQUIRE(engine.tap_count(0) > 0);

    // Parameters are clamped to their documented ranges rather than trusted.
    engine.set_length_ms(1e9);
    REQUIRE(engine.length_ms() <= na::kMaxLengthMs);
    engine.set_length_ms(-1e9);
    REQUIRE_THAT(engine.length_ms(), Catch::Matchers::WithinAbs(na::kMinLengthMs, 1e-9));
    engine.set_tone(50.0);
    REQUIRE_THAT(engine.tone(), Catch::Matchers::WithinAbs(1.0, 1e-9));

    NonlinAmbience minimum_density;
    minimum_density.prepare(kFs, na::kMaxLengthMs);
    minimum_density.set_density_pct(na::kMinDensityPct);
    minimum_density.reset();
    NonlinAmbience below_minimum_density;
    below_minimum_density.prepare(kFs, na::kMaxLengthMs);
    below_minimum_density.set_density_pct(-1e9);
    below_minimum_density.reset();
    REQUIRE(below_minimum_density.tap_count(0) == minimum_density.tap_count(0));
}

TEST_CASE("Nonlin ambience rejects non-finite frames and batches topology rebuilds",
          "[signal][nonlin-ambience][nan-recovery][rt]") {
    NonlinAmbience poisoned;
    NonlinAmbience fresh;
    poisoned.prepare(kFs, na::kMaxLengthMs);
    fresh.prepare(kFs, na::kMaxLengthMs);

    const auto before = poisoned.topology_rebuild_count();
    poisoned.set_topology(NonlinProgram::reverse, 600.0, 20.0, 80.0, 1.0, 60.0,
                          70.0);
    REQUIRE(poisoned.topology_rebuild_count() == before + 1);
    const auto unchanged = poisoned.topology_rebuild_count();
    poisoned.set_topology(NonlinProgram::reverse, 600.0, 20.0, 80.0, 1.0, 60.0,
                          70.0);
    REQUIRE(poisoned.topology_rebuild_count() == unchanged);

    // Reconfigure the reference once through the same atomic path.
    fresh.set_topology(NonlinProgram::reverse, 600.0, 20.0, 80.0, 1.0, 60.0, 70.0);
    float left = std::numeric_limits<float>::quiet_NaN();
    float right = std::numeric_limits<float>::infinity();
    {
        pulp::test::RtAllocationProbe probe;
        poisoned.process_sample(left, right);
        REQUIRE(probe.allocation_count() == 0);
    }
    REQUIRE(left == 0.0f);
    REQUIRE(right == 0.0f);

    auto stimulus = pink_ish(2048, 0xA11CEu);
    auto a_left = stimulus, a_right = stimulus;
    auto b_left = stimulus, b_right = stimulus;
    poisoned.process(a_left.data(), a_right.data(), static_cast<int>(a_left.size()));
    fresh.process(b_left.data(), b_right.data(), static_cast<int>(b_left.size()));
    REQUIRE(a_left == b_left);
    REQUIRE(a_right == b_right);
}

TEST_CASE("Nonlin ambience stages automated topology with fixed per-sample work",
          "[signal][nonlin-ambience][rt][topology]") {
    NonlinAmbience engine;
    engine.prepare(kFs, na::kMaxLengthMs);
    const auto before = engine.topology_rebuild_count();
    engine.request_topology(NonlinProgram::reverse, na::kMaxLengthMs, 20.0, 100.0,
                            2.0, 60.0, 70.0);

    float left = 0.0f, right = 0.0f;
    int frames = 0;
    while (engine.topology_rebuild_count() == before && frames++ < 20000) {
        engine.process_sample(left, right);
        REQUIRE(engine.topology_work_units_last_sample() <=
                NonlinAmbience::kTopologyWorkPerSample);
    }
    REQUIRE(engine.topology_rebuild_count() == before + 1);
    REQUIRE(frames > 1);  // regression guard: never rebuild the whole bank at once

    NonlinAmbience immediate;
    immediate.prepare(kFs, na::kMaxLengthMs);
    immediate.set_topology(NonlinProgram::reverse, na::kMaxLengthMs, 20.0, 100.0,
                           2.0, 60.0, 70.0);
    while (engine.swap_in_progress() || immediate.swap_in_progress()) {
        engine.process_sample(left, right);
        float ref_left = 0.0f, ref_right = 0.0f;
        immediate.process_sample(ref_left, ref_right);
    }
    for (int channel = 0; channel < 2; ++channel) {
        REQUIRE(engine.tap_count(channel) == immediate.tap_count(channel));
        REQUIRE(engine.tap_norm(channel) == immediate.tap_norm(channel));
        for (int tap = 0; tap < engine.tap_count(channel); ++tap) {
            REQUIRE(engine.tap(channel, tap).delay == immediate.tap(channel, tap).delay);
            REQUIRE(engine.tap(channel, tap).gain == immediate.tap(channel, tap).gain);
            REQUIRE(engine.tap(channel, tap).segment == immediate.tap(channel, tap).segment);
        }
    }
}

TEST_CASE("Nonlin ambience: the engine works at every supported sample rate",
          "[signal][nonlin-ambience][lifecycle]") {
    for (double rate : {44100.0, 48000.0, 88200.0, 96000.0, 192000.0}) {
        NonlinAmbience engine;
        engine.prepare(rate, na::kMaxLengthMs);
        engine.set_program(NonlinProgram::ambience);
        engine.set_length_ms(300.0);
        engine.reset();

        INFO("sample rate " << rate);
        // The envelope is scale invariant (series law 7): the tap COUNT depends
        // only on time and density, never on the sample rate. Ambience rather
        // than a gated program, so the expectation is the plain density
        // integral with no truncation term — the gated truncation is asserted
        // on its own in N7.
        REQUIRE_THAT(static_cast<double>(engine.tap_count(0)),
                     Catch::Matchers::WithinRel(
                         0.3 * (na::kNdMin + (na::kNdMax - na::kNdMin) /
                                                 (na::kGammaDefault + 1.0)),
                         0.02));

        const int length = engine.window_samples() + 8000;
        std::vector<float> left(static_cast<std::size_t>(length), 0.0f);
        std::vector<float> right(static_cast<std::size_t>(length), 0.0f);
        left[0] = 1.0f;
        right[0] = 1.0f;
        engine.process(left.data(), right.data(), length);
        for (float v : left) REQUIRE(std::isfinite(v));

        // The allpass delays land on primes at every rate, which is what keeps
        // the two combs from aligning.
        for (int i = 0; i < na::kNumAllpass; ++i) {
            const int m = engine.allpass_length(i);
            for (int d = 2; d * d <= m; ++d) REQUIRE(m % d != 0);
        }
    }
}
