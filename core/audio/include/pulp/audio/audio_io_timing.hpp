#pragma once

#include <cstdint>
#include <limits>
#include <optional>

namespace pulp::audio {

/// Clock domain used by the backend's audio timestamps.
enum class AudioTimestampDomain : std::uint8_t {
    unspecified,
    device_sample_frames,
    monotonic_host_time,
};

/// Authority behind the reported I/O timing.
enum class AudioTimingSource : std::uint8_t {
    unspecified,
    device_clock,
    os_estimate,
    user_calibration,
};

/// How strongly callers may rely on the reported values.
enum class AudioTimingConfidence : std::uint8_t {
    unavailable,
    estimated,
    reported,
    calibrated,
};

/// Immutable control-thread snapshot of one audio route's I/O timing.
///
/// Every present latency, safety-offset, and I/O-buffer value is measured in
/// audio sample frames at `sample_rate_hz`; absence is distinct from a
/// reported zero. A
/// backend must increment
/// `calibration_generation` whenever a route change or calibration update can
/// make a previously published value stale. Generation zero is reserved for an
/// unavailable/uninitialized report.
struct AudioIoTiming {
    std::optional<std::uint32_t> input_latency_frames;
    std::optional<std::uint32_t> output_latency_frames;
    std::optional<std::uint32_t> input_safety_offset_frames;
    std::optional<std::uint32_t> output_safety_offset_frames;
    /// Frames in each CoreAudio I/O cycle. For CoreAudio this contributes once
    /// to each complete directional presentation path: once for input
    /// placement, once for output scheduling, and therefore twice for a
    /// complete input-through-graph-to-output monitoring path.
    std::optional<std::uint32_t> io_buffer_frames;
    double sample_rate_hz = 0.0;
    AudioTimestampDomain timestamp_domain = AudioTimestampDomain::unspecified;
    AudioTimingSource timestamp_source = AudioTimingSource::unspecified;
    AudioTimingConfidence confidence = AudioTimingConfidence::unavailable;
    /// Process-local identity of the AudioDevice instance that minted this
    /// report. Backends must publish a nonzero token and must not reuse it.
    std::uint64_t route_instance_token = 0;
    std::uint64_t calibration_generation = 0;
};

constexpr bool is_supported_audio_sample_rate(double sample_rate_hz) noexcept {
    // Comparisons also reject NaN. The upper bound is deliberately above
    // shipping audio hardware while refusing infinities and corrupt metadata.
    return sample_rate_hz >= 1000.0 && sample_rate_hz <= 1536000.0;
}

constexpr std::uint64_t saturating_frame_add(
    std::uint64_t lhs,
    std::uint64_t rhs) noexcept {
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    return rhs > max - lhs ? max : lhs + rhs;
}

constexpr std::optional<std::uint64_t> next_calibration_generation(
    std::uint64_t generation) noexcept {
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    return generation == max
        ? std::nullopt
        : std::optional<std::uint64_t>{generation + 1};
}

/// Control-thread composition of backend I/O timing with the graph's already
/// computed total latency. This type consumes graph PDC; it never derives or
/// recomputes it.
struct LatencySnapshot {
    AudioIoTiming audio_io{};
    double sample_rate_hz = 0.0;
    std::uint64_t graph_latency_frames = 0;
    std::optional<std::uint64_t> input_placement_offset_frames;
    std::optional<std::uint64_t> monitoring_latency_frames;
    std::optional<std::uint64_t> output_scheduling_offset_frames;

    /// Whether this snapshot still describes the route calibration supplied by
    /// the backend. Route identity prevents equal generation values from two
    /// AudioDevice instances from aliasing.
    constexpr bool is_current_for(const AudioIoTiming& timing) const noexcept {
        return audio_io.route_instance_token != 0 &&
               audio_io.route_instance_token == timing.route_instance_token &&
               audio_io.calibration_generation != 0 &&
               audio_io.calibration_generation == timing.calibration_generation;
    }
};

/// Compose I/O timing with a graph total obtained from the graph's public
/// latency accessor. The three offsets intentionally expose distinct jobs:
/// recorded-input placement, live monitoring, and scheduling output early.
/// Unspecified timestamp authority or unavailable confidence fails closed.
constexpr std::optional<LatencySnapshot> make_latency_snapshot(
    const AudioIoTiming& timing,
    std::uint64_t graph_latency_frames,
    double graph_sample_rate_hz) noexcept {
    if (!is_supported_audio_sample_rate(timing.sample_rate_hz) ||
        graph_sample_rate_hz != timing.sample_rate_hz ||
        timing.timestamp_domain == AudioTimestampDomain::unspecified ||
        timing.timestamp_source == AudioTimingSource::unspecified ||
        timing.confidence == AudioTimingConfidence::unavailable ||
        timing.route_instance_token == 0 ||
        timing.calibration_generation == 0) {
        return std::nullopt;
    }

    std::optional<std::uint64_t> input_path;
    if (timing.input_latency_frames && timing.input_safety_offset_frames &&
        timing.io_buffer_frames) {
        input_path = saturating_frame_add(
            saturating_frame_add(
                *timing.input_latency_frames,
                *timing.input_safety_offset_frames),
            *timing.io_buffer_frames);
    }
    std::optional<std::uint64_t> output_path;
    if (timing.output_latency_frames && timing.output_safety_offset_frames &&
        timing.io_buffer_frames) {
        output_path = saturating_frame_add(
            saturating_frame_add(
                *timing.output_latency_frames,
                *timing.output_safety_offset_frames),
            *timing.io_buffer_frames);
    }
    if (!input_path && !output_path) return std::nullopt;

    std::optional<std::uint64_t> monitoring;
    if (input_path && output_path) {
        monitoring = saturating_frame_add(
            saturating_frame_add(*input_path, graph_latency_frames),
            *output_path);
    }
    std::optional<std::uint64_t> output_scheduling;
    if (output_path) {
        output_scheduling =
            saturating_frame_add(graph_latency_frames, *output_path);
    }

    return LatencySnapshot{
        timing,
        timing.sample_rate_hz,
        graph_latency_frames,
        input_path,
        monitoring,
        output_scheduling,
    };
}

} // namespace pulp::audio
