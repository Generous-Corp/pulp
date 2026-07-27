#include "test_signal_granular_support.hpp"

TEST_CASE("The causality guard is derived from the declared ranges",
          "[granular][live]") {
    GranularEngine64 engine;
    engine.prepare(kFs);

    // guard = fs · (max position spray + max grain length × max pitch-up ratio)
    const double r_max = std::exp2(GranularEngine64::kMaxPitchSemitones / 12.0);
    const double expected =
        std::ceil(kFs * (GranularEngine64::kMaxPositionSprayMs * 0.001 +
                         GranularEngine64::kMaxGrainMs * 0.001 * r_max));
    CHECK(static_cast<double>(engine.causality_guard_samples()) == Approx(expected));
    CHECK(r_max == Approx(4.0));

    // The ring has to clear the guard by at least one full grain, or there
    // would be no valid region left for a grain span to live in. That is why
    // the ring-length range floor is where it is.
    const int valid = engine.ring_length() - engine.causality_guard_samples();
    CHECK(valid > 0);
    CHECK(static_cast<double>(valid) >= GranularEngine64::kMaxGrainMs * 0.001 * kFs);
}

TEST_CASE("Live mode can spawn grains at the declared duration and pitch maxima",
          "[granular][live][ranges]") {
    GranularEngine64 engine;
    engine.prepare(kFs);
    engine.set_source(GrainSource::live_ring);
    engine.set_window_taper(0.0);
    engine.set_grain_ms(GranularEngine64::kMaxGrainMs);
    engine.set_density_hz(20.0);
    engine.set_position(0.5);
    engine.set_position_spray_ms(GranularEngine64::kMaxPositionSprayMs);
    engine.set_pitch_semitones(GranularEngine64::kMaxPitchSemitones);
    engine.set_max_grains(GranularEngine64::kMaxGrainBudget);
    engine.set_seed(0x51A7E5u);
    engine.reset();

    // Run long enough to fill and wrap the live ring. The previous placement
    // bounds left no valid interval at this legal corner, so every spawn was
    // silently discarded forever even after all source history was available.
    const auto input = noise_buffer(static_cast<int>(6.0 * kFs), 0xBADC0DEu);
    std::vector<double> left(input.size());
    std::vector<double> right(input.size());
    int max_active = 0;
    for (std::size_t i = 0; i < input.size(); ++i) {
        engine.process(&input[i], &left[i], &right[i], 1);
        max_active = std::max(max_active, engine.active_grain_count());
    }

    CHECK(max_active > 0);
    CHECK(rms(left, static_cast<int>(4.0 * kFs)) > 1e-6);
}

TEST_CASE("Freeze holds a stationary texture while input continues",
          "[granular][live]") {
    GranularEngine64 engine;
    engine.prepare(kFs);
    engine.set_source(GrainSource::live_ring);
    engine.set_window_taper(1.0);
    engine.set_grain_ms(50.0);
    engine.set_density_hz(200.0);
    engine.set_position(0.25);
    engine.reset();

    const auto input = noise_buffer(480000, 7u);
    std::vector<double> left(480000);
    std::vector<double> right(480000);

    engine.process(input.data(), left.data(), right.data(), 240000);
    engine.set_stretch(0.0);  // freeze: hold the captured centre
    engine.process(input.data() + 240000, left.data() + 240000, right.data() + 240000, 240000);

    auto segment_rms = [&left](int from, int to) {
        double sum = 0.0;
        for (int i = from; i < to; ++i) sum += left[static_cast<std::size_t>(i)] * left[static_cast<std::size_t>(i)];
        return std::sqrt(sum / static_cast<double>(to - from));
    };

    const double reference = segment_rms(300000, 340000);
    for (int start : {340000, 380000, 420000}) {
        CHECK(20.0 * std::log10(segment_rms(start, start + 40000) / reference) ==
              Approx(0.0).margin(0.5));
    }
}

TEST_CASE("Granular engine allocates nothing after prepare", "[granular][rt]") {
    const auto source = noise_buffer(48000, 5u);
    GranularEngine engine;
    engine.prepare(kFs);
    engine.set_buffer(nullptr, 0, 1);

    std::vector<float> input(512);
    std::vector<float> left(512);
    std::vector<float> right(512);
    for (std::size_t i = 0; i < input.size(); ++i) {
        input[i] = static_cast<float>(source[i]);
    }
    std::vector<float> buffer_source(source.size());
    for (std::size_t i = 0; i < source.size(); ++i) {
        buffer_source[i] = static_cast<float>(source[i]);
    }

    pulp::test::RtAllocationProbe probe;

    engine.set_buffer(buffer_source.data(), static_cast<int>(buffer_source.size()), 1);
    for (int block = 0; block < 8; ++block) {
        engine.process(left.data(), right.data(), static_cast<int>(left.size()));
    }
    engine.set_source(GrainSource::live_ring);
    for (int block = 0; block < 8; ++block) {
        engine.write_live(input.data(), static_cast<int>(input.size()));
        engine.process(left.data(), right.data(), static_cast<int>(left.size()));
        engine.process(input.data(), left.data(), right.data(), static_cast<int>(left.size()));
    }

    // Every setter on the control surface, including the two that rebuild the
    // window table.
    engine.set_density_hz(700.0);
    engine.set_grain_ms(17.0);
    engine.set_async_jitter(0.5);
    engine.set_max_grains(64);
    engine.set_steal_policy(StealPolicy::quietest);
    engine.set_window_taper(0.3);
    engine.set_window_trapezoid(true);
    engine.set_pitch_semitones(-7.0);
    engine.set_pitch_spray_semitones(3.0);
    engine.set_pan_spray(0.8);
    engine.set_coherence(Coherence::coherent);
    engine.set_interp(GrainInterp::linear);
    engine.set_level_db(-6.0);
    engine.set_mix(0.4);
    engine.set_position(0.6);
    engine.set_position_spray_ms(80.0);
    engine.set_stretch(2.0);
    engine.set_seed(1234u);
    engine.reset();

    CHECK(probe.allocation_count() == 0);
}

TEST_CASE("Granular parameters clamp to their declared ranges", "[granular][params]") {
    GranularEngine64 engine;
    engine.prepare(kFs);

    engine.set_density_hz(1e9);
    CHECK(engine.density_hz() == Approx(GranularEngine64::kMaxDensityHz));
    engine.set_density_hz(-1.0);
    CHECK(engine.density_hz() == Approx(GranularEngine64::kMinDensityHz));

    engine.set_grain_ms(1e9);
    CHECK(engine.grain_ms() == Approx(GranularEngine64::kMaxGrainMs));
    engine.set_grain_ms(0.0);
    CHECK(engine.grain_ms() == Approx(GranularEngine64::kMinGrainMs));

    engine.set_max_grains(1000);
    CHECK(engine.max_grains() == GranularEngine64::kMaxGrainBudget);
    engine.set_max_grains(1);
    CHECK(engine.max_grains() == GranularEngine64::kMinGrainBudget);

    engine.set_pitch_semitones(100.0);
    CHECK(engine.pitch_semitones() == Approx(GranularEngine64::kMaxPitchSemitones));

    engine.set_stretch(99.0);
    CHECK(engine.stretch() == Approx(GranularEngine64::kMaxStretch));
    engine.set_stretch(-1.0);
    CHECK(engine.stretch() == Approx(0.0));

    engine.set_async_jitter(5.0);
    CHECK(engine.async_jitter() == Approx(1.0));
    engine.set_position(5.0);
    CHECK(engine.position() == Approx(1.0));

    // Lowering the budget must not leave a grain sounding in a slot the pool no
    // longer scans.
    const std::vector<double> dc(48000, 1.0);
    engine.set_buffer(dc.data(), static_cast<int>(dc.size()), 1);
    engine.set_max_grains(64);
    engine.set_grain_ms(200.0);
    engine.set_density_hz(1000.0);
    engine.reset();
    render(engine, 24000);
    REQUIRE(engine.active_grain_count() > 32);
    engine.set_max_grains(32);
    CHECK(engine.active_grain_count() <= 32);
    for (int slot = 32; slot < GranularEngine64::kMaxGrainBudget; ++slot) {
        CHECK_FALSE(engine.grain(slot).active);
    }
}

TEST_CASE("Granular numeric controls retain exact configuration after non-finite automation",
          "[granular][params][nan-recovery]") {
    const std::vector<double> source = noise_buffer(48000, 0x51A7E5u);
    std::vector<double> input(8192);
    for (int i = 0; i < static_cast<int>(input.size()); ++i) {
        input[static_cast<std::size_t>(i)] =
            0.2 * std::sin(kTwoPi * 173.0 * static_cast<double>(i) / kFs);
    }

    const auto configure = [&](GranularEngine64& engine) {
        engine.prepare(kFs);
        engine.set_source(GrainSource::buffer);
        engine.set_buffer(source.data(), static_cast<int>(source.size()), 1);
        engine.set_stretch(0.75);
        engine.set_position(0.37);
        engine.set_position_spray_ms(11.0);
        engine.set_density_hz(137.0);
        engine.set_grain_ms(23.0);
        engine.set_async_jitter(0.42);
        engine.set_window_taper(0.63);
        engine.set_pitch_semitones(-5.0);
        engine.set_pitch_spray_semitones(2.75);
        engine.set_pan_spray(0.71);
        engine.set_coherence(Coherence::incoherent);
        engine.set_seed(0xC001D00Du);
        engine.reset();
        // Set these after reset so both smoothers are in flight when the bad
        // automation arrives. Re-applying the old target would restart their
        // ramps and would therefore fail the exact continuation below.
        engine.set_level_db(-9.0);
        engine.set_mix(0.36);
    };

    for (double invalid : {std::numeric_limits<double>::quiet_NaN(),
                           std::numeric_limits<double>::infinity(),
                           -std::numeric_limits<double>::infinity()}) {
        CAPTURE(invalid);
        GranularEngine64 reference;
        GranularEngine64 retained;
        configure(reference);
        configure(retained);

        constexpr int warmup_samples = 113;
        std::vector<double> ref_warm_left(warmup_samples);
        std::vector<double> ref_warm_right(warmup_samples);
        std::vector<double> retained_warm_left(warmup_samples);
        std::vector<double> retained_warm_right(warmup_samples);
        reference.process(input.data(), ref_warm_left.data(), ref_warm_right.data(),
                          warmup_samples);
        retained.process(input.data(), retained_warm_left.data(), retained_warm_right.data(),
                         warmup_samples);
        REQUIRE(retained_warm_left == ref_warm_left);
        REQUIRE(retained_warm_right == ref_warm_right);

        retained.set_stretch(invalid);
        retained.set_position(invalid);
        retained.set_position_spray_ms(invalid);
        retained.set_density_hz(invalid);
        retained.set_grain_ms(invalid);
        retained.set_async_jitter(invalid);
        retained.set_window_taper(invalid);
        retained.set_pitch_semitones(invalid);
        retained.set_pitch_spray_semitones(invalid);
        retained.set_pan_spray(invalid);
        retained.set_level_db(invalid);
        retained.set_mix(invalid);

        // Direct state proves retention where the public surface exposes it.
        CHECK(retained.stretch() == reference.stretch());
        CHECK(retained.position() == reference.position());
        CHECK(retained.position_spray_ms() == reference.position_spray_ms());
        CHECK(retained.density_hz() == reference.density_hz());
        CHECK(retained.grain_ms() == reference.grain_ms());
        CHECK(retained.async_jitter() == reference.async_jitter());
        CHECK(retained.window_taper() == reference.window_taper());
        CHECK(retained.pitch_semitones() == reference.pitch_semitones());
        CHECK(retained.mix() == reference.mix());

        // The window setter must not rebuild from a rejected value. Checking
        // the measured table catches poisoning even if later output happens to
        // have no active grain on the sampled frame.
        CHECK(retained.window_mean() == reference.window_mean());
        CHECK(retained.window_rms() == reference.window_rms());
        for (double phase : {0.0, 0.125, 0.5, 0.875, 1.0}) {
            CHECK(retained.window_at(phase) == reference.window_at(phase));
        }

        std::vector<double> ref_left(input.size());
        std::vector<double> ref_right(input.size());
        std::vector<double> retained_left(input.size());
        std::vector<double> retained_right(input.size());
        reference.process(input.data(), ref_left.data(), ref_right.data(),
                          static_cast<int>(input.size()));
        retained.process(input.data(), retained_left.data(), retained_right.data(),
                         static_cast<int>(input.size()));

        // Exact render and scheduler equality covers controls without getters:
        // pitch/pan spray, level, and every scheduler input. A default-reset or
        // merely-finite substitution fails this comparison.
        CHECK(retained_left == ref_left);
        CHECK(retained_right == ref_right);
        CHECK(retained.grain_index() == reference.grain_index());
        CHECK(retained.active_grain_count() == reference.active_grain_count());
        CHECK(retained.steal_count() == reference.steal_count());
        CHECK(retained.clamp_count() == reference.clamp_count());
    }
}

TEST_CASE("Granular prepare retains its finite sample rate on non-finite input",
          "[granular][params][nan-recovery]") {
    GranularEngine64 engine;
    engine.prepare(kFs);
    const int expected_ring_length = engine.ring_length();
    const int expected_guard = engine.causality_guard_samples();

    for (double invalid : {std::numeric_limits<double>::quiet_NaN(),
                           std::numeric_limits<double>::infinity(),
                           -std::numeric_limits<double>::infinity()}) {
        CAPTURE(invalid);
        engine.prepare(invalid);
        CHECK(engine.ring_length() == expected_ring_length);
        CHECK(engine.causality_guard_samples() == expected_guard);
    }
}

TEST_CASE("Granular live input rejects non-finite samples and recovers exactly",
          "[granular][live][nan-recovery][rt]") {
    const std::vector<double> continuation = noise_buffer(8192, 0xBAD5A11u);
    constexpr double ring_sentinel = 0.375;
    constexpr int sentinel_index = 2048;
    const std::vector<double> warmup(4096, ring_sentinel);

    const auto configure = [](GranularEngine64& engine) {
        engine.prepare(kFs);
        engine.set_source(GrainSource::live_ring);
        engine.set_density_hz(173.0);
        engine.set_grain_ms(37.0);
        engine.set_async_jitter(0.31);
        engine.set_position(0.17);
        engine.set_position_spray_ms(9.0);
        engine.set_pitch_semitones(-3.0);
        engine.set_pitch_spray_semitones(2.0);
        engine.set_pan_spray(0.63);
        engine.set_window_taper(0.71);
        engine.set_level_db(-4.0);
        engine.set_mix(0.58);
        engine.set_seed(0xC001D00Du);
        engine.reset();
    };

    for (double invalid : {std::numeric_limits<double>::quiet_NaN(),
                           std::numeric_limits<double>::infinity(),
                           -std::numeric_limits<double>::infinity()}) {
        CAPTURE(invalid);

        SECTION("interleaved live callback") {
            GranularEngine64 poisoned;
            GranularEngine64 fresh;
            configure(poisoned);
            configure(fresh);

            std::vector<double> warm_left(warmup.size());
            std::vector<double> warm_right(warmup.size());
            poisoned.process(warmup.data(), warm_left.data(), warm_right.data(),
                             static_cast<int>(warmup.size()));
            REQUIRE(poisoned.active_grain_count() > 0);
            REQUIRE(poisoned.ring_storage_sample(sentinel_index) == ring_sentinel);

            double bad_left = 1.0;
            double bad_right = 1.0;
            {
                pulp::test::RtAllocationProbe probe;
                poisoned.process(&invalid, &bad_left, &bad_right, 1);
                REQUIRE(probe.allocation_count() == 0);
            }
            REQUIRE(bad_left == 0.0);
            REQUIRE(bad_right == 0.0);
            // Timing-independent proof that the audio-thread recovery did not
            // perform the O(ring_length) clear that public reset() performs.
            REQUIRE(poisoned.ring_storage_sample(sentinel_index) == ring_sentinel);

            fresh.reset();
            // The logical write head has rewound even though physical storage
            // remains. With no newly written samples, stale sentinels must be
            // unreachable and therefore inaudible.
            constexpr int stale_probe_samples = 512;
            std::vector<double> stale_left(stale_probe_samples);
            std::vector<double> stale_right(stale_probe_samples);
            std::vector<double> fresh_stale_left(stale_probe_samples);
            std::vector<double> fresh_stale_right(stale_probe_samples);
            poisoned.process(stale_left.data(), stale_right.data(), stale_probe_samples);
            fresh.process(fresh_stale_left.data(), fresh_stale_right.data(), stale_probe_samples);
            REQUIRE(stale_left == fresh_stale_left);
            REQUIRE(stale_right == fresh_stale_right);
            REQUIRE(std::all_of(stale_left.begin(), stale_left.end(),
                                [](double sample) { return sample == 0.0; }));
            REQUIRE(std::all_of(stale_right.begin(), stale_right.end(),
                                [](double sample) { return sample == 0.0; }));

            std::vector<double> recovered_left(continuation.size());
            std::vector<double> recovered_right(continuation.size());
            std::vector<double> reference_left(continuation.size());
            std::vector<double> reference_right(continuation.size());
            poisoned.process(continuation.data(), recovered_left.data(), recovered_right.data(),
                             static_cast<int>(continuation.size()));
            fresh.process(continuation.data(), reference_left.data(), reference_right.data(),
                          static_cast<int>(continuation.size()));

            REQUIRE(recovered_left == reference_left);
            REQUIRE(recovered_right == reference_right);
            REQUIRE(poisoned.grain_index() == fresh.grain_index());
            REQUIRE(poisoned.active_grain_count() == fresh.active_grain_count());

            poisoned.reset();
            REQUIRE(poisoned.ring_storage_sample(sentinel_index) == 0.0);
        }

        SECTION("two-call write_live path") {
            GranularEngine64 poisoned;
            GranularEngine64 fresh;
            configure(poisoned);
            configure(fresh);

            poisoned.write_live(warmup.data(), static_cast<int>(warmup.size()));
            std::vector<double> discarded_left(warmup.size());
            std::vector<double> discarded_right(warmup.size());
            poisoned.process(discarded_left.data(), discarded_right.data(),
                             static_cast<int>(warmup.size()));
            REQUIRE(poisoned.active_grain_count() > 0);
            REQUIRE(poisoned.ring_storage_sample(sentinel_index) == ring_sentinel);

            {
                pulp::test::RtAllocationProbe probe;
                poisoned.write_live(&invalid, 1);
                REQUIRE(probe.allocation_count() == 0);
            }
            REQUIRE(poisoned.grain_index() == 0);
            REQUIRE(poisoned.active_grain_count() == 0);
            REQUIRE(poisoned.ring_storage_sample(sentinel_index) == ring_sentinel);

            fresh.reset();
            constexpr int stale_probe_samples = 512;
            std::vector<double> stale_left(stale_probe_samples);
            std::vector<double> stale_right(stale_probe_samples);
            std::vector<double> fresh_stale_left(stale_probe_samples);
            std::vector<double> fresh_stale_right(stale_probe_samples);
            poisoned.process(stale_left.data(), stale_right.data(), stale_probe_samples);
            fresh.process(fresh_stale_left.data(), fresh_stale_right.data(), stale_probe_samples);
            REQUIRE(stale_left == fresh_stale_left);
            REQUIRE(stale_right == fresh_stale_right);
            REQUIRE(std::all_of(stale_left.begin(), stale_left.end(),
                                [](double sample) { return sample == 0.0; }));
            REQUIRE(std::all_of(stale_right.begin(), stale_right.end(),
                                [](double sample) { return sample == 0.0; }));

            poisoned.write_live(continuation.data(), static_cast<int>(continuation.size()));
            fresh.write_live(continuation.data(), static_cast<int>(continuation.size()));
            std::vector<double> recovered_left(continuation.size());
            std::vector<double> recovered_right(continuation.size());
            std::vector<double> reference_left(continuation.size());
            std::vector<double> reference_right(continuation.size());
            poisoned.process(recovered_left.data(), recovered_right.data(),
                             static_cast<int>(continuation.size()));
            fresh.process(reference_left.data(), reference_right.data(),
                          static_cast<int>(continuation.size()));

            REQUIRE(recovered_left == reference_left);
            REQUIRE(recovered_right == reference_right);

            poisoned.reset();
            REQUIRE(poisoned.ring_storage_sample(sentinel_index) == 0.0);
        }
    }
}

TEST_CASE("Granular borrowed buffers reject non-finite source frames",
          "[granular][buffer][nan-recovery]") {
    std::vector<double> source(4096, 0.25);
    source[1024] = std::numeric_limits<double>::quiet_NaN();
    source[2048] = std::numeric_limits<double>::infinity();
    GranularEngine64 engine;
    engine.prepare(kFs);
    configure_buffer_engine(engine, source);
    engine.set_density_hz(200.0);
    engine.set_grain_ms(80.0);
    const Stereo out = render(engine, 8192);
    REQUIRE(std::all_of(out.left.begin(), out.left.end(),
                        [](double v) { return std::isfinite(v); }));
    REQUIRE(std::all_of(out.right.begin(), out.right.end(),
                        [](double v) { return std::isfinite(v); }));
}
