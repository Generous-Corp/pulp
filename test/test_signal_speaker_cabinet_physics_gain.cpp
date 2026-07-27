#include "test_signal_speaker_cabinet_support.hpp"

TEST_CASE("Instruments agree", "[signal][speaker][instrument]") {
    // The FFT is only used for broad scans, but a broad scan that disagrees
    // with the exact DTFT would silently move every peak it reports. Compare
    // them on bin centres, where the FFT has no scalloping error to explain
    // away.
    SpeakerModel64 model;
    sealed_reference(model);
    const auto h = impulse_response(model);
    Spectrum spec(h);
    double worst = 0.0;
    for (int bin : {32, 64, 100, 200, 400, 800, 1600, 3200}) {
        const double hz = Spectrum::hz_for(bin);
        const double fft_db = amplitude_db(spec.at_bin(bin));
        const double dtft_db = response_db(h, hz);
        worst = std::max(worst, std::abs(fft_db - dtft_db));
    }
    INFO("max FFT-vs-DTFT disagreement " << worst << " dB");
    // The IR is twice the FFT length, so the FFT truncates it. The discarded
    // tail is below -180 dB, hence the tight bound.
    REQUIRE(worst < 0.01);
}

TEST_CASE("Inductance accessor is faithful", "[signal][speaker][instrument]") {
    // `inductance_magnitude_db` is a closed form, and AT-3 leans on it
    // entirely, so it has to be shown to describe the shipped filter rather
    // than a plausible model of it. The empirical counterpart is a pure ratio:
    // two renders differing ONLY in `treble_rolloff_hz`, so every other stage
    // is bit-identical and divides out with no assumption about its response.
    SpeakerModel64 reference;
    sealed_reference(reference);
    reference.set_treble_rolloff_hz(SpeakerModel64::kTrebleRolloffHzMax);
    reference.prepare(kFs);
    const auto h_ref = impulse_response(reference);

    for (double corner : {4000.0, 2000.0, 1500.0}) {
        SpeakerModel64 model;
        sealed_reference(model);
        model.set_treble_rolloff_hz(corner);
        model.prepare(kFs);
        const auto h = impulse_response(model);

        double worst = 0.0;
        for (double hz : {500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0}) {
            const double measured = response_db(h, hz) - response_db(h_ref, hz);
            const double closed_form =
                model.inductance_magnitude_db(hz) - reference.inductance_magnitude_db(hz);
            worst = std::max(worst, std::abs(measured - closed_form));
        }
        INFO("corner " << corner << " Hz: max accessor-vs-measured " << worst << " dB");
        REQUIRE(worst < 1e-6);
    }
}

TEST_CASE("AT-1 Thiele-Small resonance", "[signal][speaker][acceptance]") {
    SpeakerModel64 model;
    sealed_reference(model);

    const auto& driver = SpeakerModel64::archetype(0);
    // alpha, fc and Qtc computed from the shipped archetype row and box volume
    // — Small 1972, closed-box analysis.
    const double alpha = driver.vas_litres / SpeakerModel64::kBoxVolumeLDefault;
    const double fc = driver.fs_hz * std::sqrt(1.0 + alpha);
    const double qtc = driver.qts * std::sqrt(1.0 + alpha);
    REQUIRE_THAT(model.compliance_ratio(), Catch::Matchers::WithinRel(alpha, 1e-12));
    REQUIRE_THAT(model.resonance_fc_hz(), Catch::Matchers::WithinRel(fc, 1e-12));
    REQUIRE_THAT(model.resonance_q(), Catch::Matchers::WithinRel(qtc, 1e-12));

    // The peak of a second-order high-pass is NOT at fc; see deviation 1.
    const double peak_hz = fc / std::sqrt(1.0 - 1.0 / (2.0 * qtc * qtc));
    const double peak_db = amplitude_db(qtc / std::sqrt(1.0 - 1.0 / (4.0 * qtc * qtc)));
    REQUIRE_THAT(model.resonance_peak_hz(), Catch::Matchers::WithinRel(peak_hz, 1e-12));
    REQUIRE_THAT(model.resonance_peak_db(), Catch::Matchers::WithinRel(peak_db, 1e-12));
    INFO("fc " << fc << " Hz, peak at " << peak_hz << " Hz = +"
               << 100.0 * (peak_hz - fc) / fc << " % above fc");
    REQUIRE(peak_hz > 1.15 * fc);  // the spec's +-3 %-of-fc window cannot contain it

    const auto h = impulse_response(model);

    // Locate the rendered peak on a fine grid.
    double best = 0.0, best_hz = 0.0;
    for (double hz = 0.5 * fc; hz < 2.0 * fc; hz += 0.25) {
        const double v = response_at(h, hz);
        if (v > best) { best = v; best_hz = hz; }
    }
    INFO("measured peak " << best_hz << " Hz at " << amplitude_db(best) << " dB");
    // Half the search step is the resolution limit of the scan, not slack.
    REQUIRE(std::abs(best_hz - peak_hz) < 0.5);
    REQUIRE(std::abs(amplitude_db(best) - peak_db) < 0.05);

    // The rendered chain tracks the closed-form high-pass across the whole
    // resonance region, which is what makes the peak numbers above meaningful:
    // the residual colouring from the stages that cannot be neutralised is
    // measured here rather than assumed away.
    for (double hz : {150.0, fc, peak_hz, 400.0, 800.0}) {
        const double residual = response_db(h, hz) - amplitude_db(highpass2_mag(hz, fc, qtc));
        INFO("residual at " << hz << " Hz = " << residual << " dB");
        REQUIRE(std::abs(residual) < 0.3);
    }

    // +12 dB/oct asymptote below fc (the -12 dB/oct rolloff read upward).
    for (double lo : {0.0625, 0.125}) {
        const double slope = response_db(h, 2.0 * lo * fc) - response_db(h, lo * fc);
        INFO("slope " << lo * fc << " -> " << 2.0 * lo * fc << " Hz = " << slope << " dB/oct");
        REQUIRE(std::abs(slope - 12.0) < 1.0);
    }
}

TEST_CASE("AT-2 sealed versus open-back", "[signal][speaker][acceptance]") {
    SpeakerModel64 sealed, open;
    sealed_reference(sealed);
    neutralise(open);
    open.set_driver_archetype(0);
    open.set_box_type(SpeakerBoxType::open_back);
    open.prepare(kFs);

    // Open-back is the driver near free air: no trapped-air stiffness, so
    // alpha is 0 and the corner drops back to the driver's own fs.
    REQUIRE(open.compliance_ratio() == 0.0);
    REQUIRE_THAT(open.resonance_fc_hz(),
                 Catch::Matchers::WithinRel(SpeakerModel64::archetype(0).fs_hz, 1e-12));
    REQUIRE(open.resonance_fc_hz() < sealed.resonance_fc_hz());

    const auto h_sealed = impulse_response(sealed);
    const auto h_open = impulse_response(open);

    const double delta = response_db(h_sealed, 50.0) - response_db(h_open, 50.0);
    INFO("50 Hz: sealed " << response_db(h_sealed, 50.0) << " dB, open "
                          << response_db(h_open, 50.0) << " dB, delta " << delta << " dB");
    REQUIRE(delta >= 6.0);

    // The sealed 50 Hz level is the closed-form high-pass, which is what makes
    // the difference above attributable to the dipole rather than to the chain.
    const double predicted =
        amplitude_db(highpass2_mag(50.0, sealed.resonance_fc_hz(), sealed.resonance_q()));
    REQUIRE(std::abs(response_db(h_sealed, 50.0) - predicted) < 0.3);

    // The dipole notch: find the minimum of open/sealed, which isolates the
    // open-back-only stages.
    double worst = 1e9, worst_hz = 0.0;
    const double expected_notch = open.dipole_hz() * SpeakerModel64::kDipoleNotchRatio;
    for (double hz = 0.5 * expected_notch; hz < 1.6 * expected_notch; hz += 0.5) {
        const double ratio = response_at(h_open, hz) / response_at(h_sealed, hz);
        if (ratio < worst) { worst = ratio; worst_hz = hz; }
    }
    INFO("dipole notch measured " << worst_hz << " Hz, expected " << expected_notch << " Hz ("
                                  << 100.0 * (worst_hz - expected_notch) / expected_notch << " %)");
    REQUIRE(std::abs(worst_hz - expected_notch) / expected_notch < 0.05);

    // And the geometry the notch is derived from is the shipped constant.
    REQUIRE_THAT(open.dipole_hz(),
                 Catch::Matchers::WithinRel(
                     SpeakerModel64::kSpeedOfSoundMs / (2.0 * SpeakerModel64::kDipolePathM), 1e-12));
}

TEST_CASE("AT-3 treble rolloff", "[signal][speaker][acceptance]") {
    // Measured on the stage itself via the accessor `Inductance accessor is
    // faithful` validated against an empirical chain ratio. Measuring it on the
    // whole chain instead would fold in the off-axis lowpass, which is a second
    // HF rolloff that is always present and is not what this test is about.
    //
    // At the nominal corner the first-order lowpass contributes -3.01 dB and
    // the semi-inductance shelf contributes half its floor, so the blend is
    // already past -3 dB there — see deviation 3.
    const double lp_at_corner = 1.0 / std::sqrt(2.0);
    const double shelf_at_corner =
        std::pow(10.0, SpeakerModel64::kSemiInductanceShelfDb / 40.0);
    const double blend = (1.0 - SpeakerModel64::kSemiInductanceDefault) * lp_at_corner +
                         SpeakerModel64::kSemiInductanceDefault * shelf_at_corner;
    const double expected_at_corner = amplitude_db(blend);
    INFO("blend at the corner = " << expected_at_corner << " dB (not -3.01)");
    REQUIRE(expected_at_corner < -3.0);

    for (double corner : {1500.0, 2000.0, 4000.0}) {
        SpeakerModel64 model;
        sealed_reference(model);
        model.set_treble_rolloff_hz(corner);
        model.prepare(kFs);

        // Level at the nominal corner: identical at every corner, which is the
        // structural statement — the stage is scale-invariant in its corner.
        const double at_corner = model.inductance_magnitude_db(corner);
        INFO("corner " << corner << " Hz: stage is " << at_corner << " dB there");
        // The closed form above sums magnitudes; the filter sums complex
        // responses, so they agree only to the extent the two phases align.
        // 0.05 dB is that residual, measured.
        REQUIRE(std::abs(at_corner - expected_at_corner) < 0.05);

        // The -3 dB point consequently sits ~12 % low, at every corner.
        double minus3 = 0.0;
        for (double hz = 0.2 * corner; hz < 4.0 * corner; hz *= 1.0002)
            if (model.inductance_magnitude_db(hz) <= -3.0) { minus3 = hz; break; }
        const double offset_pct = 100.0 * (minus3 - corner) / corner;
        INFO("-3 dB at " << minus3 << " Hz (" << offset_pct << " % of nominal)");
        REQUIRE(offset_pct < -8.0);
        REQUIRE(offset_pct > -15.0);

        // AT-3's slope clause, asserted exactly as written. Restricted to
        // corners whose second octave stays clear of Nyquist, where a digital
        // shelf's response bends for reasons that have nothing to do with
        // semi-inductance.
        if (4.0 * corner < 0.4 * kFs) {
            const double slope = model.inductance_magnitude_db(2.0 * corner) - at_corner;
            INFO("slope across the octave above = " << slope << " dB/oct");
            REQUIRE(slope <= -3.0);
            REQUIRE(slope >= -6.0);
            // And it IS gentler than a pure first-order lowpass, which is the
            // whole point of the Leach blend.
            REQUIRE(slope > -6.02);
        }
    }
}

TEST_CASE("AT-4 small-signal transparency", "[signal][speaker][acceptance][nonlinear]") {
    // Series law 1: at rest the stage must be exactly transparent. `g_bl` is 1
    // at x = 0 and fc' is fc, so a quiet signal must be indistinguishable from
    // the compression-disabled path.
    SpeakerModel64 hot, cold;
    sealed_reference(hot);
    sealed_reference(cold);
    hot.set_drive_db(SpeakerModel64::kDriveDbMin);
    cold.set_drive_db(SpeakerModel64::kDriveDbMin);
    hot.set_compression_amount(100.0);
    cold.set_compression_amount(0.0);
    hot.prepare(kFs);
    cold.prepare(kFs);

    constexpr int kN = 32768;
    const double amplitude = std::pow(10.0, -40.0 / 20.0);  // -40 dBFS
    const double f0 = kFs * 128 / kN;                       // exact analysis bin
    const auto y_hot = render_sine(hot, f0, amplitude, kN);
    const auto y_cold = render_sine(cold, f0, amplitude, kN);

    const double level_error =
        amplitude_db(coherent_bin(y_hot, f0)) - amplitude_db(coherent_bin(y_cold, f0));
    INFO("level difference vs the linear path: " << level_error << " dB");
    REQUIRE(std::abs(level_error) < 0.05);

    // THD of the compressed path at rest.
    const double fundamental = coherent_bin(y_hot, f0);
    double distortion = 0.0;
    for (int harmonic = 2; harmonic <= 8; ++harmonic) {
        const double v = coherent_bin(y_hot, harmonic * f0);
        distortion += v * v;
    }
    const double thd_db = amplitude_db(std::sqrt(distortion) / fundamental);
    INFO("THD at rest = " << thd_db << " dB");
    REQUIRE(thd_db < -80.0);
}

TEST_CASE("AT-5 compression responds to level", "[signal][speaker][acceptance][nonlinear]") {
    // The load-bearing claim of the whole module: a stage that behaves the same
    // at -40 dBFS and 0 dBFS is not modelling excursion.
    constexpr int kN = 16384;
    const double f0 = kFs * 64 / kN;

    // Gain reduction is read from the FUNDAMENTAL's coherent bin, never from
    // the peak sample. Under heavy compression the waveform is grossly
    // distorted, and its peak stops being an amplitude: measured by peak, this
    // very sweep reads 29.5 dB at -12 dBFS, 25.8 dB at -9 and 27.9 dB at -6 —
    // apparently non-monotone, entirely as an artefact of peak-picking a
    // distorted wave. The coherent fundamental is monotone across the whole
    // range (0.21 dB to 58.20 dB).
    double previous = -1.0;
    double top = 0.0;
    for (double level_db : {-40.0, -30.0, -24.0, -20.0, -16.0, -12.0, -6.0, 0.0}) {
        SpeakerModel64 hot, cold;
        sealed_reference(hot);
        sealed_reference(cold);
        hot.set_drive_db(12.0);
        cold.set_drive_db(12.0);
        hot.set_compression_amount(100.0);
        cold.set_compression_amount(0.0);
        hot.prepare(kFs);
        cold.prepare(kFs);

        const double amplitude = std::pow(10.0, level_db / 20.0);
        const auto y_hot = render_sine(hot, f0, amplitude, kN);
        const auto y_cold = render_sine(cold, f0, amplitude, kN);
        const double reduction =
            amplitude_db(coherent_bin(y_cold, f0)) - amplitude_db(coherent_bin(y_hot, f0));
        INFO("input " << level_db << " dBFS -> " << reduction << " dB of gain reduction");
        REQUIRE(reduction > previous);  // strictly monotone in level
        previous = reduction;
        top = reduction;
    }
    INFO("gain reduction at the top of the sweep = " << top << " dB");
    REQUIRE(top >= 3.0);

    // A compressor that behaves identically at -40 dBFS and 0 dBFS is not
    // modelling excursion; the span across the sweep is the claim.
    REQUIRE(top > 40.0);
}

TEST_CASE("Peak-sample amplitude misreads a compressed wave",
          "[signal][speaker][instrument]") {
    // This exists to justify the measurement choice above rather than leave it
    // as an assertion. The same sweep, read by peak, is NOT monotone — so a
    // suite that measured amplitude by peak would report a compression defect
    // that is not there.
    constexpr int kN = 16384;
    const double f0 = kFs * 64 / kN;
    std::vector<double> by_peak, by_fundamental;
    for (double level_db : {-12.0, -9.0, -6.0}) {
        SpeakerModel64 hot, cold;
        sealed_reference(hot);
        sealed_reference(cold);
        hot.set_drive_db(12.0);
        cold.set_drive_db(12.0);
        hot.set_compression_amount(100.0);
        cold.set_compression_amount(0.0);
        hot.prepare(kFs);
        cold.prepare(kFs);
        const double amplitude = std::pow(10.0, level_db / 20.0);
        const auto y_hot = render_sine(hot, f0, amplitude, kN);
        const auto y_cold = render_sine(cold, f0, amplitude, kN);
        by_peak.push_back(amplitude_db(settled_peak(y_cold)) - amplitude_db(settled_peak(y_hot)));
        by_fundamental.push_back(amplitude_db(coherent_bin(y_cold, f0)) -
                                 amplitude_db(coherent_bin(y_hot, f0)));
    }
    INFO("by peak: " << by_peak[0] << ", " << by_peak[1] << ", " << by_peak[2]);
    INFO("by fundamental: " << by_fundamental[0] << ", " << by_fundamental[1] << ", "
                            << by_fundamental[2]);
    REQUIRE(by_peak[1] < by_peak[0]);            // the artefact
    REQUIRE(by_fundamental[1] > by_fundamental[0]);  // the truth
    REQUIRE(by_fundamental[2] > by_fundamental[1]);
}

TEST_CASE("AT-5 harmonic structure is odd", "[signal][speaker][acceptance][nonlinear]") {
    // `g_bl` is EVEN in x and x is odd in the input, so the stage is
    // odd-symmetric and can only make odd harmonics. Expanding
    // `g_bl ~ 1 - beta x^2` against a carrier puts sidebands at f and 3f with
    // amplitude `beta x^2 / 4` — not a second harmonic at `beta x^2 / 2`. See
    // deviation 2.
    constexpr int kN = 65536;
    const double f0 = kFs * 256 / kN;

    for (double level_db : {-46.0, -40.0, -34.0, -28.0}) {
        SpeakerModel64 model;
        sealed_reference(model);
        model.set_compression_amount(100.0);
        model.prepare(kFs);

        const double amplitude = std::pow(10.0, level_db / 20.0);
        std::vector<double> y(kN);
        double excursion_peak = 0.0;
        for (int i = 0; i < kN; ++i) {
            y[static_cast<std::size_t>(i)] =
                model.process(amplitude * std::sin(2.0 * kPi * f0 * i / kFs));
            if (i > kN / 2) excursion_peak = std::max(excursion_peak, std::abs(model.excursion()));
        }

        const double h1 = coherent_bin(y, f0);
        const double h2 = coherent_bin(y, 2.0 * f0);
        const double h3 = coherent_bin(y, 3.0 * f0);
        const double beta = model.bl_beta();
        const double predicted_h3 =
            amplitude_db(beta * excursion_peak * excursion_peak / 4.0);

        INFO("input " << level_db << " dBFS, x_peak " << excursion_peak << ": h2 "
                      << amplitude_db(h2 / h1) << " dB, h3 " << amplitude_db(h3 / h1)
                      << " dB, predicted h3 " << predicted_h3 << " dB");

        // The second harmonic is structurally absent, not merely small.
        REQUIRE(amplitude_db(h2 / h1) < -200.0);
        // The third matches the BL closed form derived from the shipped k_bl.
        // It lands consistently ~0.47 dB BELOW it, and that residual is not
        // slop: it is the Cms(x) stiffening's own third harmonic, which opposes
        // the BL one. `Cms opposes the BL third harmonic` isolates it. The
        // specification's formula models only BL, so this bound is the BL
        // prediction plus the measured Cms contribution.
        REQUIRE(amplitude_db(h3 / h1) < predicted_h3);
        REQUIRE(std::abs(amplitude_db(h3 / h1) - predicted_h3) < 0.7);
        // The spec's formula names the wrong harmonic AND is 6 dB out; record
        // the second half of that here so the deviation note is not a claim.
        const double spec_formula = amplitude_db(beta * excursion_peak * excursion_peak / 2.0);
        REQUIRE(std::abs(spec_formula - predicted_h3 - 6.0206) < 0.01);
    }
}

TEST_CASE("Third harmonic scales as the square of excursion",
          "[signal][speaker][acceptance][nonlinear]") {
    // The strong form of the closed form, and the one that is free of any
    // constant offset: `h3 ~ beta * x^2`, so doubling x (a 6 dB louder input,
    // while the stage is still in its small-signal regime) must raise the third
    // harmonic by exactly 12 dB.
    constexpr int kN = 65536;
    const double f0 = kFs * 256 / kN;
    std::vector<double> ratios;
    for (double level_db : {-52.0, -46.0, -40.0, -34.0}) {
        SpeakerModel64 model;
        sealed_reference(model);
        model.set_compression_amount(100.0);
        model.prepare(kFs);
        const double amplitude = std::pow(10.0, level_db / 20.0);
        std::vector<double> y(kN);
        for (int i = 0; i < kN; ++i)
            y[static_cast<std::size_t>(i)] =
                model.process(amplitude * std::sin(2.0 * kPi * f0 * i / kFs));
        ratios.push_back(amplitude_db(coherent_bin(y, 3.0 * f0) / coherent_bin(y, f0)));
    }
    for (std::size_t i = 1; i < ratios.size(); ++i) {
        const double step = ratios[i] - ratios[i - 1];
        INFO("step " << i << " = " << step << " dB per 6 dB of input");
        REQUIRE(std::abs(step - 12.0) < 0.1);
    }
}

TEST_CASE("A low note compresses the treble riding on it",
          "[signal][speaker][acceptance][nonlinear]") {
    // The module's headline physical claim, and the one that separates this
    // path from a convolution: compression tracks cone DISPLACEMENT, so a loud
    // low note ducks a quiet high one. A memoryless waveshaper on the pressure
    // signal could not do this, which is why `SaturatorT` is not the primitive
    // here (see the header).
    //
    // It is also the assertion that pins the BL stage specifically. Cms(x)
    // shifts the resonance, which is flat by 3 kHz and therefore cannot move
    // the probe: whatever ducks the probe is BL, and the amount is closed-form.
    // Averaging `1/(1 + beta X^2 sin^2)` over a cycle gives `1/sqrt(1+beta X^2)`,
    // so the probe's carrier must drop by exactly `10*log10(1 + beta X^2)`.
    constexpr int kN = 65536;
    const double f_low = kFs * 128 / kN;    // 93.75 Hz, exact bin, well into excursion
    const double f_probe = kFs * 4096 / kN;  // 3 kHz, exact bin, above the resonance
    const double probe_amplitude = std::pow(10.0, -40.0 / 20.0);

    double previous_drop = -1.0;
    for (double low_db : {-60.0, -24.0, -18.0, -12.0}) {
        SpeakerModel64 hot, cold;
        sealed_reference(hot);
        sealed_reference(cold);
        hot.set_compression_amount(100.0);
        cold.set_compression_amount(0.0);
        hot.prepare(kFs);
        cold.prepare(kFs);

        const double low_amplitude = std::pow(10.0, low_db / 20.0);
        std::vector<double> y_hot(kN), y_cold(kN), excursion(kN);
        for (int i = 0; i < kN; ++i) {
            const double s = low_amplitude * std::sin(2.0 * kPi * f_low * i / kFs) +
                             probe_amplitude * std::sin(2.0 * kPi * f_probe * i / kFs);
            y_hot[static_cast<std::size_t>(i)] = hot.process(s);
            excursion[static_cast<std::size_t>(i)] = hot.excursion();
            y_cold[static_cast<std::size_t>(i)] = cold.process(s);
        }

        const double x_amplitude = coherent_bin(excursion, f_low);
        const double beta = hot.bl_beta();
        const double predicted = 10.0 * std::log10(1.0 + beta * x_amplitude * x_amplitude);
        const double measured = amplitude_db(coherent_bin(y_cold, f_probe)) -
                                amplitude_db(coherent_bin(y_hot, f_probe));

        INFO("low tone " << low_db << " dBFS, excursion " << x_amplitude << ": probe ducked "
                         << measured << " dB, predicted " << predicted << " dB");
        REQUIRE(std::abs(measured - predicted) < 0.05);
        REQUIRE(measured > previous_drop);  // louder low note ducks the probe harder
        previous_drop = measured;
    }
    // And the effect is audible, not merely present.
    REQUIRE(previous_drop > 1.0);
}

TEST_CASE("Cms opposes the BL third harmonic", "[signal][speaker][nonlinear]") {
    // Attribution of the 0.47 dB residual above. Reconstructing the output from
    // the module's OWN linear path and its OWN excursion — that is, BL alone,
    // with no Cms feedback — reproduces the closed form essentially exactly.
    // The shipped model sits below it because the suspension stiffening is a
    // second harmonic source of opposite sign, which is a real property of the
    // two-mechanism model and not an error in either.
    constexpr int kN = 65536;
    const double f0 = kFs * 256 / kN;
    const double amplitude = std::pow(10.0, -46.0 / 20.0);

    SpeakerModel64 hot, cold;
    sealed_reference(hot);
    sealed_reference(cold);
    hot.set_compression_amount(100.0);
    cold.set_compression_amount(0.0);
    hot.prepare(kFs);
    cold.prepare(kFs);

    std::vector<double> y_real(kN), y_linear(kN), x_linear(kN), y_bl_only(kN);
    for (int i = 0; i < kN; ++i) {
        const double s = amplitude * std::sin(2.0 * kPi * f0 * i / kFs);
        y_real[static_cast<std::size_t>(i)] = hot.process(s);
        y_linear[static_cast<std::size_t>(i)] = cold.process(s);
        x_linear[static_cast<std::size_t>(i)] = cold.excursion();
    }
    const double beta = hot.bl_beta();
    for (int i = 0; i < kN; ++i) {
        const double x = x_linear[static_cast<std::size_t>(i)];
        y_bl_only[static_cast<std::size_t>(i)] =
            y_linear[static_cast<std::size_t>(i)] / (1.0 + beta * x * x);
    }

    const double x_amplitude = coherent_bin(x_linear, f0);
    const double closed_form = amplitude_db(beta * x_amplitude * x_amplitude / 4.0);
    const double bl_only =
        amplitude_db(coherent_bin(y_bl_only, 3.0 * f0) / coherent_bin(y_bl_only, f0));
    const double shipped =
        amplitude_db(coherent_bin(y_real, 3.0 * f0) / coherent_bin(y_real, f0));

    INFO("closed form " << closed_form << " dB, BL-only reconstruction " << bl_only
                        << " dB, shipped " << shipped << " dB");
    // BL alone IS the closed form, to the precision of the measurement.
    REQUIRE(std::abs(bl_only - closed_form) < 0.01);
    // The shipped model, which also has Cms, sits measurably below it.
    REQUIRE(shipped < bl_only - 0.2);
    REQUIRE(shipped > bl_only - 1.0);
}

TEST_CASE("AT-5 Cms stiffening raises the resonance", "[signal][speaker][acceptance][nonlinear]") {
    // The other half of the excursion model: larger displacement stiffens the
    // suspension, so fc climbs. It is compressive negative feedback — a higher
    // fc reduces the displacement that produced it — so it cannot run away.
    SpeakerModel64 model;
    sealed_reference(model);
    model.set_compression_amount(100.0);
    model.set_drive_db(12.0);
    model.prepare(kFs);

    const double nominal = model.resonance_fc_hz();
    REQUIRE_THAT(model.dynamic_fc_hz(), Catch::Matchers::WithinRel(nominal, 1e-9));

    // Drive it hard and watch the dynamic cutoff climb.
    constexpr int kN = 8192;
    double highest = 0.0;
    for (int i = 0; i < kN; ++i) {
        model.process(0.7 * std::sin(2.0 * kPi * 180.0 * i / kFs));
        highest = std::max(highest, model.dynamic_fc_hz());
    }
    INFO("fc climbed from " << nominal << " Hz to " << highest << " Hz");
    REQUIRE(highest > nominal);
    // Bounded: the stiffening term is gamma * x^2 with gamma from the shipped
    // constant, and the excursion it acts on is itself reduced by the rise.
    REQUIRE(highest < nominal * 20.0);

    // Zero compression pins it exactly.
    SpeakerModel64 linear;
    sealed_reference(linear);
    linear.set_drive_db(12.0);
    linear.prepare(kFs);
    for (int i = 0; i < kN; ++i) linear.process(0.7 * std::sin(2.0 * kPi * 180.0 * i / kFs));
    REQUIRE_THAT(linear.dynamic_fc_hz(),
                 Catch::Matchers::WithinRel(linear.resonance_fc_hz(), 1e-9));
}

TEST_CASE("AT-6 mic moves", "[signal][speaker][acceptance][mic]") {
    SECTION("cap to cone edge darkens, monotonically") {
        // Isolated by ratio: two renders differing only in `mic_position_pct`.
        std::vector<double> at_3k;
        for (double pct = 0.0; pct <= 100.0; pct += 12.5) {
            SpeakerModel64 model;
            sealed_reference(model);
            model.set_mic_position_pct(pct);
            model.prepare(kFs);
            at_3k.push_back(response_db(impulse_response(model), 3000.0));
        }
        for (std::size_t i = 1; i < at_3k.size(); ++i) REQUIRE(at_3k[i] < at_3k[i - 1]);

        const double drop_3k = at_3k.front() - at_3k.back();
        INFO("cap-to-edge drop at 3 kHz = " << drop_3k << " dB");
        // The shelf spans kPresenceCapDb - kPresenceEdgeDb = 11 dB, but a shelf
        // cornered at 2.5 kHz has only reached part of it by 3 kHz — see
        // deviation 4. What the shipped constants deliver:
        REQUIRE(drop_3k > 7.3);

        SpeakerModel64 cap, edge;
        sealed_reference(cap);
        sealed_reference(edge);
        cap.set_mic_position_pct(0.0);
        edge.set_mic_position_pct(100.0);
        cap.prepare(kFs);
        edge.prepare(kFs);
        const auto h_cap = impulse_response(cap);
        const auto h_edge = impulse_response(edge);
        REQUIRE(response_db(h_cap, 4000.0) - response_db(h_edge, 4000.0) > 9.5);
        // By 8 kHz the shelf is on its plateau and the full span is present.
        const double span = SpeakerModel64::kPresenceCapDb - SpeakerModel64::kPresenceEdgeDb;
        const double plateau = response_db(h_cap, 8000.0) - response_db(h_edge, 8000.0);
        INFO("plateau " << plateau << " dB against a shelf span of " << span << " dB");
        REQUIRE(plateau > 0.98 * span);
        REQUIRE(plateau <= span + 1e-9);

        // The shelf gains themselves come from the shipped constants.
        REQUIRE_THAT(cap.presence_shelf_db(),
                     Catch::Matchers::WithinRel(SpeakerModel64::kPresenceCapDb, 1e-12));
        REQUIRE_THAT(edge.presence_shelf_db(),
                     Catch::Matchers::WithinRel(SpeakerModel64::kPresenceEdgeDb, 1e-12));
    }

    SECTION("off-axis angle lowers the HF corner") {
        SpeakerModel64 model;
        sealed_reference(model);
        model.set_mic_axis_deg(45.0);
        model.prepare(kFs);
        const double expected =
            SpeakerModel64::kOffAxisOnAxisHz *
            (1.0 - SpeakerModel64::kOffAxisFactor * std::sin(45.0 * kPi / 180.0));
        INFO("off-axis corner " << model.offaxis_corner_hz() << " Hz, expected " << expected);
        REQUIRE_THAT(model.offaxis_corner_hz(), Catch::Matchers::WithinRel(expected, 1e-12));

        // And it monotonically darkens the top end.
        double previous = 1e9;
        for (double deg = 0.0; deg <= 90.0; deg += 15.0) {
            SpeakerModel64 m;
            sealed_reference(m);
            m.set_mic_axis_deg(deg);
            m.prepare(kFs);
            const double level = response_db(impulse_response(m), 6000.0);
            REQUIRE(level < previous);
            previous = level;
        }
    }

    SECTION("proximity lifts the low end and clamps") {
        SpeakerModel64 far, near;
        sealed_reference(far);
        sealed_reference(near);
        far.set_mic_distance_cm(SpeakerModel64::kProximityReferenceCm);
        near.set_mic_distance_cm(3.0);
        far.prepare(kFs);
        near.prepare(kFs);

        // At the reference distance the term is exactly zero by construction.
        REQUIRE(far.proximity_gain_db() == 0.0);
        const double expected_3cm =
            SpeakerModel64::kProximityGainK *
            (1.0 / 3.0 - 1.0 / SpeakerModel64::kProximityReferenceCm);
        REQUIRE_THAT(near.proximity_gain_db(), Catch::Matchers::WithinRel(expected_3cm, 1e-12));

        const double lift = response_db(impulse_response(near), 100.0) -
                            response_db(impulse_response(far), 100.0);
        INFO("100 Hz lift from 30 cm to 3 cm = " << lift << " dB");
        REQUIRE(lift >= 3.0);
        REQUIRE(lift <= SpeakerModel64::kProximityCeilingDb);

        // The clamp: at 1 cm the raw law asks for more than the ceiling.
        SpeakerModel64 closest;
        sealed_reference(closest);
        closest.set_mic_distance_cm(SpeakerModel64::kMicDistanceCmMin);
        closest.prepare(kFs);
        const double raw = SpeakerModel64::kProximityGainK *
                           (1.0 / SpeakerModel64::kMicDistanceCmMin -
                            1.0 / SpeakerModel64::kProximityReferenceCm);
        INFO("raw law asks for " << raw << " dB, ceiling is "
                                 << SpeakerModel64::kProximityCeilingDb);
        REQUIRE(raw > SpeakerModel64::kProximityCeilingDb);
        REQUIRE(closest.proximity_gain_db() == SpeakerModel64::kProximityCeilingDb);

        // Boost-only: beyond the reference distance the shelf never cuts.
        SpeakerModel64 distant;
        sealed_reference(distant);
        distant.set_mic_distance_cm(SpeakerModel64::kMicDistanceCmMax);
        distant.prepare(kFs);
        REQUIRE(distant.proximity_gain_db() == 0.0);
    }
}

TEST_CASE("AT-7 aliasing floor", "[signal][speaker][acceptance][nonlinear]") {
    // The no-oversampling decision rests on this measurement, so it is made at
    // the most hostile setting the parameter surface allows.
    SpeakerModel64 model;
    neutralise(model);
    model.set_driver_archetype(0);
    model.set_box_type(SpeakerBoxType::sealed);
    model.set_drive_db(SpeakerModel64::kDriveDbMax);
    model.set_compression_amount(100.0);
    model.prepare(kFs);

    constexpr int kN = 65536;
    // Two tones on exact analysis bins so their own harmonics and intermodulation
    // products land on bins too, and everything left over is alias.
    const double f1 = kFs * 1365 / kN;
    const double f2 = kFs * 4096 / kN;
    const double amplitude = std::pow(10.0, -6.0 / 20.0) / 2.0;

    std::vector<double> y(kN);
    for (int i = 0; i < kN; ++i)
        y[static_cast<std::size_t>(i)] =
            model.process(amplitude * (std::sin(2.0 * kPi * f1 * i / kFs) +
                                       std::sin(2.0 * kPi * f2 * i / kFs)));

    Spectrum spec(std::vector<double>(y.begin() + kN / 2, y.end()));
    double worst = 0.0, worst_hz = 0.0;
    for (int bin = 1; bin < spec.size(); ++bin) {
        const double hz = Spectrum::hz_for(bin);
        if (hz < 30.0 || hz > 0.48 * kFs) continue;
        // Skip anything within a few bins of a harmonic or intermodulation
        // product of the two tones — those are legitimate distortion, not
        // aliasing.
        bool legitimate = false;
        for (int p = -8; p <= 8 && !legitimate; ++p)
            for (int q = -8; q <= 8 && !legitimate; ++q) {
                const double product = std::abs(p * f1 + q * f2);
                if (product > 1.0 && std::abs(hz - product) < 4.0 * kFs / kFftSize)
                    legitimate = true;
            }
        if (legitimate) continue;
        if (spec.at_bin(bin) > worst) { worst = spec.at_bin(bin); worst_hz = hz; }
    }
    // Normalise to the analysis so the figure is dBFS of the rendered signal.
    const double scale = 2.0 / (kFftSize);
    const double worst_dbfs = amplitude_db(worst * scale);
    INFO("worst non-harmonic bin " << worst_dbfs << " dBFS at " << worst_hz << " Hz");
    REQUIRE(worst_dbfs < -60.0);
    REQUIRE(model.latency_samples() == 0);
}

TEST_CASE("AT-8 worst-case gain invariant", "[signal][speaker][acceptance][gain]") {
    // Every magnitude-maximising combination the parameter surface allows,
    // excluding `output_trim_db` — see deviation 5.
    double worst = 0.0;
    double worst_hz = 0.0;
    int worst_archetype = -1;

    for (int archetype = 0; archetype < SpeakerModel64::kArchetypeCount; ++archetype) {
        for (int box = 0; box < 2; ++box) {
            for (double volume : {SpeakerModel64::kBoxVolumeLMin, 28.0,
                                  SpeakerModel64::kBoxVolumeLMax}) {
                for (double q : {SpeakerModel64::kQResonanceMin, SpeakerModel64::kQResonanceMax}) {
                    SpeakerModel64 model;
                    model.set_driver_archetype(archetype);
                    model.set_box_type(box ? SpeakerBoxType::open_back : SpeakerBoxType::sealed);
                    model.set_box_volume_l(volume);
                    model.set_q_resonance(q);
                    model.set_cone_breakup_amount(100.0);
                    model.set_diffraction_amount(100.0);
                    model.set_mic_distance_cm(SpeakerModel64::kMicDistanceCmMin);  // max proximity
                    model.set_mic_position_pct(0.0);                               // brightest
                    model.set_mic_axis_deg(0.0);
                    model.set_treble_rolloff_hz(SpeakerModel64::kTrebleRolloffHzMax);
                    model.set_compression_amount(0.0);  // compression only ever reduces
                    model.set_drive_db(0.0);
                    model.set_output_trim_db(0.0);
                    model.prepare(kFs);

                    Spectrum spec(impulse_response(model));
                    for (int bin = Spectrum::bin_for(20.0); bin < spec.size(); ++bin) {
                        if (spec.at_bin(bin) > worst) {
                            worst = spec.at_bin(bin);
                            worst_hz = Spectrum::hz_for(bin);
                            worst_archetype = archetype;
                        }
                    }
                }
            }
        }
    }

    INFO("worst chain gain " << worst << " x (" << amplitude_db(worst) << " dB) at " << worst_hz
                             << " Hz, archetype " << worst_archetype);
    REQUIRE(worst <= SpeakerModel64::kWorstCaseGain);
    SpeakerModel64 probe;
    REQUIRE(probe.worst_case_gain() == SpeakerModel64::kWorstCaseGain);
    // The bound is not vacuous: the measured worst case is within 6 dB of it.
    REQUIRE(worst > 0.25 * SpeakerModel64::kWorstCaseGain);
}
