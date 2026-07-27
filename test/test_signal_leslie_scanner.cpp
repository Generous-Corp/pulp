#include "harness/leslie_test_support.hpp"

// ── 6. Scanner vibrato ────────────────────────────────────────────────────

TEST_CASE("the scanner shifts pitch by the slope of its own sweep",
          "[scanner][vibrato]") {
    // A linearly ramping delay is a CONSTANT pitch shift, so the depth follows
    // from the ramp's slope and nothing else: the scanner crosses `depth·line`
    // seconds of delay in each half period. The module publishes that
    // prediction and the render has to match it.
    const auto measure = [](ScannerMode mode) {
        Scanner s;
        s.prepare(kSr);
        s.set_mode(mode);
        s.reset();

        constexpr double kCarrierHz = 1000.0;
        const int n = static_cast<int>(kSr * 3.0);
        std::vector<double> rendered;
        rendered.reserve(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i)
            rendered.push_back(s.process(0.5 * std::sin(kTwoPi * kCarrierHz * i / kSr)));

        // 120 Hz is deliberate: wide enough to pass a 6.9 Hz square and its
        // first dozen harmonics, narrow enough that the image at 2 kHz cannot
        // put a ripple on the frequency trace. At 300 Hz that image alone reads
        // as 11 % of extra depth in the shallowest mode.
        const auto d = demodulate(rendered, kCarrierHz, kSr, 120.0, 24);
        return median_deviation(d.freq_hz, kCarrierHz) / kCarrierHz;
    };

    const double v1 = measure(ScannerMode::v1);
    const double v2 = measure(ScannerMode::v2);
    const double v3 = measure(ScannerMode::v3);

    Scanner reference;
    reference.prepare(kSr);
    for (auto mode : {ScannerMode::v1, ScannerMode::v2, ScannerMode::v3}) {
        reference.set_mode(mode);
        const double predicted = reference.peak_pitch_shift_ratio();
        const double got = mode == ScannerMode::v1 ? v1 : (mode == ScannerMode::v2 ? v2 : v3);
        REQUIRE_THAT(got, WithinRel(predicted, 0.05));
    }

    // The switch is a depth ladder in the V positions.
    REQUIRE(v1 < v2);
    REQUIRE(v2 < v3);

    // And the depths are the shipped fractions of one line, not three
    // independently tuned curves.
    REQUIRE_THAT(v1 / v3, WithinRel(Scanner::kV1 / Scanner::kV3, 0.05));
    REQUIRE_THAT(v2 / v3, WithinRel(Scanner::kV2 / Scanner::kV3, 0.05));
}

TEST_CASE("the chorus positions are a delay comb, not a level blend",
          "[scanner][chorus]") {
    // V and C are DIFFERENT EFFECTS, not two depths of one. A V position moves
    // the pitch and leaves the level alone; a C position adds the dry back, so
    // a stationary-pitch copy interferes with a moving-pitch one and the result
    // is a comb. Freezing the scanner mid-sweep makes that comb stand still
    // where its null can be located and compared with the delay that must have
    // produced it.
    const double mid_delay_s = Scanner::kV3 * Scanner::kLineDelayMs * 0.001 * 0.5;
    const double expected_null_hz = 1.0 / (2.0 * mid_delay_s);

    const auto magnitude_at = [](double hz) {
        Scanner s;
        s.prepare(kSr);
        s.set_mode(ScannerMode::c3);
        s.set_scan_hz(0.0);  // frozen at mid-sweep: phase 0 is the triangle's centre
        s.reset();
        const int n = static_cast<int>(kSr * 0.5);
        const int skip = n / 2;
        double re = 0.0;
        double im = 0.0;
        for (int i = 0; i < n; ++i) {
            const double y = s.process(std::sin(kTwoPi * hz * i / kSr));
            if (i >= skip) {
                re += y * std::cos(kTwoPi * hz * i / kSr);
                im += y * std::sin(kTwoPi * hz * i / kSr);
            }
        }
        return 2.0 * std::hypot(re, im) / static_cast<double>(n - skip);
    };

    double null_hz = 0.0;
    double null_mag = 1e9;
    for (double hz = expected_null_hz * 0.8; hz <= expected_null_hz * 1.2; hz += 5.0) {
        const double m = magnitude_at(hz);
        if (m < null_mag) {
            null_mag = m;
            null_hz = hz;
        }
    }
    REQUIRE_THAT(null_hz, WithinRel(expected_null_hz, 0.02));

    // A genuine cancellation, not a dip: the null is far below the passband
    // peaks either side of it. A level blend could not do this at all.
    const double away = magnitude_at(expected_null_hz * 0.5);
    REQUIRE(null_mag < 0.1 * away);

    // The matching V position has no comb at all — the pitch moves, the
    // magnitude does not.
    Scanner vibrato;
    vibrato.prepare(kSr);
    vibrato.set_mode(ScannerMode::v3);
    vibrato.set_scan_hz(0.0);
    vibrato.reset();
    double re = 0.0;
    double im = 0.0;
    const int n = static_cast<int>(kSr * 0.5);
    for (int i = 0; i < n; ++i) {
        const double y = vibrato.process(std::sin(kTwoPi * expected_null_hz * i / kSr));
        if (i >= n / 2) {
            re += y * std::cos(kTwoPi * expected_null_hz * i / kSr);
            im += y * std::sin(kTwoPi * expected_null_hz * i / kSr);
        }
    }
    const double vibrato_mag = 2.0 * std::hypot(re, im) / static_cast<double>(n - n / 2);
    REQUIRE(vibrato_mag > 0.9);
}

TEST_CASE("the scanner's off position is a bit-exact bypass", "[scanner]") {
    Scanner s;
    s.prepare(kSr);
    s.set_mode(ScannerMode::off);
    s.reset();
    for (int i = 0; i < 4800; ++i) {
        const double x = 0.7 * std::sin(kTwoPi * 220.0 * i / kSr);
        REQUIRE(s.process(x) == x);
    }
}

TEST_CASE("the 50 Hz-mains rate is derived from the 60 Hz one", "[scanner]") {
    // Rather than inventing a second unverified number: the scanner is geared
    // off the mains-synchronous motor, so the European rate is the same gearing
    // at a different mains frequency.
    REQUIRE_THAT(Scanner::kScanHz50, WithinRel(Scanner::kScanHz * 50.0 / 60.0, 1e-12));
}

// ── 7. The feedforward gain bound ─────────────────────────────────────────

TEST_CASE("the constructive-sum bound holds across the parameter space",
          "[leslie][scanner][gain]") {
    // Neither engine has a feedback path, so there is no loop gain to bound —
    // series law 1 is satisfied by structure, not by compensation. What the
    // registry needs instead is the worst constructive sum, and law 8 says it
    // must be a TESTED invariant. This is that test; the registry cites its
    // measured maximum, with the shipped constant as the budget.
    double leslie_max = 0.0;
    for (auto speed : {LeslieSpeed::stop, LeslieSpeed::chorale, LeslieSpeed::tremolo}) {
        for (double radius : {0.10, 0.25}) {
            for (double am : {0.0, 0.9}) {
                for (double dir : {0.0, 12.0}) {
                    for (double refl : {-60.0, -6.0}) {
                        for (double mix : {0.5, 1.0}) {
                            Leslie l;
                            l.prepare(kSr);
                            l.set_speed(speed);
                            l.set_horn_radius_m(radius);
                            l.set_drum_radius_m(std::min(radius, 0.18));
                            l.set_am_depth(am);
                            l.set_dir_depth_db(dir);
                            l.set_drum_dir_depth_db(std::min(dir, 6.0));
                            l.set_reflection_db(refl);
                            l.set_num_reflections(Leslie::kMaxReflections);
                            l.set_mix(mix);
                            l.reset();

                            double peak_in = 0.0;
                            double peak_out = 0.0;
                            for (int i = 0; i < static_cast<int>(kSr * 1.0); ++i) {
                                // An impulse to excite every tap, then a
                                // full-scale sweep to find where they align.
                                double x = 0.0;
                                if (i < 64) {
                                    x = i == 0 ? 1.0 : 0.0;
                                } else {
                                    const double t = (i - 64) / kSr;
                                    x = std::sin(kTwoPi * 20.0 * std::pow(1000.0, t / 0.9) * t);
                                }
                                double a = 0.0;
                                double b = 0.0;
                                l.process(x, a, b);
                                peak_in = std::max(peak_in, std::abs(x));
                                peak_out = std::max({peak_out, std::abs(a), std::abs(b)});
                            }
                            leslie_max = std::max(leslie_max, peak_out / peak_in);
                        }
                    }
                }
            }
        }
    }
    INFO("Forge registry worst_case_gain for LeslieRotary: " << leslie_max);
    REQUIRE(leslie_max <= Leslie::kWorstCaseGain);
    // Not a vacuous ceiling — the sweep gets within reach of it.
    REQUIRE(leslie_max > 0.5 * Leslie::kWorstCaseGain);

    double scanner_max = 0.0;
    for (auto mode : {ScannerMode::off, ScannerMode::v3, ScannerMode::c1, ScannerMode::c3}) {
        for (double hz : {0.0, 7.5}) {
            for (double line : {0.6, 1.4}) {
                for (double chorus : {0.0, 0.5, 1.0}) {
                    Scanner s;
                    s.prepare(kSr);
                    s.set_mode(mode);
                    s.set_scan_hz(hz);
                    s.set_line_ms(line);
                    s.set_chorus_mix(chorus);
                    s.reset();
                    double peak_in = 0.0;
                    double peak_out = 0.0;
                    for (int i = 0; i < static_cast<int>(kSr * 1.0); ++i) {
                        double x = 0.0;
                        if (i < 64) {
                            x = i == 0 ? 1.0 : 0.0;
                        } else {
                            const double t = (i - 64) / kSr;
                            x = std::sin(kTwoPi * 20.0 * std::pow(1000.0, t / 0.9) * t);
                        }
                        peak_in = std::max(peak_in, std::abs(x));
                        peak_out = std::max(peak_out, std::abs(s.process(x)));
                    }
                    scanner_max = std::max(scanner_max, peak_out / peak_in);
                }
            }
        }
    }
    INFO("Forge registry worst_case_gain for ScannerVibrato: " << scanner_max);
    REQUIRE(scanner_max <= Scanner::kWorstCaseGain);
    REQUIRE(scanner_max > 0.5 * Scanner::kWorstCaseGain);
}
