#include "test_signal_vocoder_support.hpp"

TEST_CASE("vocoder band centres follow the geometric ratio", "[signal][vocoder]") {
    auto v = make_bank();
    const double ratio =
        std::pow(kHiHz / kLoHz, 1.0 / static_cast<double>(kBands - 1));
    INFO("r = " << v.band_ratio() << " (recomputed " << ratio << ")");
    REQUIRE_THAT(v.band_ratio(), WithinRel(ratio, 1e-12));

    for (int k = 0; k < kBands; ++k) {
        const double expected = kLoHz * std::pow(ratio, static_cast<double>(k));
        REQUIRE_THAT(v.band_center_hz(k), WithinRel(expected, 1e-12));

        const auto peak = locate_peak(
            [&](double hz) { return analysis_magnitude(v, k, hz); }, expected);
        INFO("band " << k << ": table " << expected << " Hz, measured peak " << peak.hz
                     << " Hz, gain " << peak.magnitude);
        REQUIRE_THAT(peak.hz, WithinRel(expected, 0.03));

        // The bands are unity-peak by construction — the SVF's bandpass output
        // peaks at Q, and two cascaded sections at Q², so the normalisation is
        // 1/Q². Every gain claim in the module depends on this being 1.
        REQUIRE_THAT(peak.magnitude, WithinRel(1.0, 0.02));
    }
}

TEST_CASE("vocoder analysis and synthesis banks are matched", "[signal][vocoder]") {
    // The failure this catches is the classic one: the two banks drifting apart
    // gives a reconstruction whose formants are detuned from the modulator's,
    // and it sounds like a tuning fault rather than a filter fault. Measuring
    // the synthesis side through the OUTPUT (not through an accessor) is what
    // makes it a real check.
    auto v = make_bank();
    v.set_carrier_source(Voc::CarrierSource::external);
    v.set_sibilance_mix(0.0);
    v.set_dry_wet(1.0);

    for (int k : {0, 5, 10, 15}) {
        const double expected = v.band_center_hz(k);
        // Open exactly one synthesis band: silence the modulator, then latch
        // the (zero) envelopes and write the one gain we want through freeze.
        // Freeze is a value copy, so a held bank with one band open is a
        // legitimate configuration rather than a test-only hook.
        v.reset();
        // Drive band k alone, tracking each band's envelope PEAK over the last
        // stretch rather than snapshotting it. Band envelopes ripple — 14 % at
        // the bottom of the bank, which is the whole subject of the follower
        // floors — so a single instant compares two values at unrelated ripple
        // phases. The peak is also what `reference_settled` predicts, so the
        // two sides of the comparison are the same quantity.
        std::array<double, static_cast<std::size_t>(Voc::kMaxBands)> peak_envelope{};
        {
            const auto total = static_cast<std::size_t>(0.6 * kSr);
            const auto measure_from = static_cast<std::size_t>(0.5 * kSr);
            double scratch = 0.0;
            for (std::size_t i = 0; i < total; ++i) {
                v.process(std::sin(2.0 * kPi * expected * static_cast<double>(i) / kSr), 0.0,
                          scratch);
                if (i < measure_from) continue;
                for (int j = 0; j < kBands; ++j)
                    peak_envelope[static_cast<std::size_t>(j)] =
                        std::max(peak_envelope[static_cast<std::size_t>(j)], v.band_envelope(j));
            }
            v.set_formant_freeze(true);
            v.process(0.0, 0.0, scratch);  // the latch edge
        }

        const double reference_k =
            reference_settled(expected, v.attack_eff_ms(k), v.release_eff_ms(k));
        for (int j = 0; j < kBands; ++j) {
            if (j == k) continue;
            // Predicted ENVELOPE ratio, not response ratio: the bank's own
            // response at the neighbour's detuning, times the fraction of the
            // peak that neighbour's follower settles at. Those fractions differ
            // between bands because the floors do.
            const double predicted =
                cascade_response(expected, v.band_center_hz(j), v.section_q()) *
                reference_settled(expected, v.attack_eff_ms(j), v.release_eff_ms(j)) /
                reference_k;
            const double measured = peak_envelope[static_cast<std::size_t>(j)] /
                                    peak_envelope[static_cast<std::size_t>(k)];
            INFO("band " << j << " reads " << measured << " of band " << k
                         << ", bank + follower predict " << predicted);
            REQUIRE_THAT(measured, WithinAbs(predicted, 0.03));
        }

        const auto peak = locate_peak(
            [&](double hz) { return synthesis_magnitude(v, k, hz); }, expected);
        INFO("synthesis band " << k << ": analysis centre " << expected << " Hz, synthesis peak "
                               << peak.hz << " Hz at magnitude " << peak.magnitude);
        // A silent bank would let the peak search return its grid edge and look
        // like a spacing failure, so the level is asserted before the location.
        REQUIRE(peak.magnitude > 1e-3);
        REQUIRE_THAT(peak.hz, WithinRel(expected, 0.03));
        v.set_formant_freeze(false);
    }
}

TEST_CASE("vocoder band selectivity matches the computed Q", "[signal][vocoder]") {
    auto v = make_bank();
    const double q_band = v.band_q();
    // Q_band recomputed from the ratio, not read back from the module.
    const double root = std::sqrt(v.band_ratio());
    REQUIRE_THAT(q_band, WithinRel(1.0 / (root - 1.0 / root), 1e-12));

    for (int k : {4, 8, 12}) {
        const double center = v.band_center_hz(k);
        auto magnitude = [&](double hz) { return analysis_magnitude(v, k, hz); };
        const auto peak = locate_peak(magnitude, center);
        const double lower = bisect_edge(magnitude, peak.magnitude, peak.hz, peak.hz / 2.0);
        const double upper = bisect_edge(magnitude, peak.magnitude, peak.hz, peak.hz * 2.0);
        const double bandwidth = upper - lower;
        const double expected = center / q_band;
        INFO("band " << k << " (f_c " << center << " Hz): −3 dB span " << lower << " … " << upper
                     << " = " << bandwidth << " Hz, expected f_c/Q_band = " << expected);
        REQUIRE_THAT(bandwidth, WithinRel(expected, 0.10));

        // One octave out, against the cascade's own prewarped transfer
        // function computed from the shipped section Q.
        const double predicted_db =
            20.0 * std::log10(cascade_response(center * 2.0, center, v.section_q()));
        const double measured_db =
            20.0 * std::log10(magnitude(center * 2.0) / peak.magnitude);
        INFO("one octave up: measured " << measured_db << " dB, predicted " << predicted_db
                                        << " dB");
        REQUIRE_THAT(measured_db, WithinAbs(predicted_db, 0.3));
        REQUIRE(measured_db <= -20.0);

        // ... and the asymptote is 12 dB/oct, not the 24 the spec's
        // parenthetical claims. Only measured where the far point is still well
        // inside the band: eight times band 12's centre is 24.8 kHz, above
        // Nyquist, and near Nyquist the bilinear transform's zero pulls the
        // digital response down far faster than any analogue asymptote — that
        // steepness is the transform, not the skirt.
        if (center * 8.0 < 0.30 * kSr) {
            const double octave_4 = 20.0 * std::log10(magnitude(center * 4.0) / peak.magnitude);
            const double octave_8 = 20.0 * std::log10(magnitude(center * 8.0) / peak.magnitude);
            INFO("asymptotic skirt " << (octave_8 - octave_4) << " dB/oct");
            REQUIRE(octave_8 - octave_4 > -20.0);
            REQUIRE(octave_8 - octave_4 < -11.0);
        }
    }
}

TEST_CASE("vocoder cascade bandwidth factor is the shipped identity", "[signal][vocoder]") {
    // kCascadeBWFactor is algebra, not a citation, so it is checked against the
    // algebra: √(2^(1/n) − 1) at n = 2.
    REQUIRE_THAT(Voc::kCascadeBWFactor, WithinRel(std::sqrt(std::sqrt(2.0) - 1.0), 1e-12));

    auto v = make_bank();
    REQUIRE_THAT(v.section_q(), WithinRel(Voc::kCascadeBWFactor * v.band_q(), 1e-12));

    // And the factor is what the cascade actually delivers: one section at
    // Q_section would be f_c/Q_section wide; the pair measures narrower by
    // kCascadeBWFactor.
    const int k = 8;
    const double center = v.band_center_hz(k);
    auto magnitude = [&](double hz) { return analysis_magnitude(v, k, hz); };
    const auto peak = locate_peak(magnitude, center);
    const double lower = bisect_edge(magnitude, peak.magnitude, peak.hz, peak.hz / 2.0);
    const double upper = bisect_edge(magnitude, peak.magnitude, peak.hz, peak.hz * 2.0);
    const double measured_factor = (upper - lower) / (center / v.section_q());
    INFO("measured cascade narrowing " << measured_factor << " against " << Voc::kCascadeBWFactor);
    REQUIRE_THAT(measured_factor, WithinRel(Voc::kCascadeBWFactor, 0.10));
}

TEST_CASE("vocoder follower floors follow the per-band law", "[signal][vocoder]") {
    // The actual normative content of §4, and it is exactly testable.
    for (int bands : {10, 16, 20}) {
        auto v = make_bank(bands);
        for (double attack_ms : {0.1, 1.5, 50.0}) {
            for (double release_ms : {2.0, 15.0, 200.0}) {
                v.set_attack_ms(attack_ms);
                v.set_release_ms(release_ms);
                for (int k = 0; k < bands; ++k) {
                    const double center = v.band_center_hz(k);
                    const double attack_expected =
                        std::max(attack_ms, 1000.0 * Voc::kAttackCycles / center);
                    const double release_expected =
                        std::max(release_ms, 1000.0 * Voc::kRippleCycles / center);
                    INFO("bands " << bands << " band " << k << " f_c " << center);
                    REQUIRE_THAT(v.attack_eff_ms(k), WithinRel(attack_expected, 1e-12));
                    REQUIRE_THAT(v.release_eff_ms(k), WithinRel(release_expected, 1e-12));
                }
            }
        }
    }

    // The floors bite at the bottom and not at the top — the whole point of
    // expressing them in cycles.
    auto v = make_bank();
    v.set_attack_ms(1.5);
    v.set_release_ms(15.0);
    REQUIRE(v.release_eff_ms(0) > 15.0);
    REQUIRE_THAT(v.release_eff_ms(kBands - 1), WithinRel(15.0, 1e-12));
    REQUIRE(v.attack_eff_ms(0) > 1.5);
    REQUIRE_THAT(v.attack_eff_ms(kBands - 1), WithinRel(1.5, 1e-12));
}

TEST_CASE("vocoder envelope ballistics match the shipped follower", "[signal][vocoder]") {
    auto v = make_bank();
    v.set_attack_ms(1.5);
    v.set_release_ms(15.0);

    struct Measurement {
        double steady;
        double ripple;
        double release_90_10_ms;
        double attack_10_90_ms;
    };
    auto measure = [&](int k) {
        const double center = v.band_center_hz(k);
        const auto gate = static_cast<std::size_t>(0.40 * kSr);
        const auto total = static_cast<std::size_t>(0.80 * kSr);
        v.reset();
        std::vector<double> envelope(total);
        double out = 0.0;
        for (std::size_t i = 0; i < total; ++i) {
            const double m =
                i < gate ? std::sin(2.0 * kPi * center * static_cast<double>(i) / kSr) : 0.0;
            v.process(m, 0.0, out);
            envelope[i] = v.band_envelope(k);
        }
        const auto settled = gate - static_cast<std::size_t>(0.05 * kSr);
        double high = 0.0;
        double low = 1e30;
        for (std::size_t i = settled; i < gate; ++i) {
            high = std::max(high, envelope[i]);
            low = std::min(low, envelope[i]);
        }
        std::size_t a10 = 0;
        std::size_t a90 = 0;
        for (std::size_t i = 0; i < gate; ++i) {
            if (a10 == 0 && envelope[i] >= 0.1 * high) a10 = i;
            if (a90 == 0 && envelope[i] >= 0.9 * high) {
                a90 = i;
                break;
            }
        }
        std::size_t r90 = 0;
        std::size_t r10 = 0;
        for (std::size_t i = gate; i < total; ++i) {
            if (r90 == 0 && envelope[i] <= 0.9 * high) r90 = i;
            if (r90 != 0 && envelope[i] <= 0.1 * high) {
                r10 = i;
                break;
            }
        }
        return Measurement{high, (high - low) / high,
                           1000.0 * static_cast<double>(r10 - r90) / kSr,
                           1000.0 * static_cast<double>(a90 - a10) / kSr};
    };

    SECTION("ripple matches the reference follower at every band") {
        for (int k : {0, 4, 8, 12, 15}) {
            const auto measured = measure(k);
            const double predicted =
                reference_ripple(v.band_center_hz(k), v.attack_eff_ms(k), v.release_eff_ms(k));
            INFO("band " << k << " (f_c " << v.band_center_hz(k) << "): measured ripple "
                         << measured.ripple << " (" << 20.0 * std::log10(measured.ripple)
                         << " dB), reference " << predicted << " ("
                         << 20.0 * std::log10(predicted) << " dB)");
            REQUIRE_THAT(measured.ripple, WithinAbs(predicted, 0.02));
        }
    }

    SECTION("the -40 dB ripple criterion is met at the top of the bank and not at the bottom") {
        // Recorded with the numbers that show why, rather than argued.
        const double top = measure(kBands - 1).ripple;
        const double bottom = measure(0).ripple;
        INFO("band " << kBands - 1 << " ripple " << 20.0 * std::log10(top) << " dB, band 0 ripple "
                     << 20.0 * std::log10(bottom) << " dB");
        REQUIRE(20.0 * std::log10(top) < -35.0);
        REQUIRE(20.0 * std::log10(bottom) > -25.0);

        // Even at the top of its declared range, kRippleCycles cannot deliver
        // −40 dB at band 0: the reference follower says so directly.
        const double widest_floor = 1000.0 * 4.0 / v.band_center_hz(0);  // kRippleCycles max
        const double best_case = reference_ripple(v.band_center_hz(0), v.attack_eff_ms(0),
                                                  std::max(15.0, widest_floor));
        INFO("band 0 ripple at kRippleCycles = 4 (range max): " << 20.0 * std::log10(best_case)
                                                                << " dB");
        REQUIRE(20.0 * std::log10(best_case) > -40.0);
    }

    SECTION("release through a band whose ring is fast equals the floored time") {
        // At the top of the bank the band's own ring time is
        // Q_section/(π·f_c), two orders of magnitude below the follower's, so
        // what is measured is the follower and the criterion holds.
        const int k = kBands - 1;
        const double ring_ms = 1000.0 * v.section_q() / (kPi * v.band_center_hz(k));
        INFO("band " << k << " ring time " << ring_ms << " ms vs release floor "
                     << v.release_eff_ms(k) << " ms");
        REQUIRE(ring_ms < 0.02 * v.release_eff_ms(k));
        const auto measured = measure(k);
        INFO("measured release 90→10 " << measured.release_90_10_ms << " ms");
        REQUIRE_THAT(measured.release_90_10_ms, WithinRel(v.release_eff_ms(k), 0.15));
    }

    SECTION("at the bottom of the bank the band's own ring dominates the measurement") {
        const int k = 0;
        const double ring_ms = 1000.0 * v.section_q() / (kPi * v.band_center_hz(k));
        const auto measured = measure(k);
        INFO("band 0: ring " << ring_ms << " ms, release floor " << v.release_eff_ms(k)
                             << " ms, measured 90→10 " << measured.release_90_10_ms << " ms");
        // The ring is comparable to the follower here, so the composite is
        // necessarily slower than the floor — by about a factor of two.
        REQUIRE(ring_ms > 0.3 * v.release_eff_ms(k));
        REQUIRE(measured.release_90_10_ms > 1.5 * v.release_eff_ms(k));
        REQUIRE(measured.release_90_10_ms < 2.5 * v.release_eff_ms(k));
    }

    SECTION("attack against a sinusoid is about twice the follower's own time") {
        // Not a fault: a follower attacks only while its input exceeds its
        // state, which is about half of each half-cycle.
        const auto measured = measure(kBands - 1);
        INFO("band " << kBands - 1 << ": measured attack 10→90 " << measured.attack_10_90_ms
                     << " ms against a floor of " << v.attack_eff_ms(kBands - 1) << " ms");
        REQUIRE(measured.attack_10_90_ms > 1.4 * v.attack_eff_ms(kBands - 1));
        REQUIRE(measured.attack_10_90_ms < 2.6 * v.attack_eff_ms(kBands - 1));
    }
}

TEST_CASE("vocoder voicing detector separates buzz from hiss", "[signal][vocoder]") {
    auto v = make_bank();

    auto settle = [&](const std::vector<double>& modulator) {
        v.reset();
        const std::vector<double> silence(modulator.size(), 0.0);
        render(v, modulator, silence);
        return v.unvoiced();
    };

    const auto noise = seeded_noise(static_cast<std::size_t>(0.5 * kSr), 0.5, 0x51F0u);
    const double u_noise = settle(noise);
    const double zcr_noise = v.zcr_hz();
    const auto saw = sawtooth(static_cast<std::size_t>(0.5 * kSr), 150.0, 0.8);
    const double u_saw = settle(saw);
    const double zcr_saw = v.zcr_hz();

    INFO("noise: u = " << u_noise << ", zcr = " << zcr_noise << " Hz; saw: u = " << u_saw
                       << ", zcr = " << zcr_saw << " Hz");
    REQUIRE_THAT(u_noise, WithinAbs(1.0, 0.05));
    REQUIRE_THAT(u_saw, WithinAbs(0.0, 0.05));

    // The ZCR instrument itself, against ground truth: a sawtooth at f0 crosses
    // zero exactly twice per cycle, and white noise's consecutive samples are
    // independent so it crosses on about half of them.
    REQUIRE_THAT(zcr_saw, WithinRel(300.0, 0.02));
    REQUIRE_THAT(zcr_noise, WithinRel(0.5 * kSr, 0.05));
    REQUIRE_THAT(v.zcr_window_ms(), WithinRel(Voc::kZcrWindowMs, 0.01));
}

TEST_CASE("vocoder voicing transition is smooth and does not chatter", "[signal][vocoder]") {
    auto v = make_bank();
    const auto half = static_cast<std::size_t>(0.30 * kSr);
    auto modulator = sawtooth(half, 150.0, 0.8);
    const auto hiss = seeded_noise(half, 0.5, 0x0A0Bu);
    modulator.insert(modulator.end(), hiss.begin(), hiss.end());
    const std::vector<double> silence(modulator.size(), 0.0);

    v.reset();
    std::vector<double> trace(modulator.size());
    double out = 0.0;
    for (std::size_t i = 0; i < modulator.size(); ++i) {
        v.process(modulator[i], silence[i], out);
        trace[i] = v.unvoiced();
    }

    // The one-pole on the latched decision is specified as a 10→90 % time, so
    // that is what is measured — computed from the shipped constant, not
    // restated.
    std::size_t t10 = 0;
    std::size_t t90 = 0;
    for (std::size_t i = half; i < trace.size(); ++i) {
        if (t10 == 0 && trace[i] >= 0.1) t10 = i;
        if (t90 == 0 && trace[i] >= 0.9) {
            t90 = i;
            break;
        }
    }
    REQUIRE(t90 > t10);
    const double measured_ms = 1000.0 * static_cast<double>(t90 - t10) / kSr;
    INFO("voiced→unvoiced 10→90 % in " << measured_ms << " ms against kUvSmoothMs = "
                                       << Voc::kUvSmoothMs << " ms");
    REQUIRE_THAT(measured_ms, WithinAbs(Voc::kUvSmoothMs, 2.0));

    // No chatter: once settled, the decision is monotone in each half and does
    // not oscillate back across the middle.
    int crossings = 0;
    bool above = false;
    for (std::size_t i = static_cast<std::size_t>(0.02 * kSr); i < trace.size(); ++i) {
        const bool now = trace[i] > 0.5;
        if (now != above) ++crossings;
        above = now;
    }
    INFO("decision crossed 0.5 " << crossings << " times over one voiced→unvoiced transition");
    REQUIRE(crossings == 1);
}

TEST_CASE("vocoder voicing hysteresis holds across level wobble", "[signal][vocoder]") {
    // ±1 dB of level wobble on the modulator must not move the decision. It
    // cannot in principle — both cues are ratios (a crossing rate and an energy
    // fraction), neither of which depends on level — and this pins that the
    // implementation did not accidentally introduce a level dependence.
    auto v = make_bank();
    const auto n = static_cast<std::size_t>(0.4 * kSr);
    const std::vector<double> silence(n, 0.0);

    for (double db : {-1.0, 0.0, 1.0}) {
        const double gain = units::db_to_linear(db);
        for (int voiced = 0; voiced < 2; ++voiced) {
            auto modulator = voiced != 0 ? sawtooth(n, 150.0, 0.8 * gain)
                                         : seeded_noise(n, 0.5 * gain, 0x77u);
            v.reset();
            render(v, modulator, silence);
            INFO((voiced != 0 ? "saw" : "noise") << " at " << db << " dB: u = " << v.unvoiced());
            if (voiced != 0) REQUIRE(v.unvoiced() < 0.05);
            else REQUIRE(v.unvoiced() > 0.95);
        }
    }
}

TEST_CASE("vocoder formant shift moves the spectral envelope by whole octaves",
          "[signal][vocoder]") {
    auto centroid = [](double semitones) {
        auto v = make_bank();
        v.set_carrier_source(Voc::CarrierSource::external);
        v.set_sibilance_mix(0.0);
        v.set_formant_shift_semitones(semitones);
        // A white carrier makes the output spectrum the synthesis bank's own
        // shape; a harmonic carrier would weight the measurement by its own
        // comb.
        const auto settle = static_cast<std::size_t>(0.5 * kSr);
        const std::size_t window = 32768;
        const auto carrier = seeded_noise(settle + window, 0.5, 0xC0DEu);
        const auto modulator = sine(settle + window, v.band_center_hz(5), 1.0);
        const auto out = render(v, modulator, carrier);
        const std::vector<double> tail(out.begin() + static_cast<std::ptrdiff_t>(settle),
                                       out.end());

        double numerator = 0.0;
        double denominator = 0.0;
        for (double hz = 60.0; hz < 12000.0; hz *= 1.05) {
            const double energy = std::pow(coherent_magnitude(tail, hz), 2.0);
            numerator += std::log(hz) * energy;
            denominator += energy;
        }
        return std::exp(numerator / denominator);
    };

    const double base = centroid(0.0);
    const double up = centroid(12.0);
    const double down = centroid(-12.0);
    INFO("log-spectral centroid: " << base << " Hz → " << up << " Hz (+12 st, ratio "
                                   << up / base << ") and " << down << " Hz (−12 st, ratio "
                                   << down / base << ")");
    // r^offset_bands with offset_bands = 12/(12·log2 r) is exactly 2, whatever
    // the ratio is — the control is scale-invariant by construction.
    REQUIRE_THAT(up / base, WithinRel(2.0, 0.05));
    REQUIRE_THAT(down / base, WithinRel(0.5, 0.05));
}

TEST_CASE("vocoder formant shift maps semitones through the bank's own ratio",
          "[signal][vocoder]") {
    for (int bands : {10, 16, 20}) {
        auto v = make_bank(bands);
        const double expected_per_octave = 1.0 / std::log2(v.band_ratio());
        REQUIRE_THAT(v.bands_per_octave(), WithinRel(expected_per_octave, 1e-12));
        for (double st : {-24.0, -12.0, 0.0, 7.0, 12.0, 24.0}) {
            v.set_formant_shift_semitones(st);
            const double expected = st / (12.0 * std::log2(v.band_ratio()));
            INFO("bands " << bands << ", " << st << " st → " << v.shift_bands() << " bands");
            REQUIRE_THAT(v.shift_bands(), WithinRel(expected, 1e-12));
        }
    }

    // The routing itself: with one band held at a known level, the shifted bank
    // reads that level from the neighbours the offset points at, and the ends
    // clamp to zero instead of wrapping.
    auto v = make_bank();
    v.set_formant_shift_semitones(0.0);
    const auto tone = sine(static_cast<std::size_t>(0.5 * kSr), v.band_center_hz(8), 4.0);
    const std::vector<double> silence(tone.size(), 0.0);
    v.reset();
    render(v, tone, silence);
    v.set_formant_freeze(true);
    double scratch = 0.0;
    v.process(0.0, 0.0, scratch);
    const double held = v.synthesis_gain(8);
    REQUIRE(held > 0.5);

    // A whole number of bands of shift is an exact re-index of the level
    // array. The first draft asserted that band 8 went quiet, which it does
    // not and should not: it now reads band 6, whose envelope is the bank's own
    // two-band overlap and is not zero. The identity below is both exact and
    // the thing the control actually promises.
    std::array<double, static_cast<std::size_t>(Voc::kMaxBands)> before{};
    for (int k = 0; k < v.band_count(); ++k) before[static_cast<std::size_t>(k)] = v.synthesis_gain(k);

    const double semitones_per_band = 12.0 * std::log2(v.band_ratio());
    v.set_formant_shift_semitones(2.0 * semitones_per_band);
    v.process(0.0, 0.0, scratch);
    REQUIRE_THAT(v.shift_bands(), WithinAbs(2.0, 1e-9));
    for (int j = 0; j < v.band_count(); ++j) {
        const int source = j - 2;
        const double expected = source >= 0 ? before[static_cast<std::size_t>(source)] : 0.0;
        INFO("after a 2-band shift, gain[" << j << "] should read band " << source);
        REQUIRE_THAT(v.synthesis_gain(j), WithinAbs(expected, 1e-9));
    }
    // ... including the ends, which clamp to zero rather than wrapping.
    REQUIRE(v.synthesis_gain(0) == 0.0);
    REQUIRE(v.synthesis_gain(1) == 0.0);

    // Shifting the whole bank off the end drops it rather than folding the
    // bottom of the bank into the top, which would be audible and wrong.
    v.set_formant_shift_semitones(Voc::kFormantShiftMaxSt);
    v.process(0.0, 0.0, scratch);
    INFO("+" << Voc::kFormantShiftMaxSt << " st = " << v.shift_bands() << " bands");
    REQUIRE(v.shift_bands() > 5.0);
    for (int j = 0; j < 5; ++j) REQUIRE(v.synthesis_gain(j) == 0.0);
}

TEST_CASE("vocoder formant shift leaves the carrier's pitch alone", "[signal][vocoder]") {
    // The separation is the whole point of the control, so it is measured
    // rather than argued: with a harmonic carrier the output's energy must stay
    // on the carrier's harmonic comb at every shift setting.
    const double pitch = 150.0;
    auto harmonic_fraction = [&](double semitones) {
        auto v = make_bank();
        v.set_carrier_source(Voc::CarrierSource::internal);
        v.set_internal_wave(Voc::InternalWave::saw);
        v.set_internal_pitch_hz(pitch);
        v.set_noise_mix(0.0);
        v.set_sibilance_mix(0.0);
        v.set_formant_shift_semitones(semitones);
        const auto settle = static_cast<std::size_t>(0.5 * kSr);
        const std::size_t window = 32768;
        const auto modulator = sine(settle + window, v.band_center_hz(6), 1.0);
        const std::vector<double> no_carrier(settle + window, 0.0);
        const auto out = render(v, modulator, no_carrier);
        const std::vector<double> tail(out.begin() + static_cast<std::ptrdiff_t>(settle),
                                       out.end());

        double on_comb = 0.0;
        double off_comb = 0.0;
        for (int h = 1; h <= 40; ++h) {
            on_comb += std::pow(coherent_magnitude(tail, pitch * h), 2.0);
            // Midway between harmonics: energy here would mean the shift moved
            // pitch, not formants.
            off_comb += std::pow(coherent_magnitude(tail, pitch * (h + 0.5)), 2.0);
        }
        return off_comb / (on_comb + 1e-30);
    };

    for (double st : {-12.0, 0.0, 12.0}) {
        const double leak = harmonic_fraction(st);
        INFO(st << " st: inter-harmonic energy is " << 10.0 * std::log10(leak + 1e-30)
                << " dB below the comb");
        REQUIRE(leak < 0.01);
    }
}

TEST_CASE("vocoder formant freeze holds the spectral envelope", "[signal][vocoder]") {
    auto v = make_bank();
    v.set_carrier_source(Voc::CarrierSource::internal);
    v.set_internal_pitch_hz(110.0);
    v.set_noise_mix(0.0);

    // Track a two-formant "vowel", then freeze and feed silence for 5 s.
    const auto n = static_cast<std::size_t>(0.5 * kSr);
    std::vector<double> vowel(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / kSr;
        vowel[i] = 0.5 * std::sin(2.0 * kPi * v.band_center_hz(4) * t) +
                   0.4 * std::sin(2.0 * kPi * v.band_center_hz(11) * t);
    }
    const std::vector<double> silence(n, 0.0);
    v.reset();
    render(v, vowel, silence);

    v.set_formant_freeze(true);
    double scratch = 0.0;
    v.process(0.0, 0.0, scratch);
    std::array<double, static_cast<std::size_t>(Voc::kMaxBands)> latched{};
    for (int k = 0; k < v.band_count(); ++k) latched[static_cast<std::size_t>(k)] = v.synthesis_gain(k);

    // The latched vector must be the vowel: a local maximum at each formant.
    // An absolute level threshold would be arbitrary here, because
    // pre-emphasis is a differentiator and costs band 4 (355 Hz) a factor of
    // 15 before the follower ever sees it — the position is the claim, not the
    // level.
    for (int k : {4, 11}) {
        REQUIRE(latched[static_cast<std::size_t>(k)] > 0.0);
        for (int d : {-2, -1, 1, 2}) {
            const int j = k + d;
            if (j < 0 || j >= v.band_count()) continue;
            INFO("formant at band " << k << " (" << latched[static_cast<std::size_t>(k)]
                                    << ") against neighbour " << j << " ("
                                    << latched[static_cast<std::size_t>(j)] << ")");
            REQUIRE(latched[static_cast<std::size_t>(k)] > latched[static_cast<std::size_t>(j)]);
        }
    }

    const auto five_seconds = static_cast<std::size_t>(5.0 * kSr);
    double worst_drift_db = 0.0;
    for (std::size_t i = 0; i < five_seconds; ++i) {
        v.process(0.0, 0.0, scratch);
        for (int k = 0; k < v.band_count(); ++k) {
            const double held = latched[static_cast<std::size_t>(k)];
            if (held < 1e-6) continue;
            worst_drift_db =
                std::max(worst_drift_db, std::abs(units::linear_to_db(v.synthesis_gain(k) / held)));
        }
    }
    INFO("worst band drift over 5 s of silence: " << worst_drift_db << " dB");
    REQUIRE(worst_drift_db < 0.1);

    // Releasing freeze resumes tracking: the levels fall away within about one
    // release, computed from the shipped floors rather than restated.
    v.set_formant_freeze(false);
    const auto one_release =
        static_cast<std::size_t>(2.0 * v.release_eff_ms(0) * 0.001 * kSr);
    for (std::size_t i = 0; i < one_release; ++i) v.process(0.0, 0.0, scratch);
    for (int k = 0; k < v.band_count(); ++k) {
        INFO("band " << k << " after release: " << v.synthesis_gain(k) << " was "
                     << latched[static_cast<std::size_t>(k)]);
        REQUIRE(v.synthesis_gain(k) < 0.1 * latched[static_cast<std::size_t>(k)] + 1e-9);
    }
}

TEST_CASE("vocoder reconstruction stays inside its registry bound", "[signal][vocoder]") {
    // "All bands open" is achieved by construction rather than by hoping a
    // modulator happens to fill the bank: `VcaT` clamps its control to [0, 1],
    // so a modulator loud enough to drive every band envelope past 1 leaves
    // every synthesis gain at exactly 1. That is the true static worst case and
    // it is reproducible from the shipped constants.
    for (int bands = Voc::kMinBands; bands <= Voc::kMaxBands; ++bands) {
        auto v = make_bank(bands);
        v.set_carrier_source(Voc::CarrierSource::external);
        v.set_sibilance_mix(0.0);   // a voiced probe would gate it to zero anyway
        v.set_output_trim_db(0.0);
        v.set_dry_wet(1.0);

        const auto n = static_cast<std::size_t>(2.0 * kSr);
        const auto carrier = seeded_noise(n, 1.0, 0xABCDu);
        // Loud enough that even band 0 saturates: pre-emphasis is a
        // differentiator and costs the bottom of the bank about 33 dB, so a
        // 50× modulator leaves band 0's envelope at 0.06, not at 1.
        const auto modulator = seeded_noise(n, 1e6, 0x1234u);
        v.reset();
        const auto out = render(v, modulator, carrier);

        double peak = 0.0;
        for (std::size_t i = static_cast<std::size_t>(0.5 * kSr); i < n; ++i)
            peak = std::max(peak, std::abs(out[i]));
        const double pre_trim = peak / Voc::kOutputHeadroomTrim;

        // Every band's control really is saturated — otherwise this measures
        // something quieter than the worst case and passes for the wrong
        // reason. `synthesis_gain` reports env' before `VcaT` clamps it, so the
        // check is "at or past 1", and the gain actually applied is exactly 1.
        for (int k = 0; k < bands; ++k) {
            INFO("band " << k << " control " << v.synthesis_gain(k));
            REQUIRE(v.synthesis_gain(k) >= 1.0);
        }

        INFO("bands " << bands << ": post-trim peak " << peak << ", pre-trim " << pre_trim
                      << " against kWorstCaseGain " << Voc::kWorstCaseGain);
        REQUIRE(pre_trim <= Voc::kWorstCaseGain);
        REQUIRE(peak <= 1.0);
    }
}

TEST_CASE("vocoder renders are bit-identical after reset", "[signal][vocoder]") {
    const auto n = static_cast<std::size_t>(2.0 * kSr);
    const auto modulator = seeded_noise(n, 0.5, 0x51F0u);
    const auto carrier = seeded_noise(n, 0.5, 0x3C3Cu);

    for (auto source : {Voc::CarrierSource::internal, Voc::CarrierSource::external}) {
        for (bool freeze : {false, true}) {
            auto v = make_bank();
            v.set_carrier_source(source);
            v.set_noise_mix(0.5);
            v.set_formant_freeze(freeze);
            auto run = [&] {
                v.reset();
                return render(v, modulator, carrier);
            };
            const auto first = run();
            const auto second = run();
            INFO("source " << static_cast<int>(source) << " freeze " << freeze);
            REQUIRE(first == second);
        }
    }
}

TEST_CASE("vocoder noise carrier comes from the shipped seed", "[signal][vocoder]") {
    // The spec proves the seed is live by rebuilding with a different one,
    // which a single build cannot do. This proves the same thing without a
    // rebuild, and more directly: an independently constructed generator at
    // kNoiseSeed reproduces the carrier the module used, sample for sample.
    //
    // With noise_mix = 1 the carrier IS the noise, so with every band's gain
    // pinned the output is a deterministic function of that stream — and a
    // reference vocoder fed the same stream as an external carrier must match.
    // The modulator must be VOICED: with an external carrier the module
    // substitutes its own noise above the sibilance corner whenever u > 0, and
    // the two renders would then differ in the high bands for a reason that has
    // nothing to do with the seed.
    const auto n = static_cast<std::size_t>(0.25 * kSr);
    const auto modulator = sawtooth(n, 150.0, 0.8);

    auto internal = make_bank();
    internal.set_carrier_source(Voc::CarrierSource::internal);
    internal.set_noise_mix(1.0);
    internal.reset();
    const std::vector<double> unused(n, 0.0);
    const auto from_module = render(internal, modulator, unused);

    // The same stream, drawn here: one sample per process() call, in order.
    Xorshift32 reference{Voc::kNoiseSeed};
    std::vector<double> stream(n);
    for (auto& s : stream) s = reference.next_bipolar<double>();

    auto external = make_bank();
    external.set_carrier_source(Voc::CarrierSource::external);
    external.set_noise_mix(1.0);
    external.reset();
    const auto from_reference = render(external, modulator, stream);

    REQUIRE_THAT(external.unvoiced(), WithinAbs(0.0, 1e-9));
    REQUIRE(from_module == from_reference);
    REQUIRE(std::any_of(from_module.begin(), from_module.end(),
                        [](double x) { return x != 0.0; }));
    REQUIRE(Xorshift32{Voc::kNoiseSeed}.seed() == Voc::kNoiseSeed);
}

TEST_CASE("vocoder reports zero latency and responds on sample 0", "[signal][vocoder]") {
    for (auto source : {Voc::CarrierSource::internal, Voc::CarrierSource::external}) {
        auto v = make_bank();
        v.set_carrier_source(source);
        v.set_dry_wet(1.0);
        REQUIRE(v.latency_samples() == 0);

        v.reset();
        double out = 0.0;
        // Modulator and carrier both impulsive, so both banks are excited on
        // the first sample and there is no way for a bulk pre-delay to hide.
        v.process(1.0, 1.0, out);
        INFO("source " << static_cast<int>(source) << ": first output sample " << out);
        REQUIRE(out != 0.0);
    }
}
