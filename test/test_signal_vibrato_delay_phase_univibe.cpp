#include "test_signal_vibrato_support.hpp"

TEST_CASE("Pitch instrument recovers a known FM deviation", "[vibrato][instrument]") {
    // Synthetic tone with an exactly known peak deviation of beta*f_m Hz.
    constexpr double kBeta = 0.4225;
    const auto cycle = static_cast<std::size_t>(kFs / kProbeRateHz);

    for (double carrier : {200.0, 1000.0, 4000.0}) {
        std::vector<double> x(cycle * kRenderCycles);
        for (std::size_t i = 0; i < x.size(); ++i) {
            const double t = static_cast<double>(i) / kFs;
            x[i] = std::sin(kTwoPi * carrier * t + kBeta * std::sin(kTwoPi * kProbeRateHz * t));
        }
        const auto measured =
            peak_cents_deviation(instantaneous_phase(x, carrier),
                                 static_cast<int>(cycle * kSkipCycles), carrier, kProbeRateHz);

        const double expected_up =
            1200.0 * std::log2((carrier + kBeta * kProbeRateHz) / carrier);
        const double expected_down =
            1200.0 * std::log2((carrier - kBeta * kProbeRateHz) / carrier);
        // 0.5 % covers the one-period window's own sinc droop, which is largest
        // at the lowest carrier because the window is longest there.
        CHECK(measured.up_cents == Approx(expected_up).epsilon(0.005));
        CHECK(measured.down_cents == Approx(expected_down).epsilon(0.005));
    }

    // An unmodulated tone must read as no vibrato at all, or every "the engine
    // does nothing" branch below would pass for the wrong reason.
    const auto flat = peak_cents_deviation(instantaneous_phase(sine_buffer(cycle * kRenderCycles, 1000.0), 1000.0),
                                           static_cast<int>(cycle * kSkipCycles), 1000.0, kProbeRateHz);
    CHECK(std::abs(flat.up_cents) < 1e-3);
    CHECK(std::abs(flat.down_cents) < 1e-3);
}

TEST_CASE("DelayVibrato hits the cents depth its own constants predict", "[vibrato][delay]") {
    DelayVibrato64 engine;
    engine.prepare(kFs);
    engine.set_rate_hz(kProbeRateHz);
    engine.set_depth_cents(DelayVibrato64::kDefaultDepthCents);
    engine.reset();

    // Predicted from the SHIPPED amplitude, not from a restated literal:
    // d'(t) peaks at A*2*pi*f_m, and the frequency scale is 1 - d'(t).
    const double slope_peak =
        engine.modulation_amplitude_samples() / kFs * kTwoPi * engine.rate_hz();
    const double expected_up = 1200.0 * std::log2(1.0 + slope_peak);
    const double expected_down = 1200.0 * std::log2(1.0 - slope_peak);

    const auto measured = measure_engine(engine, 1000.0, kProbeRateHz);
    CHECK(measured.up_cents == Approx(expected_up).epsilon(0.02));
    CHECK(measured.down_cents == Approx(expected_down).epsilon(0.02));

    // The upward and downward excursions are NOT equal, and the asymmetry is a
    // property of the arithmetic rather than an artefact: a symmetric swing in
    // delay is an asymmetric swing in cents because cents are logarithmic.
    CHECK(std::abs(expected_down) > std::abs(expected_up));
}

TEST_CASE("DelayVibrato shifts every frequency by the same cents", "[vibrato][delay]") {
    // The normative distinction. A modulated tap scales frequency by the delay's
    // time derivative, which knows nothing about the frequency it is scaling.
    std::vector<Deviation> readings;
    for (double carrier : {200.0, 1000.0, 4000.0}) {
        DelayVibrato64 engine;
        engine.prepare(kFs);
        engine.set_rate_hz(kProbeRateHz);
        engine.reset();
        readings.push_back(measure_engine(engine, carrier, kProbeRateHz));
    }

    for (std::size_t i = 1; i < readings.size(); ++i) {
        CHECK(readings[i].up_cents == Approx(readings[0].up_cents).epsilon(0.02));
        CHECK(readings[i].down_cents == Approx(readings[0].down_cents).epsilon(0.02));
    }
}

TEST_CASE("DelayVibrato pitch-modulates without amplitude-modulating",
          "[vibrato][delay][interpolator]") {
    // The reason the spec calls for a Lagrange-3 read rather than the delay
    // line's own two-point default. Both track pitch identically — the delay
    // trajectory sets that, not the interpolator — so the pitch tests above
    // cannot tell them apart. What separates them is magnitude flatness across
    // the fractional range: a two-point read loses roughly 0.3 dB at a half
    // sample by 4 kHz and over a dB by 8 kHz, which arrives as amplitude
    // modulation at the vibrato rate on exactly the material a lead line lives
    // in.
    //
    // Bounds are acceptance-class. The residual at 1 kHz is the measurement's
    // own floor (the one-period analysis window dips slightly under FM), so the
    // 1 kHz bound documents that floor rather than the interpolator.
    const auto cycle = static_cast<std::size_t>(kFs / kProbeRateHz);
    const std::vector<std::pair<double, double>> limits{
        {1000.0, 0.15}, {4000.0, 0.20}, {8000.0, 0.60}};

    for (const auto& [carrier, limit_db] : limits) {
        DelayVibrato64 engine;
        engine.prepare(kFs);
        engine.set_rate_hz(kProbeRateHz);
        engine.reset();
        const auto input = sine_buffer(cycle * kRenderCycles, carrier);
        std::vector<double> output(input.size());
        for (std::size_t i = 0; i < input.size(); ++i) output[i] = engine.process(input[i]);
        CHECK(envelope_ripple_db(output, carrier, static_cast<int>(cycle * kSkipCycles)) <
              limit_db);
    }
}

TEST_CASE("DelayVibrato reports its exact latency", "[vibrato][delay][latency]") {
    DelayVibrato64 engine;
    engine.prepare(kFs);
    engine.reset();

    const auto expected = static_cast<std::size_t>(
        std::ceil(engine.base_delay_samples() + engine.modulation_amplitude_samples()));
    CHECK(engine.latency_samples() == expected);
    CHECK(engine.latency_samples() > 0);

    // Zero depth still costs the interpolator's base delay: a fractional read
    // cannot be free, and reporting 0 would misalign a host's compensation.
    DelayVibrato64 flat;
    flat.prepare(kFs);
    flat.set_depth_cents(0.0);
    flat.reset();
    CHECK(flat.latency_samples() > 0);
    CHECK(flat.base_delay_samples() == Approx(DelayVibrato64::kMinBaseDelaySamples));
}

TEST_CASE("DelayVibrato lifecycle delays then fades the depth in", "[vibrato][delay][lifecycle]") {
    constexpr double kDelayMs = 400.0;
    constexpr double kFadeMs = 600.0;
    const auto delay_samples = static_cast<int>(kDelayMs * 0.001 * kFs);
    const auto full_samples = static_cast<int>((kDelayMs + kFadeMs) * 0.001 * kFs);

    DelayVibrato64 engine;
    engine.prepare(kFs);
    engine.set_delay_ms(kDelayMs);
    engine.set_fade_in_ms(kFadeMs);
    engine.reset();

    // A bit-exactly periodic carrier: one tabulated period, tiled. Built this
    // way because `sin(2*pi*f*i/fs)` is NOT bit-periodic in i — the argument
    // differs by ulps between periods — and the assertion below is about exact
    // sample equality.
    constexpr int kPeriod = 100;  // 480 Hz at 48 kHz
    std::vector<double> period_table(kPeriod);
    for (int i = 0; i < kPeriod; ++i) {
        period_table[static_cast<std::size_t>(i)] =
            std::sin(kTwoPi * static_cast<double>(i) / static_cast<double>(kPeriod));
    }

    int first_moving = -1;
    int first_full = -1;
    std::vector<double> rendered(static_cast<std::size_t>(2 * full_samples));
    for (int i = 0; i < 2 * full_samples; ++i) {
        rendered[static_cast<std::size_t>(i)] =
            engine.process(period_table[static_cast<std::size_t>(i % kPeriod)]);
        if (first_moving < 0 && engine.depth_envelope() > 0.0) first_moving = i;
        if (first_full < 0 && engine.depth_envelope() >= 1.0) first_full = i;
    }

    // Both land one sample late against the nominal count. That is the
    // envelope's accumulator summing 1/N exactly N times and landing a few ulp
    // short of 1, not a policy difference — hence the spec's own +/-1 sample.
    CHECK(std::abs(first_moving - delay_samples) <= 1);
    CHECK(std::abs(first_full - (full_samples - 1)) <= 1);

    // Zero pitch deviation, stated as a property of the audio rather than of the
    // envelope: a constant delay applied to an exactly periodic input yields an
    // exactly periodic output. Any modulation at all breaks it.
    const int settle = static_cast<int>(std::ceil(engine.latency_samples())) + kPeriod;
    for (int i = settle; i + kPeriod < delay_samples; ++i) {
        REQUIRE(rendered[static_cast<std::size_t>(i)] ==
                rendered[static_cast<std::size_t>(i + kPeriod)]);
    }

    // And the same check must FAIL once the fade has finished, or it would be
    // passing because the instrument cannot see modulation rather than because
    // there is none.
    int broken = 0;
    for (int i = full_samples; i + kPeriod < 2 * full_samples; ++i) {
        if (rendered[static_cast<std::size_t>(i)] !=
            rendered[static_cast<std::size_t>(i + kPeriod)]) {
            ++broken;
        }
    }
    CHECK(broken > 0);
}

TEST_CASE("DelayVibrato delay-only lifecycle uses its declared zero fade",
          "[vibrato][delay][lifecycle]") {
    constexpr double kDelayMs = 20.0;
    const auto delay_samples = static_cast<int>(kDelayMs * 0.001 * kFs);

    DelayVibrato64 engine;
    engine.prepare(kFs);
    engine.set_delay_ms(kDelayMs);
    engine.reset();

    for (int i = 0; i + 1 < delay_samples; ++i) {
        engine.process(0.0);
        CHECK(engine.depth_envelope() == 0.0);
    }
    engine.process(0.0);
    CHECK(engine.depth_envelope() == 1.0);
}

TEST_CASE("PhaseVibrato wobble depends strongly on frequency", "[vibrato][phase]") {
    // The counterpart of the DelayVibrato frequency-independence test, at the
    // engine's documented default blend. The spec asks for at least 6 dB of
    // difference; the mechanism delivers roughly 34.
    auto measure = [](double carrier) {
        PhaseVibrato64 engine;
        engine.prepare(kFs);
        engine.set_rate_hz(kProbeRateHz);
        engine.reset();
        return measure_engine(engine, carrier, kProbeRateHz);
    };

    const auto low = measure(200.0);
    const auto high = measure(4000.0);

    const double ratio_db = 20.0 * std::log10(low.span() / high.span());
    CHECK(ratio_db > 6.0);

    // Named the other way round too, so a future change that made the engine
    // frequency-independent could not pass by shrinking both readings.
    CHECK(low.up_cents > 10.0);
    CHECK(high.up_cents < 1.0);
}

TEST_CASE("PhaseVibrato matches an independent allpass phase model", "[vibrato][phase]") {
    // Independent ground truth: rebuild the corner trajectory from a separate
    // LFO and the shipped sweep formula, then predict the carrier's frequency
    // deviation from textbook allpass phase. Agreement means the engine really
    // is a two-stage allpass cascade swept the way the doc says.
    for (double carrier : {200.0, 4000.0}) {
        PhaseVibrato64 engine;
        engine.prepare(kFs);
        engine.set_rate_hz(kProbeRateHz);
        engine.reset();
        const auto measured = measure_engine(engine, carrier, kProbeRateHz);

        const auto cycle = static_cast<std::size_t>(kFs / kProbeRateHz);
        EffectLfoT<double> lfo;
        lfo.set_wave(LfoWave::sine);
        lfo.prepare(kFs);
        lfo.set_rate_hz(kProbeRateHz);
        lfo.reset();
        std::vector<std::vector<double>> corners(cycle * kRenderCycles);
        for (auto& sample : corners) {
            const double fc = PhaseVibrato64::kDefaultCenterHz *
                              std::exp2(PhaseVibrato64::kDefaultDepth *
                                        PhaseVibrato64::kSweepOctaves * lfo.next());
            sample.assign(static_cast<std::size_t>(PhaseVibrato64::kDefaultStageCount), fc);
        }
        const auto predicted =
            predict_from_corners(corners, carrier, PhaseVibrato64::kDefaultMix,
                                 static_cast<int>(cycle * kSkipCycles));

        CHECK(measured.up_cents == Approx(predicted.up_cents).epsilon(0.05));
        CHECK(measured.down_cents == Approx(predicted.down_cents).epsilon(0.05));
    }
}

TEST_CASE("PhaseVibrato two stages sweep deeper than one", "[vibrato][phase]") {
    auto measure = [](int stages) {
        PhaseVibrato64 engine;
        engine.prepare(kFs);
        engine.set_rate_hz(kProbeRateHz);
        engine.set_mix(1.0);
        engine.set_stage_count(stages);
        engine.reset();
        return measure_engine(engine, 200.0, kProbeRateHz).span();
    };

    const double one = measure(1);
    const double two = measure(2);
    CHECK(two / one > 1.5);
    // Cascading identical stages adds phase linearly, so the honest expectation
    // is 2x, not merely "more". Asserting the real number would catch a stage
    // that silently stopped contributing where ">= 1.5" would not.
    CHECK(two / one == Approx(2.0).epsilon(0.02));
}

TEST_CASE("PhaseVibrato reports zero latency", "[vibrato][phase][latency]") {
    PhaseVibrato64 engine;
    engine.prepare(kFs);
    CHECK(engine.latency_samples() == 0);
    UniVibe64 vibe;
    vibe.prepare(kFs);
    CHECK(vibe.latency_samples() == 0);
}

TEST_CASE("Every allpass stage is unity gain across the band", "[vibrato][allpass]") {
    // All test frequencies are multiples of 10 Hz, so 4800 samples is a whole
    // number of periods for each and the RMS ratio is a coherent measurement.
    const std::vector<double> frequencies{20.0,   50.0,   100.0,  200.0,   500.0,
                                          1000.0, 2000.0, 5000.0, 10000.0, 20000.0};
    const std::vector<double> corners{70.0, 100.0, 200.0, 500.0, 1900.0, 3800.0, 5700.0};

    for (double fc : corners) {
        for (double hz : frequencies) {
            TptFilter64 stage;
            stage.prepare(kFs);
            stage.set_cutoff(fc);
            const double db =
                coherent_gain_db([&stage](double x) { return stage.process_allpass(x); }, hz);
            CHECK(std::abs(db) < 0.05);
        }
    }

    // And the shipped cascades, held static, are unity too — the property has to
    // survive composition, not just hold per stage.
    for (double hz : frequencies) {
        PhaseVibrato64 engine;
        engine.prepare(kFs);
        engine.set_depth(0.0);
        engine.set_mix(1.0);
        engine.set_stage_count(PhaseVibrato64::kMaxStages);
        engine.reset();
        const double db =
            coherent_gain_db([&engine](double x) { return engine.process(x); }, hz);
        CHECK(std::abs(db) < 0.05);
    }
}

TEST_CASE("Allpass phase crosses ninety degrees at the corner", "[vibrato][allpass]") {
    // What makes a corner frequency mean something in phase terms, and therefore
    // what makes the Univibe's corner accessors testable claims rather than
    // labels.
    for (double fc : {200.0, 430.0, 900.0, 1900.0}) {
        TptFilter64 stage;
        stage.prepare(kFs);
        stage.set_cutoff(fc);
        const auto period = static_cast<std::size_t>(std::llround(kFs / 10.0));
        std::vector<double> out(period * 40);
        for (std::size_t i = 0; i < out.size(); ++i) {
            out[i] = stage.process_allpass(std::sin(kTwoPi * fc * static_cast<double>(i) / kFs));
        }
        // Coherent single-bin phase over whole periods of the drive.
        double real = 0.0;
        double imag = 0.0;
        const std::size_t start = out.size() / 2;
        for (std::size_t i = start; i < out.size(); ++i) {
            const double angle = kTwoPi * fc * static_cast<double>(i) / kFs;
            real += out[i] * std::cos(angle);
            imag -= out[i] * std::sin(angle);
        }
        // Drive is sin, so the reference itself sits at -90 degrees in this
        // basis; the allpass adds another -90 at its corner.
        const double degrees = std::atan2(imag, real) * 180.0 / std::numbers::pi;
        CHECK(degrees == Approx(-180.0).margin(3.0));
    }
}

TEST_CASE("UniVibe corners stay staggered and track the shipped formula",
          "[vibrato][univibe]") {
    UniVibe64 engine;
    engine.prepare(kFs);
    engine.reset();

    for (int n = 0; n < 6000; ++n) {
        double left = 0.0;
        double right = 0.0;
        engine.process(0.0, left, right);
    }

    const double scale = UniVibe64::corner_scale(engine.control(), engine.depth());
    for (int i = 0; i < UniVibe64::kStageCount; ++i) {
        const double expected =
            UniVibe64::kStageBaseHz[static_cast<std::size_t>(i)] * scale;
        CHECK(engine.stage_corner_hz(i) == Approx(expected).epsilon(1e-12));
    }

    // Unequal by a wide margin, at every instant, because all four ride one
    // shared scale. Pairwise, not just adjacent.
    for (int i = 0; i < UniVibe64::kStageCount; ++i) {
        for (int j = i + 1; j < UniVibe64::kStageCount; ++j) {
            CHECK(engine.stage_corner_hz(j) / engine.stage_corner_hz(i) > 1.5);
        }
    }

    // The stagger is a set of ratios, and those ratios must not breathe with the
    // sweep — one dimensionless shape over four base frequencies.
    const double ratio_now = engine.stage_corner_hz(3) / engine.stage_corner_hz(0);
    for (int n = 0; n < 4000; ++n) {
        double left = 0.0;
        double right = 0.0;
        engine.process(0.0, left, right);
    }
    CHECK(engine.stage_corner_hz(3) / engine.stage_corner_hz(0) ==
          Approx(ratio_now).epsilon(1e-12));
    CHECK(ratio_now == Approx(UniVibe64::kStageBaseHz[3] / UniVibe64::kStageBaseHz[0])
                           .epsilon(1e-12));
}

TEST_CASE("UniVibe audio matches its staggered corner trajectory", "[vibrato][univibe]") {
    // End-to-end: the corners are not just computed, they are the ones the audio
    // actually passed through. Predicted from an independently driven LFO,
    // vactrol, and the textbook four-stage allpass phase.
    constexpr double kRate = 3.0;
    const auto cycle = static_cast<std::size_t>(kFs / kRate);

    for (double carrier : {200.0, 4000.0}) {
        UniVibe64 engine;
        engine.prepare(kFs);
        engine.set_rate_hz(kRate);
        engine.reset();

        const auto input = sine_buffer(cycle * 4, carrier);
        std::vector<double> output(input.size());
        for (std::size_t i = 0; i < input.size(); ++i) {
            double left = 0.0;
            double right = 0.0;
            engine.process(input[i], left, right);
            output[i] = left;
        }
        const auto measured =
            peak_cents_deviation(instantaneous_phase(output, carrier),
                                 static_cast<int>(cycle), carrier, kRate, 24);

        EffectLfoT<double> lfo;
        lfo.set_wave(LfoWave::sine);
        lfo.prepare(kFs);
        lfo.set_rate_hz(kRate);
        lfo.reset();
        VactrolConditioner64 vactrol;
        vactrol.prepare(kFs);
        vactrol.set_rise_ms(UniVibe64::kVactrolRiseMs);
        vactrol.set_fall_ms(UniVibe64::kVactrolFallMs);
        vactrol.reset();

        std::vector<std::vector<double>> corners(input.size());
        for (auto& sample : corners) {
            const double control = vactrol.process(lfo.next_unipolar());
            const double scale = UniVibe64::corner_scale(control, UniVibe64::kDefaultDepth);
            sample.resize(UniVibe64::kStageCount);
            for (int k = 0; k < UniVibe64::kStageCount; ++k) {
                sample[static_cast<std::size_t>(k)] =
                    UniVibe64::kStageBaseHz[static_cast<std::size_t>(k)] * scale;
            }
        }
        const auto predicted =
            predict_from_corners(corners, carrier, UniVibe64::kVibratoMix, static_cast<int>(cycle));

        CHECK(measured.up_cents == Approx(predicted.up_cents).epsilon(0.05));
        CHECK(measured.down_cents == Approx(predicted.down_cents).epsilon(0.05));
    }
}

TEST_CASE("UniVibe vactrol rises fast and falls slow", "[vibrato][univibe][vactrol]") {
    VactrolConditioner64 vactrol;
    vactrol.prepare(kFs);
    vactrol.set_rise_ms(UniVibe64::kVactrolRiseMs);
    vactrol.set_fall_ms(UniVibe64::kVactrolFallMs);
    vactrol.reset();

    auto crossing = [](double previous, double current, double level, int index) {
        return static_cast<double>(index) - 1.0 + (level - previous) / (current - previous);
    };

    double previous = vactrol.control();
    double t10 = 0.0;
    double t90 = 0.0;
    for (int i = 1; i < 48000; ++i) {
        const double current = vactrol.process(1.0);
        if (previous < 0.1 && current >= 0.1) t10 = crossing(previous, current, 0.1, i);
        if (previous < 0.9 && current >= 0.9) {
            t90 = crossing(previous, current, 0.9, i);
            break;
        }
        previous = current;
    }
    for (int i = 0; i < 48000; ++i) vactrol.process(1.0);

    previous = vactrol.control();
    double f90 = 0.0;
    double f10 = 0.0;
    for (int i = 1; i < 480000; ++i) {
        const double current = vactrol.process(0.0);
        if (previous > 0.9 && current <= 0.9) f90 = crossing(previous, current, 0.9, i);
        if (previous > 0.1 && current <= 0.1) {
            f10 = crossing(previous, current, 0.1, i);
            break;
        }
        previous = current;
    }

    const double rise_ms = (t90 - t10) / kFs * 1000.0;
    const double fall_ms = (f10 - f90) / kFs * 1000.0;

    // A one-pole's 10-90 % time is tau*ln(9), so the ratio of the two 10-90 %
    // times is exactly the ratio of the shipped time constants. Computed, not
    // restated.
    const double expected_ratio = UniVibe64::kVactrolFallMs / UniVibe64::kVactrolRiseMs;
    CHECK(fall_ms / rise_ms == Approx(expected_ratio).epsilon(0.05));
    CHECK(rise_ms == Approx(UniVibe64::kVactrolRiseMs * std::log(9.0)).epsilon(0.02));
    CHECK(fall_ms == Approx(UniVibe64::kVactrolFallMs * std::log(9.0)).epsilon(0.02));
}

TEST_CASE("UniVibe sweep is lopsided where the Magnatone sweep is not",
          "[vibrato][univibe][vactrol]") {
    // The vactrol has to be audible in the SHAPE of the sweep, not just present
    // in a step response. A symmetric sine drives both engines; only the one
    // with an asymmetric conditioner produces an asymmetric pitch excursion.
    constexpr double kRate = 3.0;

    UniVibe64 vibe;
    vibe.prepare(kFs);
    vibe.set_rate_hz(kRate);
    vibe.reset();
    const auto cycle = static_cast<std::size_t>(kFs / kRate);
    const auto input = sine_buffer(cycle * 4, 200.0);
    std::vector<double> wet(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        double left = 0.0;
        double right = 0.0;
        vibe.process(input[i], left, right);
        wet[i] = left;
    }
    const auto vibe_dev = peak_cents_deviation(instantaneous_phase(wet, 200.0),
                                               static_cast<int>(cycle), 200.0, kRate, 24);

    PhaseVibrato64 magnatone;
    magnatone.prepare(kFs);
    magnatone.set_rate_hz(kRate);
    magnatone.set_mix(1.0);
    magnatone.reset();
    const auto magnatone_dev = measure_engine(magnatone, 200.0, kRate);

    CHECK(vibe_dev.asymmetry() > 1.10);
    CHECK(magnatone_dev.asymmetry() == Approx(1.0).margin(0.03));
}

TEST_CASE("UniVibe chorus splits the paths and vibrato does not", "[vibrato][univibe][mode]") {
    const auto input = sine_buffer(24000, 1000.0);

    UniVibe64 chorus;
    chorus.prepare(kFs);
    chorus.set_mode(UniVibe64::Mode::chorus);
    chorus.reset();

    double sum_input = 0.0;
    double sum_difference = 0.0;
    for (double x : input) {
        double left = 0.0;
        double right = 0.0;
        chorus.process(x, left, right);
        // Documented split-path behaviour: one output is untouched. Bit-exact,
        // because "untouched" is a claim about identity, not about level.
        CHECK(left == x);
        sum_input += x * x;
        sum_difference += (right - x) * (right - x);
    }
    const double difference_db = 10.0 * std::log10(sum_difference / sum_input);
    CHECK(difference_db > -40.0);

    UniVibe64 vibrato;
    vibrato.prepare(kFs);
    vibrato.set_mode(UniVibe64::Mode::vibrato);
    vibrato.reset();
    bool moved = false;
    for (double x : input) {
        double left = 0.0;
        double right = 0.0;
        vibrato.process(x, left, right);
        CHECK(left == right);
        if (left != x) moved = true;
    }
    CHECK(moved);
}

TEST_CASE("Lagrange kernel peak gain is the shipped constant", "[vibrato][delay][gain]") {
    // The reason DelayVibrato's worst-case sample gain is not 0 dB. Scanned over
    // the whole fractional range rather than assumed at the midpoint.
    double worst = 0.0;
    double worst_at = 0.0;
    for (int i = 0; i <= 100000; ++i) {
        const double frac = static_cast<double>(i) / 100000.0;
        const double l1 = std::abs(Interpolator::lagrange(frac, 1.0, 0.0, 0.0, 0.0)) +
                          std::abs(Interpolator::lagrange(frac, 0.0, 1.0, 0.0, 0.0)) +
                          std::abs(Interpolator::lagrange(frac, 0.0, 0.0, 1.0, 0.0)) +
                          std::abs(Interpolator::lagrange(frac, 0.0, 0.0, 0.0, 1.0));
        if (l1 > worst) {
            worst = l1;
            worst_at = frac;
        }
    }
    CHECK(worst == Approx(DelayVibrato64::kInterpolatorPeakGain).epsilon(1e-9));
    CHECK(worst_at == Approx(0.5).margin(1e-4));
}

TEST_CASE("Worst-case sample gain stays under the cascade L1 ceiling", "[vibrato][gain]") {
    // Series law 8: the registry number has to be a bound this suite asserts.
    //
    // The ceiling is the L1 norm of the impulse response at the LOWEST corner
    // each engine's parameter range can reach, with every stage active and the
    // path fully wet — the exact worst-case sample gain of a static cascade over
    // all bounded inputs. The unity-MAGNITUDE property bounds steady-state
    // sinusoids only; it says nothing about sample gain, and an allpass
    // amplifies a sign-matched input by exactly this factor.
    const double phase_low_corner =
        PhaseVibrato64::kMinCenterHz * std::exp2(-PhaseVibrato64::kSweepOctaves);
    const double phase_ceiling = cascade_impulse_l1(std::vector<double>(
        static_cast<std::size_t>(PhaseVibrato64::kMaxStages), phase_low_corner));

    const double vibe_scale = UniVibe64::corner_scale(0.0, 1.0);
    std::vector<double> vibe_corners(UniVibe64::kStageCount);
    for (int i = 0; i < UniVibe64::kStageCount; ++i) {
        vibe_corners[static_cast<std::size_t>(i)] =
            UniVibe64::kStageBaseHz[static_cast<std::size_t>(i)] * vibe_scale;
    }
    const double vibe_ceiling = cascade_impulse_l1(vibe_corners);

    // Both ceilings sit far above the +6 dB an unnormalised direct+shifted sum
    // would suggest. That is the point of measuring instead of quoting.
    CHECK(phase_ceiling > 5.0);
    CHECK(vibe_ceiling > 5.0);

    const double phase_peak = battery_peak_gain([] {
        PhaseVibrato64 engine;
        engine.prepare(kFs);
        engine.set_center_hz(PhaseVibrato64::kMinCenterHz);
        engine.set_depth(1.0);
        engine.set_stage_count(PhaseVibrato64::kMaxStages);
        engine.set_mix(1.0);
        engine.set_rate_hz(PhaseVibrato64::kMinRateHz);
        engine.reset();
        return engine;
    });
    CHECK(phase_peak < phase_ceiling);
    CHECK(phase_peak > 2.0);  // and genuinely above the sinusoidal bound

    const double vibe_peak = battery_peak_gain([] {
        UniVibeWetTap tap;
        tap.engine.prepare(kFs);
        tap.engine.set_depth(1.0);
        tap.engine.set_rate_hz(UniVibe64::kMinRateHz);
        tap.engine.reset();
        return tap;
    });
    CHECK(vibe_peak < vibe_ceiling);
    CHECK(vibe_peak > 2.0);

    const double delay_peak = battery_peak_gain([] {
        DelayVibrato64 engine;
        engine.prepare(kFs);
        engine.set_rate_hz(DelayVibrato64::kMinRateHz);
        engine.set_depth_cents(DelayVibrato64::kMaxDepthCents);
        engine.reset();
        return engine;
    });
    CHECK(delay_peak < DelayVibrato64::kInterpolatorPeakGain);
    CHECK(delay_peak > 1.0);  // not 0 dB, despite being one unit-gain tap

    // Steady-state sinusoids ARE bounded by 1 for a crossfade of unity-magnitude
    // paths, which is the claim the allpass property actually supports.
    for (double hz : {100.0, 1000.0, 10000.0}) {
        PhaseVibrato64 engine;
        engine.prepare(kFs);
        engine.set_depth(0.0);
        engine.set_mix(1.0);
        engine.reset();
        const double db =
            coherent_gain_db([&engine](double x) { return engine.process(x); }, hz);
        CHECK(db <= 20.0 * std::log10(PhaseVibrato64::kSinusoidalGainBound) + 0.05);
    }
}

TEST_CASE("Vibrato engines render deterministically", "[vibrato][determinism]") {
    const auto input = sine_buffer(8000, 220.0);

    auto render_delay = [&input] {
        DelayVibrato delay;
        delay.prepare(kFs);
        delay.set_delay_ms(30.0);
        delay.set_fade_in_ms(50.0);
        delay.reset();
        std::vector<float> out(input.size());
        for (std::size_t i = 0; i < input.size(); ++i) {
            out[i] = delay.process(static_cast<float>(input[i]));
        }
        return out;
    };
    CHECK(render_delay() == render_delay());

    auto render_phase = [&input] {
        PhaseVibrato phase;
        phase.prepare(kFs);
        phase.reset();
        std::vector<float> out(input.size());
        for (std::size_t i = 0; i < input.size(); ++i) {
            out[i] = phase.process(static_cast<float>(input[i]));
        }
        return out;
    };
    CHECK(render_phase() == render_phase());

    auto render_vibe = [&input] {
        UniVibe vibe;
        vibe.prepare(kFs);
        vibe.set_mode(UniVibe::Mode::chorus);
        vibe.reset();
        std::vector<float> out(input.size() * 2);
        for (std::size_t i = 0; i < input.size(); ++i) {
            vibe.process(static_cast<float>(input[i]), out[2 * i], out[2 * i + 1]);
        }
        return out;
    };
    CHECK(render_vibe() == render_vibe());

    // Determinism has to survive a reset mid-stream, which is what a host does
    // between transport stops.
    DelayVibrato reused;
    reused.prepare(kFs);
    reused.reset();
    std::vector<float> first(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        first[i] = reused.process(static_cast<float>(input[i]));
    }
    reused.reset();
    std::vector<float> second(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        second[i] = reused.process(static_cast<float>(input[i]));
    }
    CHECK(first == second);
}

TEST_CASE("Vibrato engines allocate nothing after prepare", "[vibrato][rt]") {
    DelayVibrato delay;
    PhaseVibrato phase;
    UniVibe vibe;
    delay.prepare(kFs);
    phase.prepare(kFs);
    vibe.prepare(kFs);

    pulp::test::RtAllocationProbe probe;
    for (int i = 0; i < 4096; ++i) {
        const auto x = static_cast<float>(std::sin(kTwoPi * 440.0 * i / kFs));
        float left = 0.0f;
        float right = 0.0f;
        delay.process(x);
        phase.process(x);
        vibe.process(x, left, right);
    }

    // Every setter on the RT surface, including the ones that re-derive
    // coefficients or re-arm the lifecycle.
    delay.set_rate_hz(7.0);
    delay.set_depth_cents(45.0);
    delay.set_delay_ms(120.0);
    delay.set_fade_in_ms(250.0);
    phase.set_rate_hz(4.0);
    phase.set_depth(0.9);
    phase.set_center_hz(1200.0);
    phase.set_stage_count(4);
    phase.set_mix(0.25);
    vibe.set_rate_hz(6.0);
    vibe.set_depth(0.4);
    vibe.set_mode(UniVibe::Mode::chorus);
    delay.reset();
    phase.reset();
    vibe.reset();

    CHECK(probe.allocation_count() == 0);
}
