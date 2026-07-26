#include "test_signal_phaser_stages_support.hpp"

TEST_CASE("Peak-sample amplitude under-reads a discrete sine",
          "[signal][phaser][measurement]") {
    // Not a property of the phaser — a property of the RULER. An 8 kHz sine at
    // 48 kHz has exactly six samples per cycle and none of them lands on the
    // crest, so its largest sample is sin(60 deg) = 0.866 of the true
    // amplitude: 1.25 dB low. Measured through a bypassed phaser, so what is
    // being compared is two readings of the SAME signal.
    Config bypass;
    bypass.mix = 0.0;

    const int window = 6000;         // 8 kHz is exactly 1000 cycles of it
    const int cycles = 1000;
    const int settle = 0;            // mix = 0 is a wire; nothing to settle

    Phaser engine;
    configure_frozen(engine, bypass);
    std::vector<double> in(static_cast<std::size_t>(window));
    for (std::size_t i = 0; i < in.size(); ++i)
        in[i] = std::sin(2.0 * kPi * 8000.0 * static_cast<double>(i) / kSr);
    std::vector<double> ol, orr;
    render(engine, in, ol, orr);

    double peak = 0.0;
    for (double v : ol) peak = std::max(peak, std::abs(v));

    const double coherent = coherent_sine_amplitude(bypass, cycles, window, settle);

    // The coherent DFT recovers the true unit amplitude exactly.
    REQUIRE_THAT(coherent, WithinRel(1.0, 1e-9));
    // The peak sample is low by exactly the computed amount — 20·log10(sin(pi/3)).
    const double expected_error_db = db(std::sin(kPi / 3.0));
    REQUIRE_THAT(db(peak), WithinAbs(expected_error_db, 1e-6));
    // Which is over a decibel: large enough to be mistaken for a real defect.
    REQUIRE(expected_error_db < -1.0);
}

TEST_CASE("Instruments agree on the same impulse response",
          "[signal][phaser][measurement]") {
    // The FFT is fast and coarse; the DTFT is exact at a point. If they ever
    // disagree, a physics test below is reading a broken ruler. Checked on the
    // hardest configuration — maximum feedback, where the response has the
    // sharpest features.
    Config cfg;
    cfg.stages = 12;
    cfg.feedback = Phaser::kFeedbackMax;
    cfg.mix = 1.0;

    const auto h = frozen_impulse_response(cfg, kIrLenClosedLoop);
    const auto s = spectrum_of(h);

    for (std::size_t bin : {std::size_t{1}, std::size_t{137}, std::size_t{4001},
                            std::size_t{20000}}) {
        const double from_fft = s.magnitude[bin];
        const double from_dtft = magnitude_at(h, s.frequency(bin));
        REQUIRE_THAT(from_fft, WithinRel(from_dtft, 1e-9));
    }
}

TEST_CASE("Coherent DFT of a real rendered sine matches the impulse response",
          "[signal][phaser][measurement]") {
    // The impulse-response instruments describe an LTI system. This closes the
    // loop on that claim by pushing an actual sine through `process()` and
    // measuring its output amplitude coherently. Agreement to 0.01 dB means
    // the frozen engine really is LTI and its impulse response really does
    // describe it.
    Config cfg;
    cfg.stages = 6;
    cfg.feedback = Phaser::kColorOnFeedback;

    const auto h = frozen_impulse_response(cfg, kIrLenClosedLoop);

    const int window = 48000;   // 1 s, so `cycles` is a frequency in whole Hz
    const int settle = 16384;   // past the closed-loop response's useful support

    for (int hz : {110, 400, 963, 2500, 7000}) {
        const double measured = coherent_sine_amplitude(cfg, hz, window, settle);
        const double predicted = magnitude_at(h, static_cast<double>(hz));
        REQUIRE_THAT(db(measured), WithinAbs(db(predicted), 0.01));
    }
}

TEST_CASE("Notch positions match the shipped digital law in Small Stone mode",
          "[signal][phaser][notch]") {
    Config cfg;   // stages 4, centre 400 Hz, feedback 0, mix 0.5 — the spec's
                  // reference configuration, by way of the shipped defaults.
    const auto h = frozen_impulse_response(cfg, kIrLenOpenLoop);

    REQUIRE(Phaser::notch_count(kSmallStoneStages) == 2);

    for (int k = 1; k <= Phaser::notch_count(kSmallStoneStages); ++k) {
        const double predicted = Phaser::notch_frequency_hz(
            k, kSmallStoneStages, kRefCenterHz, kSr);
        // A ±10 % bracket around the prediction contains exactly one null (the
        // neighbouring notches are 5.8x away at N = 4), so the search is
        // unimodal inside it and cannot slide onto the wrong notch.
        const double measured = refine_minimum(h, predicted * 0.9, predicted * 1.1);

        // Two orders tighter than the ±2 % the spec asked for. The law is not
        // an approximation to this filter; it is its closed form.
        REQUIRE_THAT(measured, WithinRel(predicted, 1e-4));

        // ...and it is a real null, not a shallow dip. The spec's floor is
        // 20 dB; exact cancellation at mix = 0.5 delivers far more, and
        // asserting the generous number would let a genuine mix-law regression
        // sneak past. 60 dB is still 40 dB above the criterion.
        const double depth_db = db(1.0 / magnitude_at(h, measured));
        REQUIRE(depth_db > 60.0);
    }
}

TEST_CASE("A notch sits exactly where the cascade phase reaches pi",
          "[signal][phaser][notch]") {
    // Ground truth INDEPENDENT of any notch formula. A notch is a cancellation
    // between the dry path and a unity-magnitude wet path, which can only
    // happen where the wet path's phase is an odd multiple of pi. So: measure
    // the notches from the mix = 0.5 response, then read the phase of the
    // mix = 1.0 (bare cascade) response at those same frequencies. If the
    // implementation, the digital law and this physics ever disagree, this is
    // the test that says which one is wrong.
    for (int stages = Phaser::kMinStages; stages <= Phaser::kMaxStages;
         stages += 2) {
        for (double fc : {100.0, kRefCenterHz, 2000.0}) {
            Config notched;
            notched.stages = stages;
            notched.center_hz = fc;
            Config wet = notched;
            wet.mix = 1.0;

            const auto h_notched = frozen_impulse_response(notched, kIrLenOpenLoop);
            const auto h_wet = frozen_impulse_response(wet, kIrLenOpenLoop);

            for (int k = 1; k <= Phaser::notch_count(stages); ++k) {
                const double predicted =
                    Phaser::notch_frequency_hz(k, stages, fc, kSr);
                const double measured =
                    refine_minimum(h_notched, predicted * 0.9, predicted * 1.1);

                REQUIRE_THAT(measured, WithinRel(predicted, 1e-4));

                // The bare cascade is unity magnitude everywhere...
                REQUIRE_THAT(std::abs(response_at(h_wet, measured)),
                             WithinAbs(1.0, 1e-6));
                // ...and exactly out of phase with dry at the null.
                REQUIRE_THAT(std::cos(std::arg(response_at(h_wet, measured))),
                             WithinAbs(-1.0, 1e-6));
            }
        }
    }
}

TEST_CASE("Analog prototype law versus the shipped digital law",
          "[signal][phaser][notch][spec-defect]") {
    // SPEC DEFECT, with the arithmetic. Acceptance tests 1 and 2 ask for
    // measured notches within +/-2 % of the CITED ANALOG law
    // `f_k = fc·tan((2k-1)pi/2N)`. The shipped stage is that prototype's
    // bilinear/TPT discretisation, whose notches are
    // `(fs/pi)·arctan(tan(pi·fc/fs)·tan((2k-1)pi/2N))`. Where the prewarp is
    // mild the two agree and the criterion is meetable; where it is not, no
    // correct implementation can pass. Both halves are asserted here so the
    // finding survives as an executable fact rather than a comment.

    // Half one — at the spec's own reference centre the laws agree, and the
    // prototype is a legitimate mental model there.
    for (int stages = Phaser::kMinStages; stages <= Phaser::kMaxStages;
         stages += 2) {
        for (int k = 1; k <= Phaser::notch_count(stages); ++k) {
            const double digital =
                Phaser::notch_frequency_hz(k, stages, kRefCenterHz, kSr);
            const double analog =
                Phaser::notch_frequency_analog_hz(k, stages, kRefCenterHz);
            REQUIRE(std::abs(analog - digital) / digital < 0.02);
        }
    }

    // Half two — inside the catalog's own `center_hz` range (100..2000 Hz) the
    // prototype breaks the +/-2 % criterion, and it does so at the SMALL STONE
    // stage count, not only at the exotic ones.
    {
        const double digital = Phaser::notch_frequency_hz(2, 4, 2000.0, kSr);
        const double analog = Phaser::notch_frequency_analog_hz(2, 4, 2000.0);
        REQUIRE(std::abs(analog - digital) / digital > 0.02);
    }
    {
        const double digital = Phaser::notch_frequency_hz(6, 12, 2000.0, kSr);
        const double analog = Phaser::notch_frequency_analog_hz(6, 12, 2000.0);
        REQUIRE(std::abs(analog - digital) / digital > 0.25);
    }

    // And the reason it is the prototype that is wrong rather than the code:
    // at `center_hz = 2000` with `depth = 100 %` the sweep reaches fc = 4 kHz,
    // where the prototype predicts a notch ABOVE Nyquist — a notch that cannot
    // exist. The digital law's arctan is bounded by pi/2, so it places every
    // notch strictly below Nyquist at any fc.
    REQUIRE(Phaser::notch_frequency_analog_hz(6, 12, 4000.0) > kSr / 2.0);
    for (int stages = Phaser::kMinStages; stages <= Phaser::kMaxStages;
         stages += 2)
        for (int k = 1; k <= Phaser::notch_count(stages); ++k)
            for (double fc : {20.0, 400.0, 4000.0, 20000.0})
                REQUIRE(Phaser::notch_frequency_hz(k, stages, fc, kSr) < kSr / 2.0);
}

TEST_CASE("Notch count is half the stage count",
          "[signal][phaser][notch]") {
    for (int stages = Phaser::kMinStages; stages <= Phaser::kMaxStages;
         stages += 2) {
        Config cfg;
        cfg.stages = stages;
        const auto h = frozen_impulse_response(cfg, kIrLenClosedLoop);
        const auto s = spectrum_of(h);

        // 0.73 Hz bins. The narrowest null in this configuration — the lowest
        // notch at 12 stages — is about +/-3 Hz wide at its 20 dB point, so the
        // grid resolves every one of them; the widest are hundreds of Hz.
        // 25 dB rather than the spec's 20 dB as the run threshold, purely to
        // stay clear of the 0 dB peaks between notches.
        REQUIRE(count_notches(s, 25.0) == Phaser::notch_count(stages));

        // Each of those notches is where the law says it is.
        for (int k = 1; k <= Phaser::notch_count(stages); ++k) {
            const double predicted =
                Phaser::notch_frequency_hz(k, stages, kRefCenterHz, kSr);
            const double measured =
                refine_minimum(h, predicted * 0.95, predicted * 1.05);
            REQUIRE_THAT(measured, WithinRel(predicted, 1e-4));
        }
    }
}

TEST_CASE("Adding stages interleaves new notches without moving the old ones",
          "[signal][phaser][notch]") {
    // The sonic claim behind multi-stage phasers: more stages reads as DENSER,
    // not merely different. Concretely, the N = 12 notch set contains the
    // N = 4 set — `tan((2k-1)pi/2N)` at (k=2,N=12) and (k=5,N=12) reproduce
    // (k=1,N=4) and (k=2,N=4) exactly, because 3/24 = 1/8 and 9/24 = 3/8.
    for (auto [k4, k12] : {std::pair{1, 2}, std::pair{2, 5}}) {
        REQUIRE_THAT(Phaser::notch_frequency_hz(k4, 4, kRefCenterHz, kSr),
                     WithinRel(Phaser::notch_frequency_hz(k12, 12, kRefCenterHz, kSr),
                               1e-12));
    }

    // And the notch sequence is strictly increasing in k at every stage count,
    // which is what makes "interleaves" the right word.
    for (int stages = Phaser::kMinStages; stages <= Phaser::kMaxStages;
         stages += 2)
        for (int k = 2; k <= Phaser::notch_count(stages); ++k)
            REQUIRE(Phaser::notch_frequency_hz(k, stages, kRefCenterHz, kSr) >
                    Phaser::notch_frequency_hz(k - 1, stages, kRefCenterHz, kSr));
}

TEST_CASE("Notch depth follows the mix cancellation law",
          "[signal][phaser][mix]") {
    // The spec asks only for "deepest at 0.5, less elsewhere". The law is a
    // closed form — depth = -20·log10|1 - 2·mix| — so assert the closed form.
    // A wrong mix law (equal-power, say, which is the tempting default for a
    // dry/wet control) puts sqrt(0.5) on each path at the midpoint and yields
    // NO null at all; a "less than" test would let that through as long as the
    // midpoint still happened to be the deepest point.
    const double f1 =
        Phaser::notch_frequency_hz(1, kSmallStoneStages, kRefCenterHz, kSr);

    double previous_depth = -1.0;
    for (double mix : {0.0, 0.25, 0.5, 0.75, 1.0}) {
        Config cfg;
        cfg.mix = mix;
        const auto h = frozen_impulse_response(cfg, kIrLenOpenLoop);

        // The passband reference: the mix-0.5 response peaks at exactly 1.0
        // wherever the cascade phase is an even multiple of pi, and so does
        // every other mix. DC is one such point for any stage count.
        const double passband = magnitude_at(h, 0.0);
        REQUIRE_THAT(passband, WithinAbs(1.0, 1e-9));

        const double depth_db = db(passband / magnitude_at(h, f1));

        if (mix == 0.5) {
            // Exact cancellation: bounded only by arithmetic, not by physics.
            REQUIRE(depth_db > 100.0);
        } else {
            REQUIRE_THAT(depth_db,
                         WithinAbs(-db(std::abs(1.0 - 2.0 * mix)), 1e-6));
        }

        // Monotone rising into the midpoint, monotone falling out of it — the
        // spec's "monotonic falloff on each side", read as one pass.
        if (mix <= 0.5) REQUIRE(depth_db > previous_depth);
        previous_depth = depth_db;
    }

    // Notch POSITION does not depend on mix: the phase geometry is upstream of
    // the mixer. Checked at the two extremes of usable mix.
    for (double mix : {0.25, 0.75}) {
        Config cfg;
        cfg.mix = mix;
        const auto h = frozen_impulse_response(cfg, kIrLenOpenLoop);
        REQUIRE_THAT(refine_minimum(h, f1 * 0.9, f1 * 1.1), WithinRel(f1, 1e-4));
    }
}

TEST_CASE("Mix at 1.0 is a flat allpass rather than a deeper phaser",
          "[signal][phaser][mix]") {
    // The corollary of the mix law, and the one users get wrong: fully wet is
    // NOT more phaser. It is the bare cascade — spectrally flat, only its
    // phase moving.
    Config cfg;
    cfg.mix = 1.0;
    const auto h = frozen_impulse_response(cfg, kIrLenOpenLoop);
    for (double hz : {20.0, 165.0, 400.0, 965.0, 3000.0, 12000.0, 20000.0})
        REQUIRE_THAT(magnitude_at(h, hz), WithinAbs(1.0, 1e-6));
}

TEST_CASE("A single TPT allpass stage is unity gain on noise",
          "[signal][phaser][allpass]") {
    // The identity the whole feedback-gain proof rests on, checked on the
    // SHIPPED stage — `TptFilterT::process_allpass` is what the cascade runs,
    // so there is no second copy of this filter to drift from.
    TptFilter64 stage;
    stage.prepare(kSr);
    stage.set_cutoff(kRefCenterHz);

    Xorshift32 rng(0x9E3779B9u);
    const int n = 1'000'000;
    const int settle = 4096;   // past the stage's own start-up transient
    double in_sq = 0.0, out_sq = 0.0;
    for (int i = 0; i < n + settle; ++i) {
        const double x = rng.next_bipolar<double>();
        const double y = stage.process_allpass(x);
        if (i >= settle) {
            in_sq += x * x;
            out_sq += y * y;
        }
    }
    const double in_rms = std::sqrt(in_sq / n);
    const double out_rms = std::sqrt(out_sq / n);
    REQUIRE_THAT(db(out_rms / in_rms), WithinAbs(0.0, 0.05));
}

TEST_CASE("The stage reproduces the spec's published worked trace",
          "[signal][phaser][allpass][spec-defect]") {
    // Section 6.1 of the spec publishes a hand trace of one sample through one
    // stage, offered as the thing "implementers can replay by hand ... to
    // confirm their AllpassStageT matches section 3.3 bit-for-bit". So it is
    // external ground truth for the coefficient formula — the one place in
    // this file where a number is restated rather than computed — and it is
    // worth checking exactly.
    //
    //   fc = 400 Hz, fs = 48 kHz, fresh state, x[0] = 1
    //   spec: g = tan(pi·400/48000) = 0.026186    <- correct
    //   spec: G = g/(1+g)           = 0.025519    <- SPEC DEFECT: 0.0255177
    //   spec: y_ap[0] = 2·G − 1     = −0.948962   <- follows from the above
    //   spec: s = y_lp + v = 2·v    =  0.051038   <- follows from the above
    //
    // One arithmetic slip in G, in its sixth decimal place, propagated through
    // the two figures derived from it. Tiny, but the trace's stated purpose is
    // bit-for-bit confirmation, and an implementer who trusts it will go
    // hunting for a 2.6e-6 discrepancy that is not in their code.
    const double g = std::tan(kPi * kRefCenterHz / kSr);
    const double G = g / (1.0 + g);

    // `g` as published is right.
    REQUIRE_THAT(g, WithinAbs(0.026186, 5e-7));

    // `G` as published is not, and this is by how much.
    REQUIRE_THAT(G, WithinAbs(0.0255177, 5e-8));
    REQUIRE(std::abs(G - 0.025519) > 1e-6);

    // The shipped stage agrees with the correct arithmetic to 1.4e-9 — and
    // that residual is itself a finding, not noise. `tpt_filter.hpp` spells its
    // pi as `SampleType{3.14159265358979323846f}`; the `f` suffix makes the
    // literal a FLOAT even in the `double` instantiation, so `TptFilter64`
    // computes its coefficient from a pi that is 2.78e-8 too large. The
    // effective corner frequency carries that same relative error.
    //
    // It is pre-existing, in a shared header this module does not own, and it
    // is 4.7e-7 of a cent of frequency — far below anything audible and four
    // orders below the 1e-4 the notch-position tests assert to. Recorded here
    // rather than worked around silently, and asserted against the exact
    // float-pi prediction so that FIXING the suffix fails this test loudly
    // instead of drifting past it.
    constexpr double kPiAsFloat = 3.14159274101257324219;   // (float) pi, widened
    const double wa_f = 2.0 * kSr * std::tan(2.0 * kPiAsFloat * kRefCenterHz /
                                             (2.0 * kSr));
    const double g_float_pi = wa_f / (2.0 * kSr + wa_f);

    TptFilter64 stage;
    stage.prepare(kSr);
    stage.set_cutoff(kRefCenterHz);
    const double y_ap = stage.process_allpass(1.0);

    REQUIRE_THAT(y_ap, WithinAbs(2.0 * g_float_pi - 1.0, 1e-15));
    REQUIRE_THAT(y_ap, WithinAbs(2.0 * G - 1.0, 2e-9));
    REQUIRE_THAT(kPiAsFloat / kPi - 1.0, WithinAbs(2.78e-8, 1e-10));

    // ...and the stage still differs from the SPEC's published figure by
    // essentially twice the slip in G, which is the signature that says "the
    // document is wrong here, not the code": the spec's error is a thousand
    // times the implementation's.
    REQUIRE_THAT(y_ap - (-0.948962), WithinAbs(2.0 * (G - 0.025519), 1e-8));
    REQUIRE(std::abs(2.0 * (G - 0.025519)) > 1000.0 * std::abs(2.0 * G - 1.0 - y_ap));

    // The trace's step 4: after four such stages the mixer sees
    // `out[0] = 0.5·x[0] + 0.5·y_ap[0]`, the impulse having passed through
    // four fresh stages in series.
    Config cfg;   // the trace's configuration is the shipped default
    const auto h = frozen_impulse_response(cfg, 8);
    REQUIRE_THAT(h[0], WithinAbs(0.5 + 0.5 * std::pow(2.0 * G - 1.0, 4), 1e-8));
}

TEST_CASE("The whole cascade is unity magnitude at every frequency",
          "[signal][phaser][allpass]") {
    // Stronger than the RMS statement above, and it is the version the gain
    // proof actually needs: |A(e^jw)| = 1 at EVERY w, not merely on average.
    for (int stages = Phaser::kMinStages; stages <= Phaser::kMaxStages;
         stages += 2) {
        Config cfg;
        cfg.stages = stages;
        cfg.mix = 1.0;
        const auto h = frozen_impulse_response(cfg, kIrLenOpenLoop);
        const auto s = spectrum_of(h);
        for (std::size_t bin = 0; bin < s.magnitude.size(); ++bin)
            REQUIRE_THAT(s.magnitude[bin], WithinAbs(1.0, 1e-6));
    }
}

TEST_CASE("Feedback worst-case gain equals the shipped registry bound",
          "[signal][phaser][feedback][gain]") {
    // The number Forge's registry row cites. `worst_case_gain()` is
    // 1/(1 - kFeedbackMax) = 10.0x = 20.0 dB, and it is an EQUALITY, not a
    // ceiling: |A| = 1 exactly, so the loop peaks at exactly 1/(1-|k|)
    // wherever its phase comes back around. Both halves are asserted, because
    // a one-sided "<=" would still pass if the feedback path were silently
    // broken.
    REQUIRE_THAT(Phaser::worst_case_gain(), WithinRel(10.0, 1e-12));
    REQUIRE_THAT(db(Phaser::worst_case_gain()), WithinAbs(20.0, 1e-9));

    for (double sign : {+1.0, -1.0}) {
        Config cfg;
        cfg.feedback = sign * Phaser::kFeedbackMax;
        cfg.mix = 1.0;   // the worst case over mix: 1/(1-k) > 1, so all-wet
                         // beats any blend with the unity-gain dry path.
        const auto h = frozen_impulse_response(cfg, kIrLenClosedLoop);
        const auto s = spectrum_of(h);
        const double peak =
            *std::max_element(s.magnitude.begin(), s.magnitude.end());

        REQUIRE(peak <= Phaser::worst_case_gain() + 1e-3);
        // Attained. Bin sampling can only miss a peak, never overshoot it, and
        // the 0.73 Hz grid lands within 0.3 % of this loop's peak.
        REQUIRE(peak > 0.99 * Phaser::worst_case_gain());
    }
}

TEST_CASE("The gain bound holds across the whole parameter range",
          "[signal][phaser][feedback][gain]") {
    // Series law 8 asks for a bound the module's OWN tests assert across the
    // range, not at one flattering operating point. Every combination of the
    // extremes of stage count, feedback sign and magnitude, mix, and the
    // catalog's `center_hz` span.
    for (int stages = Phaser::kMinStages; stages <= Phaser::kMaxStages;
         stages += 2) {
        for (double fb : {-Phaser::kFeedbackMax, -Phaser::kColorOnFeedback, 0.0,
                          Phaser::kColorOnFeedback, Phaser::kFeedbackMax}) {
            for (double mix : {0.0, 0.5, 1.0}) {
                for (double fc : {100.0, 2000.0}) {
                    Config cfg;
                    cfg.stages = stages;
                    cfg.feedback = fb;
                    cfg.mix = mix;
                    cfg.center_hz = fc;

                    const auto h =
                        frozen_impulse_response(cfg, kIrLenClosedLoop);
                    const auto s = spectrum_of(h);
                    const double peak =
                        *std::max_element(s.magnitude.begin(), s.magnitude.end());
                    REQUIRE(peak <= Phaser::worst_case_gain() + 1e-3);
                }
            }
        }
    }
}

TEST_CASE("Feedback cannot be pushed past the bound it was proved for",
          "[signal][phaser][feedback][gain]") {
    // The bound is `1/(1 - kFeedbackMax)`, so the clamp IS the proof's
    // hypothesis. If a caller could widen it the registry number would become
    // fiction, and at |k| >= 1 the supremum is infinite.
    Phaser engine;
    engine.prepare(kSr);
    for (float requested : {5.0f, 1.0f, 0.95f, -0.95f, -1.0f, -7.0f}) {
        engine.set_feedback(requested);
        REQUIRE(std::abs(engine.feedback()) <= Phaser::kFeedbackMax);
    }
    engine.set_feedback(static_cast<float>(Phaser::kColorOnFeedback));
    REQUIRE_THAT(engine.feedback(), WithinRel(Phaser::kColorOnFeedback, 1e-6));
}

TEST_CASE("Maximum feedback stays bounded on 30 seconds of full-scale noise",
          "[signal][phaser][feedback][stability]") {
    // The empirical companion to the small-gain argument, run on the modulating
    // engine (not the frozen one) so the time-varying case is covered too.
    Phaser engine;
    engine.prepare(kSr);
    engine.set_stage_count(Phaser::kMaxStages);
    engine.set_feedback(static_cast<float>(Phaser::kFeedbackMax));
    engine.set_mix(1.0f);
    engine.set_rate_hz(2.0);
    engine.set_depth(1.0f);
    engine.set_center_hz(kRefCenterHz);
    engine.set_stereo_spread(0.25f);
    engine.reset();

    Xorshift32 rng(0x1234567u);
    const int total = static_cast<int>(30.0 * kSr);
    const int block = 512;
    std::vector<double> in(block), ol, orr;

    double in_sq = 0.0, out_sq = 0.0;
    double peak_out = 0.0;
    for (int done = 0; done < total; done += block) {
        for (auto& v : in) v = rng.next_bipolar<double>();
        render(engine, in, ol, orr);
        for (int i = 0; i < block; ++i) {
            REQUIRE(std::isfinite(ol[static_cast<std::size_t>(i)]));
            REQUIRE(std::isfinite(orr[static_cast<std::size_t>(i)]));
            in_sq += in[static_cast<std::size_t>(i)] * in[static_cast<std::size_t>(i)];
            out_sq += ol[static_cast<std::size_t>(i)] * ol[static_cast<std::size_t>(i)];
            peak_out = std::max(peak_out, std::abs(ol[static_cast<std::size_t>(i)]));
        }
    }
    const double in_rms = std::sqrt(in_sq / total);
    const double out_rms = std::sqrt(out_sq / total);

    // RMS is bounded by the worst-case gain applied to the input RMS — a
    // necessary condition of the frequency-domain bound, checked in the time
    // domain on a signal that excites every frequency at once.
    REQUIRE(out_rms <= Phaser::worst_case_gain() * in_rms);
    REQUIRE(std::isfinite(peak_out));
    // And it does not merely stay finite: it resonates, which is the whole
    // point of the control.
    REQUIRE(out_rms > in_rms);
}

TEST_CASE("Stereo spread offsets the right channel sweep by a quarter cycle",
          "[signal][phaser][stereo]") {
    const double rate_hz = 1.0;
    const double depth = 0.9;   // short of 100 % so the sweep never reaches the
                                // `kSweepFloorHz` clamp, which would flatten
                                // the contour and hide a real phase error
    const int quarter = static_cast<int>(0.25 / rate_hz * kSr);
    const int frames = static_cast<int>(3.0 / rate_hz * kSr);

    const auto c = sweep_contours(frames, rate_hz, 0.25, depth, kRefCenterHz);

    // The right channel LEADS by a quarter cycle: its LFO phase is
    // `phase + 0.25`, so `fc_R(t) = fc_L(t + 0.25/rate)`.
    REQUIRE_THAT(correlation_at_lag(c.left, c.right, -quarter),
                 WithinAbs(1.0, 1e-3));
    // Quadrature: orthogonal at zero lag. Exact for any odd-symmetric shape —
    // a triangle's harmonics are all odd, and every one of them picks up a
    // quarter-turn whose cosine is zero.
    REQUIRE_THAT(correlation_at_lag(c.left, c.right, 0), WithinAbs(0.0, 1e-3));
}

TEST_CASE("Stereo spread at half a cycle inverts the sweep exactly",
          "[signal][phaser][stereo]") {
    // The sharpest available statement of the phase relationship, and it needs
    // no correlation: a triangle is odd-symmetric about its half cycle, so a
    // 0.5-cycle offset is an exact inversion and the two channels' corner
    // frequencies must sum to twice the centre, sample for sample.
    const auto c = sweep_contours(static_cast<int>(2.0 * kSr), 1.0, 0.5, 0.9,
                                  kRefCenterHz);
    for (std::size_t i = 0; i < c.left.size(); ++i)
        REQUIRE_THAT(c.left[i] + c.right[i], WithinAbs(2.0 * kRefCenterHz, 1e-9));
}

TEST_CASE("Zero stereo spread makes the two channels identical",
          "[signal][phaser][stereo]") {
    Phaser engine;
    engine.prepare(kSr);
    engine.set_stereo_spread(0.0f);
    engine.set_rate_hz(1.5);
    engine.set_feedback(static_cast<float>(Phaser::kColorOnFeedback));
    engine.reset();

    Xorshift32 rng(0xABCDEF01u);
    std::vector<double> in(8192);
    for (auto& v : in) v = rng.next_bipolar<double>();
    std::vector<double> ol, orr;
    render(engine, in, ol, orr);

    for (std::size_t i = 0; i < ol.size(); ++i) REQUIRE(ol[i] == orr[i]);
}

TEST_CASE("Saw stereo spread remains a literal phase offset",
          "[signal][phaser][stereo]") {
    constexpr double center = kRefCenterHz;
    const int frames = static_cast<int>(2.0 * kSr);
    const auto zero = sweep_contours(frames, 1.0, 0.0, 0.9, center, LfoWave::saw_up);
    const auto quarter = sweep_contours(frames, 1.0, 0.25, 0.9, center, LfoWave::saw_up);
    const auto half = sweep_contours(frames, 1.0, 0.5, 0.9, center, LfoWave::saw_up);

    double quarter_difference = 0.0;
    double half_inversion_error = 0.0;
    double spread_change = 0.0;
    for (std::size_t i = 0; i < zero.left.size(); ++i) {
        REQUIRE(zero.left[i] == zero.right[i]);
        quarter_difference =
            std::max(quarter_difference, std::abs(quarter.left[i] - quarter.right[i]));
        half_inversion_error =
            std::max(half_inversion_error, std::abs(half.left[i] + half.right[i] - 2.0 * center));
        spread_change =
            std::max(spread_change, std::abs(quarter.right[i] - half.right[i]));
    }
    REQUIRE(quarter_difference > 0.1 * center);
    REQUIRE(half_inversion_error > 0.1 * center);
    REQUIRE(spread_change > 0.1 * center);
}

TEST_CASE("Triangle sweeps at a constant rate and sine lingers at its extremes",
          "[signal][phaser][lfo]") {
    const double rate_hz = 1.0;
    const double depth = 0.9;   // clear of the sweep floor, as above
    const int frames = static_cast<int>(2.0 / rate_hz * kSr);

    // Triangle: |d(fc)/dt| is one value, computed from the shipped mapping —
    // a triangle traverses 4 units of its bipolar range per cycle, and
    // `fc = center·(1 + depth·kSweepRangeRatio·lfo)`.
    {
        const auto c = sweep_contours(frames, rate_hz, 0.0, depth, kRefCenterHz,
                                      LfoWave::triangle);
        const double expected_step = 4.0 * rate_hz / kSr * depth *
                                     Phaser::kSweepRangeRatio * kRefCenterHz;

        int reversals = 0;
        for (std::size_t i = 2; i < c.left.size(); ++i) {
            const double step = std::abs(c.left[i] - c.left[i - 1]);
            if (std::abs(step - expected_step) > 1e-6 * expected_step) {
                // Only the frames straddling the two extrema may differ, where
                // the ramp reverses inside one sample.
                ++reversals;
                continue;
            }
            REQUIRE_THAT(step, WithinRel(expected_step, 1e-6));
        }
        // Two turning points per cycle, two cycles rendered.
        REQUIRE(reversals <= 4);
    }

    // Sine: stationary at the extremes, fastest through the middle.
    {
        const auto c = sweep_contours(frames, rate_hz, 0.0, depth, kRefCenterHz,
                                      LfoWave::sine);
        std::vector<double> step(c.left.size(), 0.0);
        for (std::size_t i = 1; i < c.left.size(); ++i)
            step[i] = std::abs(c.left[i] - c.left[i - 1]);

        const double max_step = *std::max_element(step.begin() + 1, step.end());

        const auto top = static_cast<std::size_t>(
            std::max_element(c.left.begin(), c.left.end()) - c.left.begin());
        const auto bottom = static_cast<std::size_t>(
            std::min_element(c.left.begin(), c.left.end()) - c.left.begin());

        // At the turning points the sweep is essentially stopped...
        REQUIRE(step[top] < 0.01 * max_step);
        REQUIRE(step[bottom] < 0.01 * max_step);
        // ...and it is moving fastest where it crosses the centre.
        const auto fastest = static_cast<std::size_t>(
            std::max_element(step.begin() + 1, step.end()) - step.begin());
        REQUIRE_THAT(c.left[fastest], WithinRel(kRefCenterHz, 0.01));

        // Which is the audible difference the doc block claims: a sine spends
        // longer near its extremes than a triangle does. Measured as the
        // fraction of frames within the top 10 % of the excursion.
        const auto dwell = [](const std::vector<double>& contour) {
            const double lo = *std::min_element(contour.begin(), contour.end());
            const double hi = *std::max_element(contour.begin(), contour.end());
            const double edge = hi - 0.1 * (hi - lo);
            return static_cast<double>(
                       std::count_if(contour.begin(), contour.end(),
                                     [edge](double v) { return v >= edge; })) /
                   static_cast<double>(contour.size());
        };
        const auto tri = sweep_contours(frames, rate_hz, 0.0, depth,
                                        kRefCenterHz, LfoWave::triangle);
        REQUIRE(dwell(c.left) > dwell(tri.left));
    }
}

TEST_CASE("The sweep mapping is linear in Hz and clamped at both ends",
          "[signal][phaser][lfo]") {
    // The documented mapping: `fc = center·(1 + depth·kSweepRangeRatio·lfo)`,
    // clamped into `[kSweepFloorHz, kSweepCeilingRatio·fs]`. Linear in Hz
    // rather than logarithmic because the OTA topology it models sweeps a
    // control CURRENT, which moves the corner linearly.
    {
        // Half depth, no clamping: the excursion is exactly proportional to
        // depth and symmetric about the centre.
        const auto c = sweep_contours(static_cast<int>(2.0 * kSr), 1.0, 0.0, 0.5,
                                      kRefCenterHz);
        const double hi = *std::max_element(c.left.begin(), c.left.end());
        const double lo = *std::min_element(c.left.begin(), c.left.end());
        REQUIRE_THAT(hi, WithinRel(kRefCenterHz * 1.5, 1e-6));
        REQUIRE_THAT(lo, WithinRel(kRefCenterHz * 0.5, 1e-6));
        REQUIRE_THAT(0.5 * (hi + lo), WithinRel(kRefCenterHz, 1e-9));
    }
    {
        // Full depth: the mapping reaches 0 Hz, where the stage would degenerate
        // to a sign flip, so the floor takes over. Documented behaviour, not an
        // accident — and it is the reason the shape tests above run at 90 %.
        const auto c = sweep_contours(static_cast<int>(2.0 * kSr), 1.0, 0.0, 1.0,
                                      kRefCenterHz);
        const double lo = *std::min_element(c.left.begin(), c.left.end());
        REQUIRE_THAT(lo, WithinAbs(Phaser::kSweepFloorHz, 1e-9));
        REQUIRE_THAT(*std::max_element(c.left.begin(), c.left.end()),
                     WithinRel(2.0 * kRefCenterHz, 1e-6));
    }
    {
        // And the ceiling: a high centre at full depth is held below Nyquist
        // with margin, so the bilinear prewarp never diverges.
        Phaser engine;
        engine.prepare(kSr);
        engine.set_center_hz(kSr);   // far past the ceiling on its own
        REQUIRE_THAT(engine.center_hz(),
                     WithinRel(Phaser::kSweepCeilingRatio * kSr, 1e-12));

        const auto c = sweep_contours(static_cast<int>(1.0 * kSr), 1.0, 0.0, 1.0,
                                      20000.0);
        REQUIRE(*std::max_element(c.left.begin(), c.left.end()) <=
                Phaser::kSweepCeilingRatio * kSr + 1e-9);
    }
}

TEST_CASE("Renders are bit-identical for the same parameters and input",
          "[signal][phaser][determinism]") {
    // No RNG is involved anywhere in this module — even the LFO's stochastic
    // shapes are seeded and rewound by `reset()` — so equality here is BIT
    // equality, not an epsilon.
    const auto run = [](LfoWave wave) {
        Phaser engine;
        engine.prepare(kSr);
        engine.set_stage_count(8);
        engine.set_rate_hz(3.0);
        engine.set_depth(0.8f);
        engine.set_feedback(static_cast<float>(Phaser::kColorOnFeedback));
        engine.set_stereo_spread(0.25f);
        engine.set_wave(wave);
        engine.reset();

        Xorshift32 rng(0x5EED0001u);
        std::vector<double> in(4096);
        for (auto& v : in) v = rng.next_bipolar<double>();
        std::vector<double> ol, orr;
        render(engine, in, ol, orr);
        ol.insert(ol.end(), orr.begin(), orr.end());
        return ol;
    };

    for (LfoWave wave : {LfoWave::triangle, LfoWave::sine,
                         LfoWave::sample_hold, LfoWave::smooth_random}) {
        const auto a = run(wave);
        const auto b = run(wave);
        REQUIRE(a.size() == b.size());
        for (std::size_t i = 0; i < a.size(); ++i) REQUIRE(a[i] == b[i]);
    }
}
