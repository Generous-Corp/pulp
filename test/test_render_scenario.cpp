// test_render_scenario.cpp — render-scenario harness coverage.
//
// Covers the render-scenario harness acceptance for
// planning/2026-06-09-audio-observability-and-validation-harness-plan.md:
//   - Buffer<float>::view() const
//   - deterministic signal generators (seeded noise, impulse, step, DC,
//     multi-sine, swept sine, automation + MIDI scripts)
//   - RenderScenario typed scenarios for one effect (PulpGain) and one
//     instrument (PulpTone), additive to their existing golden suites
//   - 64/128/256 block-partition invariance for both
//   - the sample-rate × block-size matrix runner
//
// Analyzer Determinism Contract: all stimuli here are deterministic
// (documented expressions; seeded xorshift64* noise, no random_device, no
// clock); analysis is the pure-arithmetic audio_metrics layer (no FFT);
// the frequency estimator is the documented zero-crossing interpolator.
// Tolerance classes are stated per assertion (numeric unless noted exact).

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/analysis/audio_artifacts.hpp>
#include "support/render_scenario.hpp"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <vector>

#include "pulp_gain.hpp"
#include "pulp_tone.hpp"

using namespace pulp::test::audio;

namespace {

// Windowed RMS via the metrics layer (no duplicated math): analyze a
// const-view slice of channel 0. `length` is clamped by slice().
double window_rms_dbfs(const pulp::audio::Buffer<float>& buffer,
                       std::size_t start, std::size_t length, double sr) {
    return to_dbfs(analyze(buffer.view().slice(start, length), sr)
                       .channels[0].rms);
}

} // namespace

TEST_CASE("Buffer const view exposes identical read-only data",
          "[audio][buffer]") {
    auto buf = make_sine(2, 256, 440.0f, 48000.0, 0.5f);
    const auto& const_buf = buf;

    pulp::audio::BufferView<const float> view = const_buf.view();
    REQUIRE(view.num_channels() == 2);
    REQUIRE(view.num_samples() == 256);
    for (std::size_t ch = 0; ch < 2; ++ch) {
        REQUIRE(view.channel_ptr(ch) == buf.channel(ch).data());
        REQUIRE(std::memcmp(view.channel(ch).data(), buf.channel(ch).data(),
                            256 * sizeof(float)) == 0);
    }

    // Slicing a const view keeps the read-only window aligned.
    auto slice = view.slice(64, 32);
    REQUIRE(slice.num_samples() == 32);
    REQUIRE(slice.channel(0).data() == buf.channel(0).data() + 64);
}

TEST_CASE("Signal generators are deterministic and shaped as documented",
          "[audio][generators]") {
    SECTION("silence, DC, impulse, impulse train, step") {
        const auto silence = make_silence(2, 128);
        CHECK(analyze(silence, 48000.0).max_peak() == 0.0);

        const auto dc = make_dc(1, 512, 0.25f);
        const auto dc_metrics = analyze(dc, 48000.0);
        CHECK(dc_metrics.channels[0].dc_offset == 0.25);
        CHECK(dc_metrics.channels[0].peak == 0.25);

        const auto impulse = make_impulse(1, 256, 1.0f, 37);
        auto span = impulse.channel(0);
        for (std::size_t i = 0; i < span.size(); ++i)
            CHECK(span[i] == (i == 37 ? 1.0f : 0.0f));

        const auto train = make_impulse_train(1, 256, 64, 0.5f);
        int nonzero = 0;
        for (float s : train.channel(0))
            nonzero += (s != 0.0f);
        CHECK(nonzero == 4); // frames 0, 64, 128, 192

        const auto step = make_step(1, 100, 0.5f, 40);
        CHECK(step.channel(0)[39] == 0.0f);
        CHECK(step.channel(0)[40] == 0.5f);
        CHECK(step.channel(0)[99] == 0.5f);
    }

    SECTION("multi-sine RMS matches the partial sum") {
        // RMS of independent partials adds in power: sqrt(Σ aᵢ²/2).
        const SinePartial partials[] = {{440.0, 0.3}, {1370.0, 0.2}};
        const auto buf = make_multi_sine(1, 48000, partials, 48000.0);
        const auto expected = std::sqrt((0.3 * 0.3 + 0.2 * 0.2) / 2.0);
        const auto m = analyze(buf, 48000.0);
        CHECK(std::abs(m.channels[0].rms - expected) < 0.005); // numeric
        CHECK(m.max_peak() <= 0.5);
    }

    SECTION("swept sine rises in frequency and stays bounded") {
        const auto sweep = make_swept_sine(1, 48000, 100.0, 2000.0, 48000.0, 0.5f);
        const auto m = analyze(sweep, 48000.0);
        CHECK(m.max_peak() <= 0.5);
        CHECK_FALSE(m.has_nan_or_inf());
        // Zero-crossing estimate over the first/last 10% — a sweep is not
        // a single pitch, but the windowed mean must rise monotonically.
        const auto head = estimate_frequency(sweep.channel(0).subspan(0, 4800),
                                             48000.0);
        const auto tail = estimate_frequency(sweep.channel(0).subspan(43200),
                                             48000.0);
        CHECK(head.hz > 90.0);
        CHECK(head.hz < 250.0);
        CHECK(tail.hz > 1500.0);
        CHECK(tail.hz < 2100.0);
    }

    SECTION("seeded noise: reproducible, seed- and channel-decorrelated") {
        const auto a = make_white_noise(2, 4096, 1234, 0.5f);
        const auto b = make_white_noise(2, 4096, 1234, 0.5f);
        for (std::size_t ch = 0; ch < 2; ++ch)
            CHECK(std::memcmp(a.channel(ch).data(), b.channel(ch).data(),
                              4096 * sizeof(float)) == 0);

        const auto other_seed = make_white_noise(2, 4096, 99, 0.5f);
        CHECK(std::memcmp(a.channel(0).data(), other_seed.channel(0).data(),
                          4096 * sizeof(float)) != 0);
        CHECK(assert_channels_independent(a).passed);

        const auto m = analyze(a, 48000.0);
        CHECK(m.max_peak() <= 0.5);
        // Uniform noise RMS = amplitude/sqrt(3) ≈ 0.2887 (numeric ±0.01).
        CHECK(std::abs(m.channels[0].rms - 0.5 / std::sqrt(3.0)) < 0.01);

        for (const auto& buf : {make_pink_noise(2, 4096, 7, 0.25f),
                                make_brown_noise(2, 4096, 7, 0.25f)}) {
            const auto nm = analyze(buf, 48000.0);
            CHECK_FALSE(nm.has_nan_or_inf());
            CHECK(nm.max_rms() > 0.001);
            CHECK(assert_channels_independent(buf).passed);
        }
        // Brown noise honors its hard clamp; pink declares no hard bound.
        CHECK(analyze(make_brown_noise(1, 48000, 7, 0.25f), 48000.0)
                  .max_peak() <= 0.25);
    }

    SECTION("script builders") {
        const float values[] = {0.0f, -10.0f, -20.0f};
        const auto steps = make_stepped_automation(2, values, 1000, 500);
        REQUIRE(steps.size() == 3);
        CHECK(steps[1].frame == 1500);
        CHECK(steps[2].value == -20.0f);

        const auto notes = make_note_script(69, 100, 0, 4800);
        REQUIRE(notes.size() == 2);
        CHECK(notes[0].event.is_note_on());
        CHECK(notes[1].event.is_note_off());
        CHECK(notes[1].frame == 4800);
    }
}

TEST_CASE("RenderScenario: PulpGain effect scenario",
          "[audio][scenario][pulpgain]") {
    // -12 dBFS sine peak → RMS -15.05 dBFS; -6 dB output gain → -21.05.
    auto scenario = RenderScenario(pulp::examples::create_pulp_gain)
        .name("pulpgain.minus6")
        .sample_rate(48000.0)
        .block_size(128)
        .input(make_sine(2, 24000, 440.0f, 48000.0,
                         static_cast<float>(from_dbfs(-12.0))))
        .set_param(pulp::examples::kOutputGain, -6.0f);
    const auto result = scenario.render();

    INFO(summarize(result.metrics));
    CHECK(result.scenario ==
          "pulpgain.minus6 sr=48000 block=128 in=2 out=2 frames=24000");
    CHECK(assert_no_nan_inf(result.metrics).passed);
    CHECK(assert_not_clipped(result.metrics, -0.1).passed);
    CHECK(assert_rms_between(result.metrics, -21.6, -20.6).passed); // numeric
    CHECK(assert_frequency_near(result.output.channel(0), 48000.0, 440.0, 5.0)
              .passed);

    // Provenance feeds the artifact writer in one call.
    const auto artifact = write_metrics_artifact(result.metrics, result.scenario);
    CHECK(std::filesystem::exists(artifact));
}

TEST_CASE("RenderScenario: PulpGain bypass nulls against unity gain",
          "[audio][scenario][pulpgain]") {
    // Bypass copies input; unity gain multiplies by exactly 1.0f. Both must
    // be bit-identical to the stimulus, hence to each other (tolerance
    // class: exact).
    auto base = RenderScenario(pulp::examples::create_pulp_gain)
        .sample_rate(48000.0)
        .block_size(256)
        .input(make_sine(2, 12288, 220.0f, 48000.0, 0.5f));
    const auto bypassed = RenderScenario(base)
        .name("pulpgain.bypass")
        .set_param(pulp::examples::kBypass, 1.0f)
        .render();
    const auto unity = RenderScenario(base).name("pulpgain.unity").render();

    const auto null_check = assert_null_near(bypassed.output, unity.output,
                                             kExactPartitionToleranceDb);
    INFO(null_check.message);
    CHECK(null_check.passed);
}

TEST_CASE("RenderScenario: PulpGain block-quantized automation",
          "[audio][scenario][pulpgain][automation]") {
    // Output gain drops 0 → -20 dB exactly at the half-way block boundary;
    // the two halves must differ by 20 dB (numeric ±0.2 dB).
    constexpr std::int64_t half = 12000;
    const ParamStep steps[] = {{pulp::examples::kOutputGain, half, -20.0f}};
    const auto result = RenderScenario(pulp::examples::create_pulp_gain)
        .name("pulpgain.step-automation")
        .sample_rate(48000.0)
        .block_size(128) // half is a multiple of 128
        .input(make_sine(2, 2 * half, 440.0f, 48000.0, 0.5f))
        .automate(steps)
        .render();

    const auto first = window_rms_dbfs(result.output, 0, half, 48000.0);
    const auto second = window_rms_dbfs(result.output, half, half, 48000.0);
    INFO("first half " << first << " dBFS, second half " << second << " dBFS");
    CHECK(std::abs((first - second) - 20.0) < 0.2);
}

TEST_CASE("RenderScenario: PulpTone instrument scenario with MIDI script",
          "[audio][scenario][pulptone]") {
    // A4 note-on at frame 0, note-off at 200 ms, render 400 ms: pitch must
    // be 440 Hz (numeric ±5 cents over the held half) and the release tail
    // must decay (default release is 200 ms).
    const auto result = RenderScenario(pulp::examples::create_pulp_tone)
        .name("pulptone.a4-release")
        .sample_rate(48000.0)
        .block_size(128)
        .channels(0, 2)
        .duration_ms(400.0)
        .midi(make_note_script(69, 100, 0, 9600))
        .render();

    INFO(summarize(result.metrics));
    CHECK(assert_no_nan_inf(result.metrics).passed);
    CHECK(assert_not_clipped(result.metrics, -0.1).passed);
    CHECK(assert_not_silent(result.metrics, -60.0).passed);
    CHECK(assert_frequency_near(result.output.channel(0).subspan(0, 9600),
                                48000.0, 440.0, 5.0)
              .passed);

    // Held half vs the final 50 ms of the release tail: ≥ 20 dB quieter.
    const auto held = window_rms_dbfs(result.output, 0, 9600, 48000.0);
    const auto tail = window_rms_dbfs(result.output, 16800, 2400, 48000.0);
    INFO("held " << held << " dBFS, tail " << tail << " dBFS");
    CHECK(tail < held - 20.0);
}

TEST_CASE("RenderScenario: 64/128/256 block partition invariance",
          "[audio][scenario][partition]") {
    constexpr int kBlocks[] = {64, 128, 256};

    SECTION("PulpGain (effect) is exactly partition-invariant") {
        auto scenario = RenderScenario(pulp::examples::create_pulp_gain)
            .name("pulpgain.partition")
            .sample_rate(48000.0)
            .input(make_sine(2, 9600, 440.0f, 48000.0, 0.5f))
            .set_param(pulp::examples::kOutputGain, -6.0f);
        const auto check = assert_block_partition_invariant(scenario, kBlocks);
        INFO(check.message);
        CHECK(check.passed);
    }

    SECTION("PulpTone (instrument) is exactly partition-invariant") {
        auto scenario = RenderScenario(pulp::examples::create_pulp_tone)
            .name("pulptone.partition")
            .sample_rate(48000.0)
            .channels(0, 2)
            .duration_frames(9600)
            .midi(make_note_script(69, 100, 0, 4800));
        const auto check = assert_block_partition_invariant(scenario, kBlocks);
        INFO(check.message);
        CHECK(check.passed);
    }

    SECTION("misconfigured scenarios fail loudly") {
        auto no_duration = RenderScenario(pulp::examples::create_pulp_tone)
            .channels(0, 2);
        CHECK_THROWS_AS(no_duration.render(), std::invalid_argument);
        const int one_block[] = {64};
        CHECK_FALSE(assert_block_partition_invariant(no_duration, one_block)
                        .passed);
    }
}

TEST_CASE("RenderScenario matrix runner sweeps sample rate × block size",
          "[audio][scenario][matrix]") {
    // Generator input so the 100 ms stimulus tracks each cell's rate.
    auto scenario = RenderScenario(pulp::examples::create_pulp_gain)
        .name("pulpgain.matrix")
        .duration_ms(100.0)
        .input([](double sr, int channels, std::int64_t frames) {
            return make_sine(channels, static_cast<int>(frames), 440.0f, sr, 0.5f);
        })
        .set_param(pulp::examples::kOutputGain, -6.0f);

    const double rates[] = {44100.0, 48000.0, 96000.0};
    const int blocks[] = {1, 64, 1024}; // includes the partial-block edge
    const auto cells = run_matrix(scenario, rates, blocks);
    REQUIRE(cells.size() == 9);

    for (const auto& cell : cells) {
        INFO(cell.result.scenario);
        CHECK(cell.result.sample_rate == cell.sample_rate);
        CHECK(cell.result.block_size == cell.block_size);
        CHECK(cell.result.metrics.num_frames ==
              static_cast<int>(std::llround(cell.sample_rate * 0.1)));
        CHECK(assert_no_nan_inf(cell.result.metrics).passed);
        // Sine RMS -9.05 dBFS, -6 dB gain → -15.05 dBFS at every cell
        // (numeric ±0.5 dB; short windows truncate partial cycles).
        CHECK(assert_rms_between(cell.result.metrics, -15.6, -14.6).passed);
        CHECK(assert_frequency_near(cell.result.output.channel(0),
                                    cell.sample_rate, 440.0, 10.0)
                  .passed);
    }
}
