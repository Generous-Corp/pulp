#pragma once

/// @file spectrum_analyzer.hpp
/// Display-oriented spectrum analysis with a lock-free audio-thread tap.
///
/// The audio thread calls `push_samples()`, which copies samples into a
/// fixed-capacity single-producer ring and returns. The UI thread calls
/// `update(dt)` once per displayed frame: it drains the ring, runs the windowed
/// FFTs, applies the level mode and the time-based release, and exposes one dB
/// value per FFT bin in `bins_db()`. Feed those bins to a `SpectrumBinMapping`
/// to put them on a log-frequency axis.
///
/// Level modes (all calibrated so full-scale content reads 0 dB):
///  - `peak`     per-bin peak amplitude; a full-scale sine reads 0 dB at any
///               window. Frames within one update keep the maximum.
///  - `rms`      per-bin RMS amplitude; a full-scale sine reads -3.01 dB. Frames
///               within one update are power-averaged.
///  - `power_db` per-bin power, scaled so the bins of a signal sum to its mean
///               square. Pair with `SpectrumAggregation::power_sum`.
///  - `psd`      power per Hz (dB re 1/Hz). Pair with `power_mean`.
///
/// Thread rules:
///  - `push_samples()` is the ONLY call for the audio thread. It is wait-free,
///    takes no lock, and never allocates. When the ring is full the newest
///    samples are dropped and counted (`overflow_count()`).
///  - Every other call belongs to the single UI thread: `configure()`
///    allocates, `update()` does not allocate after `configure()`.
///  - The ring is sized once at construction for the largest FFT, so changing
///    the FFT size, window or overlap never touches memory the audio thread
///    is using and is safe while audio is running.
///
/// Backlog: when the UI stalls, the ring holds more audio than is worth
/// analysing. `update()` analyses at most `max_frames_per_update` of the NEWEST
/// frames and discards the older audio (counted in `skipped_frame_count()`)
/// instead of working through the whole backlog.

#include <pulp/runtime/spsc_ring_index.hpp>
#include <pulp/signal/fft.hpp>
#include <pulp/signal/spectrum_bin_mapping.hpp>
#include <pulp/signal/windowing.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace pulp::signal {

enum class SpectrumLevelMode : std::uint8_t { peak, rms, power_db, psd };

struct SpectrumAnalyzerConfig {
    double sample_rate = 48000.0;
    int fft_size = 2048; ///< power of two in [kMinFftSize, kMaxFftSize]
    float overlap = 0.5f; ///< fraction of a frame shared with the next, [0, 0.9375]
    WindowFunction::Type window = WindowFunction::Type::hann;
    SpectrumLevelMode level_mode = SpectrumLevelMode::peak;
    float floor_db = -120.0f;
    /// Time for the displayed level to fall by a factor of e toward a quieter
    /// input (the decay is exp(-dt / release)). Zero or less is instant.
    float release_seconds = 0.25f;
    /// Frames analysed per update() at most; older backlog is skipped.
    int max_frames_per_update = 8;
};

class SpectrumAnalyzer {
  public:
    static constexpr int kMinFftSize = 64;
    static constexpr int kMaxFftSize = 16384;
    /// Ring capacity in samples (one slot is reserved internally). Holds four
    /// of the largest frames, so a UI hiccup of a few frames loses nothing.
    static constexpr int kRingCapacity = 4 * kMaxFftSize + 1;

    SpectrumAnalyzer() : ring_(static_cast<std::size_t>(kRingCapacity), 0.0f), index_(kRingCapacity) {
        configure(SpectrumAnalyzerConfig{});
    }

    // ── Audio thread ──────────────────────────────────────────────────────

    /// Copy `count` mono samples into the ring. Wait-free and allocation-free.
    /// Stereo callers mix down (or push one channel) before calling.
    void push_samples(const float* samples, std::size_t count) noexcept {
        if (samples == nullptr || count == 0)
            return;
        // Offer the ring at most what it can ever hold; the rest is dropped.
        const int want = static_cast<int>(std::min<std::size_t>(count, kRingCapacity));
        int s1 = 0, n1 = 0, s2 = 0, n2 = 0;
        index_.prepare_to_write(want, s1, n1, s2, n2);
        std::copy_n(samples, n1, ring_.data() + s1);
        std::copy_n(samples + n1, n2, ring_.data() + s2);
        index_.finish_write(n1 + n2);
        const auto dropped = count - static_cast<std::size_t>(n1 + n2);
        if (dropped > 0)
            overflow_.fetch_add(dropped, std::memory_order_relaxed);
    }

    // ── UI thread ─────────────────────────────────────────────────────────

    /// Apply a configuration. Allocates. Invalid values are clamped (FFT size
    /// to a power of two in range, overlap to [0, 0.9375]). Audio already in
    /// the ring is discarded so no frame mixes two configurations.
    void configure(SpectrumAnalyzerConfig config) {
        config.fft_size = clamp_fft_size(config.fft_size);
        config.overlap = std::isfinite(config.overlap) ? std::clamp(config.overlap, 0.0f, 0.9375f)
                                                       : 0.5f;
        if (!(config.sample_rate > 0.0) || !std::isfinite(config.sample_rate))
            config.sample_rate = 48000.0;
        if (!std::isfinite(config.floor_db))
            config.floor_db = -120.0f;
        config.max_frames_per_update = std::clamp(config.max_frames_per_update, 1, 64);
        config_ = config;

        const int n = config_.fft_size;
        num_bins_ = n / 2 + 1;
        hop_ = std::max(1, static_cast<int>(std::lround(n * (1.0 - config_.overlap))));

        // Periodic window (N points of an N+1 symmetric one): the right shape
        // for spectral analysis, with no repeated end sample.
        auto w = WindowFunction::generate<float>(n + 1, config_.window);
        w.resize(static_cast<std::size_t>(n));
        window_ = std::move(w);
        double sum = 0.0, sum_sq = 0.0;
        for (float v : window_) {
            sum += v;
            sum_sq += static_cast<double>(v) * v;
        }
        coherent_gain_ = sum / n;
        // Guard a degenerate window; every shipped type has a positive sum.
        if (!(coherent_gain_ > 1e-6)) coherent_gain_ = 1.0;
        if (!(sum_sq > 1e-12)) sum_sq = n;
        sum_sq_ = sum_sq;

        fft_ = std::make_unique<Fft>(n);
        history_.assign(static_cast<std::size_t>(n), 0.0f);
        frame_.assign(static_cast<std::size_t>(n), 0.0f);
        spectrum_.assign(static_cast<std::size_t>(num_bins_), std::complex<float>{});
        fresh_.assign(static_cast<std::size_t>(num_bins_), 0.0);
        display_db_.assign(static_cast<std::size_t>(num_bins_), config_.floor_db);
        floor_linear_ = std::pow(10.0, config_.floor_db * 0.1);
        reset_stream();
    }

    /// Analyse whatever audio arrived since the last call and advance the
    /// release by `dt_seconds` of wall time. Returns the number of FFT frames
    /// analysed. Call once per displayed frame; `dt_seconds` is the elapsed
    /// time since the previous call (clamped to [0, 1] against clock jumps).
    int update(double dt_seconds) {
        dt_seconds = std::isfinite(dt_seconds) ? std::clamp(dt_seconds, 0.0, 1.0) : 0.0;
        std::fill(fresh_.begin(), fresh_.end(), 0.0);
        int frames = 0;
        drain(frames);

        const bool peak = config_.level_mode == SpectrumLevelMode::peak;
        // Frame-rate-independent release: the same decay per second whether
        // the UI runs at 30 or 144 Hz.
        const float keep =
            config_.release_seconds > 0.0f
                ? static_cast<float>(std::exp(-dt_seconds / config_.release_seconds))
                : 0.0f;
        const float floor_db = config_.floor_db;
        for (std::size_t k = 0; k < display_db_.size(); ++k) {
            double v = fresh_[k];
            if (frames > 1 && !peak)
                v /= frames; // power modes average the frames of this update
            const float target =
                v > floor_linear_ ? static_cast<float>(10.0 * std::log10(v)) : floor_db;
            float& shown = display_db_[k];
            if (target >= shown)
                shown = target; // instant attack
            else
                shown = target + (shown - target) * keep;
            if (!(shown > floor_db))
                shown = floor_db;
        }
        return frames;
    }

    /// Per-bin display levels in dB after the last update(): bin_count() values,
    /// DC..Nyquist.
    std::span<const float> bins_db() const noexcept {
        return display_db_;
    }
    std::size_t bin_count() const noexcept {
        return static_cast<std::size_t>(num_bins_);
    }
    const SpectrumAnalyzerConfig& config() const noexcept {
        return config_;
    }
    int hop_size() const noexcept {
        return hop_;
    }
    /// The column aggregation that matches the configured level mode.
    SpectrumAggregation aggregation() const noexcept {
        switch (config_.level_mode) {
        case SpectrumLevelMode::peak:
            return SpectrumAggregation::peak;
        case SpectrumLevelMode::power_db:
            return SpectrumAggregation::power_sum;
        default:
            return SpectrumAggregation::power_mean;
        }
    }
    /// Samples the audio thread could not queue because the ring was full.
    std::uint64_t overflow_count() const noexcept {
        return overflow_.load(std::memory_order_relaxed);
    }
    /// Frames of older audio discarded because the UI fell behind.
    std::uint64_t skipped_frame_count() const noexcept {
        return skipped_frames_;
    }
    /// Frames analysed since construction or configure().
    std::uint64_t frame_count() const noexcept {
        return frames_total_;
    }
    /// Forget all levels and queued audio (UI thread).
    void clear() {
        std::fill(display_db_.begin(), display_db_.end(), config_.floor_db);
        reset_stream();
    }

    static int clamp_fft_size(int requested) noexcept {
        int n = kMinFftSize;
        while (n < requested && n < kMaxFftSize)
            n <<= 1;
        // Round to the nearest power of two at or above the request.
        return n;
    }

  private:
    void reset_stream() {
        int ready = index_.num_ready();
        index_.finish_read(ready);
        filled_ = 0;
        write_pos_ = 0;
        since_frame_ = 0;
        skipped_frames_ = 0;
        frames_total_ = 0;
    }

    // Pull queued audio, skipping stale backlog, and analyse every full hop.
    void drain(int& frames) {
        const int n = config_.fft_size;
        int ready = index_.num_ready();
        if (ready <= 0)
            return;

        const int keep = n + (config_.max_frames_per_update - 1) * hop_;
        if (ready > keep) {
            const int skip = ready - keep;
            index_.finish_read(skip);
            skipped_frames_ += static_cast<std::uint64_t>(skip / hop_);
            ready = keep;
            // History no longer connects to what follows.
            filled_ = 0;
            write_pos_ = 0;
            since_frame_ = 0;
        }

        int s1 = 0, n1 = 0, s2 = 0, n2 = 0;
        index_.prepare_to_read(ready, s1, n1, s2, n2);
        consume(ring_.data() + s1, n1, frames);
        consume(ring_.data() + s2, n2, frames);
        index_.finish_read(n1 + n2);
    }

    void consume(const float* data, int count, int& frames) {
        const int n = config_.fft_size;
        for (int i = 0; i < count; ++i) {
            history_[static_cast<std::size_t>(write_pos_)] = data[i];
            write_pos_ = write_pos_ + 1 == n ? 0 : write_pos_ + 1;
            if (filled_ < n)
                ++filled_;
            ++since_frame_;
            if (filled_ == n && since_frame_ >= hop_) {
                since_frame_ = 0;
                analyse_frame();
                ++frames;
            }
        }
    }

    void analyse_frame() {
        const int n = config_.fft_size;
        // Oldest sample sits at write_pos_ once the history is full.
        int src = write_pos_;
        for (int i = 0; i < n; ++i) {
            const float s = history_[static_cast<std::size_t>(src)];
            frame_[static_cast<std::size_t>(i)] =
                (std::isfinite(s) ? s : 0.0f) * window_[static_cast<std::size_t>(i)];
            src = src + 1 == n ? 0 : src + 1;
        }
        fft_->forward_real_half(frame_.data(), spectrum_.data());
        ++frames_total_;

        const double nd = n;
        const double amp_scale = 1.0 / (nd * coherent_gain_);      // |X| -> amplitude/2
        const double pow_scale = 1.0 / (nd * sum_sq_);              // |X|^2 -> power/2
        const double bin_hz = config_.sample_rate / nd;
        const auto last = static_cast<std::size_t>(num_bins_ - 1);
        for (std::size_t k = 0; k <= last; ++k) {
            const double re = spectrum_[k].real();
            const double im = spectrum_[k].imag();
            const double mag_sq = re * re + im * im;
            // DC and Nyquist are their own mirror image; the other bins carry
            // half of a real signal's energy each, so double them.
            const double fold = (k == 0 || k == last) ? 1.0 : 2.0;
            double v = 0.0;
            switch (config_.level_mode) {
            case SpectrumLevelMode::peak: {
                const double a = fold * std::sqrt(mag_sq) * amp_scale; // peak amplitude
                v = a * a;
                fresh_[k] = std::max(fresh_[k], v);
                continue;
            }
            case SpectrumLevelMode::rms: {
                const double a = fold * std::sqrt(mag_sq) * amp_scale;
                v = 0.5 * a * a;
                break;
            }
            case SpectrumLevelMode::power_db:
                v = fold * mag_sq * pow_scale;
                break;
            case SpectrumLevelMode::psd:
                v = fold * mag_sq * pow_scale / bin_hz;
                break;
            }
            fresh_[k] += v;
        }
    }

    SpectrumAnalyzerConfig config_{};
    std::vector<float> ring_;
    runtime::SpscRingIndex index_;
    std::atomic<std::uint64_t> overflow_{0};

    // UI-thread state below.
    int num_bins_ = 0;
    int hop_ = 1;
    std::vector<float> window_;
    double coherent_gain_ = 1.0;
    double sum_sq_ = 1.0;
    std::unique_ptr<Fft> fft_;
    std::vector<float> history_; // circular, last fft_size samples
    std::vector<float> frame_;
    std::vector<std::complex<float>> spectrum_;
    std::vector<double> fresh_;      // linear power-like value gathered this update
    std::vector<float> display_db_;  // released levels
    double floor_linear_ = 1e-12;
    int filled_ = 0;
    int write_pos_ = 0;
    int since_frame_ = 0;
    std::uint64_t skipped_frames_ = 0;
    std::uint64_t frames_total_ = 0;
};

} // namespace pulp::signal
