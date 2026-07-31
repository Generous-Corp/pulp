#pragma once

/// @file realtime_time_stretch.hpp
/// Audio-domain contract for a prepared realtime time-stretch stream.
///
/// The spectral implementation is intentionally private to pulp-audio. Engine
/// layers can admit, prepare, and run a bounded stretch stream without exposing
/// signal-layer types or headers through their public API.

#include <cstdint>
#include <memory>

namespace pulp::audio {

enum class RealtimeTimeStretchQuality : std::uint8_t {
    quality,
    low_latency,
};

enum class RealtimeTimeStretchPrepareStatus : std::uint8_t {
    prepared,
    invalid_sample_rate,
    invalid_channel_count,
    invalid_max_block,
    invalid_max_time_ratio,
    invalid_spectral_geometry,
    unrepresentable_capacity,
};

enum class RealtimeTimeStretchStreamFeedStatus : std::uint8_t {
    accepted,
    backpressure,
    input_closed,
    invalid_request,
};

enum class RealtimeTimeStretchStreamFinalizeStatus : std::uint8_t {
    draining,
    backpressure,
    complete,
    invalid_request,
};

enum class RealtimeTimeStretchStreamFinalizePlanStatus : std::uint8_t {
    ready,
    needs_drain,
    complete,
    invalid_request,
};

struct RealtimeTimeStretchStreamFinalizePlan {
    RealtimeTimeStretchStreamFinalizePlanStatus status =
        RealtimeTimeStretchStreamFinalizePlanStatus::invalid_request;
    int samples = 0;
};

struct RealtimeTimeStretchConfig {
    RealtimeTimeStretchQuality quality = RealtimeTimeStretchQuality::quality;
    int channels = 1;
    int max_block = 4096;
    float max_time_ratio = 2.0f;
    int fft_size = 0;
    int analysis_hop = 0;
};

struct RealtimeTimeStretchPreparedGeometry {
    int maximum_stream_output_lag_samples = 0;
    std::uint64_t retained_bytes = 0;
};

inline constexpr int kRealtimeTimeStretchMaximumChannels = 64;

RealtimeTimeStretchPrepareStatus checked_realtime_time_stretch_prepared_geometry(
    const RealtimeTimeStretchConfig& config, std::uint64_t requested_max_bytes,
    RealtimeTimeStretchPreparedGeometry& prepared) noexcept;

/// Move-only prepared stream. Construction, prepare(), re-prepare, move,
/// destruction, and move-assignment are control-thread operations. After a
/// successful prepare(), reset(), controls, feed/read/finalize, and the query
/// methods are allocation-free and suitable for the audio thread.
class RealtimeTimeStretchProcessor {
  public:
    RealtimeTimeStretchProcessor() noexcept;
    ~RealtimeTimeStretchProcessor();
    RealtimeTimeStretchProcessor(RealtimeTimeStretchProcessor&&) noexcept;
    RealtimeTimeStretchProcessor& operator=(RealtimeTimeStretchProcessor&&) noexcept;
    RealtimeTimeStretchProcessor(const RealtimeTimeStretchProcessor&) = delete;
    RealtimeTimeStretchProcessor& operator=(const RealtimeTimeStretchProcessor&) = delete;

    RealtimeTimeStretchPrepareStatus prepare(
        double sample_rate, const RealtimeTimeStretchConfig& config,
        std::uint64_t requested_max_bytes);

    void reset() noexcept;
    void set_time_ratio(float ratio) noexcept;
    RealtimeTimeStretchStreamFeedStatus feed(const float* const* input,
                                             int num_samples) noexcept;
    int available_stretched() const noexcept;
    int output_free_space() const noexcept;
    int samples_until_next_analysis_frame() const noexcept;
    RealtimeTimeStretchStreamFinalizePlan plan_finalize(int max_samples) const noexcept;
    RealtimeTimeStretchStreamFinalizeStatus finalize(int max_samples) noexcept;
    int read_stretched(float* const* output, int num_samples) noexcept;

  private:
    friend RealtimeTimeStretchPrepareStatus checked_realtime_time_stretch_prepared_geometry(
        const RealtimeTimeStretchConfig&, std::uint64_t,
        RealtimeTimeStretchPreparedGeometry&) noexcept;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulp::audio
