#pragma once

/// @file stn_decomposer.hpp
/// Sines / Transients / Noise (STN) decomposition of a spectral frame
/// stream by median-filter masking.
///
/// Tonal (sinusoidal) content forms horizontal ridges in a spectrogram
/// (stable across time, narrow in frequency); transients form vertical
/// ridges (broadband, brief in time); the rest is noise. Median-filtering
/// the magnitude spectrogram along time emphasizes the tonal part, along
/// frequency emphasizes the transient part, and soft masks derived from
/// the two filtered versions partition each time-frequency bin into S, T,
/// and N = 1 - S - T (Fitzgerald, "Harmonic/Percussive Separation using
/// Median Filtering," DAFx 2010; STN extension and the soft-mask
/// saturating function in Damskägg & Välimäki and Fierro & Välimäki; used
/// as the front end of noise-morphing time-stretch in Moliner et al.,
/// "Noise Morphing for Audio Time Stretching," 2023).
///
/// The decomposition is the front end of a transparent stretcher: sines
/// go through phase-vocoder time-scaling, transients are repositioned,
/// and noise is re-synthesized by morphing (separate primitives). This
/// class only produces the three soft masks (and optionally the split
/// component magnitudes) for one frame at a time, keeping a short rolling
/// history for the time-direction median.
///
/// Magnitude-domain, real-time-safe: a fixed history ring and per-frame
/// median over odd windows; no allocation after prepare().

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <vector>

namespace pulp::signal {

struct StnConfig {
    int num_bins = 1025;       ///< fft_size/2 + 1
    int time_median = 9;       ///< odd; frames for the horizontal (tonal) median
    int freq_median = 9;       ///< odd; bins for the vertical (transient) median
    /// Soft-mask separation factor (Fitzgerald β): the bin is assigned to
    /// a class when that class's filtered magnitude exceeds the other by
    /// this factor. 1.0 = binary Wiener-style split at the crossover.
    float beta = 2.0f;
    /// Align the masks to the NEWEST pushed frame instead of the ring's
    /// center frame. The centered evaluation is less biased (it sees future
    /// frames) but lags the input by (time_median-1)/2 frames, so a caller
    /// that applies the mask to the frame it just pushed (e.g. the noise-
    /// morph split in RealtimePitchTimeProcessor) gets a STALE mask: at a
    /// transient onset the newest frame holds the transient while the mask
    /// still describes a pre-onset (mostly-noise) frame, so the onset's
    /// broadband energy is misrouted into the noise path and decohered.
    /// Causal mode evaluates the time-median as a trailing window ending at
    /// the newest frame and the freq-median on that same frame, so the mask
    /// and the frame it is applied to are aligned (latency 0). This is the
    /// standard real-time HPSS approximation (trailing vs centered median).
    bool causal = false;
};

/// Per-frame soft masks. Each spans num_bins and sums to ~1 per bin.
struct StnMasks {
    std::vector<float> sines;
    std::vector<float> transients;
    std::vector<float> noise;
};

class StnDecomposer {
public:
    /// RT contract: prepare() allocates history, scratch, and mask storage and
    /// is not audio-thread safe. After prepare(), process(), center_magnitude(),
    /// latency_frames(), reset(), and accessors are allocation-free for the
    /// prepared num_bins/window sizes.
    void prepare(const StnConfig& config) {
        assert(config.num_bins > 0);
        assert(config.time_median >= 1 && (config.time_median % 2) == 1);
        assert(config.freq_median >= 1 && (config.freq_median % 2) == 1);
        config_ = config;
        const auto bins = static_cast<size_t>(config.num_bins);

        history_.assign(static_cast<size_t>(config.time_median) * bins, 0.0f);
        history_pos_ = 0;
        history_filled_ = 0;

        masks_.sines.assign(bins, 0.0f);
        masks_.transients.assign(bins, 0.0f);
        masks_.noise.assign(bins, 1.0f);

        harmonic_.assign(bins, 0.0f);
        percussive_.assign(bins, 0.0f);
        time_window_.assign(static_cast<size_t>(config.time_median), 0.0f);
        freq_window_.assign(static_cast<size_t>(config.freq_median), 0.0f);
    }

    int num_bins() const { return config_.num_bins; }

    /// Push one magnitude frame and compute its STN masks. `mag` holds
    /// num_bins magnitudes (linear). The returned masks reference internal
    /// storage valid until the next call. The time-median is centered on
    /// the frame that sits in the middle of the history, so there is an
    /// inherent (time_median-1)/2-frame latency before a frame's masks are
    /// final; transient material in the newest frame is still detected
    /// (the frequency-median is computed on the newest frame directly).
    const StnMasks& process(const float* mag) {
        const int n = config_.num_bins;
        const int th = config_.time_median, fh = config_.freq_median;

        // Store the newest magnitude frame in the ring.
        float* slot = history_.data() + static_cast<size_t>(history_pos_) * n;
        std::copy(mag, mag + n, slot);
        history_pos_ = (history_pos_ + 1) % th;
        if (history_filled_ < th) ++history_filled_;

        // Horizontal (time) median → harmonic estimate. The time-median is
        // order-independent over the filled ring (trailing window), so its
        // value is the same either way; what differs is which frame the masks
        // describe: the ring center (lower bias, lags the input) or the newest
        // frame (causal, aligned to a caller that just pushed it).
        const int center = config_.causal
            ? (history_pos_ - 1 + th) % th
            : (history_pos_ + th / 2) % th;
        const float* center_mag = history_.data() + static_cast<size_t>(center) * n;
        for (int k = 0; k < n; ++k) {
            int count = 0;
            for (int t = 0; t < history_filled_; ++t)
                time_window_[static_cast<size_t>(count++)] =
                    history_[static_cast<size_t>(t) * n + k];
            harmonic_[static_cast<size_t>(k)] = median(time_window_.data(), count);
        }

        // Vertical (frequency) median on the center frame → percussive.
        for (int k = 0; k < n; ++k) {
            int count = 0;
            for (int j = k - fh / 2; j <= k + fh / 2; ++j) {
                const int kk = std::clamp(j, 0, n - 1);
                freq_window_[static_cast<size_t>(count++)] = center_mag[kk];
            }
            percussive_[static_cast<size_t>(k)] = median(freq_window_.data(), count);
        }

        // Soft masks from the two estimates (Fitzgerald saturating split).
        const float beta = config_.beta;
        for (int k = 0; k < n; ++k) {
            const float h = harmonic_[static_cast<size_t>(k)];
            const float pc = percussive_[static_cast<size_t>(k)];
            const float total = h + pc + 1e-12f;
            // Sine confidence rises when the time-median dominates; transient
            // when the freq-median dominates; the residue is noise.
            const float s_raw = saturate(h, beta * pc);
            const float t_raw = saturate(pc, beta * h);
            const float s = s_raw;
            const float t = t_raw * (1.0f - s);
            const float nse = std::max(0.0f, 1.0f - s - t);
            (void)total;
            masks_.sines[static_cast<size_t>(k)] = s;
            masks_.transients[static_cast<size_t>(k)] = t;
            masks_.noise[static_cast<size_t>(k)] = nse;
        }
        return masks_;
    }

    /// The center-frame magnitude the current masks apply to (delayed by
    /// (time_median-1)/2 frames vs the newest pushed frame).
    const float* center_magnitude() const {
        const int th = config_.time_median;
        const int center = config_.causal
            ? (history_pos_ - 1 + th) % th
            : (history_pos_ + th / 2) % th;
        return history_.data() + static_cast<size_t>(center) * config_.num_bins;
    }

    int latency_frames() const {
        return config_.causal ? 0 : (config_.time_median - 1) / 2;
    }

    void reset() {
        std::fill(history_.begin(), history_.end(), 0.0f);
        history_pos_ = 0;
        history_filled_ = 0;
    }

private:
    // 1 when a >= b, 0 when a <= b/beta-equivalent, smooth in between —
    // Fitzgerald's sin^2 ramp on the ratio gives a soft Wiener-like mask.
    static float saturate(float a, float b) {
        const float denom = a + b + 1e-12f;
        const float r = a / denom; // 0.5 at the crossover
        // Map r in [0,1] through a smoothstep centered at 0.5.
        const float x = std::clamp((r - 0.25f) / 0.5f, 0.0f, 1.0f);
        return x * x * (3.0f - 2.0f * x);
    }

    static float median(float* v, int count) {
        const int mid = count / 2;
        std::nth_element(v, v + mid, v + count);
        return v[mid];
    }

    StnConfig config_;
    std::vector<float> history_;     // time_median * num_bins ring
    int history_pos_ = 0;
    int history_filled_ = 0;

    StnMasks masks_;
    std::vector<float> harmonic_;
    std::vector<float> percussive_;
    std::vector<float> time_window_;
    std::vector<float> freq_window_;
};

} // namespace pulp::signal
