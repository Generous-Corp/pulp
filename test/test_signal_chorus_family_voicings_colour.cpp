#include "test_signal_chorus_family_support.hpp"

TEST_CASE("chorus recovers exactly after non-finite audio", "[signal][chorus][nonfinite]") {
    for (double bad : {NAN, INFINITY, -INFINITY}) {
        Chorus a, b;
        for (auto* c : {&a, &b}) {
            c->prepare(kSr); c->set_rate_hz(1.7); c->set_depth(0.63);
            c->set_mix(0.71); c->set_stereo_width(0.42); c->reset();
        }
        double al = bad, ar = 0.2;
        a.process(&al, &ar, 1);
        REQUIRE(al == 0.0); REQUIRE(ar == 0.0);
        b.reset();
        for (int i = 0; i < 64; ++i) {
            al = ar = 0.2; double bl = 0.2, br = 0.2;
            a.process(&al, &ar, 1); b.process(&bl, &br, 1);
            REQUIRE(al == bl); REQUIRE(ar == br);
        }
    }
}

TEST_CASE("chorus delay range matches the shipped calibration", "[signal][chorus][chorus-family]") {
    struct Case {
        Voicing voicing;
        JunoMode mode;
        int voice;
        bool right_channel;
    };
    const Case cases[] = {
        {Voicing::ce2, JunoMode::mode_I, 0, false},
        {Voicing::juno_ensemble, JunoMode::mode_I, 0, false},
        {Voicing::juno_ensemble, JunoMode::mode_I, 1, true},
        {Voicing::juno_ensemble, JunoMode::mode_II, 0, false},
        {Voicing::juno_ensemble, JunoMode::mode_I_plus_II, 0, false},
        {Voicing::dimension_d, JunoMode::mode_I, 0, false},
        {Voicing::dimension_d, JunoMode::mode_I, 1, true},
        {Voicing::tri_chorus, JunoMode::mode_I, 0, false},
        {Voicing::tri_chorus, JunoMode::mode_I, 2, true},
    };

    constexpr long long kClickPeriod = 2400;  // 50 ms — the spec's recipe
    constexpr std::size_t kSamples = 20 * static_cast<std::size_t>(kSr);

    for (const auto& c : cases) {
        const Window w = shipped_window(c.voicing, c.mode);
        const double lo_ms = w.center_ms - w.depth_ms;
        const double hi_ms = w.center_ms + w.depth_ms;

        std::vector<double> in(kSamples, 0.0);
        for (std::size_t i = 0; i < kSamples; i += static_cast<std::size_t>(kClickPeriod))
            in[i] = 1.0;

        Config cfg;
        cfg.voicing = c.voicing;
        cfg.mode = c.mode;
        cfg.depth = 1.0;
        cfg.mix = 1.0;
        cfg.width = 0.0;
        const auto out = render(cfg, in, in);
        const auto& channel = c.right_channel ? out.right : out.left;

        // Search window: the shipped range plus a few samples of slack, never
        // reaching back to the dry impulse at lag 0.
        const double lo_samples = lo_ms * kSr * 0.001 - 8.0;
        const double hi_samples = hi_ms * kSr * 0.001 + 8.0;
        REQUIRE(lo_samples > 4.0);
        const auto tracked = track_clicks(channel, kClickPeriod, lo_samples, hi_samples);
        REQUIRE(tracked.delay_samples.size() > 100);

        std::vector<double> measured_ms;
        std::vector<double> reference_m;
        measured_ms.reserve(tracked.delay_samples.size());
        reference_m.reserve(tracked.delay_samples.size());
        for (std::size_t j = 0; j < tracked.delay_samples.size(); ++j) {
            measured_ms.push_back(tracked.delay_samples[j] * 1000.0 / kSr);
            // The echo carries the delay in force when it ARRIVED, so the
            // reference LFO is evaluated at the arrival index.
            reference_m.push_back(reference_modulation(c.voicing, c.mode, c.voice,
                                                       tracked.arrival_index[j] + 1, w.rate_hz));
        }

        const Fit fit = fit_modulation(measured_ms, reference_m);
        const double measured_lo = fit.center - std::abs(fit.depth);
        const double measured_hi = fit.center + std::abs(fit.depth);

        INFO(voicing_name(c.voicing) << " voice " << c.voice << ": fit centre " << fit.center
                                     << " ms (table " << w.center_ms << "), depth "
                                     << std::abs(fit.depth) << " ms (table " << w.depth_ms
                                     << "), residual " << fit.residual_rms << " ms");

        // A wrong SHAPE would still fit two parameters; the residual is what
        // makes this a test of the modulation and not just of its extremes.
        REQUIRE(fit.residual_rms < 0.02 * w.depth_ms + 0.01);
        REQUIRE_THAT(measured_lo, WithinRel(lo_ms, 0.02));
        REQUIRE_THAT(measured_hi, WithinRel(hi_ms, 0.02));

        // The accessor is now calibrated against the audio path, so tests 2–5
        // may rely on it.
        const auto trace = delay_trace(cfg, c.voice, 4096);
        for (std::size_t i = 0; i < trace.size(); ++i) {
            const double expected =
                w.center_ms + w.depth_ms * reference_modulation(c.voicing, c.mode, c.voice,
                                                                static_cast<long long>(i) + 1,
                                                                w.rate_hz);
            REQUIRE_THAT(trace[i], WithinAbs(expected, 1e-9));
        }
    }
}

TEST_CASE("chorus taps clear the interpolation guard band", "[signal][chorus][chorus-family]") {
    // The guard analysis in the header, asserted rather than asserted-in-prose.
    const std::pair<Voicing, JunoMode> configs[] = {
        {Voicing::ce2, JunoMode::mode_I},
        {Voicing::juno_ensemble, JunoMode::mode_I},
        {Voicing::juno_ensemble, JunoMode::mode_II},
        {Voicing::juno_ensemble, JunoMode::mode_I_plus_II},
        {Voicing::dimension_d, JunoMode::mode_I},
        {Voicing::tri_chorus, JunoMode::mode_I},
    };
    for (const auto& [voicing, mode] : configs) {
        const Window w = shipped_window(voicing, mode);
        const double min_samples = (w.center_ms - w.depth_ms) * kSr * 0.001;
        INFO(voicing_name(voicing) << " minimum instantaneous delay " << min_samples
                                   << " samples against a " << Chorus::kGuardSamples
                                   << "-sample guard");
        REQUIRE(min_samples > static_cast<double>(Chorus::kGuardSamples));
        // With the BBD colour stage engaged the line keeps `kBbdColorGuardMs`
        // and the stage carries the rest, so the guard holds there too.
        REQUIRE(Chorus::kBbdColorGuardMs * kSr * 0.001 > static_cast<double>(Chorus::kGuardSamples));
    }
}

TEST_CASE("chorus LFO rate is exact through the tap", "[signal][chorus][chorus-family]") {
    struct Case {
        Voicing voicing;
        JunoMode mode;
    };
    const Case cases[] = {
        {Voicing::ce2, JunoMode::mode_I},
        {Voicing::juno_ensemble, JunoMode::mode_I},
        {Voicing::juno_ensemble, JunoMode::mode_II},
        {Voicing::dimension_d, JunoMode::mode_I},
        {Voicing::tri_chorus, JunoMode::mode_I},
    };
    constexpr std::size_t kSamples = 100 * static_cast<std::size_t>(kSr);

    for (const auto& c : cases) {
        const Window w = shipped_window(c.voicing, c.mode);
        Config cfg;
        cfg.voicing = c.voicing;
        cfg.mode = c.mode;
        const auto trace = delay_trace(cfg, 0, kSamples);

        double first = -1.0;
        double last = -1.0;
        long long cycles = 0;
        for (std::size_t i = 1; i < trace.size(); ++i) {
            const double a = trace[i - 1] - w.center_ms;
            const double b = trace[i] - w.center_ms;
            if (!(a <= 0.0 && b > 0.0)) continue;  // upward crossing of centre
            const double frac = b != a ? -a / (b - a) : 0.0;
            const double t = static_cast<double>(i - 1) + frac;
            if (first < 0.0) {
                first = t;
            } else {
                last = t;
                ++cycles;
            }
        }
        REQUIRE(cycles > 10);
        const double measured_hz = static_cast<double>(cycles) * kSr / (last - first);
        INFO(voicing_name(c.voicing) << " mode " << static_cast<int>(c.mode) << ": measured "
                                     << measured_hz << " Hz against " << w.rate_hz << " Hz over "
                                     << cycles << " cycles");
        REQUIRE_THAT(measured_hz, WithinRel(w.rate_hz, 1e-4));
    }
}

TEST_CASE("chorus inverted-phase pairs are exact inversions", "[signal][chorus][chorus-family]") {
    // Juno and Dimension D: L and R modulators half a cycle apart. For an
    // odd-symmetric shape that is an exact inversion, so the two delay traces
    // sum to twice the centre at EVERY sample — a stronger statement than the
    // spec's "near-zero correlation at zero lag", and it also holds for the
    // Dimension D's trapezoid, whose clamp is odd and therefore inversion-safe.
    const std::pair<Voicing, JunoMode> configs[] = {
        {Voicing::juno_ensemble, JunoMode::mode_I},
        {Voicing::juno_ensemble, JunoMode::mode_II},
        {Voicing::juno_ensemble, JunoMode::mode_I_plus_II},
        {Voicing::dimension_d, JunoMode::mode_I},
    };
    for (const auto& [voicing, mode] : configs) {
        const Window w = shipped_window(voicing, mode);
        Config cfg;
        cfg.voicing = voicing;
        cfg.mode = mode;
        constexpr std::size_t kSamples = 300000;
        const auto left = delay_trace(cfg, 0, kSamples);
        const auto right = delay_trace(cfg, 1, kSamples);

        double sxy = 0.0;
        double sxx = 0.0;
        double syy = 0.0;
        for (std::size_t i = 0; i < kSamples; ++i) {
            REQUIRE_THAT(left[i] + right[i], WithinAbs(2.0 * w.center_ms, 1e-9));
            const double a = left[i] - w.center_ms;
            const double b = right[i] - w.center_ms;
            sxy += a * b;
            sxx += a * a;
            syy += b * b;
        }
        const double correlation = sxy / std::sqrt(sxx * syy);
        INFO(voicing_name(voicing) << " L/R zero-lag correlation " << correlation);
        REQUIRE(correlation < -0.9999);

        // ... and the spec's other half: peak correlation at half-period lag.
        const auto half_period = static_cast<std::size_t>(0.5 * kSr / w.rate_hz);
        double lagged = 0.0;
        double lag_xx = 0.0;
        double lag_yy = 0.0;
        for (std::size_t i = 0; i + half_period < kSamples; ++i) {
            const double a = left[i] - w.center_ms;
            const double b = right[i + half_period] - w.center_ms;
            lagged += a * b;
            lag_xx += a * a;
            lag_yy += b * b;
        }
        if (voicing == Voicing::juno_ensemble && mode == JunoMode::mode_I_plus_II) continue;
        REQUIRE(lagged / std::sqrt(lag_xx * lag_yy) > 0.999);
    }
}

TEST_CASE("chorus tri-voice modulators sit 120 degrees apart", "[signal][chorus][chorus-family]") {
    const Window w = shipped_window(Voicing::tri_chorus, JunoMode::mode_I);
    // A whole number of LFO periods, so the coherent bin sees no leakage from
    // the trace's large DC term (the centre delay).
    const auto period = static_cast<std::size_t>(std::llround(kSr / w.rate_hz));
    REQUIRE_THAT(static_cast<double>(period), WithinAbs(kSr / w.rate_hz, 1e-9));
    const std::size_t samples = period * 4;

    Config cfg;
    cfg.voicing = Voicing::tri_chorus;
    std::array<double, 3> phase_deg{};
    for (int k = 0; k < 3; ++k) {
        const auto trace = delay_trace(cfg, k, samples);
        const auto bin = coherent_bin(trace, 0, samples, w.rate_hz);
        phase_deg[static_cast<std::size_t>(k)] = std::arg(bin) * 180.0 / kPi;
    }

    auto wrapped = [](double deg) {
        while (deg > 180.0) deg -= 360.0;
        while (deg < -180.0) deg += 360.0;
        return deg;
    };
    const double d01 = wrapped(phase_deg[1] - phase_deg[0]);
    const double d12 = wrapped(phase_deg[2] - phase_deg[1]);
    const double d20 = wrapped(phase_deg[0] - phase_deg[2]);
    INFO("tri_chorus pairwise phase: 0->1 " << d01 << " deg, 1->2 " << d12 << " deg, 2->0 " << d20
                                            << " deg");
    REQUIRE_THAT(std::abs(d01), WithinAbs(120.0, 1.0));
    REQUIRE_THAT(std::abs(d12), WithinAbs(120.0, 1.0));
    REQUIRE_THAT(std::abs(d20), WithinAbs(120.0, 1.0));
    // Same sign for all three: the voices step around the circle in one
    // direction rather than two of them collapsing onto each other.
    REQUIRE(d01 * d12 > 0.0);
    REQUIRE(d12 * d20 > 0.0);
}

TEST_CASE("chorus juno I+II combines two triangles", "[signal][chorus][chorus-family]") {
    const auto spec = Chorus::juno_spec(JunoMode::mode_I_plus_II);
    const double beat_period_s = 1.0 / std::abs(spec.rate_b_hz - spec.rate_a_hz);
    INFO("beat period " << beat_period_s << " s");
    REQUIRE_THAT(beat_period_s, WithinRel(2.857142857, 1e-6));

    const auto samples = static_cast<std::size_t>(std::ceil(beat_period_s * kSr));
    Config cfg;
    cfg.voicing = Voicing::juno_ensemble;
    cfg.mode = JunoMode::mode_I_plus_II;
    const auto trace = delay_trace(cfg, 0, samples);

    double worst_relative = 0.0;
    double largest_combined = 0.0;
    for (int j = 0; j < 1000; ++j) {
        const auto i = static_cast<std::size_t>(
            static_cast<double>(j) * static_cast<double>(samples - 1) / 999.0);
        const double n = static_cast<double>(i) + 1.0;
        const double a = ref_triangle(n * spec.rate_a_hz / kSr);
        const double b = ref_triangle(n * spec.rate_b_hz / kSr);
        largest_combined = std::max(largest_combined, std::abs(0.5 * (a + b)));
        const double expected =
            spec.center_ms + spec.depth_ms * std::clamp(0.5 * (a + b), -1.0, 1.0);
        worst_relative = std::max(worst_relative, std::abs(trace[i] - expected) / expected);
    }
    INFO("worst relative error " << worst_relative << " over 1000 points");
    REQUIRE(worst_relative < 0.01);

    // The clamp in the combination law is belt-and-braces, not a shaper:
    // |0.5·(a + b)| ≤ 1 for any two bipolar triangles. Recorded so a future
    // change to the law that makes it bite is visible rather than silent.
    INFO("largest |0.5(tri_I + tri_II)| " << largest_combined);
    REQUIRE(largest_combined <= 1.0);

    // The measured window is the narrower published one, not mode I's.
    const auto lo = *std::min_element(trace.begin(), trace.end());
    const auto hi = *std::max_element(trace.begin(), trace.end());
    REQUIRE(lo >= spec.center_ms - spec.depth_ms - 1e-9);
    REQUIRE(hi <= spec.center_ms + spec.depth_ms + 1e-9);
}

TEST_CASE("chorus juno I+II has no third oscillator", "[signal][chorus][chorus-family]") {
    // §4.2's other normative claim: the ~9–10 Hz structure that short-window
    // analysis reports in the I+II position is the BEAT of the two component
    // rates, not an oscillator. A separately implemented 9.75 Hz LFO would put
    // real energy in a 9.75 Hz bin; a beat does not.
    //
    // (The obvious check — "the trace returns to its start after one beat
    // period" — is wrong and was removed after it failed: one beat period
    // returns the two triangles' relative PHASE, not their absolute phases.
    // 0.513 Hz advances 1.466 cycles over 2.857 s, so the combined value at the
    // end of a beat period has no reason to equal its value at the start.)
    const auto spec = Chorus::juno_spec(JunoMode::mode_I_plus_II);
    Config cfg;
    cfg.voicing = Voicing::juno_ensemble;
    cfg.mode = JunoMode::mode_I_plus_II;
    constexpr std::size_t kSamples = 60 * static_cast<std::size_t>(kSr);
    const auto trace = delay_trace(cfg, 0, kSamples);

    const double component_a = std::abs(coherent_bin(trace, 0, kSamples, spec.rate_a_hz));
    const double component_b = std::abs(coherent_bin(trace, 0, kSamples, spec.rate_b_hz));
    const double beat_report = std::abs(coherent_bin(trace, 0, kSamples, 9.75));
    const double reference = std::max(component_a, component_b);
    INFO("component bins " << component_a << " / " << component_b << " ms; 9.75 Hz bin "
                           << beat_report << " ms");
    REQUIRE(component_a > 0.1 * spec.depth_ms);
    REQUIRE(component_b > 0.1 * spec.depth_ms);
    REQUIRE(beat_report < 0.01 * reference);
}

TEST_CASE("chorus dimension D dwells at the modulation extremes", "[signal][chorus][chorus-family]") {
    const Window w = shipped_window(Voicing::dimension_d, JunoMode::mode_I);
    const auto period = static_cast<std::size_t>(std::llround(kSr / w.rate_hz));
    REQUIRE_THAT(static_cast<double>(period), WithinAbs(kSr / w.rate_hz, 1e-9));

    Config cfg;
    cfg.voicing = Voicing::dimension_d;
    const auto trace = delay_trace(cfg, 0, period * 4);

    std::size_t at_rail = 0;
    for (const double d : trace) {
        const double m = (d - w.center_ms) / w.depth_ms;
        if (std::abs(m) >= 0.99) ++at_rail;
    }
    const double fraction = static_cast<double>(at_rail) / static_cast<double>(trace.size());
    const double expected = 1.0 - 1.0 / Chorus::kTrapK;
    INFO("dwell fraction " << fraction << " against 1 - 1/k = " << expected);
    // The 1 %-of-full-scale acceptance band admits a sliver either side of the
    // clamp (|tri| between 0.99/k and 1/k), worth 0.01/k = 0.56 points here —
    // inside the ±2-point tolerance, which is why the criterion is stated that
    // way rather than as an exact equality.
    REQUIRE_THAT(fraction, WithinAbs(expected, 0.02));
}

TEST_CASE("chorus tap gain matches the shipped interpolation kernel",
          "[signal][chorus][chorus-family]") {
    // `kTapL1` recomputed from `Interpolator::lagrange` itself, by probing it
    // with the four unit basis vectors — no algebra restated.
    double worst = 0.0;
    double worst_at = 0.0;
    for (int i = 0; i <= 20000; ++i) {
        const double frac = static_cast<double>(i) / 20000.0;
        double l1 = 0.0;
        for (int j = 0; j < 4; ++j) {
            double y[4] = {0.0, 0.0, 0.0, 0.0};
            y[j] = 1.0;
            l1 += std::abs(Interpolator::lagrange(frac, y[0], y[1], y[2], y[3]));
        }
        if (l1 > worst) {
            worst = l1;
            worst_at = frac;
        }
    }
    INFO("Lagrange kernel L1 norm peaks at " << worst << " at fractional offset " << worst_at);
    REQUIRE_THAT(worst, WithinRel(Chorus::kTapL1, 1e-9));
    REQUIRE_THAT(worst_at, WithinAbs(0.5, 1e-4));
}

TEST_CASE("chorus worst-case gain stays inside its closed-form bound",
          "[signal][chorus][chorus-family]") {
    const std::pair<Voicing, JunoMode> configs[] = {
        {Voicing::ce2, JunoMode::mode_I},
        {Voicing::juno_ensemble, JunoMode::mode_I},
        {Voicing::juno_ensemble, JunoMode::mode_I_plus_II},
        {Voicing::dimension_d, JunoMode::mode_I},
        {Voicing::tri_chorus, JunoMode::mode_I},
    };
    constexpr std::size_t kSamples = 96000;
    const std::size_t skip = static_cast<std::size_t>(Chorus::kMaxDelayMs * kSr * 0.001) + 64;

    for (const auto& [voicing, mode] : configs) {
        Chorus reference;
        reference.prepare(kSr);
        Config cfg;
        cfg.voicing = voicing;
        cfg.mode = mode;
        cfg.depth = 1.0;
        cfg.mix = 1.0;
        cfg.width = 1.0;
        configure(reference, cfg);
        const double bound = reference.worst_case_gain();
        // §1.3's (1 + N) counted PER CHANNEL, which is how the spec states it:
        // the Juno's N = 2 is one tap per channel, so its per-channel bound is
        // 2, not 3. The TriChorus's centre voice arrives at half weight.
        const double spec_bound = voicing == Voicing::dimension_d
                                      ? units::db_to_linear(3.0) + 2.0  // §4.3's 3.41
                                      : (voicing == Voicing::tri_chorus ? 2.5 : 2.0);

        // Three maximal-crest-factor probes, each reproducible from the shipped
        // constants rather than found by search: DC (every feedforward tap
        // constructive by construction, since a delay is transparent at DC),
        // Nyquist alternation, and full-scale seeded noise (which is what
        // exercises the interpolation kernel's sign pattern).
        auto peak_of = [&](const std::vector<double>& probe) {
            const auto out = render(cfg, probe, probe);
            double peak = 0.0;
            for (std::size_t i = skip; i < kSamples; ++i)
                peak = std::max({peak, std::abs(out.left[i]), std::abs(out.right[i])});
            return peak;
        };
        std::vector<double> alternating(kSamples);
        for (std::size_t i = 0; i < kSamples; ++i) alternating[i] = (i % 2 == 0) ? 1.0 : -1.0;

        const double dc_peak = peak_of(std::vector<double>(kSamples, 1.0));
        const double worst = std::max(
            {dc_peak, peak_of(alternating), peak_of(seeded_noise(kSamples, 1.0, 0x5A17u))});

        INFO(voicing_name(voicing) << ": peak " << worst << " against the L1 bound " << bound
                                   << " (headroom " << (bound - worst) / bound * 100.0
                                   << " %); DC probe alone reaches " << dc_peak
                                   << " against the spec's " << spec_bound);
        REQUIRE(worst <= bound * (1.0 + 1e-9));

        // The spec's ceiling is attained exactly at DC, so the "≥10 % headroom"
        // clause is unachievable — recorded here as the measurement that shows
        // it, not argued in prose.
        if (voicing != Voicing::dimension_d) REQUIRE_THAT(dc_peak, WithinRel(spec_bound, 1e-9));
        REQUIRE(bound > spec_bound);
    }
}

TEST_CASE("chorus dimension D cross-mix engages only above its corner",
          "[signal][chorus][chorus-family]") {
    // Depth is parked at 0 so both taps sit at a fixed delay: a modulated tap
    // spreads a probe tone's energy across sidebands, which would make a
    // single-bin magnitude a measurement of the modulation rather than of the
    // cross-mix.
    constexpr std::size_t kWindow = 65536;
    constexpr std::size_t kSettle = 32768;
    constexpr double kAmplitude = 0.5;
    const double corner = Chorus::kDimCornerHz;

    Config cfg;
    cfg.voicing = Voicing::dimension_d;
    cfg.depth = 0.0;
    cfg.mix = 1.0;

    // Bin-exact probe frequencies: `hz(p)` completes exactly p cycles in the
    // analysis window, so the coherent bin is leakage-free at any resolution.
    auto hz = [&](int p) { return kSr * static_cast<double>(p) / static_cast<double>(kWindow); };

    auto mono_bin = [&](double f, double width) {
        cfg.width = width;
        const auto in = sine(kSettle + kWindow, f, kAmplitude);
        const auto out = render(cfg, in, in);
        std::vector<double> mono(out.left.size());
        for (std::size_t i = 0; i < mono.size(); ++i) mono[i] = 0.5 * (out.left[i] + out.right[i]);
        return coherent_bin(mono, kSettle, kWindow, f);
    };
    auto mono_delta_db = [&](double f) {
        return units::linear_to_db(std::abs(mono_bin(f, 1.0))) -
               units::linear_to_db(std::abs(mono_bin(f, 0.0)));
    };

    // The first-order high-pass the topology specifies, at the shipped corner,
    // in the prewarped form the TPT section actually realises.
    auto highpass_law = [&](double f) {
        const double w = std::tan(kPi * f / kSr);
        const double wc = std::tan(kPi * corner / kSr);
        return w / std::sqrt(w * w + wc * wc);
    };

    SECTION("the cross-feed follows the shipped first-order high-pass") {
        for (int p : {7, 14, 27, 55, 109, 164, 273, 546, 1366, 2731, 5461}) {
            const double f = hz(p);
            // width = 1 minus width = 0 removes the dry, the shelf and both
            // wet taps, leaving exactly the cross term: −hpf(wet), and `wet` is
            // the input delayed, so its magnitude is the input's.
            const double term = std::abs(mono_bin(f, 1.0) - mono_bin(f, 0.0)) / kAmplitude;
            INFO("probe " << f << " Hz: cross term " << term << " against the law "
                          << highpass_law(f));
            REQUIRE_THAT(term, WithinAbs(highpass_law(f), 0.02));
        }
    }

    SECTION("bass stays out of the cross-mix and the top end does not") {
        // The criterion's LF clause, restated at a frequency a one-pole can
        // deliver it at: a decade below the corner.
        const double deep = hz(static_cast<int>(std::llround(
            corner / 10.0 * static_cast<double>(kWindow) / kSr)));
        REQUIRE(deep < corner / 8.0);
        INFO("a decade below the corner (" << deep << " Hz): " << mono_delta_db(deep) << " dB");
        REQUIRE(std::abs(mono_delta_db(deep)) < 0.5);

        // The criterion's HF clause, at frequencies where the 6 ms comb is not
        // sitting on a null. 400 Hz is also above 2·f_c and yields only
        // −0.67 dB, which is why the clause cannot be stated for every such
        // frequency.
        for (int p : {1366, 2731, 5461}) {
            const double f = hz(p);
            REQUIRE(f > 2.0 * corner);
            INFO("probe " << f << " Hz: mono sum changes by " << mono_delta_db(f) << " dB");
            REQUIRE(std::abs(mono_delta_db(f)) >= 1.0);
        }
        INFO("recorded counterexample at 400 Hz (> 2 f_c): " << mono_delta_db(hz(546)) << " dB");
        REQUIRE(std::abs(mono_delta_db(hz(546))) < 1.0);
    }
}

TEST_CASE("chorus BBD colour narrows the wet path to the composed bandwidth law",
          "[signal][chorus][chorus-family]") {
    constexpr std::size_t kWindow = 65536;
    constexpr std::size_t kSettle = 32768;

    Config cfg;
    cfg.voicing = Voicing::ce2;
    cfg.depth = 0.0;  // matched depth, and a static delay so the probe is clean
    cfg.mix = 1.0;

    Chorus probe;
    probe.prepare(kSr);
    Config with_bbd = cfg;
    with_bbd.bbd = true;
    configure(probe, with_bbd);
    const double bandwidth = probe.bbd_bandwidth_hz();
    const double stage_ms = probe.bbd_stage_delay_ms();

    // The law itself, recomputed from the composed module's own published
    // constants rather than read back from it.
    const double stages = chardelay::kBbdStages[2];  // kBbdColorCharacter = 1 → the 1024-stage knot
    const double expected_bandwidth =
        std::clamp(stages / (stage_ms * 0.001) / chardelay::kBbdBandwidthDivisor,
                   chardelay::kBbdBandwidthMinHz, chardelay::kBbdBandwidthMaxHz);
    INFO("BBD stage delay " << stage_ms << " ms, bandwidth law " << expected_bandwidth << " Hz");
    REQUIRE_THAT(bandwidth, WithinRel(expected_bandwidth, 1e-9));

    // Recorded, because it changes what §6 can promise: with the 1024-stage
    // knot the clock term is 62 kHz at the CE-2's sub-delay and 19 kHz even at
    // the longest chorus-scale delay in the family, so the law sits on its
    // 10 kHz ceiling everywhere here. The colour's bandwidth is CONSTANT across
    // the sweep — it does not "narrow with depth" at chorus delays.
    REQUIRE_THAT(bandwidth, WithinRel(chardelay::kBbdBandwidthMaxHz, 1e-9));
    REQUIRE(stages / (stage_ms * 0.001) / chardelay::kBbdBandwidthDivisor >
            chardelay::kBbdBandwidthMaxHz);

    auto hz = [&](int p) { return kSr * static_cast<double>(p) / static_cast<double>(kWindow); };
    auto wet_response_db = [&](bool bbd, double f) {
        Config c = cfg;
        c.bbd = bbd;
        const auto wet = wet_only(c, sine(kSettle + kWindow, f, 0.1));
        return units::linear_to_db(std::abs(coherent_bin(wet, kSettle, kWindow, f)));
    };

    // Without the colour the wet path is a pure integer-sample delay (12 ms is
    // 576 samples exactly at 48 kHz, so the Lagrange stencil is exact) — flat
    // to Nyquist, which is what makes the colour's narrowing attributable.
    const double flat_reference = wet_response_db(false, hz(683));  // ~500 Hz
    for (int p : {683, 5461, 10923, 21845}) {                       // 0.5, 4, 8, 16 kHz
        INFO("colour off at " << hz(p) << " Hz: " << wet_response_db(false, hz(p)) << " dB");
        REQUIRE_THAT(wet_response_db(false, hz(p)) - flat_reference, WithinAbs(0.0, 0.2));
    }

    const double plateau = wet_response_db(true, hz(683));
    auto relative = [&](double f) { return wet_response_db(true, f) - plateau; };

    // 1. Flat well inside the bandwidth. A fifth of the corner is where a
    //    4-pole cascade has spent 0.1 dB, so 0.5 dB is a real ceiling.
    const double inside = 0.2 * bandwidth;
    INFO("at 0.2x the bandwidth (" << inside << " Hz): " << relative(inside) << " dB");
    REQUIRE(relative(inside) > -0.5);

    // 2. Well down AT the bandwidth — the "narrowing" the toggle exists for.
    INFO("at the bandwidth (" << bandwidth << " Hz): " << relative(bandwidth) << " dB");
    REQUIRE(relative(bandwidth) < -10.0);

    // 3. Monotone through the transition, so the narrowing is a rolloff and not
    //    a resonance or an aliasing artefact.
    double previous = 1.0;
    for (int p : {683, 2731, 4096, 5461, 6827, 8192, 9557, 13653, 16384}) {
        const double db = relative(hz(p));
        INFO("colour on at " << hz(p) << " Hz: " << db << " dB");
        REQUIRE(db < previous + 0.05);
        previous = db;
    }

    // Recorded: the composite −3 dB point against the law, the number the
    // criterion asks to be within ±15 % and structurally cannot be.
    int lo_bin = 4096;   // 3 kHz
    int hi_bin = 16384;  // 12 kHz
    while (hi_bin - lo_bin > 4) {
        const int mid = (lo_bin + hi_bin) / 2;
        (relative(hz(mid)) > -3.0 ? lo_bin : hi_bin) = mid;
    }
    INFO("composite −3 dB near " << hz((lo_bin + hi_bin) / 2) << " Hz, i.e. "
                                 << hz((lo_bin + hi_bin) / 2) / bandwidth
                                 << "x the bandwidth law");
    REQUIRE(hz(lo_bin) < bandwidth);
}

TEST_CASE("chorus reports zero latency and never reads ahead", "[signal][chorus][chorus-family]") {
    const std::pair<Voicing, JunoMode> configs[] = {
        {Voicing::ce2, JunoMode::mode_I},
        {Voicing::juno_ensemble, JunoMode::mode_I},
        {Voicing::juno_ensemble, JunoMode::mode_II},
        {Voicing::juno_ensemble, JunoMode::mode_I_plus_II},
        {Voicing::dimension_d, JunoMode::mode_I},
        {Voicing::tri_chorus, JunoMode::mode_I},
    };
    constexpr std::size_t kImpulseAt = 4096;

    for (const auto& [voicing, mode] : configs) {
        // The colour-off wet arrival, measured first so the colour-on pass has
        // a reference to size "an early arrival would be this big" against.
        double clean_peak = 0.0;
        for (bool bbd : {false, true}) {
            const Window w = shipped_window(voicing, mode);
            Config cfg;
            cfg.voicing = voicing;
            cfg.mode = mode;
            cfg.mix = 1.0;
            cfg.width = 0.0;
            cfg.bbd = bbd;

            Chorus c;
            c.prepare(kSr);
            configure(c, cfg);
            REQUIRE(c.latency_samples() == 0);

            std::vector<double> in(12288, 0.0);
            in[kImpulseAt] = 1.0;
            auto left = in;
            auto right = in;
            c.process(left.data(), right.data(), static_cast<int>(left.size()));

            INFO(voicing_name(voicing) << " mode " << static_cast<int>(mode) << " bbd=" << bbd);
            // Causality, exactly: a module claiming zero latency may not put a
            // single non-zero sample ahead of its input.
            for (std::size_t i = 0; i < kImpulseAt; ++i) {
                REQUIRE(left[i] == 0.0);
                REQUIRE(right[i] == 0.0);
            }

            // ... and the wet arrival sits inside the calibration window, with
            // the interpolation stencil's footprint as the only slack.
            const auto lo = kImpulseAt +
                            static_cast<std::size_t>((w.center_ms - w.depth_ms) * kSr * 0.001) -
                            Chorus::kGuardSamples;
            const auto hi = kImpulseAt +
                            static_cast<std::size_t>((w.center_ms + w.depth_ms) * kSr * 0.001) +
                            Chorus::kGuardSamples;
            std::size_t peak_at = lo;
            double peak = 0.0;
            for (std::size_t i = kImpulseAt + 1; i < left.size(); ++i) {
                if (std::abs(left[i]) > peak) {
                    peak = std::abs(left[i]);
                    peak_at = i;
                }
            }
            // Everything between the input and the window's opening: a filter
            // tail belongs here (the Dimension D's dry shelf leaves 0.0048 at
            // the very next sample), an early wet arrival would not.
            double before_window = 0.0;
            for (std::size_t i = kImpulseAt + 1; i < lo; ++i)
                before_window = std::max(before_window, std::abs(left[i]));

            if (!bbd) clean_peak = peak;
            INFO("wet peak " << peak << " at sample " << peak_at - kImpulseAt << ", window ["
                             << lo - kImpulseAt << ", " << hi - kImpulseAt
                             << "], largest sample before the window " << before_window
                             << ", colour-off arrival " << clean_peak);

            // An early wet arrival would be the same tap at the wrong time, so
            // it would be of the same order as the real one. Two percent of the
            // colour-off arrival is comfortably below that and comfortably
            // above what a causal filter tail leaves here — the Dimension D's
            // dry low-shelf puts 0.00481 at the very next sample, which is
            // (A − 1)·h_lp[1] reproduced to six figures by hand.
            REQUIRE(clean_peak > 0.1);
            REQUIRE(before_window < 0.02 * clean_peak);

            // Where the arrival LANDS is asserted only with the colour off.
            // The compander's expander multiplies by an envelope that is still
            // climbing when the arrival gets there, so the peak of an impulse
            // response through it sits later than the arrival — measured at
            // 8.1 ms for the Juno's 3.5 ms tap — and the compressor's
            // divide-by-the-floor scales the whole thing to ~0.004. Both are
            // amplitude-domain, and a late peak cannot indicate negative
            // latency, which is the only thing this test exists to exclude.
            // The colour's effect on the actual delay is measured properly by
            // the group-delay case below.
            if (!bbd) {
                REQUIRE(peak_at >= lo);
                REQUIRE(peak_at <= hi);
            }
        }
    }
}

TEST_CASE("chorus BBD colour does not move the tap it colours",
          "[signal][chorus][chorus-family]") {
    // The load-bearing claim of the §6 substitution: the colour stage is a
    // clocked delay, so it is given a fixed sub-delay and the Lagrange line
    // carries the remainder, leaving the TOTAL on the calibration table. That
    // arithmetic is asserted here rather than assumed — a sign error in the
    // split would move every voicing's delay and nothing else in the suite
    // would notice, because every other test runs with the colour off.
    //
    // Noise at a working level, not an impulse: a compander's response to an
    // impulse says more about the compander than about the delay.
    for (auto voicing : {Voicing::ce2, Voicing::juno_ensemble, Voicing::dimension_d,
                         Voicing::tri_chorus}) {
        Config cfg;
        cfg.voicing = voicing;
        cfg.depth = 0.0;  // static delay, so a single lag is well defined
        cfg.mix = 1.0;
        cfg.width = 0.0;
        const auto source = seeded_noise(131072, 0.3, 0x4B1Du);

        Config off = cfg;
        Config on = cfg;
        on.bbd = true;
        const auto wet_off = wet_only(off, source);
        const auto wet_on = wet_only(on, source);

        constexpr std::size_t kBegin = 65536;
        constexpr std::size_t kCount = 32768;
        constexpr int kSearch = 48;
        int best_lag = 0;
        double best = -1.0;
        for (int lag = -kSearch; lag <= kSearch; ++lag) {
            double acc = 0.0;
            for (std::size_t i = 0; i < kCount; ++i)
                acc += wet_off[kBegin + i] * wet_on[static_cast<std::size_t>(
                                                 static_cast<long long>(kBegin + i) + lag)];
            if (std::abs(acc) > best) {
                best = std::abs(acc);
                best_lag = lag;
            }
        }
        INFO(voicing_name(voicing) << ": colour adds " << best_lag << " samples of group delay");
        // The colour stage adds a few samples of fixed group delay — its
        // band-limiting pair (a 2-pole Butterworth at 10 kHz is √2/(2π·10 kHz)
        // = 22.5 µs ≈ 1.1 samples at 48 kHz) plus the clocked core's write-side
        // ramp and read-position alignment. Measured: 3 samples, identically
        // for all four voicings. The tolerance bounds that, and nothing wider:
        // the sub-delays being split off here run from 56 to 528 samples, so a
        // sign or scale error in the split would land far outside the ±48
        // sample search window rather than inside this bound.
        REQUIRE(std::abs(best_lag) <= 4);
    }
}
