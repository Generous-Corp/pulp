#pragma once

/// @file visualization_bridge.hpp
/// Realtime-safe bridge for audio visualization data. The audio callback only
/// meters the block and copies it into a fixed-capacity SPSC tap. FFT and
/// waveform assembly happen when the UI owner calls `poll()`. Snapshot reads
/// stay cheap and never drain or analyze audio. This keeps visualization
/// cadence independent of audio callback cadence and makes the producer path
/// allocation-free after `configure()`.
///
/// The same data published here is accessible from JS via AudioBridge →
/// widget_bridge.cpp bindings.

#include <pulp/runtime/triple_buffer.hpp>
#include <pulp/audio/planar_audio_ring_buffer.hpp>
#include <pulp/signal/stft.hpp>
#include <pulp/signal/multi_channel_meter.hpp>
#include <pulp/signal/spectrogram.hpp>
#include <array>
#include <atomic>
#include <cmath>
#include <algorithm>
#include <vector>
#include <cstdint>

// The public view header must NOT include
// pulp/render/bench/perf_counters.hpp — it leaks render include paths through
// every view consumer under PULP_BENCHMARK, even those that don't link
// pulp::render. Forward-declare instead; the non-trivial process() body that
// actually dereferences PerfCounters lives in visualization_bridge.cpp, which
// DOES include the render header.
#ifdef PULP_BENCHMARK
namespace pulp::render::bench {
struct PerfCounters;
}
#endif

namespace pulp::view {

/// Published spectrum data (lock-free via TripleBuffer).
struct SpectrumData {
    static constexpr int kMaxBins = 4097; // FFT 8192 → 4097 bins

    std::array<float, kMaxBins> magnitude_db{};
    int num_bins = 0;
    std::uint64_t epoch = 0;
    std::uint64_t sequence_number = 0;
    std::uint64_t dropped_frames = 0;
    int source_channels = 0;
    int fft_size = 0;
    float sample_rate = 0.0f;
    float floor_db = -120.0f;
};

/// Published waveform capture (latest buffer snapshot).
struct WaveformData {
    static constexpr int kMaxSamples = 8192;

    std::array<float, kMaxSamples> samples{};
    int num_samples = 0;
    int num_channels = 0; // source layout; samples contain first channel only
    std::uint64_t sequence_number = 0;
};

/// Configuration for the visualization bridge.
struct VisualizationConfig {
    // STFT
    int fft_size = 1024;
    int hop_size = 256;
    signal::WindowFunction::Type window = signal::WindowFunction::Type::hann;
    float window_param = 0.0f;

    // Metering and capture layout. The captured channel count is invariant
    // until the bridge is quiescent and configured again. Blocks with another
    // layout are metered but intentionally not added to the analyzer stream.
    int num_channels = 2;
    float sample_rate = 44100.0f;

    // Waveform capture
    bool capture_waveform = true;
    int waveform_length = 1024; // samples to capture per snapshot

    // Audio frames retained between non-RT polls. Zero selects an automatic
    // capacity of at least four FFT windows. When the consumer falls behind,
    // new frames are dropped and the cumulative count is stamped in snapshots.
    int capture_buffer_frames = 0;

    // Optional upper bound on frames analyzed by one poll. Zero consumes the
    // frame count visible at entry. A positive value supports predictable UI
    // pacing while leaving the remainder queued for the next tick.
    int max_frames_per_poll = 0;

    // dB value published for silence and used as the finite lower bound.
    float spectrum_floor_db = -120.0f;
};

/// Central visualization bridge: a fixed multichannel audio tap plus a
/// caller-driven, non-RT analyzer and lock-free latest-value publication.
///
/// Thread model:
///   - Audio thread: calls process() from the audio callback (no allocation)
///   - Exactly one UI thread owns poll() and all snapshot reads
///
/// The bridge owns the STFT processor and multi-channel meter internally.
/// No external STFT or meter instances needed.
/// configure() and reset() require full quiescence: neither process(), poll(),
/// nor snapshot reads may be active while either lifecycle method runs.
class VisualizationBridge {
public:
    VisualizationBridge() = default;

    /// Configure all visualization subsystems. Call only while fully quiescent,
    /// before the audio producer and poll/snapshot consumers start.
    void configure(const VisualizationConfig& config) {
        config_ = config;
        config_.num_channels = std::clamp(
            config.num_channels, 0, signal::kMaxMeterChannels);
        config_.sample_rate = std::isfinite(config.sample_rate)
            && config.sample_rate > 0.0f ? config.sample_rate : 44100.0f;
        config_.spectrum_floor_db = std::isfinite(config.spectrum_floor_db)
            ? std::clamp(config.spectrum_floor_db, -300.0f, 0.0f) : -120.0f;
        config_.max_frames_per_poll = std::max(0, config.max_frames_per_poll);

        // Configure one independent STFT per channel. Combining complex audio
        // before the FFT would make anti-phase stereo disappear visually.
        signal::StftConfig stft_cfg;
        stft_cfg.fft_size = config_.fft_size;
        stft_cfg.hop_size = config_.hop_size;
        stft_cfg.window = config_.window;
        stft_cfg.window_param = config_.window_param;
        channel_stfts_.clear();
        channel_stfts_.resize(static_cast<std::size_t>(
            config_.num_channels));
        for (auto& stft : channel_stfts_) stft.configure(stft_cfg);

        // Configure meter
        meter_.prepare(config_.sample_rate, config_.num_channels);

        // Waveform capture setup
        waveform_length_ = config_.capture_waveform
            ? std::clamp(config_.waveform_length, 1,
                         static_cast<int>(WaveformData::kMaxSamples))
            : 0;
        waveform_pos_ = 0;

        const int automatic_capacity = std::max(config_.fft_size * 4,
                                                 std::max(1, waveform_length_));
        const int requested_capacity = config_.capture_buffer_frames > 0
            ? config_.capture_buffer_frames : automatic_capacity;
        capture_capacity_frames_ = std::max(config_.fft_size, requested_capacity);
        capture_ready_ = capture_.prepare(
            static_cast<std::uint32_t>(channel_stfts_.size()),
            static_cast<std::uint64_t>(capture_capacity_frames_));

        consumer_chunk_frames_ = std::min(capture_capacity_frames_, 4096);
        consumer_storage_.assign(
            channel_stfts_.size() * static_cast<std::size_t>(consumer_chunk_frames_),
            0.0f);
        consumer_channels_.resize(channel_stfts_.size());
        for (std::size_t ch = 0; ch < channel_stfts_.size(); ++ch) {
            consumer_channels_[ch] = consumer_storage_.data()
                + ch * static_cast<std::size_t>(consumer_chunk_frames_);
        }

        observed_dropped_frames_ = 0;
        discontinuity_generation_.store(0, std::memory_order_relaxed);
        acknowledged_discontinuity_.store(0, std::memory_order_relaxed);
        observed_discontinuity_ = 0;
        spectrum_sequence_ = 0;
        waveform_sequence_ = 0;
        if (++epoch_ == 0) ++epoch_;
        spectrum_buf_.write(SpectrumData{});
        waveform_buf_.write(WaveformData{});
    }

    /// Process audio from the audio callback. Allocation-free after configure:
    /// meters and copies into fixed SPSC storage; never performs an FFT.
    ///
    /// @param channels  Array of channel buffer pointers.
    /// @param num_channels  Number of channels.
    /// @param num_samples  Samples per channel in this block.
    ///
    /// Defined out-of-line in visualization_bridge.cpp so the benchmark
    /// counter paths can dereference `render::bench::PerfCounters`
    /// fields without leaking the render include path through this
    /// public header.
    void process(const float* const* channels,
                 int num_channels,
                 int num_samples) noexcept;

#ifdef PULP_BENCHMARK
    /// Install (or clear) the benchmark perf-counter sink. Call from the
    /// UI/main thread before the audio callback starts accumulating.
    /// The pointer is stored by raw reference — caller owns the counters
    /// and must outlive this bridge.
    void set_bench_counters(render::bench::PerfCounters* counters) {
        bench_counters_ = counters;
    }
#endif

    // ── Non-RT analysis and UI snapshot reads ───────────────────────────────

    /// Consume at most the captured frame count visible at entry and publish
    /// the latest spectrum/waveform. This bounded call is the only FFT path and
    /// must run on the one UI thread that owns the bridge's snapshot reads.
    bool poll();

    /// Cheap snapshot-only read. Never drains audio or performs FFT work.
    const SpectrumData& peek_spectrum() { return spectrum_buf_.read(); }

    /// Legacy method name retained for compile compatibility only. This is now
    /// a snapshot-only read: callers migrating from the former auto-poll
    /// behavior must call poll() explicitly on the sole UI owner first.
    const SpectrumData& read_spectrum() { return peek_spectrum(); }

    /// Read the latest multi-channel meter data.
    const signal::MultiChannelMeterData& read_meter() { return meter_buf_.read(); }

    /// Cheap snapshot-only waveform read. Never calls poll().
    const WaveformData& peek_waveform() { return waveform_buf_.read(); }

    /// Legacy method name retained for compile compatibility only. This is now
    /// a snapshot-only read: callers migrating from the former auto-poll
    /// behavior must call poll() explicitly on the sole UI owner first.
    const WaveformData& read_waveform() { return peek_waveform(); }

    // ── Accessors ───────────────────────────────────────────────────────────

    int fft_size() const { return config_.fft_size; }
    int num_bins() const {
        return channel_stfts_.empty() ? 0 : channel_stfts_.front().num_bins();
    }
    int num_channels() const { return config_.num_channels; }
    float sample_rate() const { return config_.sample_rate; }

    /// Reset all internal state after playback stops. Requires full quiescence:
    /// no concurrent process(), poll(), or snapshot read calls.
    void reset() {
        capture_.reset();
        for (auto& stft : channel_stfts_) stft.reset();
        meter_.reset();
        waveform_pos_ = 0;
        std::fill(waveform_ring_.begin(), waveform_ring_.end(), 0.0f);
        observed_dropped_frames_ = 0;
        discontinuity_generation_.store(0, std::memory_order_relaxed);
        acknowledged_discontinuity_.store(0, std::memory_order_relaxed);
        observed_discontinuity_ = 0;
        spectrum_sequence_ = 0;
        waveform_sequence_ = 0;
        if (++epoch_ == 0) ++epoch_;
        spectrum_buf_.write(SpectrumData{});
        meter_buf_.write(signal::MultiChannelMeterData{});
        waveform_buf_.write(WaveformData{});
    }

private:
    void reset_analysis_continuity();

    VisualizationConfig config_;

    // Fixed SPSC tap (audio producer, one UI consumer).
    audio::PlanarAudioRingBuffer capture_;
    bool capture_ready_ = false;
    int capture_capacity_frames_ = 0;

    // Non-RT consumer analysis state.
    std::vector<signal::Stft> channel_stfts_;
    std::vector<float> consumer_storage_;
    std::vector<float*> consumer_channels_;
    int consumer_chunk_frames_ = 0;

    // Multi-channel meter (audio thread)
    signal::MultiChannelMeter meter_;

    // Lock-free publication buffers
    runtime::TripleBuffer<SpectrumData> spectrum_buf_;
    runtime::TripleBuffer<signal::MultiChannelMeterData> meter_buf_;
    runtime::TripleBuffer<WaveformData> waveform_buf_;

    // Waveform capture ring buffer
    std::array<float, WaveformData::kMaxSamples> waveform_ring_{};
    int waveform_length_ = 1024;
    int waveform_pos_ = 0;

    std::uint64_t observed_dropped_frames_ = 0; // poll owner only
    // A rejected producer block creates a time discontinuity. Capture remains
    // suspended until poll() flushes the old continuity and acknowledges it,
    // so post-gap audio can never be concatenated with the old STFT history.
    std::atomic<std::uint64_t> discontinuity_generation_{0};
    std::atomic<std::uint64_t> acknowledged_discontinuity_{0};
    std::uint64_t observed_discontinuity_ = 0; // poll owner only
    std::uint64_t spectrum_sequence_ = 0;
    std::uint64_t waveform_sequence_ = 0;
    std::uint64_t epoch_ = 0;

#ifdef PULP_BENCHMARK
    render::bench::PerfCounters* bench_counters_ = nullptr;
#endif
};

} // namespace pulp::view
