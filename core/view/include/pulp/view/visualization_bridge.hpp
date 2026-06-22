#pragma once

/// @file visualization_bridge.hpp
/// Lock-free bridge for audio visualization data: STFT spectra, multi-channel
/// metering, and waveform capture. Manages all audio→UI publication paths
/// for visualization through a single configuration point.
///
/// NOT A REALTIME-SAFE GENERIC AUDIO PROBE.
/// =========================================
/// Despite running from the audio callback, `VisualizationBridge::process()`
/// is **not** an RT-safe probe and must not be copied as the model for one.
/// Its body runs a full STFT and calls `Stft::latest_magnitude_db()`, which
/// returns a `std::vector` — i.e. it performs FFT work and (potentially)
/// heap allocation on the audio thread. That is acceptable here only because
/// this bridge is an opt-in visualization path for examples/demos, not a
/// general-purpose boundary probe, and because callers accept the cost.
///
/// The realtime-safe, allocation-free, scalar-only probe path is
/// `pulp::audio::AudioProbe` (header `pulp/audio/audio_probe.hpp`). Use that
/// — never this class — when you need an output-boundary probe that satisfies
/// the strict RT ABI (no allocation, no FFT/STFT, no locks, no vector growth).
/// Reuse this bridge's UI-side rendering if you like, but never its
/// callback-side analysis.
///
/// Audio thread calls process(). UI thread reads latest data.
/// Spectrum/meter/waveform publication uses TripleBuffer (latest-value), but
/// the *analysis* feeding it (STFT) is not allocation-free — see above.
///
/// The same data published here is accessible from JS via AudioBridge →
/// widget_bridge.cpp bindings.

#include <pulp/runtime/triple_buffer.hpp>
#include <pulp/signal/stft.hpp>
#include <pulp/signal/multi_channel_meter.hpp>
#include <pulp/signal/spectrogram.hpp>
#include <array>
#include <cmath>
#include <algorithm>
#include <vector>

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
};

/// Published waveform capture (latest buffer snapshot).
struct WaveformData {
    static constexpr int kMaxSamples = 8192;

    std::array<float, kMaxSamples> samples{};
    int num_samples = 0;
    int num_channels = 0; // 1 = mono (mixed), 2+ = first channel only for now
};

/// Configuration for the visualization bridge.
struct VisualizationConfig {
    // STFT
    int fft_size = 1024;
    int hop_size = 256;
    signal::WindowFunction::Type window = signal::WindowFunction::Type::hann;
    float window_param = 0.0f;

    // Metering
    int num_channels = 2;
    float sample_rate = 44100.0f;

    // Waveform capture
    bool capture_waveform = true;
    int waveform_length = 1024; // samples to capture per snapshot
};

/// Central visualization bridge: single entry point for all audio→UI
/// visualization data. Wraps STFT, multi-channel metering, and waveform
/// capture with lock-free TripleBuffer publication.
///
/// NOT an RT-safe probe — see the file-level comment. `process()` runs STFT
/// and a vector-returning spectrum path on the audio thread. For an
/// allocation-free output-boundary probe, use `pulp::audio::AudioProbe`.
///
/// Thread model:
///   - Audio thread: calls process() from the audio callback
///   - UI thread: calls read_spectrum(), read_meter(), read_waveform()
///
/// The bridge owns the STFT processor and multi-channel meter internally.
/// No external STFT or meter instances needed.
class VisualizationBridge {
public:
    VisualizationBridge() = default;

    /// Configure all visualization subsystems. Call before first process().
    void configure(const VisualizationConfig& config) {
        config_ = config;

        // Configure STFT
        signal::StftConfig stft_cfg;
        stft_cfg.fft_size = config.fft_size;
        stft_cfg.hop_size = config.hop_size;
        stft_cfg.window = config.window;
        stft_cfg.window_param = config.window_param;
        stft_.configure(stft_cfg);

        // Configure meter
        meter_.prepare(config.sample_rate, config.num_channels);

        // Waveform capture setup
        waveform_length_ = std::min(config.waveform_length,
                                     static_cast<int>(WaveformData::kMaxSamples));
        waveform_pos_ = 0;
    }

    /// Process audio from the audio callback. Computes STFT, metering,
    /// and waveform capture, then publishes results lock-free.
    ///
    /// WARNING: this is NOT allocation-free. The STFT path calls
    /// `Stft::latest_magnitude_db()` (vector-returning) on the audio thread.
    /// Do not treat this as the RT-safe probe contract — use
    /// `pulp::audio::AudioProbe::analyze_output()` for that.
    ///
    /// @param channels  Array of channel buffer pointers.
    /// @param num_channels  Number of channels.
    /// @param num_samples  Samples per channel in this block.
    ///
    /// Defined out-of-line in visualization_bridge.cpp so the benchmark
    /// counter paths can dereference `render::bench::PerfCounters`
    /// fields without leaking the render include path through this
    /// public header.
    void process(const float* const* channels, int num_channels, int num_samples);

#ifdef PULP_BENCHMARK
    /// Install (or clear) the benchmark perf-counter sink. Call from the
    /// UI/main thread before the audio callback starts accumulating.
    /// The pointer is stored by raw reference — caller owns the counters
    /// and must outlive this bridge.
    void set_bench_counters(render::bench::PerfCounters* counters) {
        bench_counters_ = counters;
    }
#endif

    // ── UI thread reads ─────────────────────────────────────────────────────

    /// Read the latest spectrum data (dB magnitudes).
    const SpectrumData& read_spectrum() { return spectrum_buf_.read(); }

    /// Read the latest multi-channel meter data.
    const signal::MultiChannelMeterData& read_meter() { return meter_buf_.read(); }

    /// Read the latest waveform capture.
    const WaveformData& read_waveform() { return waveform_buf_.read(); }

    // ── Accessors ───────────────────────────────────────────────────────────

    int fft_size() const { return config_.fft_size; }
    int num_bins() const { return stft_.num_bins(); }
    int num_channels() const { return config_.num_channels; }
    float sample_rate() const { return config_.sample_rate; }

    /// Reset all internal state (call when playback stops/restarts).
    void reset() {
        stft_.reset();
        meter_.reset();
        waveform_pos_ = 0;
        std::fill(waveform_ring_.begin(), waveform_ring_.end(), 0.0f);
    }

private:
    VisualizationConfig config_;

    // STFT processor (audio thread)
    signal::Stft stft_;

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

#ifdef PULP_BENCHMARK
    render::bench::PerfCounters* bench_counters_ = nullptr;
#endif
};

} // namespace pulp::view
