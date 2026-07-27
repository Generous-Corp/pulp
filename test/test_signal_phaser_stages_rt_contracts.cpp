#include "test_signal_phaser_stages_support.hpp"

TEST_CASE("Nothing allocates after construction",
          "[signal][phaser][rt]") {
    // Stronger than the spec's "allocation-free post-prepare()": every buffer
    // in this class is a fixed-size member, so `prepare()` does not allocate
    // either.
    auto engine = std::make_unique<Phaser>();
    std::vector<double> in(512, 0.25), ol(512), orr(512);

    require_allocates_no_memory([&] {
        engine->prepare(kSr);
        engine->reset();
        engine->set_stage_count(11);
        engine->set_rate_hz(4.0);
        engine->set_depth(0.7f);
        engine->set_center_hz(900.0);
        engine->set_feedback(0.8f);
        engine->set_mix(0.5f);
        engine->set_stereo_spread(0.25f);
        engine->set_wave(LfoWave::sine);
        engine->set_stagger_ratio(1.08);
        engine->set_seed(12345u);
        engine->process(in.data(), in.data(), ol.data(), orr.data(), 512);
        engine->process_mono(in.data(), ol.data(), 512);
        (void) engine->sweep_frequency_hz(1);
        (void) engine->stage_count();
    });
}

TEST_CASE("Process is safe when the output aliases the input",
          "[signal][phaser][rt]") {
    // Hosts pass the same buffer for in and out constantly. Each frame's input
    // is read into a local before its output is written, so in-place is exact —
    // asserted against a rendered-to-separate-buffers reference rather than
    // merely "not silent", because the classic symptom of getting this wrong is
    // a plausible-looking but different signal.
    Xorshift32 rng(0xFACEB00Cu);
    std::vector<double> in(2048);
    for (auto& v : in) v = rng.next_bipolar<double>();

    Phaser reference;
    reference.prepare(kSr);
    reference.set_feedback(0.8f);
    reference.set_stereo_spread(0.25f);
    reference.set_rate_hz(2.0);
    reference.reset();
    std::vector<double> ref_l, ref_r;
    render(reference, in, ref_l, ref_r);

    Phaser in_place;
    in_place.prepare(kSr);
    in_place.set_feedback(0.8f);
    in_place.set_stereo_spread(0.25f);
    in_place.set_rate_hz(2.0);
    in_place.reset();
    std::vector<double> a = in, b = in;
    in_place.process(a.data(), b.data(), a.data(), b.data(),
                     static_cast<int>(in.size()));

    for (std::size_t i = 0; i < in.size(); ++i) {
        REQUIRE(a[i] == ref_l[i]);
        REQUIRE(b[i] == ref_r[i]);
    }
}

TEST_CASE("Latency is zero and an impulse produces output at sample zero",
          "[signal][phaser][latency]") {
    REQUIRE(Phaser::latency_samples() == 0);
    REQUIRE(PhaserStages::latency_samples() == 0);

    for (double mix : {0.5, 1.0}) {
        for (int stages = Phaser::kMinStages; stages <= Phaser::kMaxStages;
             stages += 2) {
            Config cfg;
            cfg.mix = mix;
            cfg.stages = stages;
            cfg.feedback = Phaser::kColorOnFeedback;
            const auto h = frozen_impulse_response(cfg, 64);
            REQUIRE(h[0] != 0.0);
        }
    }

    // The feedback tap's one-sample memory is INSIDE the loop and is not
    // reportable latency: with the loop open or closed, sample 0 is unaffected
    // by it, because at sample 0 there is no previous output to feed back.
    Config open_loop;
    Config closed_loop;
    closed_loop.feedback = Phaser::kFeedbackMax;
    REQUIRE(frozen_impulse_response(open_loop, 64)[0] ==
            frozen_impulse_response(closed_loop, 64)[0]);
}

TEST_CASE("A decaying tail reaches exact zero rather than lingering subnormal",
          "[signal][phaser][denormal]") {
    // The slowest-decaying configuration the module offers: maximum feedback,
    // maximum stage count. `snap_to_zero` in each stage's integrator and on the
    // feedback memory is what turns the tail into exact zeros instead of a
    // subnormal drizzle that stalls the CPU on FTZ-less targets.
    //
    // The spec also asks for a release-mode TIMING probe here. A wall-clock
    // assertion is not deterministic on a shared CI runner, so the property the
    // timing probe stands in for — state reaching EXACT zero — is asserted
    // directly instead.
    Phaser engine;
    Config cfg;
    cfg.stages = Phaser::kMaxStages;
    cfg.feedback = Phaser::kFeedbackMax;
    cfg.mix = 1.0;
    configure_frozen(engine, cfg);

    const int total = static_cast<int>(5.0 * kSr);
    std::vector<double> in(static_cast<std::size_t>(total), 0.0);
    in[0] = 1.0;
    std::vector<double> ol, orr;
    render(engine, in, ol, orr);

    for (double v : ol) REQUIRE(std::fpclassify(v) != FP_SUBNORMAL);

    // Exact zero, with room to spare. The flush point is a physical quantity,
    // not a free parameter: this configuration takes 2.56 s to decay from a
    // unit impulse through `snap_to_zero`'s 1e-15 threshold, so the spec's 5 s
    // window leaves 2.4 s of margin. Asserting 1 s of margin keeps the test
    // meaningful without sitting on the edge of the measurement.
    const auto last_nonzero = static_cast<std::size_t>(
        std::find_if(ol.rbegin(), ol.rend(), [](double v) { return v != 0.0; })
            .base() -
        ol.begin());
    REQUIRE(ol.size() - last_nonzero > static_cast<std::size_t>(kSr));
    for (std::size_t i = last_nonzero; i < ol.size(); ++i) REQUIRE(ol[i] == 0.0);

    // And the flush point moves the way the loop says it should — longer with
    // more stages (more group delay per trip round the loop) and longer with
    // more feedback (less decay per trip). A guard that lost its threshold
    // would flush instantly and break this ordering, not just the margin above.
    const auto flush_sample = [](int stages, double feedback) {
        Config c;
        c.stages = stages;
        c.feedback = feedback;
        c.mix = 1.0;
        const auto h = frozen_impulse_response(c, static_cast<int>(5.0 * kSr));
        return static_cast<std::size_t>(
            std::find_if(h.rbegin(), h.rend(), [](double v) { return v != 0.0; })
                .base() -
            h.begin());
    };
    REQUIRE(flush_sample(Phaser::kMaxStages, Phaser::kFeedbackMax) >
            flush_sample(Phaser::kMinStages, Phaser::kFeedbackMax));
    REQUIRE(flush_sample(Phaser::kMaxStages, Phaser::kFeedbackMax) >
            flush_sample(Phaser::kMaxStages, Phaser::kColorOnFeedback));

    // Feeding silence into an already-silent instance stays exactly silent —
    // the feedback memory has been snapped too, not just the stage integrators.
    std::vector<double> more(1024, 0.0), m_l, m_r;
    render(engine, more, m_l, m_r);
    for (double v : m_l) REQUIRE(v == 0.0);
}

TEST_CASE("Stage count clamps into range and rounds down to even",
          "[signal][phaser][stages]") {
    // Documented behaviour, not a silent no-op: out-of-range and odd requests
    // resolve to a specific value that `stage_count()` reports back.
    Phaser engine;
    engine.prepare(kSr);

    const std::pair<int, int> cases[] = {
        {-5, 4}, {0, 4},  {3, 4},  {4, 4},   {5, 4},   {6, 6},
        {7, 6},  {9, 8},  {11, 10}, {12, 12}, {13, 12}, {100, 12},
    };
    for (auto [requested, expected] : cases) {
        engine.set_stage_count(requested);
        REQUIRE(engine.stage_count() == expected);
        REQUIRE(engine.stage_count() % 2 == 0);
        REQUIRE(engine.stage_count() >= Phaser::kMinStages);
        REQUIRE(engine.stage_count() <= Phaser::kMaxStages);
    }

    // And the clamped value is what actually runs: an odd request produces the
    // notch count of the EVEN value it resolved to, not of the value asked for.
    for (auto [requested, expected] : {std::pair{5, 4}, std::pair{11, 10}}) {
        Config cfg;
        cfg.stages = requested;
        const auto s = spectrum_of(frozen_impulse_response(cfg, kIrLenClosedLoop));
        REQUIRE(count_notches(s, 25.0) == Phaser::notch_count(expected));
    }

    REQUIRE(Phaser::kStageCountDefault == kSmallStoneStages);
    REQUIRE(Phaser().stage_count() == Phaser::kStageCountDefault);
}

TEST_CASE("The module is exactly linear so there is nothing to alias",
          "[signal][phaser][aliasing]") {
    // Series law 4 asks for an oversampling policy wherever a nonlinearity
    // aliases. The policy here is "none needed", and that is a claim about the
    // code, so it gets asserted rather than asserted-in-prose.
    //
    // Part one: a pure tone produces no harmonics. Measured on the frozen
    // engine at MAXIMUM feedback, because a feedback loop is exactly where a
    // stray nonlinearity would hide.
    Config cfg;
    cfg.stages = 8;
    cfg.feedback = Phaser::kFeedbackMax;
    cfg.mix = Phaser::kMixDefault;

    const int window = 48000;
    const int settle = 16384;
    const int fundamental = 997;   // prime, so no harmonic lands exactly on a
                                   // notch and gets flattered by the notch
                                   // rather than by the module's linearity
    {
        // ONE tone rendered, then the harmonic bins of THAT render read. (The
        // wrong way to do this is to render each harmonic separately and read
        // its own bin, which measures the response to a tone that was never
        // present.)
        Phaser engine;
        configure_frozen(engine, cfg);
        const double w = 2.0 * kPi * static_cast<double>(fundamental) / kSr;
        std::vector<double> in(static_cast<std::size_t>(settle + window));
        for (std::size_t i = 0; i < in.size(); ++i)
            in[i] = std::sin(w * static_cast<double>(i));
        std::vector<double> ol, orr;
        render(engine, in, ol, orr);

        const auto bin_amplitude = [&](int cycles) {
            std::complex<double> acc(0.0, 0.0);
            for (int i = 0; i < window; ++i) {
                const double phase = -2.0 * kPi * static_cast<double>(cycles) *
                                     static_cast<double>(i) /
                                     static_cast<double>(window);
                acc += ol[static_cast<std::size_t>(settle + i)] *
                       std::complex<double>(std::cos(phase), std::sin(phase));
            }
            return 2.0 * std::abs(acc) / static_cast<double>(window);
        };

        const double first = bin_amplitude(fundamental);
        REQUIRE(first > 0.1);
        for (int harmonic : {2, 3, 4, 5})
            REQUIRE(db(bin_amplitude(fundamental * harmonic) / first) < -100.0);
    }

    // Part two: superposition and homogeneity, the definition of linearity,
    // checked on the MODULATING engine — a linear time-varying system is still
    // linear, which is why sweeping coefficients needs no oversampling either.
    const auto render_with = [](const std::vector<double>& in) {
        Phaser engine;
        engine.prepare(kSr);
        engine.set_stage_count(8);
        engine.set_feedback(static_cast<float>(Phaser::kFeedbackMax));
        engine.set_rate_hz(5.0);
        engine.set_depth(1.0f);
        engine.reset();
        std::vector<double> ol, orr;
        render(engine, in, ol, orr);
        return ol;
    };

    Xorshift32 rng(0xC0FFEEu);
    std::vector<double> x(4096), y(4096), sum(4096);
    for (std::size_t i = 0; i < x.size(); ++i) {
        x[i] = rng.next_bipolar<double>();
        y[i] = rng.next_bipolar<double>();
        sum[i] = x[i] + y[i];
    }
    const auto rx = render_with(x);
    const auto ry = render_with(y);
    const auto rsum = render_with(sum);
    for (std::size_t i = 0; i < x.size(); ++i)
        REQUIRE_THAT(rsum[i], WithinAbs(rx[i] + ry[i], 1e-9));

    std::vector<double> scaled(x.size());
    for (std::size_t i = 0; i < x.size(); ++i) scaled[i] = 7.5 * x[i];
    const auto rscaled = render_with(scaled);
    for (std::size_t i = 0; i < x.size(); ++i)
        REQUIRE_THAT(rscaled[i], WithinAbs(7.5 * rx[i], 1e-9));
}

TEST_CASE("Stagger is off by default and detunes the notch set when engaged",
          "[signal][phaser][stagger]") {
    Phaser engine;
    engine.prepare(kSr);
    REQUIRE_THAT(engine.stagger_ratio(), WithinRel(Phaser::kStaggerDefault, 1e-12));

    // Off: every stage shares one corner, so the tangent law holds exactly —
    // which the notch tests above already depend on.
    // On: the stages' corners spread, the tangent law's premise (N IDENTICAL
    // sections) no longer holds, and the notches move. Undocumented as hardware
    // behaviour, so the only claim made for it is that it does something.
    const double f1 =
        Phaser::notch_frequency_hz(1, kSmallStoneStages, kRefCenterHz, kSr);

    Config staggered;
    staggered.stagger = 1.08;
    const auto h = frozen_impulse_response(staggered, kIrLenOpenLoop);
    const double moved = refine_minimum(h, f1 * 0.8, f1 * 1.3);
    REQUIRE(std::abs(moved - f1) / f1 > 0.01);

    // Bounded to its declared range, both ways.
    engine.set_stagger_ratio(5.0);
    REQUIRE_THAT(engine.stagger_ratio(), WithinRel(Phaser::kStaggerMax, 1e-12));
    engine.set_stagger_ratio(0.1);
    REQUIRE_THAT(engine.stagger_ratio(), WithinRel(Phaser::kStaggerMin, 1e-12));

    // Even fully staggered, the gain bound survives — every stage is still an
    // allpass, so |A| = 1 regardless of where the individual corners sit.
    Config worst;
    worst.stagger = Phaser::kStaggerMax;
    worst.stages = Phaser::kMaxStages;
    worst.feedback = Phaser::kFeedbackMax;
    worst.mix = 1.0;
    const auto s = spectrum_of(frozen_impulse_response(worst, kIrLenClosedLoop));
    REQUIRE(*std::max_element(s.magnitude.begin(), s.magnitude.end()) <=
            Phaser::worst_case_gain() + 1e-3);
}

TEST_CASE("The float and double instantiations agree on the physics",
          "[signal][phaser][parity]") {
    // `PhaserStages64` exists so an analysis path can measure a 200 dB null.
    // `PhaserStages` is what ships in a plugin. They must place their notches
    // in the same place, and the float one must still reach a null deep enough
    // to be the effect rather than a wobble.
    Config cfg;
    cfg.stages = 8;
    const auto h32 = frozen_impulse_response<PhaserStages>(cfg, kIrLenOpenLoop);
    const auto h64 = frozen_impulse_response<PhaserStages64>(cfg, kIrLenOpenLoop);

    for (int k = 1; k <= Phaser::notch_count(cfg.stages); ++k) {
        const double predicted =
            Phaser::notch_frequency_hz(k, cfg.stages, cfg.center_hz, kSr);
        const double f32 = refine_minimum(h32, predicted * 0.9, predicted * 1.1);
        const double f64 = refine_minimum(h64, predicted * 0.9, predicted * 1.1);
        REQUIRE_THAT(f32, WithinRel(f64, 1e-4));
        // Comfortably past the spec's 20 dB floor even in single precision.
        REQUIRE(db(magnitude_at(h32, 0.0) / magnitude_at(h32, f32)) > 60.0);
    }

    REQUIRE_THAT(PhaserStages::worst_case_gain(),
                 WithinRel(PhaserStages64::worst_case_gain(), 1e-12));
}

TEST_CASE("A default instance is the documented Small Stone preset",
          "[signal][phaser][defaults]") {
    Phaser engine;
    REQUIRE(engine.stage_count() == Phaser::kStageCountDefault);
    REQUIRE_THAT(engine.center_hz(), WithinRel(kRefCenterHz, 1e-12));
    REQUIRE_THAT(engine.mix(), WithinRel(Phaser::kMixDefault, 1e-12));
    REQUIRE_THAT(engine.feedback(), WithinAbs(Phaser::kColorOffFeedback, 1e-12));
    REQUIRE_THAT(engine.rate_hz(), WithinRel(Phaser::kRateDefaultHz, 1e-12));
    REQUIRE_THAT(engine.stereo_spread(), WithinAbs(0.0, 1e-12));
    REQUIRE(engine.wave() == LfoWave::triangle);
    REQUIRE_THAT(engine.stagger_ratio(), WithinRel(Phaser::kStaggerDefault, 1e-12));

    // The two documented "Color" positions are preset POINTS on a continuous
    // control, not a mode flag — both are reachable through the same setter.
    REQUIRE(Phaser::kColorOffFeedback < Phaser::kColorOnFeedback);
    REQUIRE(Phaser::kColorOnFeedback < Phaser::kFeedbackMax);

}

TEST_CASE("Colour feedback adds a resonant peak and does not deepen the notch",
          "[signal][phaser][feedback][spec-defect]") {
    // SPEC DEFECT, with the arithmetic. Spec section 3.5 says positive feedback
    // "sharpens/deepens notches ... audibly deeper, more resonant notches with
    // Color engaged". Measured on the shipped code at N = 4, fc = 400 Hz,
    // mix = 0.5, relative to each configuration's own peak:
    //
    //     feedback   deepest null   null at      response peak
    //     0.00        88.5 dB        964.6 Hz     1.000  (0.0 dB)
    //     0.65        19.9 dB       1025.3 Hz     1.929  (+5.7 dB)
    //     0.90        27.5 dB       1102.4 Hz     5.499 (+14.8 dB)
    //
    // Feedback makes the null SHALLOWER, MOVES it, and adds a large resonant
    // peak. "More resonant" is right; "deeper notches" is backwards. The
    // mechanism is one line of algebra: with the loop closed the wet path is
    // W = A/(1 - k·z^-1·A), whose magnitude is 1 only where
    // cos(angle(A) - w) = k/2. At k = 0 that is everywhere, which is what makes
    // exact cancellation possible at mix = 0.5; at k != 0 it is a handful of
    // isolated frequencies, so the two paths no longer carry equal amplitude at
    // the phase-inversion point and cannot fully cancel.
    //
    // This is also WHY every notch-position test above sets feedback to zero:
    // the tangent law is an OPEN-LOOP law. It describes the cascade, not the
    // loop around it.
    const double f1_open =
        Phaser::notch_frequency_hz(1, kSmallStoneStages, kRefCenterHz, kSr);

    struct Measured {
        double peak, peak_hz, audible_peak_hz, null_depth_db, null_hz;
    };
    const auto measure = [&](double fb) {
        Config cfg;
        cfg.feedback = fb;
        const auto h = frozen_impulse_response(cfg, kIrLenClosedLoop);
        const auto s = spectrum_of(h);
        Measured m{};
        const auto highest = static_cast<std::size_t>(
            std::max_element(s.magnitude.begin(), s.magnitude.end()) -
            s.magnitude.begin());
        m.peak = s.magnitude[highest];
        m.peak_hz = s.frequency(highest);

        // The largest response inside the audible band, which excludes both
        // exact-attainment endpoints below.
        const auto lo = s.magnitude.begin() +
                        static_cast<std::ptrdiff_t>(s.bin_for(20.0));
        const auto hi = s.magnitude.begin() +
                        static_cast<std::ptrdiff_t>(s.bin_for(20000.0));
        m.audible_peak_hz = s.frequency(static_cast<std::size_t>(
            std::max_element(lo, hi) - s.magnitude.begin()));
        // The deepest null of the whole response, wherever the loop has put it.
        const auto lowest =
            static_cast<std::size_t>(std::min_element(s.magnitude.begin() + 1,
                                                      s.magnitude.end()) -
                                     s.magnitude.begin());
        m.null_hz = refine_minimum(h, s.frequency(lowest) - 2.0 * s.bin_hz,
                                   s.frequency(lowest) + 2.0 * s.bin_hz);
        m.null_depth_db = db(m.peak / magnitude_at(h, m.null_hz));
        return m;
    };

    const auto off = measure(Phaser::kColorOffFeedback);
    const auto on = measure(Phaser::kColorOnFeedback);
    const auto maxed = measure(Phaser::kFeedbackMax);

    // Open loop: flat passband, and the null is exactly where the law says.
    REQUIRE_THAT(off.peak, WithinAbs(1.0, 1e-9));
    REQUIRE(off.null_depth_db > 60.0);

    // Colour on: a real resonant peak, rising with feedback...
    REQUIRE(on.peak > off.peak);
    REQUIRE(maxed.peak > on.peak);
    // ...whose height is the closed form, not a fitted number. At mix m the
    // peak is bounded by (1-m) + m/(1-k), and the bound is essentially
    // attained because the loop's phase returns to zero at a low enough
    // frequency that the wet term is nearly real and positive there.
    const auto inverted = measure(-Phaser::kColorOnFeedback);
    const auto inverted_max = measure(-Phaser::kFeedbackMax);

    for (auto [fb, m] : {std::pair{Phaser::kColorOnFeedback, on},
                         std::pair{Phaser::kFeedbackMax, maxed},
                         std::pair{-Phaser::kColorOnFeedback, inverted},
                         std::pair{-Phaser::kFeedbackMax, inverted_max}}) {
        // The bound depends on |k|, so both signs reach the same height. That
        // is not a coincidence to be tolerated, it is the bound being tight.
        const double bound = (1.0 - Phaser::kMixDefault) +
                             Phaser::kMixDefault / (1.0 - std::abs(fb));
        REQUIRE(m.peak <= bound + 1e-3);
        REQUIRE(m.peak > 0.995 * bound);
    }

    // And the notch is neither deeper nor where the open-loop law puts it.
    REQUIRE(on.null_depth_db < off.null_depth_db);
    REQUIRE(maxed.null_depth_db < off.null_depth_db);
    REQUIRE(on.null_hz > f1_open);
    REQUIRE(maxed.null_hz > on.null_hz);

    // WHERE the bound is attained is a closed form at both ends of the
    // spectrum, and it is opposite for the two signs. An allpass cascade is
    // exactly +1 at DC (each section is `(a+1)/(1+a) = 1`) and, for EVEN stage
    // counts, exactly +1 at Nyquist too (each section is `(a-1)/(1-a) = -1`,
    // and there is an even number of them). Meanwhile `z^-1` is +1 at DC and
    // -1 at Nyquist. So the loop term is exactly `+k` at DC and exactly `-k`
    // at Nyquist, and the denominator `1 - loop` hits its minimum `1 - |k|`:
    //
    //     positive feedback -> attained exactly at DC
    //     negative feedback -> attained exactly at Nyquist
    //
    // Not approached, attained — so this is asserted to 1e-6, not to a
    // grid-resolution tolerance.
    const double peak_bound = (1.0 - Phaser::kMixDefault) +
                              Phaser::kMixDefault * Phaser::worst_case_gain();
    REQUIRE_THAT(maxed.peak_hz, WithinAbs(0.0, 1e-12));
    REQUIRE_THAT(maxed.peak, WithinRel(peak_bound, 1e-6));
    REQUIRE_THAT(inverted_max.peak_hz, WithinAbs(kSr / 2.0, 1.0));
    REQUIRE_THAT(inverted_max.peak, WithinRel(peak_bound, 1e-6));

    // Inside the audible band — away from both of those endpoints — positive
    // feedback's resonance tracks `fc`, which is what makes it read as a
    // vocal formant riding the sweep rather than as a bass or treble lift.
    // That is the "Color" sound.
    REQUIRE_THAT(on.audible_peak_hz, WithinRel(kRefCenterHz, 0.05));
    REQUIRE_THAT(maxed.audible_peak_hz, WithinRel(kRefCenterHz, 0.05));

    // Negative feedback instead leaves the audible band flattened: its
    // resonance is parked at Nyquist and its deepest null is much shallower.
    // No hardware source documents it; it is a catalog-only extension.
    REQUIRE(inverted.null_depth_db < on.null_depth_db);
}

TEST_CASE("phaser retains controls and recovers from non-finite audio",
          "[signal][phaser][nonfinite]") {
    for (double bad : {NAN, INFINITY, -INFINITY}) {
        Phaser a, b;
        for (auto* p : {&a, &b}) { p->prepare(kSr); p->set_rate_hz(1.2); p->set_depth(.7f);
            p->set_center_hz(913); p->set_feedback(.31f); p->set_mix(.62f);
            p->set_stereo_spread(.17f); p->set_stagger_ratio(1.13); p->reset(); }
        a.set_rate_hz(bad); a.set_depth(static_cast<float>(bad)); a.set_center_hz(bad);
        a.set_feedback(static_cast<float>(bad)); a.set_mix(static_cast<float>(bad));
        a.set_stereo_spread(static_cast<float>(bad)); a.set_stagger_ratio(bad);
        REQUIRE(a.center_hz() == b.center_hz()); REQUIRE(a.feedback() == b.feedback());
        double inl=bad,inr=.2,al=1,ar=1; a.process(&inl,&inr,&al,&ar,1);
        REQUIRE(al==0); REQUIRE(ar==0); b.reset();
        for(int i=0;i<64;++i){ double x=.2,ya=0,za=0,yb=0,zb=0; a.process(&x,&x,&ya,&za,1); b.process(&x,&x,&yb,&zb,1); REQUIRE(ya==yb); REQUIRE(za==zb); }
    }
}
