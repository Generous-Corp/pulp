#include "test_signal_pitch_shifter_support.hpp"

TEST_CASE("cubic interpolation is available and changes only the tap read",
          "[signal][pitch-shifter]") {
    auto linear = make_direct(7.0);
    auto cubic = make_direct(7.0);
    cubic.set_interp(PitchInterp::cubic);
    REQUIRE(cubic.interp() == PitchInterp::cubic);

    const auto wet_linear = render_wet(linear, 1000.0, kAnalysisLen / 4);
    const auto wet_cubic = render_wet(cubic, 1000.0, kAnalysisLen / 4);

    // Same pitch — the interpolant does not change the ratio…
    const double expected = 1000.0 * ratio_of(7.0);
    REQUIRE_THAT(peak_near(wet_cubic, expected, 20.0, 0.25), WithinRel(expected, 0.003));
    // …but it is a different read, so the renders are not identical.
    REQUIRE(wet_linear != wet_cubic);
    // …and it is still bounded.
    REQUIRE(peak(wet_cubic) <= Shifter::kDcBlockerPeakGain);
}

TEST_CASE("the float instantiation shifts to the same pitch",
          "[signal][pitch-shifter]") {
    // `PitchShifter` (float) is the DEFAULT alias and the one a plugin will
    // instantiate; every case above runs the double one. The delay storage, the
    // DC blocker, and the tap arithmetic all narrow to `SampleType`, so this is
    // a genuinely different numeric path rather than a template formality.
    constexpr double kF0 = 1000.0;
    constexpr double kSemitones = 7.0;

    PitchShifter shifter;
    shifter.prepare(kSr);
    shifter.set_shift_source(ShiftSource::direct);
    shifter.set_glide_ms(0.0, 0.0);
    shifter.set_mix(1.0);
    shifter.set_shift_semitones(kSemitones);
    shifter.reset();

    std::vector<double> wet;
    wet.reserve(kAnalysisLen);
    for (int n = 0; n < kSettle + kAnalysisLen; ++n) {
        const float x =
            static_cast<float>(std::sin(2.0 * kPi * kF0 * static_cast<double>(n) / kSr));
        const float y = shifter.process(x);
        REQUIRE(std::isfinite(y));
        if (n >= kSettle) wet.push_back(static_cast<double>(y));
    }

    const double expected = kF0 * ratio_of(kSemitones);
    REQUIRE_THAT(peak_near(wet, expected, 20.0, 0.25), WithinRel(expected, 0.003));
    REQUIRE(peak(wet) <= Shifter::kDcBlockerPeakGain);
    REQUIRE(shifter.latency_samples() ==
            static_cast<int>(std::lround(PitchShifter::kWindowMsDefault * kSr / 2000.0)));
}

TEST_CASE("a fresh instance survives being used before prepare",
          "[signal][pitch-shifter]") {
    // "Zero-init is valid but must see prepare before process" — valid has to
    // mean it does not read out of bounds or emit NaN, not merely that it
    // compiles.
    Shifter shifter;
    for (int n = 0; n < 128; ++n) {
        const double y = static_cast<double>(shifter.process(0.5));
        REQUIRE(std::isfinite(y));
    }
    shifter.prepare(kSr);
    shifter.reset();
    REQUIRE(std::isfinite(static_cast<double>(shifter.process(0.5))));
}

TEST_CASE("The peak-gain bound holds on sustained near-DC content too",
          "[signal][pitch-shifter][gain]") {
    // The bound was previously certified with a single 997 Hz sine — which is
    // precisely the signal for which a MAGNITUDE-response bound is valid, and
    // therefore the one input that could not reveal the error. The claimed wet
    // bound came from the DC blocker's magnitude peak at Nyquist (1.000327);
    // the real limit on a single sample is its impulse response's L1 norm,
    // exactly 2. Measured 1.97 against a claimed 1.0003 — 5.9 dB out.
    //
    // Reaching it needs sustained energy near DC, where the blocker's
    // differencing term and its pole both act on the same excursion: a slow
    // square, not a tone.
    for (double semitones : {-12.0, -5.0, 7.0, 12.0}) {
        Shifter shifter;
        shifter.prepare(kSr);
        shifter.set_shift_source(ShiftSource::direct);
        shifter.set_shift_semitones(semitones);
        shifter.set_mix(1.0);
        shifter.reset();

        double worst = 0.0;
        const int frames = static_cast<int>(kSr * 2.0);
        for (int n = 0; n < frames; ++n) {
            // 0.5 Hz square: as close to DC as anything musical gets.
            const double x = std::sin(2.0 * M_PI * 0.5 * n / kSr) >= 0.0 ? 1.0 : -1.0;
            worst = std::max(worst, std::abs(shifter.process(x)));
        }
        REQUIRE(worst <= Shifter::kDcBlockerPeakGain);
        // ...and the bound is not vacuous: this input genuinely exceeds the
        // old claim of ~1.0003, which is what made it wrong rather than loose.
        REQUIRE(worst > 1.05);
    }
}
