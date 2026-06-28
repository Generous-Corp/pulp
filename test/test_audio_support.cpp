// test_audio_support.cpp — tests for the audio harness helpers
// (audio_metrics + audio_assertions). These test the helpers
// THEMSELVES against synthetic buffers with known ground truth; existing
// golden/matrix tests use the shared helpers.
//
// Analyzer Determinism Contract (declared once for the whole file, per
// planning/2026-06-09-audio-observability-and-validation-harness-plan.md):
//   stimulus:        synthetic sine/DC/silence buffers generated inline with
//                    std::sin — coherence with any FFT length is irrelevant
//                    because no FFT is used
//   analysis window: none (pure time-domain arithmetic over all samples)
//   warm-up trim:    none (stimuli are steady-state from sample 0)
//   estimator:       positive-going zero-crossing with linear interpolation
//                    (documented limits on estimate_frequency)
//   seed:            none (no randomness anywhere in these helpers)
//   tolerance class: numeric (stable floating-point), named per assertion

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/audio/analysis/audio_assertions.hpp>
#include <pulp/audio/analysis/audio_metrics.hpp>
#include "support/audio_test_signals.hpp"

#include <pulp/format/headless.hpp>

#include <cmath>
#include <limits>
#include <numbers>

#include "pulp_gain.hpp"

using namespace pulp::test::audio;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

// ── metrics: levels ─────────────────────────────────────────────────────

TEST_CASE("audio_metrics: full-scale sine has known peak/RMS/DC",
          "[audio-support]") {
    // 440 Hz @ 48 kHz, 4800 frames = exactly 44 cycles, so RMS is the ideal
    // 1/sqrt(2) and DC is ~0 without partial-cycle bias.
    auto buf = make_sine_f(2, 4800, 440.0f, 48000.0);
    auto m = analyze(buf, 48000.0);

    REQUIRE(m.num_channels == 2);
    REQUIRE(m.num_frames == 4800);
    for (const auto& ch : m.channels) {
        REQUIRE_THAT(ch.peak, WithinAbs(1.0, 1e-3));
        REQUIRE_THAT(ch.rms, WithinAbs(1.0 / std::numbers::sqrt2, 1e-3));
        REQUIRE_THAT(ch.dc_offset, WithinAbs(0.0, 1e-4));
        REQUIRE(ch.nan_samples == 0);
        REQUIRE(ch.inf_samples == 0);
    }
    // 0 dBFS sine peak is exactly the clip threshold; sin() hits 1.0 only
    // when a sample lands on the crest, so just sanity-check dB conversion.
    REQUIRE_THAT(to_dbfs(m.max_rms()), WithinAbs(-3.01, 0.05));
}

TEST_CASE("audio_metrics: dBFS conversion round-trips",
          "[audio-support]") {
    REQUIRE_THAT(to_dbfs(1.0), WithinAbs(0.0, 1e-12));
    REQUIRE_THAT(to_dbfs(0.5), WithinAbs(-6.0206, 1e-3));
    REQUIRE_THAT(from_dbfs(to_dbfs(0.25)), WithinAbs(0.25, 1e-9));
    REQUIRE(to_dbfs(0.0) == kSilenceFloorDb);
    REQUIRE(to_dbfs(-1.0) == kSilenceFloorDb);
}

TEST_CASE("audio_metrics: DC offset is reported",
          "[audio-support]") {
    pulp::audio::Buffer<float> buf(1, 1000);
    for (auto& s : buf.channel(0)) s = 0.25f;
    auto m = analyze(buf, 48000.0);
    REQUIRE_THAT(m.channels[0].dc_offset, WithinAbs(0.25, 1e-6));
    REQUIRE_THAT(m.channels[0].peak, WithinAbs(0.25, 1e-6));
}

// ── metrics: defects ────────────────────────────────────────────────────

TEST_CASE("audio_metrics: NaN and Inf samples are counted, not propagated",
          "[audio-support]") {
    auto buf = make_sine_f(1, 1024, 440.0f, 48000.0, 0.5f);
    buf.channel(0)[10] = std::numeric_limits<float>::quiet_NaN();
    buf.channel(0)[20] = std::numeric_limits<float>::infinity();
    buf.channel(0)[30] = -std::numeric_limits<float>::infinity();

    auto m = analyze(buf, 48000.0);
    REQUIRE(m.channels[0].nan_samples == 1);
    REQUIRE(m.channels[0].inf_samples == 2);
    REQUIRE(m.has_nan_or_inf());
    // Accumulators skip the poisoned samples: RMS stays finite and sane.
    REQUIRE(std::isfinite(m.channels[0].rms));
    REQUIRE_THAT(m.channels[0].rms,
                 WithinAbs(0.5 / std::numbers::sqrt2, 1e-2));

    auto check = assert_no_nan_inf(m);
    REQUIRE_FALSE(check.passed);
    REQUIRE(check.message.find("NaN") != std::string::npos);
}

TEST_CASE("audio_metrics: clipped samples are counted at the threshold",
          "[audio-support]") {
    pulp::audio::Buffer<float> buf(1, 100);
    for (auto& s : buf.channel(0)) s = 0.9f;
    buf.channel(0)[5] = 1.0f;   // exactly at threshold — counts
    buf.channel(0)[6] = 1.5f;   // above — counts
    buf.channel(0)[7] = -1.2f;  // negative overload — counts

    auto m = analyze(buf, 48000.0);
    REQUIRE(m.channels[0].clipped_samples == 3);

    auto check = assert_not_clipped(m, -0.1);
    REQUIRE_FALSE(check.passed);
    REQUIRE(check.message.find("dBFS") != std::string::npos);
}

TEST_CASE("audio_metrics: silence runs are measured",
          "[audio-support]") {
    auto buf = make_sine_f(1, 1000, 440.0f, 48000.0, 0.5f);
    // Insert a 200-sample dropout (the plan's "standalone boundary went
    // silent for N consecutive blocks" signature, in miniature).
    for (int i = 300; i < 500; ++i) buf.channel(0)[i] = 0.0f;

    auto m = analyze(buf, 48000.0);
    REQUIRE(m.channels[0].longest_silence_run >= 200);
    // The sine's own zero crossings must not register as runs of this size.
    REQUIRE(m.channels[0].longest_silence_run < 250);
}

// ── assertions: silence / level ─────────────────────────────────────────

TEST_CASE("audio_assertions: silence and non-silence",
          "[audio-support]") {
    pulp::audio::Buffer<float> silent(2, 512);
    silent.clear();
    auto silent_m = analyze(silent, 48000.0);

    REQUIRE(assert_silent(silent_m, -90.0).passed);
    auto not_silent_check = assert_not_silent(silent_m, -60.0);
    REQUIRE_FALSE(not_silent_check.passed);
    REQUIRE(not_silent_check.message.find("-inf dBFS") != std::string::npos);

    auto tone = make_sine_f(2, 512, 440.0f, 48000.0, 0.5f);
    auto tone_m = analyze(tone, 48000.0);
    REQUIRE(assert_not_silent(tone_m, -60.0).passed);
    REQUIRE_FALSE(assert_silent(tone_m, -90.0).passed);
}

TEST_CASE("audio_assertions: peak and RMS ranges",
          "[audio-support]") {
    // -6 dBFS sine: peak ≈ -6 dBFS, RMS ≈ -9 dBFS.
    auto buf = make_sine_f(1, 4800, 440.0f, 48000.0, 0.501187f);
    auto m = analyze(buf, 48000.0);

    REQUIRE(assert_peak_between(m, -7.0, -5.0).passed);
    REQUIRE_FALSE(assert_peak_between(m, -3.0, 0.0).passed);
    REQUIRE(assert_rms_between(m, -10.0, -8.0).passed);
    REQUIRE_FALSE(assert_rms_between(m, -6.0, 0.0).passed);
}

// ── assertions: frequency ───────────────────────────────────────────────

TEST_CASE("audio_assertions: frequency estimate within cents tolerance",
          "[audio-support]") {
    // Tolerance class: numeric. The zero-crossing estimator on a clean
    // 440 Hz sine over 9600 frames (88 cycles) should land well inside
    // ±5 cents.
    auto buf = make_sine_f(1, 9600, 440.0f, 48000.0, 0.8f);
    auto check = assert_frequency_near(buf.channel(0), 48000.0, 440.0, 5.0);
    INFO(check.message);
    REQUIRE(check.passed);

    // A whole-tone mismatch (493.88 Hz expected vs 440 actual ≈ -200 cents)
    // must fail and the message must carry measured Hz and cents.
    auto wrong = assert_frequency_near(buf.channel(0), 48000.0, 493.883, 5.0);
    REQUIRE_FALSE(wrong.passed);
    REQUIRE(wrong.message.find("cents") != std::string::npos);
}

TEST_CASE("audio_assertions: frequency estimator reports no-estimate "
          "honestly", "[audio-support]") {
    pulp::audio::Buffer<float> dc(1, 1024);
    for (auto& s : dc.channel(0)) s = 0.5f; // no zero crossings
    auto check = assert_frequency_near(dc.channel(0), 48000.0, 440.0, 5.0);
    REQUIRE_FALSE(check.passed);
    REQUIRE(check.message.find("no periodic signal") != std::string::npos);

    auto est = estimate_frequency(dc.channel(0), 48000.0);
    REQUIRE(est.hz == 0.0);
    REQUIRE(est.confidence == 0.0);
}

TEST_CASE("audio_assertions: sample-rate mismatch shows as pitch shift",
          "[audio-support]") {
    // The plan's "chipmunk" regression: a 440 Hz tone rendered at 48 kHz but
    // played/analyzed as 96 kHz reads as 880 Hz. The helpers must make that
    // visible rather than vaguely "different".
    auto buf = make_sine_f(1, 9600, 440.0f, 48000.0);
    auto est = estimate_frequency(buf.channel(0), 96000.0);
    REQUIRE_THAT(est.hz, WithinRel(880.0, 0.01));
}

// ── assertions: null + channel routing ──────────────────────────────────

TEST_CASE("audio_assertions: null test detects exact and near matches",
          "[audio-support]") {
    auto a = make_sine_f(2, 1024, 440.0f, 48000.0, 0.5f);
    auto b = make_sine_f(2, 1024, 440.0f, 48000.0, 0.5f);

    REQUIRE(assert_null_near(a, b, -120.0).passed);

    // A single-sample 1e-3 divergence (-60 dBFS residual) must fail a
    // -120 dBFS null and pass a -40 dBFS one.
    b.channel(1)[100] += 1e-3f;
    auto strict = assert_null_near(a, b, -120.0);
    REQUIRE_FALSE(strict.passed);
    REQUIRE(strict.message.find("ch1") != std::string::npos);
    REQUIRE(strict.message.find("frame 100") != std::string::npos);
    REQUIRE(assert_null_near(a, b, -40.0).passed);
}

TEST_CASE("audio_assertions: null test rejects shape mismatch",
          "[audio-support]") {
    auto a = make_sine_f(2, 512, 440.0f, 48000.0);
    auto b = make_sine_f(1, 512, 440.0f, 48000.0);
    auto check = assert_null_near(a, b, -120.0);
    REQUIRE_FALSE(check.passed);
    REQUIRE(check.message.find("shape mismatch") != std::string::npos);
}

TEST_CASE("audio_assertions: duplicated channels are flagged",
          "[audio-support]") {
    pulp::audio::Buffer<float> buf(2, 512);
    for (std::size_t i = 0; i < 512; ++i) {
        const float v = std::sin(2.0f * std::numbers::pi_v<float> * 440.0f *
                                 static_cast<float>(i) / 48000.0f);
        buf.channel(0)[i] = v;
        buf.channel(1)[i] = v; // routing bug: both channels identical
    }
    auto dup = assert_channels_independent(buf);
    REQUIRE_FALSE(dup.passed);
    REQUIRE(dup.message.find("routing") != std::string::npos);

    buf.channel(1)[7] += 0.01f;
    REQUIRE(assert_channels_independent(buf).passed);
}

TEST_CASE("audio_assertions: two silent channels are not flagged as duplicated",
          "[audio-support]") {
    // Two channels that are both silent are trivially sample-identical, but
    // there is no signal to duplicate — that is not a routing bug.
    pulp::audio::Buffer<float> silent(2, 512); // zero-initialized.
    REQUIRE(assert_channels_independent(silent).passed);

    // An identical near-zero constant on both channels is also not a duplication.
    pulp::audio::Buffer<float> quiet_const(2, 512);
    for (std::size_t i = 0; i < 512; ++i) {
        quiet_const.channel(0)[i] = 1e-7f;
        quiet_const.channel(1)[i] = 1e-7f;
    }
    REQUIRE(assert_channels_independent(quiet_const).passed);

    // But two genuinely loud, identical channels still fire.
    pulp::audio::Buffer<float> loud_dup(2, 512);
    for (std::size_t i = 0; i < 512; ++i) {
        const float v = std::sin(2.0f * std::numbers::pi_v<float> * 440.0f *
                                 static_cast<float>(i) / 48000.0f);
        loud_dup.channel(0)[i] = v;
        loud_dup.channel(1)[i] = v;
    }
    REQUIRE_FALSE(assert_channels_independent(loud_dup).passed);
}

// ── signal summary ──────────────────────────────────────────────────────

TEST_CASE("audio_metrics: summarize renders the agent-readable report",
          "[audio-support]") {
    auto buf = make_sine_f(2, 9600, 220.0f, 48000.0, 0.5f);
    auto m = analyze(buf, 48000.0);
    auto est = estimate_frequency(buf.channel(0), 48000.0);
    auto text = summarize(m, est);

    REQUIRE(text.find("Signal summary:") != std::string::npos);
    REQUIRE(text.find("channels: 2") != std::string::npos);
    REQUIRE(text.find("dominant pitch: 220") != std::string::npos);
    REQUIRE(text.find("ch0:") != std::string::npos);
    REQUIRE(text.find("ch1:") != std::string::npos);
    REQUIRE(text.find("dBFS") != std::string::npos);
}

// ── end-to-end through HeadlessHost ─────────────────────────────────────

TEST_CASE("audio_support: HeadlessHost render analyzed by the helpers",
          "[audio-support]") {
    // Proves the helpers compose with the offline host: render PulpGain at
    // unity and assert signal facts instead of raw sample loops.
    pulp::format::HeadlessHost host(pulp::examples::create_pulp_gain);
    host.prepare(48000.0, 1024);

    auto in = make_sine_f(2, 1024, 440.0f, 48000.0, 0.5f);
    pulp::audio::Buffer<float> out(2, 1024);
    const float* in_ptrs[2] = {in.channel(0).data(), in.channel(1).data()};
    pulp::audio::BufferView<const float> in_view(in_ptrs, 2, 1024);
    auto out_view = out.view();
    host.process(out_view, in_view);

    auto m = analyze(out, 48000.0);
    INFO(summarize(m, estimate_frequency(out.channel(0), 48000.0)));
    REQUIRE(assert_no_nan_inf(m).passed);
    REQUIRE(assert_not_clipped(m, -0.1).passed);
    REQUIRE(assert_not_silent(m, -60.0).passed);
    REQUIRE(assert_frequency_near(out.channel(0), 48000.0, 440.0, 5.0).passed);
    REQUIRE(assert_null_near(in, out, -120.0).passed);
}
