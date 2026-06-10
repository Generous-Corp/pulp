// test_audio_doctor.cpp — offline Audio Doctor analyzers (harness Phase 7,
// slice 1): magnitude/frequency response and THD/THD+N.
//
// ── Analyzer Determinism Contract (uniform across this file) ───────────────
// All renders are deterministic: sine/impulse stimulus (no seed, no clock),
// the headless block-loop render, and pure-arithmetic FFT analysis. dB facts
// are RATIOS (response = output/input, THD = harmonics/fundamental), so the
// FFT backend's absolute normalization cancels and the asserted numbers are
// backend-stable. Per-test specifics — window, FFT length, analysis offset,
// tone coherence, sample rate — are stated on each TEST_CASE and echoed into
// the curve artifact. Tolerance class is "numeric" unless noted otherwise.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "support/audio_doctor.hpp"
#include "support/audio_doctor_artifacts.hpp"

#include "pulp_effect.hpp"

#include <choc/text/choc_JSON.h>

#include <pulp/format/processor.hpp>

#include <algorithm>
#include <cmath>

using namespace pulp::test::audio;
using namespace pulp::examples;

namespace {

constexpr double kSampleRate = 48000.0;

// ── Test-only processors for THD discrimination ────────────────────────────
// A clean unity passthrough and a hard clipper. Both stereo in/out, no
// parameters — the smallest processors that prove the THD analyzer
// discriminates a clean tone from a distorted one. Test-side only.

class PassthroughProcessor : public pulp::format::Processor {
public:
    pulp::format::PluginDescriptor descriptor() const override {
        return {.name = "DoctorPassthrough",
                .manufacturer = "Pulp",
                .bundle_id = "com.pulp.doctor.passthrough",
                .version = "1.0.0",
                .category = pulp::format::PluginCategory::Effect,
                .input_buses = {{"In", 2}},
                .output_buses = {{"Out", 2}}};
    }
    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const pulp::format::PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>& in,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {
        for (std::size_t ch = 0; ch < out.num_channels() && ch < in.num_channels();
             ++ch)
            std::copy(in.channel(ch).begin(), in.channel(ch).end(),
                      out.channel(ch).begin());
    }
};

// Symmetric hard clipper at ±0.4 — a 0.5-amplitude sine is driven well into
// the clip, generating strong odd harmonics (the canonical "clearly higher
// THD" reference).
class HardClipProcessor : public pulp::format::Processor {
public:
    pulp::format::PluginDescriptor descriptor() const override {
        return {.name = "DoctorHardClip",
                .manufacturer = "Pulp",
                .bundle_id = "com.pulp.doctor.hardclip",
                .version = "1.0.0",
                .category = pulp::format::PluginCategory::Effect,
                .input_buses = {{"In", 2}},
                .output_buses = {{"Out", 2}}};
    }
    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const pulp::format::PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>& in,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {
        constexpr float kClip = 0.4f;
        for (std::size_t ch = 0; ch < out.num_channels() && ch < in.num_channels();
             ++ch) {
            auto i = in.channel(ch);
            auto o = out.channel(ch);
            for (std::size_t n = 0; n < o.size(); ++n)
                o[n] = std::clamp(i[n], -kClip, kClip);
        }
    }
};

std::unique_ptr<pulp::format::Processor> create_passthrough() {
    return std::make_unique<PassthroughProcessor>();
}
std::unique_ptr<pulp::format::Processor> create_hard_clip() {
    return std::make_unique<HardClipProcessor>();
}

RenderScenario lowpass_scenario(const char* name, float cutoff_hz) {
    // Input/duration are overridden by the analyzer (it drives the impulse).
    return RenderScenario(create_pulp_effect)
        .name(name)
        .sample_rate(kSampleRate)
        .block_size(256)
        .set_param(kFrequency, cutoff_hz)
        .set_param(kResonance, 0.707f)
        .set_param(kFilterType, 0.0f) // lowpass
        .set_param(kMix, 100.0f);
}

// A bin-coherent fundamental for the THD analyzer: k cycles in fft_length.
double coherent_tone(int k, int fft_length, double sample_rate) {
    return static_cast<double>(k) * sample_rate / fft_length;
}

} // namespace

TEST_CASE("Doctor: PulpEffect lowpass magnitude response", "[audio][doctor]") {
    // Determinism contract: unit-impulse stimulus at frame 0, rectangular
    // window over a 16384-sample segment at offset 0, 48 kHz, response =
    // output/input dB. bin resolution = 48000/16384 ≈ 2.93 Hz. Tolerance
    // class: numeric — the passband flatness and the 8 kHz attenuation use
    // generous dB margins; the impulse spectrum is flat so the ratio is a
    // true transfer response (FFT-backend scale cancels).
    auto scenario = lowpass_scenario("doctor.lowpass-response", 200.0f);
    ResponseOptions opts;
    opts.fft_length = 16384;
    opts.window = Window::rectangular;
    const double checkpoints[] = {50.0, 8000.0};
    const auto curve =
        response_relative_to_input(scenario, checkpoints, opts);

    INFO("DC bin " << curve.magnitude_db_at(0.0) << " dB; 50 Hz "
                   << curve.magnitude_db_at(50.0) << " dB; 8 kHz "
                   << curve.magnitude_db_at(8000.0) << " dB; attenuation "
                   << curve.attenuation_db_at(8000.0) << " dB");

    // Passband (well below the 200 Hz cutoff) is ~flat near unity: within
    // 3 dB of 0 dB at 50 Hz.
    CHECK(std::abs(curve.magnitude_db_at(50.0)) < 3.0);

    // The golden contract: an 8 kHz tone drops by >= 20 dB through a 200 Hz
    // lowpass. This is now a direct response measurement, not a folded RMS
    // bound. (Measured headroom is far larger; 20 dB is the golden floor.)
    CHECK(curve.attenuation_db_at(8000.0) >= 20.0);

    // Curve carries the determinism-contract fields for review.
    CHECK(curve.fft_length == 16384);
    CHECK(curve.window == Window::rectangular);
    CHECK(curve.bin_hz > 2.9);
    CHECK(curve.bin_hz < 3.0);
    REQUIRE(curve.full.size() == 16384 / 2 + 1);
}

TEST_CASE("Doctor: THD discriminates clean vs clipped sine", "[audio][doctor]") {
    // Determinism contract: bin-coherent 1 kHz-ish tone (exactly k cycles in
    // 16384 samples at 48 kHz → rectangular window, zero leakage), amplitude
    // 0.5, 8 harmonics summed, analysis offset 0. THD is harmonics/fundamental
    // (ratio). Tolerance class: numeric — clean THD is bounded well below the
    // clipped THD with a wide separation margin.
    constexpr int kFft = 16384;
    const double tone = coherent_tone(341, kFft, kSampleRate); // ~999 Hz, coherent
    ThdOptions opts;
    opts.fft_length = kFft;
    opts.num_harmonics = 8;
    opts.amplitude = 0.5f;

    auto clean =
        measure_thd(RenderScenario(create_passthrough)
                        .name("doctor.thd-clean")
                        .sample_rate(kSampleRate)
                        .block_size(256),
                    tone, opts);
    auto clipped =
        measure_thd(RenderScenario(create_hard_clip)
                        .name("doctor.thd-clipped")
                        .sample_rate(kSampleRate)
                        .block_size(256),
                    tone, opts);

    INFO("clean THD " << clean.thd_percent() << "% (" << clean.thd_db()
                      << " dB); clipped THD " << clipped.thd_percent() << "% ("
                      << clipped.thd_db() << " dB)");

    // The coherent tone must be recognized as coherent (no leakage path).
    CHECK(clean.coherent);
    CHECK(clipped.coherent);

    // A pure passthrough sine has essentially no harmonic content: THD well
    // below 0.1% (−60 dB). The hard clipper generates strong odd harmonics:
    // THD clearly above 1% (−40 dB). The separation is the discrimination.
    CHECK(clean.thd < 0.001);
    CHECK(clipped.thd > 0.05);
    CHECK(clipped.thd > clean.thd * 50.0);

    // THD+N >= THD (it includes everything non-fundamental), and the clipped
    // case is dominated by harmonics so they track closely.
    CHECK(clipped.thd_plus_n >= clipped.thd);

    // Harmonic breakdown present: [0] fundamental, then 2nd..9th.
    REQUIRE(clipped.harmonics.size() >= 2);
    CHECK(clipped.harmonics.front().index == 1);
}

TEST_CASE("Doctor: curve artifact round-trips", "[audio][doctor]") {
    // Determinism contract: same as the response test; this asserts the
    // artifact schema, not DSP. Tolerance class: exact (field presence).
    auto scenario = lowpass_scenario("doctor.artifact-roundtrip", 1000.0f);
    const double checkpoints[] = {100.0, 5000.0};
    ResponseOptions opts;
    opts.fft_length = 8192;
    const auto curve = response_relative_to_input(scenario, checkpoints, opts);

    const auto path = write_response_artifact(curve, "doctor.artifact-roundtrip");
    REQUIRE(std::filesystem::exists(path));

    const std::string json = response_curve_to_json(curve, "doctor.artifact");
    const auto parsed = choc::json::parse(json);
    REQUIRE(parsed.isObject());
    CHECK(parsed["schema_version"].getWithDefault<int64_t>(-1) ==
          kDoctorCurveSchemaVersion);
    CHECK(parsed["analyzer"].getWithDefault<std::string>("") ==
          "magnitude_response");
    CHECK(parsed["window"].getWithDefault<std::string>("") == "rectangular");
    CHECK(parsed["fft_length"].getWithDefault<int64_t>(0) == 8192);
    CHECK(parsed["sample_rate"].getWithDefault<double>(0.0) == kSampleRate);
    REQUIRE(parsed["curve"].isArray());
    CHECK(parsed["curve"].size() == 8192 / 2 + 1);
    REQUIRE(parsed["checkpoints"].isArray());
    CHECK(parsed["checkpoints"].size() == 2);

    // THD artifact carries its own determinism fields + harmonic breakdown.
    constexpr int kFft = 8192;
    const double tone = coherent_tone(170, kFft, kSampleRate);
    ThdOptions topts;
    topts.fft_length = kFft;
    auto thd = measure_thd(RenderScenario(create_hard_clip)
                               .name("doctor.thd-artifact")
                               .sample_rate(kSampleRate)
                               .block_size(256),
                           tone, topts);
    const auto thd_path = write_thd_artifact(thd, "doctor.thd-artifact");
    REQUIRE(std::filesystem::exists(thd_path));
    const auto thd_parsed = choc::json::parse(thd_to_json(thd, "doctor.thd"));
    CHECK(thd_parsed["schema_version"].getWithDefault<int64_t>(-1) ==
          kDoctorCurveSchemaVersion);
    CHECK(thd_parsed["analyzer"].getWithDefault<std::string>("") == "thd");
    CHECK(thd_parsed["window"].getWithDefault<std::string>("") == "rectangular");
    CHECK(thd_parsed["coherent"].getWithDefault<bool>(false));
    REQUIRE(thd_parsed["harmonics"].isArray());
    CHECK(thd_parsed["harmonics"].size() >= 2);
}
