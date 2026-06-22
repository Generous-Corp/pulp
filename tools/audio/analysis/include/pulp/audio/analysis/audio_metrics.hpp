#pragma once

/// @file audio_metrics.hpp
/// Deterministic offline signal metrics for rendered audio buffers.
///
/// Test/tool layer only — analyzes buffers that have already left the audio
/// thread (typically rendered through `pulp::format::HeadlessHost`). Nothing
/// here is realtime-safe or intended to run inside an audio callback.
///
/// Analyzer determinism contract: every measurement in this header is pure
/// arithmetic over the input samples — no FFT, no windowing, no randomness,
/// no platform-dependent math beyond IEEE-754 float/double. The frequency
/// estimator is a positive-going zero-crossing detector with linear
/// interpolation (see `estimate_frequency` for its stated limits). Results
/// are bit-stable for identical input on a given platform.

#include <pulp/audio/buffer.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace pulp::test::audio {

/// Floor used when converting a zero/negative linear level to dBFS, so
/// summaries and messages stay finite. -200 dB is far below any audible or
/// asserted threshold.
inline constexpr double kSilenceFloorDb = -200.0;

/// Linear amplitude (1.0 == full scale) to dBFS, clamped to kSilenceFloorDb.
double to_dbfs(double linear);

/// dBFS to linear amplitude.
double from_dbfs(double dbfs);

/// Options for analyze(). Defaults match the harness plan's threshold policy:
/// they are named here once instead of being magic numbers in call sites.
struct AnalyzeOptions {
    /// |sample| >= this linear level counts as clipped (1.0 == 0 dBFS).
    float clip_threshold = 1.0f;
    /// |sample| < this linear level counts toward a silence run
    /// (default −90 dBFS: below 16-bit dither, above denormal noise).
    float silence_threshold = 3.1623e-5f;
};

/// Per-channel facts about a rendered buffer.
struct ChannelMetrics {
    double peak = 0.0;                       ///< Max |sample|, linear.
    double rms = 0.0;                        ///< Root mean square, linear.
    double dc_offset = 0.0;                  ///< Mean sample value.
    std::uint64_t nan_samples = 0;           ///< Count of NaN samples.
    std::uint64_t inf_samples = 0;           ///< Count of ±Inf samples.
    std::uint64_t clipped_samples = 0;       ///< Count at/above clip threshold.
    std::uint64_t longest_silence_run = 0;   ///< Longest consecutive sub-threshold run.

    double peak_dbfs() const { return to_dbfs(peak); }
    double rms_dbfs() const { return to_dbfs(rms); }
};

/// Whole-buffer facts. `channels.size() == channel count`.
struct BufferMetrics {
    int num_channels = 0;
    int num_frames = 0;
    double sample_rate = 0.0;
    std::vector<ChannelMetrics> channels;

    bool has_nan_or_inf() const;
    /// Max per-channel peak, linear.
    double max_peak() const;
    /// Max per-channel RMS, linear.
    double max_rms() const;
    std::uint64_t total_clipped_samples() const;
};

/// Analyze a const view. Pure arithmetic; safe for any buffer size.
BufferMetrics analyze(const pulp::audio::BufferView<const float>& buffer,
                      double sample_rate,
                      const AnalyzeOptions& options = {});

/// Convenience overload for an owning Buffer.
BufferMetrics analyze(const pulp::audio::Buffer<float>& buffer,
                      double sample_rate,
                      const AnalyzeOptions& options = {});

/// Result of estimate_frequency().
struct FrequencyEstimate {
    double hz = 0.0;          ///< 0.0 when no estimate is possible.
    double confidence = 0.0;  ///< 1.0 = perfectly regular periods, 0.0 = none.
};

/// Estimate the dominant frequency of one channel.
///
/// Estimator (declared per the Analyzer Determinism Contract): counts
/// positive-going zero crossings with linear interpolation between the
/// bracketing samples, then averages the crossing-to-crossing periods.
/// Confidence is 1 − (period standard deviation / mean period), clamped to
/// [0, 1]. Suited to near-periodic, single-pitch signals (test tones,
/// oscillator output). Not a pitch detector for harmonically dense or noisy
/// material — use the offline FFT analyzers for that. Needs at least three
/// crossings; returns {0.0, 0.0} otherwise.
FrequencyEstimate estimate_frequency(std::span<const float> samples,
                                     double sample_rate);

/// Compact, agent-readable signal summary. Multi-line text; stable field
/// ordering for diffing.
std::string summarize(const BufferMetrics& metrics,
                      const FrequencyEstimate& frequency = {});

} // namespace pulp::test::audio
