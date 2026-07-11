#pragma once

/// @file render_scenario.hpp
/// Typed offline render scenarios over HeadlessHost.
///
/// A RenderScenario describes one deterministic render — processor
/// factory, sample rate, block size, channel layout, duration, input
/// stimulus, MIDI script, and parameter script — and `render()` executes
/// it block by block, returning the rendered buffer, its BufferMetrics,
/// and a provenance string ready for `write_metrics_artifact()`.
///
/// @code
/// auto result = RenderScenario(pulp::examples::create_pulp_gain)
///     .name("pulpgain.unity")
///     .sample_rate(48000.0)
///     .block_size(128)
///     .input(make_sine(2, 24000, 440.0f, 48000.0, 0.25f))
///     .set_param(pulp::examples::kOutputGain, -6.0f)
///     .render();
/// REQUIRE(assert_not_silent(result.metrics).passed);
/// @endcode
///
/// Event application semantics (deterministic by construction):
/// - MIDI script events are delivered sample-accurately: an event at
///   absolute frame `t` is placed in the block containing `t` with
///   `sample_offset = t - block_start`. Identical across partitions.
/// - Parameter steps are block-quantized: a step at frame `t` is written
///   to the StateStore immediately before processing the block containing
///   `t` (Pulp processors read parameters per block). Align step frames to
///   a common multiple of the tested block sizes when a scenario must be
///   partition-invariant. Sample-accurate delivery through
///   `ParameterEventQueue` is deliberately deferred until a harness target
///   actually consumes per-sample events — mixing both channels for the
///   same step would make the effective application time ambiguous.
///
/// Test/tool layer only — renders happen entirely off the audio thread.
/// Layering (see README.md): scenarios sit on top of signals + metrics +
/// assertions; nothing below may include this header.

#include <pulp/audio/analysis/audio_metrics.hpp>
#include <pulp/audio/analysis/audio_assertions.hpp>
#include <pulp/audio/analysis/latency_evidence.hpp>

#include "audio_signal_generators.hpp"

#include <pulp/format/headless.hpp>

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace pulp::test::audio {

/// Latency facts collected from the processor while a scenario rendered.
///
/// These are FACTS, not a verdict — the report at the start and end of the
/// render, and whether it held still throughout. Turning them into a proof is
/// the job of `expect_reported_latency()`, which pairs them with a measurement
/// of the audio itself. `RenderScenario` collects them on every render (they
/// cost one virtual call per block), so a contract can ask for the proof after
/// the fact without re-rendering.
struct LatencyObservation {
    LatencyReportStatus report_status = LatencyReportStatus::unsupported;
    std::optional<int> reported_samples;
    std::optional<int> final_reported_samples;
    LatencyReportObservation report_observation =
        LatencyReportObservation::unobservable;
    LatencyObservationMode observation_mode = LatencyObservationMode::none;
};

/// Output of RenderScenario::render(): the rendered audio, its analyzed
/// metrics, the input it was rendered from, the processor's latency facts, and
/// provenance. `write_metrics_artifact(result.metrics, result.scenario)`
/// serializes the metrics in one call.
struct ScenarioResult {
    std::string scenario;  ///< "name sr=... block=... in=... out=... frames=..."
    double sample_rate = 0.0;
    int block_size = 0;
    pulp::audio::Buffer<float> output;
    BufferMetrics metrics;
    /// The stimulus actually rendered, at full render length (a fixed input
    /// shorter than the render is zero-padded). Kept so a latency contract can
    /// null the output against it without the caller having to reconstruct the
    /// padding, and so a generated stimulus is comparable at all.
    pulp::audio::Buffer<float> input;
    /// What the processor reported about its own latency during this render.
    LatencyObservation latency;
};

/// Builder for one deterministic offline render. Copyable — the matrix
/// runner copies a prototype scenario and overrides sample rate / block
/// size per cell. `render()` is const and creates a fresh processor via
/// the factory each time, so repeated renders are independent.
class RenderScenario {
public:
    /// Regenerates the input stimulus for the effective sample rate /
    /// duration (used by matrix sweeps where frames depend on the rate).
    using InputGenerator = std::function<pulp::audio::Buffer<float>(
        double sample_rate, int channels, std::int64_t frames)>;

    explicit RenderScenario(pulp::format::ProcessorFactory factory)
        : factory_(factory) {}

    /// Provenance name recorded in ScenarioResult::scenario.
    RenderScenario& name(std::string n) { name_ = std::move(n); return *this; }
    RenderScenario& sample_rate(double hz) { sample_rate_ = hz; return *this; }
    RenderScenario& block_size(int frames) { block_size_ = frames; return *this; }
    /// Channel layout. Instruments typically use (0, 2).
    RenderScenario& channels(int inputs, int outputs) {
        input_channels_ = inputs;
        output_channels_ = outputs;
        return *this;
    }
    RenderScenario& duration_frames(std::int64_t frames) {
        duration_frames_ = frames;
        return *this;
    }
    /// Duration in milliseconds, resolved against the effective sample
    /// rate at render time (rounded to the nearest frame).
    RenderScenario& duration_ms(double ms) {
        duration_ms_ = ms;
        duration_frames_ = -1;
        return *this;
    }
    /// Fixed input stimulus. Sets the input channel count from the buffer;
    /// duration defaults to the buffer length if not set otherwise. If the
    /// render is longer than the buffer, the remainder is silence.
    RenderScenario& input(pulp::audio::Buffer<float> buffer);
    /// Generated input stimulus (regenerated per render — required for
    /// sample-rate sweeps so the stimulus tracks the cell's rate).
    RenderScenario& input(InputGenerator generator) {
        input_generator_ = std::move(generator);
        return *this;
    }
    /// Initial parameter value, applied after prepare() and before the
    /// first block. Later calls for the same id win.
    RenderScenario& set_param(pulp::state::ParamID id, float value) {
        initial_params_.push_back({id, value});
        return *this;
    }
    RenderScenario& automate(ParamStep step) {
        param_script_.push_back(step);
        return *this;
    }
    RenderScenario& automate(std::span<const ParamStep> steps) {
        param_script_.insert(param_script_.end(), steps.begin(), steps.end());
        return *this;
    }
    RenderScenario& midi(MidiScriptEvent event) {
        midi_script_.push_back(std::move(event));
        return *this;
    }
    RenderScenario& midi(std::span<const MidiScriptEvent> events) {
        midi_script_.insert(midi_script_.end(), events.begin(), events.end());
        return *this;
    }

    /// Execute the scenario. Throws std::invalid_argument on a
    /// misconfigured scenario (no duration, non-positive block size, ...).
    ScenarioResult render() const;

private:
    std::int64_t resolve_duration_frames() const;

    pulp::format::ProcessorFactory factory_ = nullptr;
    std::string name_ = "unnamed-scenario";
    double sample_rate_ = 48000.0;
    int block_size_ = 128;
    int input_channels_ = 2;
    int output_channels_ = 2;
    std::int64_t duration_frames_ = -1;
    double duration_ms_ = -1.0;
    pulp::audio::Buffer<float> fixed_input_;
    bool has_fixed_input_ = false;
    InputGenerator input_generator_;
    std::vector<std::pair<pulp::state::ParamID, float>> initial_params_;
    std::vector<ParamStep> param_script_;
    std::vector<MidiScriptEvent> midi_script_;
};

// ── Matrix runner ───────────────────────────────────────────────────────

/// Canonical sweep grids from the harness plan. Callers pick subsets —
/// rendering the full cross product is a nightly-tier cost, not a PR one.
inline constexpr double kMatrixSampleRates[] = {44100.0, 48000.0, 88200.0,
                                                96000.0, 192000.0};
inline constexpr int kMatrixBlockSizes[] = {1, 16, 32, 64, 128, 256, 1024, 4096};

/// One (sample rate × block size) cell of a matrix sweep.
struct MatrixCell {
    double sample_rate = 0.0;
    int block_size = 0;
    ScenarioResult result;
};

/// Render `scenario` once per (sample_rate × block_size) cell, in grid
/// order (rates outer, blocks inner). The prototype's own rate/block are
/// overridden per cell; use a generator input so the stimulus tracks the
/// cell's sample rate.
std::vector<MatrixCell> run_matrix(const RenderScenario& scenario,
                                   std::span<const double> sample_rates,
                                   std::span<const int> block_sizes);

/// Tolerance for "exact" partition invariance: passes only when the
/// residual peak is below 1e-9 linear — true for bit-identical output,
/// failed by even a single-ULP drift near full scale (~-138 dBFS).
/// Block-independent processors (per-sample state machines that read
/// parameters per block but receive none mid-render) should meet this.
inline constexpr double kExactPartitionToleranceDb = -180.0;

/// Render the same scenario at each block size (same sample rate) and
/// null-test every render against the first. Tolerance classes per the
/// plan's Threshold Policy: use kExactPartitionToleranceDb for
/// block-independent processors, a stated numeric tolerance otherwise.
CheckResult assert_block_partition_invariant(
    const RenderScenario& scenario, std::span<const int> block_sizes,
    double tolerance_dbfs = kExactPartitionToleranceDb);

} // namespace pulp::test::audio
