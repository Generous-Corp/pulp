// test_latency_contract.cpp — measured-versus-reported latency.
//
// The bug under test: a processor tells the host it delays audio by N samples,
// the host slides the whole track back by N, and nothing ever checks that N is
// the delay the audio actually has. These tests prove the contract catches a
// dishonest report, and — just as importantly — that it refuses to certify a
// report it cannot actually verify.
//
// Analyzer Determinism Contract: the stimuli are a deterministic impulse and a
// seeded white-noise generator (explicit seed, documented PRNG); the evaluators
// are pure arithmetic over the rendered samples. Tolerances are stated per
// expectation.

#include <catch2/catch_test_macros.hpp>

#include "support/audio_contracts.hpp"

#include <pulp/format/processor.hpp>

#include <algorithm>
#include <memory>
#include <vector>

using namespace pulp::test::audio;

namespace {

// A pure delay line whose TRUE delay and REPORTED delay are set independently,
// so a test can make the processor lie on purpose. Everything else about it is
// deliberately trivial: it is the delay, and nothing else.
//
// `gain` exists to break the pass-through property without changing the delay —
// a scaled copy is still a delayed copy, so it proves the delayed-null policy
// checks amplitude too, not just alignment.
struct DelayConfig {
    int true_delay = 0;
    int reported_delay = 0;
    float gain = 1.0f;
    /// Report `reported_delay` for the first `change_after_blocks` blocks, then
    /// switch to `changed_delay` and raise the latency-changed flag. Zero means
    /// never change.
    int change_after_blocks = 0;
    int changed_delay = 0;
};

// Set by each test before the factory runs. The factory signature is a plain
// function pointer, so the configuration has to reach it out of band.
DelayConfig g_config;

class DelayProcessor : public pulp::format::Processor {
public:
    pulp::format::PluginDescriptor descriptor() const override {
        return {
            .name = "LatencyFixture",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.test.latency-fixture",
            .version = "1.0.0",
            .category = pulp::format::PluginCategory::Effect,
            .input_buses = {{"Audio In", 2}},
            .output_buses = {{"Audio Out", 2}},
        };
    }

    void define_parameters(pulp::state::StateStore&) override {}

    void prepare(const pulp::format::PrepareContext& context) override {
        config_ = g_config;
        blocks_seen_ = 0;
        current_report_ = config_.reported_delay;
        const auto channels = static_cast<std::size_t>(context.output_channels);
        // One delay line per channel, sized to the true delay. A zero-length
        // line is a wire.
        lines_.assign(channels,
                      std::vector<float>(
                          static_cast<std::size_t>(std::max(0, config_.true_delay)),
                          0.0f));
        write_pos_.assign(channels, 0);
    }

    int latency_samples() const override { return current_report_; }

    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>& in,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {
        const auto n = out.num_samples();
        const auto channels = std::min(out.num_channels(), in.num_channels());

        for (std::size_t ch = 0; ch < channels && ch < lines_.size(); ++ch) {
            const float* src = in.channel_ptr(ch);
            float* dst = out.channel_ptr(ch);
            auto& line = lines_[ch];

            if (line.empty()) {  // zero delay: straight through
                for (std::size_t i = 0; i < n; ++i) dst[i] = src[i] * config_.gain;
                continue;
            }
            auto& pos = write_pos_[ch];
            for (std::size_t i = 0; i < n; ++i) {
                const float delayed = line[pos];
                line[pos] = src[i];
                pos = (pos + 1) % line.size();
                dst[i] = delayed * config_.gain;
            }
        }

        // Optionally move the report mid-render and tell the host about it,
        // exactly as a real processor flipping FIR taps would.
        ++blocks_seen_;
        if (config_.change_after_blocks > 0 &&
            blocks_seen_ == config_.change_after_blocks) {
            current_report_ = config_.changed_delay;
            flag_latency_changed();
        }
    }

private:
    DelayConfig config_;
    std::vector<std::vector<float>> lines_;
    std::vector<std::size_t> write_pos_;
    int blocks_seen_ = 0;
    int current_report_ = 0;
};

std::unique_ptr<pulp::format::Processor> create_delay() {
    return std::make_unique<DelayProcessor>();
}

constexpr double kSampleRate = 48000.0;
constexpr int kFrames = 16384;

// A stimulus that is periodic BIT FOR BIT: one cell of noise, tiled.
//
// A recomputed sine is not this. Its phase is evaluated afresh at every index,
// so float rounding at large indices leaves samples one period apart differing
// by ~1e-4 — enough for the true delay (which nulls to exactly zero) to win by
// 100+ dB. Tiling is what genuinely erases the delay information: samples one
// period apart are the same bits, so delays one period apart are indistinguishable.
pulp::audio::Buffer<float> make_periodic(int channels, int frames, int period,
                                         std::uint64_t seed) {
    const auto cell = make_white_noise(channels, period, seed, 0.5f);
    pulp::audio::Buffer<float> buffer(static_cast<std::size_t>(channels),
                                      static_cast<std::size_t>(frames));
    for (int ch = 0; ch < channels; ++ch) {
        const auto src = cell.channel(static_cast<std::size_t>(ch));
        auto dst = buffer.channel(static_cast<std::size_t>(ch));
        for (int i = 0; i < frames; ++i)
            dst[static_cast<std::size_t>(i)] =
                src[static_cast<std::size_t>(i % period)];
    }
    return buffer;
}

// Broadband, aperiodic, deterministic: the stimulus the delayed-null policy
// needs to pin a delay down unambiguously.
RenderScenario delay_scenario(const char* name, int block_size = 256) {
    return RenderScenario(create_delay)
        .name(name)
        .sample_rate(kSampleRate)
        .block_size(block_size)
        .input(make_white_noise(2, kFrames, /*seed=*/0x51EED, 0.5f));
}

} // namespace

// ── The contract holds when the report is honest ────────────────────────────

TEST_CASE("Latency contract: an honest report is proven",
          "[audio][contract][latency]") {
    g_config = {.true_delay = 512, .reported_delay = 512};

    AudioContract contract("delay reports its true latency",
                           delay_scenario("latency.honest"));
    const auto evidence = evaluate_reported_latency(contract.result());

    INFO(latency_evidence_summary(evidence));
    REQUIRE(evidence.report_status == LatencyReportStatus::available);
    REQUIRE(evidence.reported_samples == 512);
    REQUIRE(evidence.measured_samples == 512);
    REQUIRE(evidence.delta_samples == 0);
    REQUIRE(evidence.measurement_status == LatencyMeasurementStatus::match);
    REQUIRE(evidence.contract_outcome == LatencyContractOutcome::satisfied);
    REQUIRE_FALSE(evidence.gates_failure());

    contract.expect(expect_reported_latency(contract.result()));
    const auto verdict = contract.verify();
    INFO(verdict.message);
    CHECK(verdict.passed);
}

TEST_CASE("Latency contract: a zero-latency processor reporting zero is proven",
          "[audio][contract][latency]") {
    // A supported zero is a real, provable claim -- not an absence of one.
    g_config = {.true_delay = 0, .reported_delay = 0};

    const auto evidence =
        evaluate_reported_latency(delay_scenario("latency.zero").render());
    INFO(latency_evidence_summary(evidence));
    REQUIRE(evidence.report_status == LatencyReportStatus::available);
    REQUIRE(evidence.reported_samples == 0);
    REQUIRE(evidence.measured_samples == 0);
    REQUIRE(evidence.contract_outcome == LatencyContractOutcome::satisfied);
}

// ── The contract catches a dishonest report ─────────────────────────────────

TEST_CASE("Latency contract: a one-sample misreport is caught",
          "[audio][contract][latency]") {
    // The whole point. One sample of drift is inaudible in isolation and
    // catastrophic when the DAW aligns a parallel track against it.
    g_config = {.true_delay = 512, .reported_delay = 511};

    const auto evidence =
        evaluate_reported_latency(delay_scenario("latency.off-by-one").render());
    INFO(latency_evidence_summary(evidence));
    REQUIRE(evidence.measured_samples == 512);
    REQUIRE(evidence.reported_samples == 511);
    REQUIRE(evidence.delta_samples == 1);
    REQUIRE(evidence.measurement_status == LatencyMeasurementStatus::mismatch);
    REQUIRE(evidence.contract_outcome == LatencyContractOutcome::violated);
    REQUIRE(evidence.gates_failure());
}

TEST_CASE("Latency contract: a processor that under-reports badly is caught",
          "[audio][contract][latency]") {
    // The classic: an FFT stage was added and nobody updated latency_samples().
    g_config = {.true_delay = 1024, .reported_delay = 0};

    const auto evidence =
        evaluate_reported_latency(delay_scenario("latency.unreported").render());
    INFO(latency_evidence_summary(evidence));
    REQUIRE(evidence.measured_samples == 1024);
    REQUIRE(evidence.delta_samples == 1024);
    REQUIRE(evidence.contract_outcome == LatencyContractOutcome::violated);
}

TEST_CASE("Latency contract: a tolerance admits only the drift it is given",
          "[audio][contract][latency]") {
    g_config = {.true_delay = 300, .reported_delay = 298};

    auto result = delay_scenario("latency.tolerance").render();

    DelayedNullOptions strict;  // tolerance_samples = 0
    REQUIRE(evaluate_reported_latency(result, strict).contract_outcome ==
            LatencyContractOutcome::violated);

    DelayedNullOptions loose;
    loose.tolerance_samples = 2;
    REQUIRE(evaluate_reported_latency(result, loose).contract_outcome ==
            LatencyContractOutcome::satisfied);

    DelayedNullOptions still_too_tight;
    still_too_tight.tolerance_samples = 1;
    REQUIRE(evaluate_reported_latency(result, still_too_tight).contract_outcome ==
            LatencyContractOutcome::violated);
}

// ── The contract refuses to certify what it cannot verify ───────────────────

TEST_CASE("Latency contract: a periodic stimulus is ambiguous, not a pass",
          "[audio][contract][latency]") {
    // The single most important refusal in the contract -- the one that stops it
    // from reporting a confident, WRONG latency.
    //
    // With a stimulus of period 100, a delay of 512 and a delay of 12 produce
    // bit-identical output (512 - 12 = 5 periods). The sweep's argmin walks
    // upward, so WITHOUT this guard it would settle on 12, and the contract
    // would loudly and falsely accuse an honest processor of misreporting by
    // 500 samples. The delay is not merely hard to measure here; it is genuinely
    // absent from the audio. Refusing is the only correct answer.
    g_config = {.true_delay = 512, .reported_delay = 512};

    auto scenario = RenderScenario(create_delay)
        .name("latency.ambiguous-periodic")
        .sample_rate(kSampleRate)
        .block_size(256)
        .input(make_periodic(2, kFrames, /*period=*/100, /*seed=*/0xBEEF));

    const auto evidence = evaluate_reported_latency(scenario.render());
    INFO(latency_evidence_summary(evidence));
    REQUIRE(evidence.measurement_status ==
            LatencyMeasurementStatus::not_measurable);
    REQUIRE(evidence.contract_outcome == LatencyContractOutcome::inconclusive);
    REQUIRE(evidence.reason.find("ambiguous") != std::string::npos);
    // Crucially, it did NOT report the bogus 12 as a mismatch.
    REQUIRE_FALSE(evidence.measurement_status ==
                  LatencyMeasurementStatus::mismatch);
    // An inconclusive result that was ASKED for must fail a gate. An unprovable
    // claim is a failed claim, not a silent pass.
    REQUIRE(evidence.gates_failure());
    REQUIRE_FALSE(expect_reported_latency(scenario.render()).passed);
}

TEST_CASE("Latency contract: the ambiguity guard is not blanket paranoia",
          "[audio][contract][latency]") {
    // The guard must reject what is genuinely unknowable without rejecting
    // everything. A 440 Hz sine at 48 kHz has a 109.09-sample period -- not a
    // whole number -- so no other integer delay reproduces it and the delay IS
    // recoverable. The contract should prove it rather than throwing up its
    // hands at anything that looks tonal.
    g_config = {.true_delay = 512, .reported_delay = 512};

    auto scenario = RenderScenario(create_delay)
        .name("latency.non-integer-period-sine")
        .sample_rate(kSampleRate)
        .block_size(256)
        .input(make_sine(2, kFrames, 440.0f, kSampleRate, 0.5f));

    const auto evidence = evaluate_reported_latency(scenario.render());
    INFO(latency_evidence_summary(evidence));
    REQUIRE(evidence.measured_samples == 512);
    REQUIRE(evidence.contract_outcome == LatencyContractOutcome::satisfied);
}

TEST_CASE("Latency contract: a silent stimulus cannot reveal a delay",
          "[audio][contract][latency]") {
    g_config = {.true_delay = 512, .reported_delay = 512};

    auto scenario = RenderScenario(create_delay)
        .name("latency.silent")
        .sample_rate(kSampleRate)
        .block_size(256)
        .input(make_silence(2, kFrames));

    const auto evidence = evaluate_reported_latency(scenario.render());
    INFO(latency_evidence_summary(evidence));
    REQUIRE(evidence.measurement_status ==
            LatencyMeasurementStatus::not_measurable);
    REQUIRE(evidence.contract_outcome == LatencyContractOutcome::inconclusive);
    REQUIRE(evidence.reason.find("silent") != std::string::npos);
    REQUIRE(evidence.gates_failure());
}

TEST_CASE("Latency contract: an output that is not a delayed copy is not measurable",
          "[audio][contract][latency]") {
    // The delayed-null policy states its precondition: the processor must be in
    // a declared identity / bypass / fully-dry mode. A processor that alters the
    // signal (here: halves it) does not satisfy that, and the policy must say so
    // rather than reporting the least-bad delay as if it were the answer.
    g_config = {.true_delay = 512, .reported_delay = 512, .gain = 0.5f};

    const auto evidence =
        evaluate_reported_latency(delay_scenario("latency.not-passthrough").render());
    INFO(latency_evidence_summary(evidence));
    REQUIRE(evidence.measurement_status ==
            LatencyMeasurementStatus::not_measurable);
    REQUIRE(evidence.contract_outcome == LatencyContractOutcome::inconclusive);
    REQUIRE(evidence.reason.find("not a delayed copy") != std::string::npos);
}

TEST_CASE("Latency contract: a report that moves mid-render is inconclusive",
          "[audio][contract][latency]") {
    // There is no single "the" latency to prove over a moving report, so the
    // fixed-latency contract must not stand -- even though the audio's delay
    // never actually changed and the measurement would otherwise agree.
    g_config = {.true_delay = 512,
                .reported_delay = 512,
                .change_after_blocks = 8,
                .changed_delay = 256};

    const auto evidence =
        evaluate_reported_latency(delay_scenario("latency.changed").render());
    INFO(latency_evidence_summary(evidence));
    REQUIRE(evidence.report_observation == LatencyReportObservation::changed);
    REQUIRE(evidence.final_reported_samples == 256);
    REQUIRE(evidence.contract_outcome == LatencyContractOutcome::inconclusive);
    REQUIRE(evidence.reason.find("changed during the render") != std::string::npos);
    REQUIRE(evidence.gates_failure());
}

TEST_CASE("Latency contract: a report that changes and changes back is still inconclusive",
          "[audio][contract][latency]") {
    // The subtle one. Polling alone would see 512 at the start and 512 at the
    // end and call it stable. The latency-changed FLAG is what catches it -- and
    // it must, because a host that was notified mid-render has already acted on
    // the intermediate value.
    g_config = {.true_delay = 512,
                .reported_delay = 512,
                .change_after_blocks = 8,
                .changed_delay = 512};  // "changes" to the same value, but flags

    const auto evidence =
        evaluate_reported_latency(delay_scenario("latency.reverted").render());
    INFO(latency_evidence_summary(evidence));
    REQUIRE(evidence.reported_samples == 512);
    REQUIRE(evidence.final_reported_samples == 512);
    REQUIRE(evidence.report_observation == LatencyReportObservation::changed);
    REQUIRE(evidence.contract_outcome == LatencyContractOutcome::inconclusive);
    REQUIRE(evidence.gates_failure());
}

// ── Robustness across the render grid ───────────────────────────────────────

TEST_CASE("Latency contract: the proof holds across block partitions",
          "[audio][contract][latency]") {
    // The measured delay is a property of the processor, not of how the host
    // happens to chop the stream. A ragged final block must not shift it.
    g_config = {.true_delay = 512, .reported_delay = 512};

    for (int block : {1, 64, 128, 333, 1024}) {
        const auto evidence = evaluate_reported_latency(
            delay_scenario("latency.partitions", block).render());
        INFO("block_size = " << block << " -- "
                             << latency_evidence_summary(evidence));
        REQUIRE(evidence.measured_samples == 512);
        REQUIRE(evidence.contract_outcome == LatencyContractOutcome::satisfied);
    }
}

TEST_CASE("Latency contract: the proof holds across sample rates",
          "[audio][contract][latency]") {
    // Latency is declared in SAMPLES, so the same processor must measure the
    // same number at every rate.
    g_config = {.true_delay = 512, .reported_delay = 512};

    for (double rate : {44100.0, 48000.0, 96000.0, 192000.0}) {
        auto scenario = RenderScenario(create_delay)
            .name("latency.rates")
            .sample_rate(rate)
            .block_size(256)
            .input(make_white_noise(2, kFrames, /*seed=*/0x51EED, 0.5f));

        const auto evidence = evaluate_reported_latency(scenario.render());
        INFO("sample_rate = " << rate << " -- "
                              << latency_evidence_summary(evidence));
        REQUIRE(evidence.measured_samples == 512);
        REQUIRE(evidence.contract_outcome == LatencyContractOutcome::satisfied);
    }
}

// ── The marker policy ───────────────────────────────────────────────────────

TEST_CASE("Latency contract: the marker policy proves a report from one onset",
          "[audio][contract][latency]") {
    g_config = {.true_delay = 777, .reported_delay = 777};

    auto scenario = RenderScenario(create_delay)
        .name("latency.marker")
        .sample_rate(kSampleRate)
        .block_size(256)
        .input(make_impulse(2, kFrames, 1.0f, /*position=*/64));

    MarkerOffsetOptions options;
    options.input_marker_frame = 64;

    const auto evidence =
        evaluate_reported_latency_from_marker(scenario.render(), options);
    INFO(latency_evidence_summary(evidence));
    REQUIRE(evidence.policy == LatencyPolicy::marker_offset);
    REQUIRE(evidence.measured_samples == 777);
    REQUIRE(evidence.contract_outcome == LatencyContractOutcome::satisfied);
}

TEST_CASE("Latency contract: the marker policy catches a misreport",
          "[audio][contract][latency]") {
    g_config = {.true_delay = 777, .reported_delay = 700};

    auto scenario = RenderScenario(create_delay)
        .name("latency.marker-mismatch")
        .sample_rate(kSampleRate)
        .block_size(256)
        .input(make_impulse(2, kFrames, 1.0f, /*position=*/64));

    MarkerOffsetOptions options;
    options.input_marker_frame = 64;

    const auto evidence =
        evaluate_reported_latency_from_marker(scenario.render(), options);
    INFO(latency_evidence_summary(evidence));
    REQUIRE(evidence.delta_samples == 77);
    REQUIRE(evidence.contract_outcome == LatencyContractOutcome::violated);
}

TEST_CASE("Latency contract: the marker policy refuses a non-unique marker",
          "[audio][contract][latency]") {
    // Two onsets and the output onset cannot be attributed to either. Refuse.
    g_config = {.true_delay = 512, .reported_delay = 512};

    auto scenario = RenderScenario(create_delay)
        .name("latency.marker-ambiguous")
        .sample_rate(kSampleRate)
        .block_size(256)
        .input(make_impulse_train(2, kFrames, /*period_frames=*/2048,
                                  /*amplitude=*/1.0f));

    MarkerOffsetOptions options;
    options.input_marker_frame = 0;

    const auto evidence =
        evaluate_reported_latency_from_marker(scenario.render(), options);
    INFO(latency_evidence_summary(evidence));
    REQUIRE(evidence.measurement_status ==
            LatencyMeasurementStatus::not_measurable);
    REQUIRE(evidence.contract_outcome == LatencyContractOutcome::inconclusive);
    REQUIRE(evidence.reason.find("not unique") != std::string::npos);
    REQUIRE(evidence.gates_failure());
}

TEST_CASE("Latency contract: the marker policy subtracts a declared intrinsic offset",
          "[audio][contract][latency]") {
    // A convolver whose IR begins with 100 samples of silence responds 100
    // samples late for a reason that is NOT latency. The author declares that,
    // and the contract must not charge it to the latency report.
    constexpr int kIntrinsic = 100;
    g_config = {.true_delay = 256 + kIntrinsic, .reported_delay = 256};

    auto scenario = RenderScenario(create_delay)
        .name("latency.marker-intrinsic")
        .sample_rate(kSampleRate)
        .block_size(256)
        .input(make_impulse(2, kFrames, 1.0f, /*position=*/32));

    MarkerOffsetOptions options;
    options.input_marker_frame = 32;
    options.intrinsic_response_offset = kIntrinsic;

    const auto evidence =
        evaluate_reported_latency_from_marker(scenario.render(), options);
    INFO(latency_evidence_summary(evidence));
    REQUIRE(evidence.measured_samples == 256);
    REQUIRE(evidence.contract_outcome == LatencyContractOutcome::satisfied);

    // Without the declaration, the same render reads as a 100-sample misreport.
    MarkerOffsetOptions undeclared;
    undeclared.input_marker_frame = 32;
    const auto naive =
        evaluate_reported_latency_from_marker(scenario.render(), undeclared);
    REQUIRE(naive.delta_samples == kIntrinsic);
    REQUIRE(naive.contract_outcome == LatencyContractOutcome::violated);
}

// ── Evidence is serializable, and says the same thing everywhere ────────────

TEST_CASE("Latency contract: evidence serializes with a stable schema",
          "[audio][contract][latency]") {
    g_config = {.true_delay = 512, .reported_delay = 511};

    const auto evidence =
        evaluate_reported_latency(delay_scenario("latency.json").render());
    const auto json = latency_evidence_to_json(evidence);

    INFO(json);
    REQUIRE(json.find("\"schema_version\": 1") != std::string::npos);
    REQUIRE(json.find("\"report_status\": \"available\"") != std::string::npos);
    REQUIRE(json.find("\"reported_samples\": 511") != std::string::npos);
    REQUIRE(json.find("\"measured_samples\": 512") != std::string::npos);
    REQUIRE(json.find("\"delta_samples\": 1") != std::string::npos);
    REQUIRE(json.find("\"contract_outcome\": \"violated\"") != std::string::npos);
    REQUIRE(json.find("\"gates_failure\": true") != std::string::npos);
}
