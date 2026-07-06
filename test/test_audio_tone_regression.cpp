// test_audio_tone_regression.cpp — PulpTone regression scenario.
//
// Renders PulpTone through HeadlessHost at 48 kHz across small host block
// sizes (64/128/256) and asserts the signal facts the standalone-silence
// class of bugs would have violated: non-silence, no NaN/Inf, no clipping,
// and the expected pitch. A failing scenario writes a JSON metrics
// artifact (schema_version + provenance, see audio_artifacts.hpp) and
// INFO()s its path so local/CI failures leave machine-readable signal
// facts.
//
// Analyzer Determinism Contract (per
// planning/2026-06-09-audio-observability-and-validation-harness-plan.md):
//   stimulus:        MIDI note-on A4 (note 69, velocity 100) at sample 0 of
//                    the first block, held for the whole render; PulpTone
//                    default parameters (sine, -6 dB volume, 10 ms attack)
//   analysis window: full render (9600 frames = 200 ms @ 48 kHz)
//   warm-up trim:    none — the envelope attack is part of the asserted
//                    signal, and an amplitude envelope does not move the
//                    sine's zero crossings, so it cannot bias the estimator
//   estimator:       positive-going zero-crossing with linear interpolation
//                    (documented limits on estimate_frequency)
//   seed:            none (no randomness in PulpTone or the analyzers)
//   tolerance class: numeric — ±5 cents pitch, ≥ −60 dBFS RMS non-silence,
//                    < −0.1 dBFS clip ceiling

#include <catch2/catch_test_macros.hpp>
#include <pulp/format/headless.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/midi/message.hpp>

#include <pulp/audio/analysis/audio_artifacts.hpp>
#include <pulp/audio/analysis/audio_assertions.hpp>
#include <pulp/audio/analysis/audio_metrics.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "pulp_tone.hpp"

using namespace pulp::test::audio;

namespace {

// Render `total` frames of PulpTone in `block`-frame host blocks, with a
// note-on at sample 0 of the first block. This is the test-tone render
// path the standalone-silence regressions broke: many small host blocks,
// MIDI driving an instrument, output accumulated across block boundaries.
pulp::audio::Buffer<float> render_tone(double sample_rate, int block,
                                       int total) {
    pulp::format::HeadlessHost host(pulp::examples::create_pulp_tone);
    host.prepare(sample_rate, block, /*inputs*/ 0, /*outputs*/ 2);

    pulp::audio::Buffer<float> out(2, total);
    for (int pos = 0; pos < total; pos += block) {
        const int n = std::min(block, total - pos);

        pulp::midi::MidiBuffer midi_in;
        if (pos == 0)
            midi_in.add(pulp::midi::MidiEvent::note_on(0, 69, 100)); // A4
        pulp::midi::MidiBuffer midi_out;

        std::vector<float> chunk_L(static_cast<std::size_t>(n), 0.0f);
        std::vector<float> chunk_R(static_cast<std::size_t>(n), 0.0f);
        float* out_ptrs[2] = {chunk_L.data(), chunk_R.data()};
        pulp::audio::BufferView<float> out_view(out_ptrs, 2, n);

        const float* in_ptrs[] = {nullptr};
        pulp::audio::BufferView<const float> in_view(in_ptrs, 0, n);

        host.process(out_view, in_view, midi_in, midi_out);

        std::copy(chunk_L.begin(), chunk_L.end(),
                  out.channel(0).begin() + pos);
        std::copy(chunk_R.begin(), chunk_R.end(),
                  out.channel(1).begin() + pos);
    }
    return out;
}

} // namespace

TEST_CASE("Tone regression: PulpTone 48 kHz render across host block sizes",
          "[audio][regression][pulptone]") {
    constexpr double sr = 48000.0;
    constexpr int total = 9600; // 200 ms; exact multiple of 64/128/256

    for (int block : {64, 128, 256}) {
        DYNAMIC_SECTION("block=" << block) {
            auto out = render_tone(sr, block, total);
            const auto m = analyze(out, sr);
            const auto freq = estimate_frequency(out.channel(0), sr);

            const CheckResult checks[] = {
                assert_no_nan_inf(m),
                assert_not_clipped(m, -0.1),
                assert_not_silent(m, -60.0),
                assert_frequency_near(out.channel(0), sr, 440.0, 5.0),
            };

            // On any failure, persist the metrics as a JSON artifact so the
            // failing run leaves machine-readable signal facts behind.
            std::string artifact_note;
            if (!std::all_of(std::begin(checks), std::end(checks),
                             [](const CheckResult& c) { return c.passed; })) {
                const auto path = write_metrics_artifact(
                    m, "pulptone-48000-block" + std::to_string(block));
                artifact_note = "metrics artifact: " + path.string();
            }
            INFO(artifact_note);
            INFO(summarize(m, freq));

            for (const auto& check : checks) {
                INFO(check.message);
                CHECK(check.passed);
            }
        }
    }
}

TEST_CASE("Tone regression: metrics artifact JSON carries schema and facts",
          "[audio][regression][artifact]") {
    // Round-trip the artifact JSON through choc::json::parse and verify the
    // schema version, provenance, shape, and per-channel facts survive.
    auto out = render_tone(48000.0, 256, 1024);
    const auto m = analyze(out, 48000.0);

    const auto json = metrics_to_json(m, "schema-check");
    const auto parsed = choc::json::parse(json);
    REQUIRE(parsed["schema_version"].getInt64() ==
            kMetricsArtifactSchemaVersion);
    REQUIRE(parsed["scenario"].getString() == "schema-check");
    // choc's JSON writer drops a redundant ".0", so a whole-number sample
    // rate parses back as int64 — use the coercing get<double>().
    REQUIRE(parsed["sample_rate"].get<double>() == 48000.0);
    REQUIRE(parsed["frames"].getInt64() == 1024);
    REQUIRE(parsed["num_channels"].getInt64() == 2);
    REQUIRE(parsed["channels"].size() == 2);
    const auto ch0 = parsed["channels"][0];
    // choc drops a redundant ".0", so a whole-number peak/rms parses back as
    // int64 and getFloat64() would throw — use the coercing get<double>().
    REQUIRE(ch0["peak"].get<double>() > 0.0);
    REQUIRE(ch0["rms"].get<double>() > 0.0);
    REQUIRE(ch0["nan_samples"].getInt64() == 0);
    REQUIRE(ch0["clipped_samples"].getInt64() == 0);

    // write_metrics_artifact sanitizes the scenario for the filename but
    // records it verbatim in the document.
    const auto path = write_metrics_artifact(m, "schema check/v1");
    REQUIRE(std::filesystem::exists(path));
    REQUIRE(path.filename().string() == "schema-check-v1.json");
    std::ifstream in(path, std::ios::binary);
    std::stringstream contents;
    contents << in.rdbuf();
    const auto reread = choc::json::parse(contents.str());
    REQUIRE(reread["scenario"].getString() == "schema check/v1");
    REQUIRE(reread["schema_version"].getInt64() ==
            kMetricsArtifactSchemaVersion);
}
