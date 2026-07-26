#include "test_signal_granular_support.hpp"

TEST_CASE("Window table reproduces the exact Hann identities", "[granular][window]") {
    // The normalization law is written in terms of the window's mean and RMS,
    // so those two numbers are the module's foundation. At full cosine taper
    // they are exactly 1/2 and sqrt(3/8): the discrete sums of sin^2 and sin^4
    // over a whole period are exact for any table size above 4, so this is an
    // identity check and not a tolerance check.
    GranularEngine64 engine;
    engine.prepare(kFs);
    engine.set_window_taper(1.0);
    CHECK(engine.window_mean() == Approx(0.5).margin(1e-12));
    CHECK(engine.window_rms() == Approx(std::sqrt(3.0 / 8.0)).margin(1e-12));

    engine.set_window_taper(0.0);
    CHECK(engine.window_mean() == Approx(1.0).margin(1e-12));
    CHECK(engine.window_rms() == Approx(1.0).margin(1e-12));

    // A Tukey between the two sits between the two, monotonically.
    double previous_mean = 1.0;
    for (double taper : {0.25, 0.5, 0.75, 1.0}) {
        engine.set_window_taper(taper);
        CHECK(engine.window_mean() < previous_mean);
        CHECK(engine.window_rms() > engine.window_mean());
        previous_mean = engine.window_mean();
    }
}

TEST_CASE("Composed interpolator is the specified Catmull-Rom", "[granular][interpolator]") {
    // The spec writes the four cubic coefficients out. The shared interpolator
    // already implements them, so the right move is to compose it and prove the
    // composition rather than paste a second copy into the engine.
    Xorshift32 rng(4242u);
    for (int trial = 0; trial < 200; ++trial) {
        const double t = rng.next_unit<double>();
        const double ym1 = rng.next_bipolar<double>();
        const double y0 = rng.next_bipolar<double>();
        const double y1 = rng.next_bipolar<double>();
        const double y2 = rng.next_bipolar<double>();

        const double a = -0.5 * ym1 + 1.5 * y0 - 1.5 * y1 + 0.5 * y2;
        const double b = ym1 - 2.5 * y0 + 2.0 * y1 - 0.5 * y2;
        const double c = -0.5 * ym1 + 0.5 * y1;
        const double expected = ((a * t + b) * t + c) * t + y0;

        CHECK(Interpolator::hermite(t, ym1, y0, y1, y2) == Approx(expected).margin(1e-14));
    }
}

TEST_CASE("Grain window matches its closed form", "[granular][window]") {
    GranularEngine64 engine;
    engine.prepare(kFs);

    SECTION("Hann") {
        engine.set_window_taper(1.0);
        double worst = 0.0;
        for (int i = 0; i <= 200000; ++i) {
            const double p = static_cast<double>(i) / 200000.0;
            const double truth = std::sin(kPi * p) * std::sin(kPi * p);
            worst = std::max(worst, std::abs(engine.window_at(p) - truth));
        }
        CHECK(worst < 1e-6);

        // The residual is entirely the table's linear interpolation, whose
        // error is |w''|·h²/8 with |w''| = 2π². Asserting the measured error
        // against that bound rather than only against 1e-6 is what turns "it
        // passed" into "the table is the only error source" — and it documents
        // that the margin under the spec's 1e-6 is only 1.7x at the shipped
        // table size.
        const double table = static_cast<double>(GranularEngine64::kWindowTableSize);
        const double bound = 2.0 * kPi * kPi / (8.0 * table * table);
        CHECK(worst == Approx(bound).epsilon(0.05));
    }

    SECTION("rectangular") {
        engine.set_window_taper(0.0);
        for (int i = 0; i <= 1000; ++i) {
            const double p = static_cast<double>(i) / 1000.0;
            CHECK(engine.window_at(p) == Approx(1.0).margin(1e-12));
        }
    }

    SECTION("trapezoid") {
        engine.set_window_trapezoid(true);
        engine.set_window_taper(1.0);  // degenerates to a triangle
        double worst = 0.0;
        for (int i = 0; i <= 200000; ++i) {
            const double p = static_cast<double>(i) / 200000.0;
            const double truth = p < 0.5 ? 2.0 * p : 2.0 * (1.0 - p);
            worst = std::max(worst, std::abs(engine.window_at(p) - truth));
        }
        // Piecewise linear through table nodes, and the breakpoint at p = 0.5
        // lands exactly on a node, so this is exact rather than bounded.
        CHECK(worst < 1e-12);
    }

    SECTION("slope bound") {
        // The bound T-7 leans on: dw/dp = π·sin(2πp) for Hann, so no grain's
        // contribution can step by more than π·phase_inc between samples. That
        // is what "the window has no discontinuity" means numerically.
        engine.set_window_taper(1.0);
        const double phase_increment = 1.0 / (0.050 * kFs);
        double worst = 0.0;
        for (double p = 0.0; p + phase_increment <= 1.0; p += phase_increment) {
            worst = std::max(worst, std::abs(engine.window_at(p + phase_increment) -
                                             engine.window_at(p)));
        }
        CHECK(worst <= kPi * phase_increment * 1.01);
    }
}

TEST_CASE("A single grain plays exactly its window", "[granular][window]") {
    // The audio path, not the table: a DC source and one non-overlapping grain
    // make the output literally the window times the per-grain gain times the
    // centre pan gain — every factor predicted from shipped constants.
    const std::vector<double> dc(48000, 1.0);
    GranularEngine64 engine;
    engine.prepare(kFs);
    engine.set_buffer(dc.data(), static_cast<int>(dc.size()), 1);
    engine.set_window_taper(1.0);
    engine.set_grain_ms(50.0);
    engine.set_density_hz(4.0);  // one grain per 250 ms; 50 ms grains never overlap
    engine.set_stretch(0.0);
    engine.reset();

    // Mean overlap is 0.2, so the incoherent gain is above 1 and the clamp
    // pins it — which is the clamp's whole purpose.
    REQUIRE(engine.mean_overlap() == Approx(0.2));
    REQUIRE(engine.grain_gain() == Approx(1.0));

    const auto out = render(engine, 2400);
    const double pan = std::cos(kPi / 4.0);
    double worst = 0.0;
    for (int n = 0; n < 2400; ++n) {
        const double p = static_cast<double>(n) / 2400.0;
        const double expected =
            std::sin(kPi * p) * std::sin(kPi * p) * engine.grain_gain() * pan;
        worst = std::max(worst, std::abs(out.left[static_cast<std::size_t>(n)] - expected));
    }
    CHECK(worst < 1e-6);
    CHECK(engine.active_grain_count() == 1);
}

TEST_CASE("Synchronous grain rate is exact over the long run", "[granular][schedule]") {
    for (double density : {100.0, 97.0}) {
        GranularEngine64 engine;
        engine.prepare(kFs);
        engine.set_density_hz(density);
        engine.set_async_jitter(0.0);
        engine.reset();

        const auto out = render(engine, 480000);  // 10 s
        (void)out;
        const auto expected = static_cast<std::uint64_t>(std::llround(density * 10.0));
        // The onset accumulator carries its fractional remainder, so a density
        // whose period is not a whole number of samples still lands on the
        // exact long-run count.
        CHECK(engine.grain_index() == expected);
    }

    GranularEngine64 engine;
    engine.prepare(kFs);
    engine.set_density_hz(100.0);
    engine.set_async_jitter(0.0);
    engine.reset();
    const auto onsets = onset_indices(engine, 480000);
    CHECK(interval_cv(onsets) == Approx(0.0).margin(1e-12));
}

TEST_CASE("Full jitter is a Poisson process and zero jitter is a clock",
          "[granular][schedule]") {
    // The two named endpoints, measured. Inter-onset intervals of a Poisson
    // process are exponential, whose coefficient of variation is exactly 1;
    // a fixed clock's is exactly 0. Anything that only dithers the grid
    // position lands at sqrt(1/6) = 0.408 instead and is not Poisson at all,
    // which is why the blend here is on the interval rather than the position.
    GranularEngine64 engine;
    engine.prepare(kFs);
    engine.set_density_hz(100.0);
    engine.set_async_jitter(1.0);
    engine.reset();

    const int samples = 2400000;  // 50 s, ~5000 onsets
    const auto onsets = onset_indices(engine, samples);
    CHECK(interval_cv(onsets) == Approx(1.0).margin(0.1));

    // Blending the interval keeps the mean interval at fs/D, so the long-run
    // rate is the requested density at every jitter setting.
    const double rate = static_cast<double>(onsets.size()) / (static_cast<double>(samples) / kFs);
    CHECK(rate == Approx(100.0).epsilon(0.05));
}

TEST_CASE("Pitch follows the ratio and ignores the playhead", "[granular][pitch]") {
    // The decoupling claim, measured as a matrix rather than asserted. The
    // transposition must depend only on the semitone setting; the stretch
    // column must not move it.
    //
    // Measured as band power against the three candidate frequencies rather
    // than as a peak-bin index. The grain window smears every component across
    // an 80 Hz main lobe, and the largest bin inside that lobe is not a
    // repeatable quantity — it moves by several bins under a last-bit change in
    // the arithmetic. Band power is stable, and the answer it gives is
    // unambiguous: the correct band holds better than 99.9 % of the energy in
    // every cell below.
    const auto source = sine_buffer(48000, 1000.0);
    const std::vector<double> candidates{500.0, 1000.0, 2000.0};

    for (double semitones : {0.0, 12.0, -12.0}) {
        const double expected = 1000.0 * std::exp2(semitones / 12.0);
        for (double stretch : {1.0, 0.5}) {
            GranularEngine64 engine;
            configure_buffer_engine(engine, source);
            engine.set_pitch_semitones(semitones);
            engine.set_stretch(stretch);
            engine.reset();

            const auto out = render(engine, kSpectrumRender);
            const auto power = welch_power(out.left);

            for (double candidate : candidates) {
                const double fraction =
                    band_fraction(power, candidate - 60.0, candidate + 60.0);
                if (candidate == expected) {
                    CHECK(fraction > 0.99);
                } else {
                    CHECK(fraction < 0.01);
                }
            }
            // And the transposition is accurate, not merely in the right
            // octave: the spec's +/-1 %, measured on the band centroid.
            const double centroid =
                band_centroid(power, expected * 0.88, expected * 1.12);
            CHECK(centroid == Approx(expected).epsilon(0.01));
        }
    }
}

TEST_CASE("Phase-locked grains cancel a periodic source", "[granular][pitch]") {
    // Why every measurement above sprays, stated as the level fact it is.
    //
    // With no spray, consecutive grains read source positions exactly one hop
    // apart. At 400 grains/s into a 1 kHz tone that hop is 2.5 periods, so
    // neighbouring grains sit in antiphase and annihilate each other: the
    // output collapses by about 40 dB and what survives is numerical residue.
    // This is a property of granular synthesis on periodic material, not a
    // defect — but it means a spray-free configuration measures the residue
    // rather than the effect, and any spectral claim made about it is a claim
    // about nothing.
    //
    // The exception proves the mechanism: when the grain ratio equals the
    // playhead rate every grain reads the SAME source sample at the same
    // instant, so instead of cancelling they add coherently and the output is
    // loud.
    const auto source = sine_buffer(48000, 1000.0);

    auto level = [&source](double spray, double semitones, double stretch) {
        GranularEngine64 engine;
        configure_buffer_engine(engine, source);
        engine.set_position_spray_ms(spray);
        engine.set_pitch_semitones(semitones);
        engine.set_stretch(stretch);
        engine.reset();
        const auto out = render(engine, 96000);
        return rms(out.left, 4800);
    };

    const double cancelled = level(0.0, 12.0, 1.0);
    const double sprayed = level(kDecorrelationSprayMs, 12.0, 1.0);
    CHECK(20.0 * std::log10(cancelled / sprayed) < -30.0);

    // Ratio equal to the playhead rate: fully coherent, and louder than the
    // decorrelated cloud rather than quieter.
    const double coherent = level(0.0, 0.0, 1.0);
    CHECK(20.0 * std::log10(coherent / sprayed) > 6.0);
}

TEST_CASE("Stretch dilates the output timeline", "[granular][stretch]") {
    // A marker at source 1.0 s must arrive at output 2.0 s when the playhead
    // runs at half speed. The tolerance is one grain length because that is
    // genuinely how well the marker is localised: it is only reproduced by
    // whichever grains' windows cover it.
    std::vector<double> source(96000, 0.0);
    for (int i = 0; i < 240; ++i) source[static_cast<std::size_t>(48000 + i)] = 1.0;

    GranularEngine64 engine;
    engine.prepare(kFs);
    engine.set_buffer(source.data(), static_cast<int>(source.size()), 1);
    engine.set_window_taper(1.0);
    engine.set_grain_ms(50.0);
    engine.set_density_hz(400.0);
    engine.set_stretch(0.5);
    engine.reset();

    const auto out = render(engine, 160000);

    const int window = 480;
    int peak = 0;
    double peak_energy = 0.0;
    for (int n = 0; n + window < 160000; ++n) {
        double energy = 0.0;
        for (int k = 0; k < window; ++k) {
            const double v = out.left[static_cast<std::size_t>(n + k)];
            energy += v * v;
        }
        if (energy > peak_energy) {
            peak_energy = energy;
            peak = n + window / 2;
        }
    }

    const double grain_samples = 0.050 * kFs;
    CHECK(std::abs(static_cast<double>(peak) - 96000.0) <= grain_samples);
}

TEST_CASE("Renders are bit-identical and block-size independent", "[granular][determinism]") {
    const auto source = noise_buffer(48000, 999u);
    const int n = 24000;

    SECTION("repeat and reset") {
        GranularEngine64 a;
        GranularEngine64 b;
        configure_stress(a, source, GrainSource::buffer);
        configure_stress(b, source, GrainSource::buffer);
        const auto first = render(a, n);
        const auto second = render(b, n);
        CHECK(first.left == second.left);
        CHECK(first.right == second.right);

        a.reset();
        const auto third = render(a, n);
        CHECK(first.left == third.left);
        CHECK(first.right == third.right);

        // And the stress config really does steal, or this proves nothing about
        // the keying surviving voice-steal.
        CHECK(a.steal_count() > 0);
    }

    SECTION("buffer mode block size") {
        GranularEngine64 big;
        GranularEngine64 small;
        configure_stress(big, source, GrainSource::buffer);
        configure_stress(small, source, GrainSource::buffer);
        const auto one_block = render(big, n);
        const auto per_sample = render(small, n, 1);
        CHECK(one_block.left == per_sample.left);
        CHECK(one_block.right == per_sample.right);
        CHECK(big.steal_count() == small.steal_count());
    }

    SECTION("live mode block size through the interleaved overload") {
        GranularEngine64 big;
        GranularEngine64 small;
        configure_stress(big, source, GrainSource::live_ring);
        configure_stress(small, source, GrainSource::live_ring);

        std::vector<double> big_left(static_cast<std::size_t>(n));
        std::vector<double> big_right(static_cast<std::size_t>(n));
        big.process(source.data(), big_left.data(), big_right.data(), n);

        std::vector<double> small_left(static_cast<std::size_t>(n));
        std::vector<double> small_right(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) {
            small.process(source.data() + i, small_left.data() + i, small_right.data() + i, 1);
        }
        CHECK(big_left == small_left);
        CHECK(big_right == small_right);
    }

    SECTION("the write_live pair is deliberately not block-size independent") {
        // Documented, not a bug to fix in the engine: writing a whole block
        // before rendering any of it puts samples in the ring that a grain
        // rendered at the top of that block can legally read, and rendering
        // sample by sample does not. The interleaved overload above exists
        // precisely because this ambiguity is unresolvable from inside a
        // two-call API.
        GranularEngine64 big;
        GranularEngine64 small;
        configure_stress(big, source, GrainSource::live_ring);
        configure_stress(small, source, GrainSource::live_ring);

        std::vector<double> big_left(static_cast<std::size_t>(n));
        std::vector<double> big_right(static_cast<std::size_t>(n));
        big.write_live(source.data(), n);
        big.process(big_left.data(), big_right.data(), n);

        std::vector<double> small_left(static_cast<std::size_t>(n));
        std::vector<double> small_right(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) {
            small.write_live(source.data() + i, 1);
            small.process(small_left.data() + i, small_right.data() + i, 1);
        }
        CHECK(big_left != small_left);
    }

    SECTION("the seed is actually used") {
        GranularEngine64 a;
        GranularEngine64 b;
        configure_stress(a, source, GrainSource::buffer);
        configure_stress(b, source, GrainSource::buffer);
        b.set_seed(0x12345678u);
        b.reset();
        const auto first = render(a, n);
        const auto second = render(b, n);
        CHECK(first.left != second.left);
    }
}

TEST_CASE("Grain draws depend only on the grain index", "[granular][determinism]") {
    // The property the stateless keyed hash exists for, and the one a
    // sequential generator cannot provide: grain k is born with the same pitch
    // whether or not grains before it were dropped, and whatever the pool size.
    //
    // Two engines differing only in budget. The larger never runs out of slots;
    // the smaller drops grains constantly. Every grain index they have in common
    // must carry a bit-identical ratio. Under a sequential generator the
    // dropped grains' draws never happen and every later grain in the smaller
    // engine shifts to a different value.
    const auto source = noise_buffer(48000, 31u);

    auto build = [&source](int budget) {
        auto engine = std::make_unique<GranularEngine64>();
        engine->prepare(kFs);
        engine->set_buffer(source.data(), static_cast<int>(source.size()), 1);
        engine->set_grain_ms(50.0);
        engine->set_density_hz(800.0);  // mean overlap 40: fits 64 slots, not 32
        engine->set_pitch_spray_semitones(12.0);
        engine->set_steal_policy(StealPolicy::drop_new);
        engine->set_max_grains(budget);
        engine->reset();
        return engine;
    };

    auto roomy = build(GranularEngine64::kMaxGrainBudget);
    auto cramped = build(GranularEngine64::kMinGrainBudget);

    const auto roomy_spawns = spawned_ratios(*roomy, 48000);
    const auto cramped_spawns = spawned_ratios(*cramped, 48000);

    REQUIRE(roomy_spawns.size() > 500);
    // The cramped engine must actually be dropping, or this proves nothing.
    REQUIRE(cramped_spawns.size() < roomy_spawns.size() * 9 / 10);

    std::vector<double> by_index(roomy_spawns.back().first + 1, -1.0);
    for (const auto& [index, ratio] : roomy_spawns) {
        by_index[static_cast<std::size_t>(index)] = ratio;
    }

    int compared = 0;
    for (const auto& [index, ratio] : cramped_spawns) {
        if (index >= by_index.size()) continue;
        const double reference = by_index[static_cast<std::size_t>(index)];
        if (reference < 0.0) continue;
        CHECK(ratio == reference);
        ++compared;
    }
    CHECK(compared > 400);
}

TEST_CASE("Incoherent gain holds level across a density octave", "[granular][gain]") {
    const auto source = noise_buffer(96000, 12345u);
    const double sigma = rms(source);

    auto measure = [&source](double density, double spray) {
        GranularEngine64 engine;
        engine.prepare(kFs);
        engine.set_buffer(source.data(), static_cast<int>(source.size()), 1);
        engine.set_window_taper(1.0);
        engine.set_grain_ms(50.0);
        engine.set_density_hz(density);
        engine.set_position_spray_ms(spray);
        engine.set_coherence(Coherence::incoherent);
        engine.reset();
        const auto out = render(engine, 96000);
        return rms(out.left, 4800);
    };

    SECTION("with grains decorrelated the law holds") {
        const double low = measure(200.0, 200.0);
        const double high = measure(400.0, 200.0);
        CHECK(20.0 * std::log10(high / low) == Approx(0.0).margin(0.5));

        // Absolute level, predicted end to end from shipped constants:
        // E[y²] = N̄·g²·w_rms²·σ² and g = 1/(w_rms·√N̄) collapse to σ², then the
        // centre pan takes 1/√2 and a fractional read of white noise keeps only
        // the interpolator's mean sum-of-squares — which is computed here from
        // the shipped kernel, not quoted.
        double noise_gain = 0.0;
        const int steps = 20000;
        for (int i = 0; i < steps; ++i) {
            double h[4];
            hermite_basis(static_cast<double>(i) / steps, h);
            noise_gain += h[0] * h[0] + h[1] * h[1] + h[2] * h[2] + h[3] * h[3];
        }
        noise_gain /= steps;

        const double predicted = sigma * std::sqrt(noise_gain) * std::cos(kPi / 4.0);
        CHECK(20.0 * std::log10(low / predicted) == Approx(0.0).margin(0.5));
    }

    SECTION("without spray the grains are coherent and the law does not apply") {
        // The spec's T-6 configuration. Every grain reads the same source
        // sample at the same instant, so amplitudes add and doubling the
        // density adds 3 dB instead of nothing. Asserted so the requirement
        // that grains be decorrelated is recorded rather than remembered.
        const double low = measure(200.0, 0.0);
        const double high = measure(400.0, 0.0);
        CHECK(20.0 * std::log10(high / low) == Approx(3.01).margin(0.3));
    }
}

TEST_CASE("Coherent and incoherent gains differ by the derived amount",
          "[granular][gain]") {
    GranularEngine64 incoherent;
    GranularEngine64 coherent;
    for (auto* engine : {&incoherent, &coherent}) {
        engine->prepare(kFs);
        engine->set_window_taper(1.0);
        engine->set_grain_ms(50.0);
        engine->set_density_hz(200.0);
    }
    incoherent.set_coherence(Coherence::incoherent);
    coherent.set_coherence(Coherence::coherent);

    REQUIRE(incoherent.mean_overlap() == Approx(10.0));

    // Computed from the engine's own measured window statistics.
    const double expected_incoherent = 1.0 / (incoherent.window_rms() * std::sqrt(10.0));
    const double expected_coherent = 1.0 / (coherent.window_mean() * 10.0);
    CHECK(incoherent.grain_gain() == Approx(expected_incoherent).epsilon(1e-9));
    CHECK(coherent.grain_gain() == Approx(expected_coherent).epsilon(1e-9));

    const double ratio_db =
        20.0 * std::log10(incoherent.grain_gain() / coherent.grain_gain());
    CHECK(ratio_db == Approx(8.24).margin(0.05));
}

TEST_CASE("Voice budget is never exceeded and stealing is deterministic",
          "[granular][pool]") {
    const std::vector<double> dc(48000, 1.0);

    auto build = [&dc](double overlap, StealPolicy policy) {
        auto engine = std::make_unique<GranularEngine64>();
        engine->prepare(kFs);
        engine->set_buffer(dc.data(), static_cast<int>(dc.size()), 1);
        engine->set_window_taper(1.0);
        engine->set_grain_ms(50.0);
        engine->set_density_hz(overlap / 0.050);
        engine->set_max_grains(32);
        engine->set_steal_policy(policy);
        engine->reset();
        return engine;
    };

    SECTION("the pool is a hard ceiling") {
        auto engine = build(64.0, StealPolicy::oldest);
        const auto report = observe_steals(*engine, 48000, true);
        CHECK(report.max_active <= 32);
        CHECK(report.steals > 0);
        // The policy always recycles the grain furthest through its window —
        // the best candidate available, whatever its window value happens to
        // be at that overlap.
        CHECK(report.always_picked_oldest);
    }

    SECTION("stealing is click-free while demand only just exceeds the budget") {
        // At an overlap barely above the budget the oldest grain really is
        // nearly finished, and the spec's ε_steal ≤ 0.05 holds.
        auto engine = build(33.0, StealPolicy::oldest);
        const auto report = observe_steals(*engine, 48000, true);
        REQUIRE(report.steals > 0);
        CHECK(report.worst_window <= 0.05);
    }

    SECTION("quietest never steals louder than oldest") {
        // Where `oldest` stops being quiet — the recycled grain sits at its
        // window peak once the overlap is twice the budget — `quietest` is the
        // policy that still is. This comparison is the stable invariant; the
        // absolute window value at steal time is a function of the overlap.
        auto by_age = build(64.0, StealPolicy::oldest);
        auto by_level = build(64.0, StealPolicy::quietest);
        const auto aged = observe_steals(*by_age, 24000, true);
        const auto quiet = observe_steals(*by_level, 24000, false);
        REQUIRE(aged.steals > 0);
        REQUIRE(quiet.steals > 0);
        CHECK(quiet.worst_window <= aged.worst_window);
    }

    SECTION("drop_new never cuts a sounding grain") {
        auto engine = build(64.0, StealPolicy::drop_new);
        const auto report = observe_steals(*engine, 48000, false);
        CHECK(report.max_active <= 32);
        CHECK(report.steals == 0);
        CHECK(engine->steal_count() == 0);
    }
}

TEST_CASE("A failed live placement cannot mutate or steal an active grain",
          "[granular][live][pool]") {
    GranularEngine64 engine;
    engine.prepare(kFs);
    engine.set_source(GrainSource::live_ring);
    engine.set_window_taper(0.0);
    engine.set_grain_ms(GranularEngine64::kMaxGrainMs);
    engine.set_density_hz(GranularEngine64::kMaxDensityHz);
    engine.set_pitch_semitones(0.0);
    engine.set_pitch_spray_semitones(0.0);
    engine.set_position(0.0);
    engine.set_position_spray_ms(0.0);
    engine.set_max_grains(GranularEngine64::kMinGrainBudget);
    engine.set_steal_policy(StealPolicy::oldest);
    engine.set_seed(0x51A7E5u);
    engine.reset();

    const double input = 1.0;
    double left = 0.0;
    double right = 0.0;

    // Ratio-1 grains become legal almost immediately and fill the pool.
    for (int n = 0; n < 4096 && engine.active_grain_count() < engine.max_grains(); ++n) {
        engine.process(&input, &left, &right, 1);
    }
    REQUIRE(engine.active_grain_count() == engine.max_grains());
    REQUIRE(engine.steal_count() == 0);

    // A +24 st grain needs 3 * duration more history than a ratio-1 grain. The
    // next onsets therefore fail placement while every slot is still sounding.
    engine.set_pitch_semitones(GranularEngine64::kMaxPitchSemitones);
    const auto steals_before = engine.steal_count();
    for (int n = 0; n < 128; ++n) engine.process(&input, &left, &right, 1);

    CHECK(engine.steal_count() == steals_before);
    for (int slot = 0; slot < engine.max_grains(); ++slot) {
        CHECK(engine.grain(slot).ratio == 1.0);
    }
}

TEST_CASE("Per-grain gain never exceeds unity", "[granular][gain]") {
    GranularEngine64 engine;
    engine.prepare(kFs);
    for (double taper : {0.0, 0.5, 1.0}) {
        engine.set_window_taper(taper);
        for (double grain : {1.0, 10.0, 50.0, 200.0, 500.0}) {
            engine.set_grain_ms(grain);
            for (double density : {0.1, 1.0, 40.0, 400.0, 2000.0}) {
                engine.set_density_hz(density);
                for (auto coherence : {Coherence::incoherent, Coherence::coherent}) {
                    engine.set_coherence(coherence);
                    CHECK(engine.grain_gain() <= 1.0);
                    CHECK(engine.grain_gain() > 0.0);
                }
            }
        }
    }
}

TEST_CASE("Worst-case output stays inside the derived headroom bound",
          "[granular][gain]") {
    // The registry figure has to be a bound this suite asserts. The provable
    // one is `max_grains × peak interpolator kernel gain`: at most
    // `max_grains` grains sound at once, each grain's gain is clamped to 1, the
    // window peaks at 1, and a pan gain is at most 1 — but a 4-point cubic read
    // of a unit-bounded signal is NOT bounded by 1. Its kernel L1 peaks at a
    // half-sample offset, and leaving that factor out understates the ceiling.
    double kernel_peak = 0.0;
    double kernel_peak_at = 0.0;
    for (int i = 0; i <= 200000; ++i) {
        const double t = static_cast<double>(i) / 200000.0;
        double h[4];
        hermite_basis(t, h);
        const double l1 = std::abs(h[0]) + std::abs(h[1]) + std::abs(h[2]) + std::abs(h[3]);
        if (l1 > kernel_peak) {
            kernel_peak = l1;
            kernel_peak_at = t;
        }
    }
    CHECK(kernel_peak == Approx(1.25).epsilon(1e-9));
    CHECK(kernel_peak_at == Approx(0.5).margin(1e-4));

    const double bound = static_cast<double>(GranularEngine64::kMaxGrainBudget) * kernel_peak;

    const std::vector<double> dc(48000, 1.0);
    double reachable = 0.0;
    for (double taper : {0.0, 0.5, 1.0}) {
        for (double grain : {1.0, 20.0, 50.0, 200.0}) {
            for (double density : {100.0, 640.0, 1280.0, 2000.0}) {
                GranularEngine64 engine;
                engine.prepare(kFs);
                engine.set_buffer(dc.data(), static_cast<int>(dc.size()), 1);
                engine.set_window_taper(taper);
                engine.set_grain_ms(grain);
                engine.set_density_hz(density);
                engine.set_max_grains(GranularEngine64::kMaxGrainBudget);
                engine.reset();
                const auto out = render(engine, 24000);
                for (double v : out.left) reachable = std::max(reachable, std::abs(v));
                for (double v : out.right) reachable = std::max(reachable, std::abs(v));
            }
        }
    }
    CHECK(reachable < bound);

    // And the bound is loose, because the normalization law fights the overlap
    // that would be needed to reach it: more simultaneous grains means a
    // smaller per-grain gain. Recorded so nobody mistakes the registry ceiling
    // for an operating level.
    CHECK(reachable > 4.0);
    CHECK(reachable < 0.2 * bound);
}

TEST_CASE("The engine reports zero latency", "[granular][latency]") {
    GranularEngine64 engine;
    engine.prepare(kFs);
    CHECK(engine.latency_samples() == 0);
    engine.set_source(GrainSource::live_ring);
    CHECK(engine.latency_samples() == 0);

    // Nothing is buffered ahead: a rectangular grain on a DC source produces
    // output on the very first sample. A tapered window necessarily starts at
    // zero — that is what a window is — so the "energy at sample 0" claim is a
    // statement about scheduling latency, and only a rectangular window can
    // express it.
    const std::vector<double> dc(4800, 1.0);
    GranularEngine64 immediate;
    immediate.prepare(kFs);
    immediate.set_source(GrainSource::buffer);
    immediate.set_buffer(dc.data(), static_cast<int>(dc.size()), 1);
    immediate.set_window_taper(0.0);
    immediate.set_grain_ms(50.0);
    immediate.set_density_hz(20.0);
    immediate.reset();
    const auto out = render(immediate, 16);
    CHECK(std::abs(out.left[0]) > 0.0);
}

TEST_CASE("Live grains only ever read written ring samples", "[granular][live]") {
    // A ramp makes the read index observable: with a rectangular window, unity
    // per-grain gain and centre pan, a lone grain's output IS the source value
    // it read, and the source value at absolute index n is n + 1. Anything the
    // ring has never held reads back as 0.
    GranularEngine64 engine;
    engine.prepare(kFs);
    engine.set_source(GrainSource::live_ring);
    engine.set_window_taper(0.0);
    engine.set_grain_ms(20.0);
    engine.set_density_hz(30.0);
    engine.set_max_grains(32);
    engine.set_position(0.25);
    engine.set_position_spray_ms(300.0);
    engine.set_pitch_semitones(GranularEngine64::kMaxPitchSemitones);
    engine.reset();
    REQUIRE(engine.grain_gain() == Approx(1.0));

    const double pan = std::cos(kPi / 4.0);
    const int samples = 300000;
    int observed = 0;
    int unwritten = 0;
    double worst_ahead = -1e18;
    double worst_lag = 0.0;

    for (int i = 0; i < samples; ++i) {
        const double x = static_cast<double>(i + 1);
        double left = 0.0;
        double right = 0.0;
        engine.process(&x, &left, &right, 1);
        if (engine.active_grain_count() != 1) continue;
        ++observed;
        const double read_value = left / pan;
        if (read_value <= 0.0) ++unwritten;
        worst_ahead = std::max(worst_ahead, read_value - static_cast<double>(i + 1));
        worst_lag = std::max(worst_lag, static_cast<double>(i + 1) - read_value);
    }

    REQUIRE(observed > 1000);
    CHECK(unwritten == 0);
    CHECK(worst_ahead <= 0.0);
    // Never older than the valid region the derived guard leaves behind.
    CHECK(worst_lag <=
          static_cast<double>(engine.ring_length() - engine.causality_guard_samples()));
}
