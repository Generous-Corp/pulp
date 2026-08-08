#include "test_signal_nonlin_ambience_support.hpp"

#include <limits>

TEST_CASE("Nonlin ambience: public velvet tap design fails closed outside its domain",
          "[signal][nonlin-ambience][velvet-noise][invalid]") {
    using Draw = pulp::signal::VelvetNoiseDrawT<double>;
    auto design = [](double position, double grid, int window, int predelay,
                     double hold, double attack, Draw draw) {
        return na::design_velvet_tap(NonlinProgram::gated, position, grid, window,
                                     predelay, hold, attack, draw);
    };
    auto require_closed = [](const na::VelvetTapDesign& tap) {
        CHECK_FALSE(tap.audible);
        CHECK(tap.delay == 0);
        CHECK(tap.magnitude == 0.0);
        CHECK(tap.gain == 0.0);
        CHECK(tap.segment == 0);
    };

    constexpr Draw kDraw{0.25, 1};
    const auto valid = design(8.0, 4.0, 64, 3, 0.7, 0.2, kDraw);
    REQUIRE(valid.audible);
    REQUIRE(valid.delay >= 3);
    REQUIRE(std::isfinite(valid.gain));

    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    require_closed(design(8.0, 4.0, 0, 3, 0.7, 0.2, kDraw));
    require_closed(design(-1.0, 4.0, 64, 3, 0.7, 0.2, kDraw));
    require_closed(design(nan, 4.0, 64, 3, 0.7, 0.2, kDraw));
    require_closed(design(8.0, -1.0, 64, 3, 0.7, 0.2, kDraw));
    require_closed(design(8.0, inf, 64, 3, 0.7, 0.2, kDraw));
    require_closed(design(8.0, 4.0, 64, 3, 0.7, 0.2, Draw{1.0, 1}));
    require_closed(design(8.0, 4.0, 64, 3, 0.7, 0.2, Draw{nan, 1}));
    require_closed(design(8.0, 4.0, 64, 3, 0.7, 0.2, Draw{0.25, 0}));
    require_closed(design(8.0, 4.0, 64, -1, 0.7, 0.2, kDraw));
    require_closed(design(8.0, 4.0, 64, std::numeric_limits<int>::max(),
                          0.7, 0.2, kDraw));
    require_closed(design(8.0, 4.0, 64, 3, nan, 0.2, kDraw));
    require_closed(design(8.0, 4.0, 64, 3, 0.7, inf, kDraw));
    require_closed(design(std::numeric_limits<double>::max(),
                          std::numeric_limits<double>::max(),
                          64, 3, 0.7, 0.2, kDraw));
}

TEST_CASE("Nonlin ambience: every tap gain traces the designed envelope exactly",
          "[signal][nonlin-ambience][envelope]") {
    // The envelope IS the tap-gain sequence, so this is the claim in its
    // zero-variance form: no windows, no estimator, no tolerance beyond float
    // round-off. `spec_envelope` is an independent transcription of §4.3.
    for (NonlinProgram program : kAllPrograms) {
        NonlinAmbience engine;
        engine.prepare(kFs, na::kMaxLengthMs);
        engine.set_program(program);
        engine.set_length_ms(400.0);
        engine.reset();

        INFO("program = " << program_name(program));
        REQUIRE(engine.tap_count(0) > 0);

        const double window = engine.window_samples();
        const double gate_hold = na::kGateHold;
        const double attack = na::kRevRise;

        for (int ch = 0; ch < 2; ++ch) {
            double l1 = 0.0;
            for (int k = 0; k < engine.tap_count(ch); ++k) {
                const auto& tap = engine.tap(ch, k);
                const double tau =
                    (tap.delay - engine.predelay_samples()) / window;
                const double env = spec_envelope(program, tau, gate_hold, attack);

                // The shipped gain law, including the sqrt(Td) density weight
                // that defect D5 forced. Td is the grid spacing at this tap's
                // own time.
                const double grid = kFs / spec_density(tau, na::kDensityRefPct,
                                                       na::kGammaDefault);
                const double expected = env * std::sqrt(grid) * engine.tap_norm(ch);

                INFO("channel " << ch << " tap " << k << " tau " << tau);
                REQUIRE_THAT(std::fabs(static_cast<double>(tap.gain)),
                             Catch::Matchers::WithinRel(expected, 2e-3));
                // Ternary structure: every tap is ±1 times the envelope, never
                // a third magnitude.
                REQUIRE(tap.gain != 0.0f);
                l1 += std::fabs(static_cast<double>(tap.gain));
            }
            // §4.4: the L1 budget is met exactly, which is what makes the peak
            // gain a closed form rather than a measurement.
            INFO("channel " << ch);
            REQUIRE_THAT(l1, Catch::Matchers::WithinRel(na::kL1Budget, 1e-4));
        }
    }
}

TEST_CASE("Nonlin ambience: taps carry both signs and land inside the window",
          "[signal][nonlin-ambience][envelope]") {
    NonlinAmbience engine;
    engine.prepare(kFs, na::kMaxLengthMs);
    engine.set_length_ms(400.0);
    engine.set_predelay_ms(25.0);
    engine.reset();

    const int predelay = engine.predelay_samples();
    const int window = engine.window_samples();
    REQUIRE(predelay > 0);

    int positives = 0, negatives = 0, previous = -1;
    for (int k = 0; k < engine.tap_count(0); ++k) {
        const auto& tap = engine.tap(0, k);
        REQUIRE(tap.delay >= predelay);
        REQUIRE(tap.delay < predelay + window);
        REQUIRE(tap.segment >= 0);
        REQUIRE(tap.segment < na::kSegments);
        // Velvet noise is a stratified process: exactly one pulse per grid
        // interval, so positions are strictly increasing and never collide.
        REQUIRE(tap.delay > previous);
        previous = tap.delay;
        (tap.gain > 0.0f ? positives : negatives)++;
    }
    // A ±1 coin over ~750 taps: a 30/70 split would be a 10σ event, so this
    // catches a stuck sign bit without being flaky.
    REQUIRE(positives > engine.tap_count(0) * 3 / 10);
    REQUIRE(negatives > engine.tap_count(0) * 3 / 10);
}

TEST_CASE("Nonlin ambience: the rendered envelope is the designed envelope",
          "[signal][nonlin-ambience][envelope]") {
    // The headline claim. Measured on the naked cloud so that what is compared
    // is the envelope and not the diffuser's ring or the segment tilt's
    // colour — both of which are separately asserted (N4, N15).
    //
    // Tolerance: the measured worst-case deviation across all four programs is
    // 0.23 dB; 0.75 dB is that with 3x margin and is still far inside the
    // spec's own ±1.0 dB flatness ask.
    constexpr double kEnvelopeTolDb = 0.75;
    constexpr double kFloorDb = -70.0;

    const int win = static_cast<int>(0.020 * kFs);
    const int hop = static_cast<int>(0.010 * kFs);

    for (NonlinProgram program : kAllPrograms) {
        NonlinAmbience engine;
        configure_naked(engine, program, 400.0);

        const int window = engine.window_samples();
        const int lag = bypassed_diffuser_delay(engine);
        const Stereo ir = render_impulse(engine, window + lag + 4000);

        const double pole = spec_segment_pole(0, 0.0, na::kFcBright);
        const double tilt_power = onepole_noise_power(pole);
        const double norm = engine.tap_norm(0);

        int compared = 0;
        double worst = 0.0;
        for (int start = lag; start + win <= static_cast<int>(ir.left.size());
             start += hop) {
            const double tau =
                (start + win * 0.5 - lag) / static_cast<double>(window);
            if (tau > 1.0) break;

            // Expected mean power over the window: the mean of E(τ)² across the
            // window's samples, scaled by the L1 normalisation and the (now
            // uniform) segment filter's noise gain. Every term is computed from
            // shipped constants.
            double sum_e2 = 0.0;
            for (int i = start; i < start + win; ++i) {
                const double t = (i + 0.5 - lag) / static_cast<double>(window);
                const double e = spec_envelope(program, t, na::kGateHold, na::kRevRise);
                sum_e2 += e * e;
            }
            const double expected =
                sum_e2 / win * norm * norm * tilt_power;
            if (to_db(expected) < kFloorDb) continue;

            const double measured = window_power(ir.left, start, win);
            const double deviation = to_db(measured) - to_db(expected);
            worst = std::max(worst, std::fabs(deviation));
            INFO("program " << program_name(program) << " tau " << tau
                            << " measured " << to_db(measured) << " dB expected "
                            << to_db(expected) << " dB");
            REQUIRE(std::fabs(deviation) < kEnvelopeTolDb);
            ++compared;
        }
        INFO("program " << program_name(program) << " worst deviation " << worst);
        REQUIRE(compared > 15);
    }
}

TEST_CASE("Nonlin ambience: the shipped default render matches the full model",
          "[signal][nonlin-ambience][envelope]") {
    // N2 measures the envelope with the diffuser and the tilt taken out. This
    // one measures the SHIPPED configuration and accounts for both, so that
    // nothing about the default path is left unmodelled.
    //
    // Expected power at sample n is exact in expectation because the tap signs
    // are independent, so all cross terms vanish:
    //     E[h(n)²] = Σ_k g_k² · G²(seg_k) · e_ap(n − d_k)
    // with G² the segment one-pole's white-noise power gain and e_ap the
    // allpass chain's own energy impulse response.
    //
    // Tolerance: measured worst case is 0.64 dB; 1.5 dB is that with margin.
    constexpr double kModelTolDb = 1.5;
    constexpr double kFloorDb = -80.0;

    const int win = static_cast<int>(0.020 * kFs);
    const int hop = static_cast<int>(0.010 * kFs);

    for (NonlinProgram program : kAllPrograms) {
        NonlinAmbience engine;
        engine.prepare(kFs, na::kMaxLengthMs);
        engine.set_program(program);
        engine.set_length_ms(400.0);
        engine.reset();

        const int window = engine.window_samples();
        const int length = window + 8000;
        const Stereo ir = render_impulse(engine, length);

        double tilt_power[na::kSegments];
        for (int s = 0; s < na::kSegments; ++s)
            tilt_power[s] = onepole_noise_power(spec_segment_pole(s, 0.0, na::kFcDark));

        const auto energy_ir = spec_allpass_energy_ir(
            na::kDiffusionDefault, engine.allpass_length(0), engine.allpass_length(1),
            12000);

        std::vector<double> expected(static_cast<std::size_t>(length), 0.0);
        for (int k = 0; k < engine.tap_count(0); ++k) {
            const auto& tap = engine.tap(0, k);
            const double energy = static_cast<double>(tap.gain) * tap.gain *
                                  tilt_power[tap.segment];
            const int span = std::min(static_cast<int>(energy_ir.size()),
                                      length - tap.delay);
            for (int i = 0; i < span; ++i)
                expected[static_cast<std::size_t>(tap.delay + i)] +=
                    energy * energy_ir[static_cast<std::size_t>(i)];
        }

        int compared = 0;
        for (int start = 0; start + win <= length; start += hop) {
            double model = 0.0;
            for (int i = start; i < start + win; ++i)
                model += expected[static_cast<std::size_t>(i)];
            model /= win;
            if (to_db(model) < kFloorDb) continue;

            const double measured = window_power(ir.left, start, win);
            INFO("program " << program_name(program) << " window at " << start
                            << " measured " << to_db(measured) << " dB model "
                            << to_db(model) << " dB");
            REQUIRE(std::fabs(to_db(measured) - to_db(model)) < kModelTolDb);
            ++compared;
        }
        REQUIRE(compared > 15);
    }
}

TEST_CASE("Nonlin ambience: each program has the shape a decaying tank cannot make",
          "[signal][nonlin-ambience][envelope]") {
    const int win = static_cast<int>(0.020 * kFs);
    const int hop = static_cast<int>(0.005 * kFs);

    auto envelope_db = [&](NonlinAmbience& engine, const Stereo& ir, int lag,
                           std::vector<double>& taus) {
        std::vector<double> out;
        const double window = engine.window_samples();
        for (int start = lag; start + win <= static_cast<int>(ir.left.size());
             start += hop) {
            out.push_back(to_db(window_power(ir.left, start, win)));
            taus.push_back((start + win * 0.5 - lag) / window);
        }
        return out;
    };

    SECTION("gated: the body is flat and the cut is a cut") {
        NonlinAmbience engine;
        configure_naked(engine, NonlinProgram::gated, 400.0);
        const int lag = bypassed_diffuser_delay(engine);
        const Stereo ir = render_impulse(engine, engine.window_samples() + lag + 4000);
        std::vector<double> taus;
        const auto env = envelope_db(engine, ir, lag, taus);

        // Body flat within the spec's own ±1.0 dB, once D1's spectral tilt is
        // out of the way. Windows are excluded near the fade-in and near the
        // cut, where a 20 ms window straddles a transition by construction.
        const double body_start = na::kFadeInFrac + 0.05;
        const double body_end = na::kGateHold - 0.05;
        double lo = 1e9, hi = -1e9;
        for (std::size_t i = 0; i < env.size(); ++i)
            if (taus[i] > body_start && taus[i] < body_end) {
                lo = std::min(lo, env[i]);
                hi = std::max(hi, env[i]);
            }
        INFO("gated body spread " << (hi - lo) << " dB");
        REQUIRE(hi - lo < 2.0);  // ±1.0 dB, spec T1

        // And after the cut, silence — exactly, not asymptotically. This is the
        // structural claim; N5 generalises it.
        const int cut = lag + static_cast<int>((na::kGateHold + na::kGateFall) *
                                               engine.window_samples());
        const int flush = segment_flush_samples(na::kFcBright);
        for (int i = cut + flush + last_tap_delay(engine) - engine.window_samples();
             i < static_cast<int>(ir.left.size()); ++i) {
            if (i < cut + flush) continue;
            INFO("sample " << i << " of " << ir.left.size());
            REQUIRE(ir.left[static_cast<std::size_t>(i)] == 0.0f);
            break;  // the exhaustive version is N5
        }
    }

    SECTION("gated at the shipped diffusion: the cut is bounded by the allpass ring") {
        // Defect D2: T1 wants −60 dB within 2w·L = 40 ms. The mandated diffuser
        // rings for ln(10^-3)/ln(g) repetitions of its longest delay. That
        // number, not 2w·L, is the achievable criterion.
        NonlinAmbience engine;
        engine.prepare(kFs, na::kMaxLengthMs);
        engine.set_program(NonlinProgram::gated);
        engine.set_length_ms(400.0);
        engine.reset();

        const int window = engine.window_samples();
        const Stereo ir = render_impulse(engine, window + 20000);

        double body = 0.0;
        int count = 0;
        for (int start = 0; start + win < static_cast<int>(0.6 * window);
             start += hop, ++count)
            body += window_power(ir.left, start, win);
        body /= count;

        const double repetitions = std::log(1e-3) / std::log(na::kDiffusionDefault);
        const int allpass_60db = static_cast<int>(
            std::ceil(repetitions * std::max(engine.allpass_length(0),
                                             engine.allpass_length(1))));
        const int cut = static_cast<int>((na::kGateHold + na::kGateFall) * window);

        INFO("allpass 60 dB time = " << allpass_60db << " samples ("
                                     << allpass_60db * 1000.0 / kFs << " ms); spec T1 allows "
                                     << 2.0 * na::kGateFall * window << " samples");
        REQUIRE(to_db(window_power(ir.left, cut + allpass_60db, win) / body) < -60.0);
        // ...and it is genuinely still ringing at the point T1 asks for silence,
        // which is the defect, asserted rather than described.
        REQUIRE(to_db(window_power(ir.left,
                                   cut + static_cast<int>(2.0 * na::kGateFall * window),
                                   win) /
                      body) > -60.0);
    }

    SECTION("reverse: the envelope rises, then cuts") {
        NonlinAmbience engine;
        configure_naked(engine, NonlinProgram::reverse, 400.0);
        const int lag = bypassed_diffuser_delay(engine);
        const Stereo ir = render_impulse(engine, engine.window_samples() + lag + 4000);
        std::vector<double> taus;
        const auto env = envelope_db(engine, ir, lag, taus);

        // Monotone rising up to the plateau. This is the assertion no FDN can
        // pass: a decaying tank's envelope is falling by construction.
        double previous = -1e9;
        int checked = 0;
        for (std::size_t i = 0; i < env.size(); ++i) {
            if (taus[i] < 0.10 || taus[i] > na::kRevRise - 0.05) continue;
            INFO("tau " << taus[i] << " level " << env[i] << " previous " << previous);
            REQUIRE(env[i] > previous - 0.5);  // spec T1's ≥ previous − 0.5 dB
            previous = env[i];
            ++checked;
        }
        REQUIRE(checked > 10);

        // Peak is at the plateau, not at the start.
        std::size_t peak = 0;
        for (std::size_t i = 0; i < env.size(); ++i)
            if (taus[i] <= 1.0 && env[i] > env[peak]) peak = i;
        INFO("peak at tau " << taus[peak]);
        REQUIRE(taus[peak] > na::kRevRise - 0.1);
        REQUIRE(taus[peak] <= 1.0);
    }

    SECTION("ambience: monotone falling at the designed rate") {
        NonlinAmbience engine;
        configure_naked(engine, NonlinProgram::ambience, 400.0);
        const int lag = bypassed_diffuser_delay(engine);
        const Stereo ir = render_impulse(engine, engine.window_samples() + lag + 4000);
        std::vector<double> taus;
        const auto env = envelope_db(engine, ir, lag, taus);

        double previous = 1e9;
        for (std::size_t i = 0; i < env.size(); ++i) {
            if (taus[i] < 0.10 || taus[i] > 0.95) continue;
            INFO("tau " << taus[i]);
            REQUIRE(env[i] < previous + 0.5);
            previous = env[i];
        }

        // The total drop is the shipped kAmbDropDb, in amplitude dB. Measured on
        // power, so the criterion is 2x the amplitude figure... which is exactly
        // what `to_db` on a power quantity already reports as amplitude dB.
        double at_low = 0.0, at_high = 0.0;
        for (std::size_t i = 0; i < env.size(); ++i) {
            if (std::fabs(taus[i] - na::kFadeInFrac * 2.0) < 0.02) at_low = env[i];
            if (std::fabs(taus[i] - 0.98) < 0.02) at_high = env[i];
        }
        const double expected_drop =
            20.0 * std::log10(spec_envelope(NonlinProgram::ambience, 0.98,
                                            na::kGateHold, na::kRevRise) /
                              spec_envelope(NonlinProgram::ambience,
                                            na::kFadeInFrac * 2.0, na::kGateHold,
                                            na::kRevRise));
        INFO("measured drop " << (at_high - at_low) << " dB, designed "
                              << expected_drop << " dB");
        REQUIRE(std::fabs((at_high - at_low) - expected_drop) < 2.0);
    }

    SECTION("nonlin2: exactly kNlHumps humps, then a gate") {
        NonlinAmbience engine;
        configure_naked(engine, NonlinProgram::nonlin2, 400.0);
        const int lag = bypassed_diffuser_delay(engine);
        const Stereo ir = render_impulse(engine, engine.window_samples() + lag + 4000);
        std::vector<double> taus;
        const auto env = envelope_db(engine, ir, lag, taus);

        // Humps are counted by PROMINENCE, not by raw local maxima. Counting
        // raw maxima is the wrong instrument and was measurably so: it reported
        // three, because the broad peak at τ = 0.25 is flat enough that 0.12 dB
        // of estimator noise splits it into two adjacent maxima 0.05 τ apart.
        // Prominence — how far a peak stands above the higher of the two valleys
        // that flank it — is what "a hump" means. The designed ripple is
        // 20·log10(1/(1−kNlDepth)) = 4.44 dB deep, so a threshold at a quarter
        // of that separates a real hump (measured prominence ≈ 4.3 dB) from the
        // spurious one (0.016 dB) by a factor of seventy.
        const double ripple_db = 20.0 * std::log10(1.0 / (1.0 - na::kNlDepth));
        const double min_prominence = 0.25 * ripple_db;

        // Candidates are restricted to the body; the VALLEY SEARCH is not. That
        // distinction is load-bearing: clipping the search at the body edge cut
        // the second hump's right flank off before it descended into the gate
        // and dropped its prominence to 0.82 dB, which read as one hump instead
        // of two. Prominence is a property of the whole curve.
        auto in_body = [&](std::size_t i) {
            return taus[i] >= na::kFadeInFrac * 2.0 && taus[i] <= na::kNlHold - 0.02;
        };
        std::size_t body_count = 0;
        for (std::size_t i = 0; i < env.size(); ++i) body_count += in_body(i) ? 1 : 0;
        REQUIRE(body_count > 20);

        int humps = 0;
        for (std::size_t i = 1; i + 1 < env.size(); ++i) {
            if (!in_body(i)) continue;
            const double level = env[i];
            if (!(level > env[i - 1] && level >= env[i + 1])) continue;
            // Walk out to the first higher point on each side; the deepest
            // valley reached on the way is that side's key col.
            double left_valley = level, right_valley = level;
            for (std::size_t j = i; j-- > 0;) {
                if (env[j] > level) break;
                left_valley = std::min(left_valley, env[j]);
            }
            for (std::size_t j = i + 1; j < env.size(); ++j) {
                if (env[j] > level) break;
                right_valley = std::min(right_valley, env[j]);
            }
            const double prominence = level - std::max(left_valley, right_valley);
            if (prominence >= min_prominence) {
                INFO("hump at tau " << taus[i] << " prominence " << prominence << " dB");
                ++humps;
            }
        }
        INFO("counted " << humps << " humps against a designed " << na::kNlHumps
                        << " (ripple depth " << ripple_db << " dB)");
        REQUIRE(humps == na::kNlHumps);

        // The ripple is genuinely as deep as designed — a flat body would pass
        // a hump count of zero-versus-two only by accident.
        double peak = -1e9, trough = 1e9;
        for (std::size_t i = 0; i < env.size(); ++i) {
            if (!in_body(i)) continue;
            peak = std::max(peak, env[i]);
            trough = std::min(trough, env[i]);
        }
        INFO("measured ripple " << (peak - trough) << " dB, designed " << ripple_db);
        REQUIRE(std::fabs((peak - trough) - ripple_db) < 1.0);
    }
}

TEST_CASE("Nonlin ambience: the wet impulse response is finite",
          "[signal][nonlin-ambience][structure]") {
    // The claim that separates this module from every recursive reverb. With
    // the diffuser bypassed, each allpass degenerates to a pure M-sample delay
    // and the whole wet path is an FIR — so past the last tap (plus the segment
    // filters' own flush, which is computed, not guessed) the output is
    // BIT-EXACTLY zero. No feedback design can produce that sample: a decaying
    // tank's output is asymptotic to zero, never equal to it.
    for (NonlinProgram program : kAllPrograms) {
        NonlinAmbience engine;
        configure_naked(engine, program, 400.0);

        const int lag = bypassed_diffuser_delay(engine);
        const int last = last_tap_delay(engine);
        const int flush = segment_flush_samples(na::kFcBright);
        const int length = lag + last + flush + 8000;
        const Stereo ir = render_impulse(engine, length);

        // Something happened first — otherwise "all zero" would pass trivially.
        double peak = 0.0;
        for (float v : ir.left) peak = std::max(peak, static_cast<double>(std::fabs(v)));
        INFO("program " << program_name(program));
        REQUIRE(peak > 1e-4);

        // Scanned rather than asserted per sample: one assertion carrying the
        // first offending index reads better than eight thousand identical
        // passes hiding one failure.
        const int tail_start = lag + last + flush;
        int offender = -1;
        double offending_value = 0.0;
        for (int n = tail_start; n < length && offender < 0; ++n) {
            const float l = ir.left[static_cast<std::size_t>(n)];
            const float r = ir.right[static_cast<std::size_t>(n)];
            if (l != 0.0f || r != 0.0f) {
                offender = n;
                offending_value = std::fabs(l) > std::fabs(r) ? l : r;
            }
        }
        INFO("first non-zero tail sample " << offender << " (value " << offending_value
                                           << ") after last tap + lag + flush = "
                                           << tail_start << ", of " << length);
        REQUIRE(offender == -1);
    }
}

TEST_CASE("Nonlin ambience: the only recursion is the two bounded allpasses",
          "[signal][nonlin-ambience][structure]") {
    // At the shipped diffusion the response is no longer finite — the diffuser
    // is recursive, which the spec acknowledges. What is asserted is that the
    // ring is bounded by the allpass's own closed form and nothing else: the
    // tail is below −100 dB by the time the coefficient's 100 dB point says it
    // should be, and it does eventually reach exact zero because every
    // recursive state snaps through the denormal threshold.
    NonlinAmbience engine;
    engine.prepare(kFs, na::kMaxLengthMs);
    engine.set_program(NonlinProgram::gated);
    engine.set_length_ms(200.0);
    engine.reset();

    const double repetitions = std::log(1e-5) / std::log(na::kDiffusionDefault);
    const int allpass_100db = static_cast<int>(std::ceil(
        repetitions * (engine.allpass_length(0) + engine.allpass_length(1))));
    const int length = engine.window_samples() + allpass_100db + 4000;
    const Stereo ir = render_impulse(engine, length);

    double peak = 0.0;
    for (float v : ir.left) peak = std::max(peak, static_cast<double>(std::fabs(v)));

    const int start = engine.window_samples() + allpass_100db;
    double tail = 0.0;
    for (int n = start; n < length; ++n)
        tail = std::max(tail, static_cast<double>(std::fabs(ir.left[static_cast<std::size_t>(n)])));
    INFO("tail " << 20.0 * std::log10(std::max(tail / peak, 1e-30)) << " dB below peak");
    REQUIRE(tail < peak * 1e-5);
}

TEST_CASE("Nonlin ambience: echo density follows the shipped density law",
          "[signal][nonlin-ambience][density]") {
    // Defect D3: T2's "NED ≥ 0.9" is unreachable — see N17 for the arithmetic.
    // What T2 protects is that the field starts sparse and becomes dense under
    // the physical growth law and stays flat when that law is switched off.
    // Both are asserted here against the shipped `Nd(u)`.
    auto measure_ned = [](const std::vector<float>& x, int centre, int win) {
        const int start = centre - win / 2;
        double sum = 0.0;
        for (int i = start; i < start + win; ++i) {
            const double v = x[static_cast<std::size_t>(i)];
            sum += v * v;
        }
        const double sigma = std::sqrt(sum / win);
        int above = 0;
        for (int i = start; i < start + win; ++i)
            if (std::fabs(static_cast<double>(x[static_cast<std::size_t>(i)])) > sigma)
                ++above;
        return above / (kErfcHalfRoot2 * win);
    };

    const int win = static_cast<int>(0.020 * kFs);

    SECTION("gamma = 2 grows density in proportion to Nd(u)") {
        NonlinAmbience engine;
        configure_naked(engine, NonlinProgram::ambience, 1000.0);
        const int window = engine.window_samples();
        const int lag = bypassed_diffuser_delay(engine);
        const Stereo ir = render_impulse(engine, window + lag + 2000);

        // With the tilt neutralised every tap spreads over the same number of
        // samples, so NED is proportional to Nd(u) with one unknown constant.
        // Solve that constant at the first probe and require the rest to follow
        // the law — that is the density claim, with nothing fitted per point.
        const double probes[] = {0.05, 0.2, 0.4, 0.6, 0.8, 0.95};
        double scale = 0.0;
        for (std::size_t i = 0; i < std::size(probes); ++i) {
            const double u = probes[i];
            const double ned = measure_ned(ir.left, lag + static_cast<int>(u * window), win);
            const double law = spec_density(u, na::kDensityRefPct, na::kGammaDefault);
            if (i == 0) {
                scale = ned / law;
                REQUIRE(scale > 0.0);
                continue;
            }
            INFO("u " << u << " NED " << ned << " law " << law << " scale " << scale);
            REQUIRE_THAT(ned, Catch::Matchers::WithinRel(scale * law, 0.20));
        }

        // Sparse early, dense late — Moorer 1979's t² growth, which is what the
        // shipped gamma is.
        const double early = measure_ned(ir.left, lag + static_cast<int>(0.05 * window), win);
        const double late = measure_ned(ir.left, lag + static_cast<int>(0.95 * window), win);
        const double law_ratio = spec_density(0.95, na::kDensityRefPct, na::kGammaDefault) /
                                 spec_density(0.05, na::kDensityRefPct, na::kGammaDefault);
        INFO("early " << early << " late " << late << " measured ratio " << late / early
                      << " law ratio " << law_ratio);
        REQUIRE(late > early * 2.0);
        REQUIRE_THAT(late / early, Catch::Matchers::WithinRel(law_ratio, 0.20));
    }

    SECTION("gamma = 0 holds density flat, which is how gated breaks physics") {
        NonlinAmbience engine;
        engine.prepare(kFs, na::kMaxLengthMs);
        engine.set_length_ms(1000.0);
        engine.set_density_growth(0.0);
        engine.set_diffusion(0.0);
        engine.set_hf_damp_hz(na::kFcBright);
        engine.reset();

        const int window = engine.window_samples();
        const int lag = bypassed_diffuser_delay(engine);
        const Stereo ir = render_impulse(engine, window + lag + 2000);

        const double early = measure_ned(ir.left, lag + static_cast<int>(0.1 * window), win);
        const double late = measure_ned(ir.left, lag + static_cast<int>(0.9 * window), win);
        INFO("early " << early << " late " << late);
        REQUIRE_THAT(late, Catch::Matchers::WithinRel(early, 0.15));
    }

    SECTION("a gated field's active-sample count falls to exactly zero") {
        NonlinAmbience engine;
        configure_naked(engine, NonlinProgram::gated, 1000.0);
        const int window = engine.window_samples();
        const int lag = bypassed_diffuser_delay(engine);
        const Stereo ir = render_impulse(engine, window + lag + 4000);

        const int probe =
            lag + static_cast<int>((na::kGateHold + na::kGateFall) * window) +
            segment_flush_samples(na::kFcBright);
        int active = 0;
        for (int i = probe; i < probe + win; ++i)
            if (ir.left[static_cast<std::size_t>(i)] != 0.0f) ++active;
        REQUIRE(active == 0);
    }
}

TEST_CASE("Nonlin ambience: the tap count follows the density integral",
          "[signal][nonlin-ambience][density]") {
    // Expected count is ∫₀^T Nd(t/T) dt = T·(Nd_min + (Nd_max − Nd_min)/(γ+1)),
    // computed from shipped constants. Spec T3 allows ±3 %; measured error is
    // under 0.2 %, so the tolerance is not doing any work — which is the point.
    auto expected_count = [](double length_ms, double density_pct, double gamma) {
        const double seconds = length_ms / 1000.0;
        const double nd_min = na::kNdMin * (density_pct / na::kDensityRefPct);
        return seconds * (nd_min + (na::kNdMax - nd_min) / (gamma + 1.0));
    };

    SECTION("count scales with length") {
        double previous = 0.0;
        for (double length_ms : {175.0, 350.0, 700.0}) {
            NonlinAmbience engine;
            engine.prepare(kFs, na::kMaxLengthMs);
            engine.set_length_ms(length_ms);
            engine.reset();
            const double expected =
                expected_count(length_ms, na::kDensityRefPct, na::kGammaDefault);
            INFO("length " << length_ms << " ms: " << engine.tap_count(0) << " taps, expected "
                           << expected);
            REQUIRE_THAT(static_cast<double>(engine.tap_count(0)),
                         Catch::Matchers::WithinRel(expected, 0.03));
            REQUIRE_THAT(static_cast<double>(engine.tap_count(1)),
                         Catch::Matchers::WithinRel(expected, 0.03));
            // Doubling the window doubles the count: γ acts on normalised u, so
            // the count is linear in absolute time.
            if (previous > 0.0)
                REQUIRE_THAT(engine.tap_count(0) / previous,
                             Catch::Matchers::WithinRel(2.0, 0.03));
            previous = engine.tap_count(0);
        }
    }

    SECTION("density_pct scales Nd_min only") {
        for (double pct : {10.0, 30.0, 60.0, 100.0}) {
            NonlinAmbience engine;
            engine.prepare(kFs, na::kMaxLengthMs);
            engine.set_length_ms(350.0);
            engine.set_density_pct(pct);
            engine.reset();
            INFO("density " << pct << " %");
            REQUIRE_THAT(static_cast<double>(engine.tap_count(0)),
                         Catch::Matchers::WithinRel(
                             expected_count(350.0, pct, na::kGammaDefault), 0.03));
        }
    }

    SECTION("a gated program stores only the taps inside its own gate") {
        // Taps whose envelope is exactly zero are past a hard gate and are
        // dropped rather than stored, which is what makes "the response is zero
        // after the last tap" a statement about the last AUDIBLE tap. So a
        // gated program's count is the density integral truncated at h + w, not
        // the full-window integral.
        NonlinAmbience gated, ambience;
        for (auto* engine : {&gated, &ambience}) {
            engine->prepare(kFs, na::kMaxLengthMs);
            engine->set_length_ms(300.0);
        }
        gated.set_program(NonlinProgram::gated);
        gated.reset();
        ambience.reset();

        const double seconds = 0.3;
        const double cut = na::kGateHold + na::kGateFall;
        const double truncated =
            seconds * (na::kNdMin * cut + (na::kNdMax - na::kNdMin) * std::pow(cut, 3) / 3.0);
        INFO("gated " << gated.tap_count(0) << " taps against a truncated integral of "
                      << truncated);
        REQUIRE_THAT(static_cast<double>(gated.tap_count(0)),
                     Catch::Matchers::WithinRel(truncated, 0.03));
        REQUIRE(gated.tap_count(0) < ambience.tap_count(0));
    }

    SECTION("the pre-sized table cannot be outgrown") {
        // The grid never steps less than fs/Nd_max samples, so the count is at
        // most (length_ms/1000)·Nd_max — analytically 8000 here, independent of
        // sample rate. In floating point the walk can take ONE extra step: at
        // 44.1 kHz the grid is 44100/4000 = 11.025 samples, which is not
        // representable, and 8000 accumulated additions land a hair below the
        // window end. That single tap is precisely what `kTapGuard` is for, and
        // this section is the assertion that the guard is doing real work rather
        // than being decorative.
        for (double rate : {44100.0, 48000.0, 96000.0, 192000.0}) {
            NonlinAmbience engine;
            engine.prepare(rate, na::kMaxLengthMs);
            engine.set_length_ms(na::kMaxLengthMs);
            engine.set_density_pct(100.0);
            engine.set_density_growth(0.0);
            engine.reset();

            const int analytic =
                static_cast<int>(std::ceil(na::kNdMax * na::kMaxLengthMs / 1000.0));
            const int capacity = analytic + na::kTapGuard;
            for (int ch = 0; ch < 2; ++ch) {
                INFO("rate " << rate << " channel " << ch << ": " << engine.tap_count(ch)
                             << " taps, analytic ceiling " << analytic << ", capacity "
                             << capacity);
                // Within one step of the analytic ceiling — the accumulation
                // slack, and nothing larger hiding behind the guard.
                REQUIRE(engine.tap_count(ch) <= analytic + 1);
                // And strictly inside capacity, which proves the generator's
                // hard stop never fired and silently truncated the field.
                REQUIRE(engine.tap_count(ch) < capacity);
            }
        }
    }
}

TEST_CASE("Nonlin ambience: renders are bit-identical for the same parameters",
          "[signal][nonlin-ambience][determinism]") {
    const int length = static_cast<int>(2.0 * kFs);
    const auto stimulus = pink_ish(length, 0xC0FFEEu);

    for (NonlinProgram program : kAllPrograms) {
        NonlinAmbience engine;
        engine.prepare(kFs, na::kMaxLengthMs);
        engine.set_program(program);
        engine.set_length_ms(150.0);
        engine.set_converter_amount(0.5);  // exercise the dither stream too
        engine.reset();

        auto run = [&] {
            std::vector<float> l = stimulus, r = stimulus;
            engine.process(l.data(), r.data(), length);
            return std::pair{l, r};
        };

        const auto first = run();
        engine.reset();
        const auto second = run();

        INFO("program " << program_name(program));
        REQUIRE(first.first == second.first);
        REQUIRE(first.second == second.second);

        // A round trip through another program and back rebuilds the tables;
        // the result must be the same tables, not merely similar ones.
        engine.set_program(NonlinProgram::reverse);
        engine.set_program(program);
        engine.reset();
        const auto third = run();
        REQUIRE(first.first == third.first);
        REQUIRE(first.second == third.second);
    }
}
