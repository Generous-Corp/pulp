#pragma once

/// @file transient_phase_policy.hpp
/// Spectral-flux transient detection for phase-vocoder streams.
///
/// Detection function: half-wave-rectified magnitude flux on the channel
/// group's summed spectrum, gated by BOTH a running-median comparison and
/// an energy-relative absolute floor (Bello et al., "A Tutorial on Onset
/// Detection in Music Signals," IEEE TSAP 13(5), 2005; Dixon, "Onset
/// Detection Revisited," DAFx 2006). The energy-relative floor matters:
/// a median-only gate misfires continuously on steady tones (near-zero
/// medians) and never fires on isolated clicks (zero medians) — and the
/// misfires show up as a pitch bias in the vocoder, not as obvious
/// artifacts.
///
/// On detection the caller feeds the returned confidence into
/// MultichannelPhaseCoordinator's reset amount (phase reinitialization at
/// transients, Röbel, "A New Approach to Transient Processing in the
/// Phase Vocoder," DAFx 2003). All thresholds are relative to the
/// detector's own statistics — nothing absolute to tune per source.
///
/// Deterministic; no allocation after prepare().

#include <algorithm>
#include <cassert>
#include <cmath>
#include <complex>
#include <cstring>
#include <vector>

namespace pulp::signal {

template <typename SampleType = float>
class TransientPhasePolicyT {
public:
    struct Config {
        int fft_size = 2048;
        /// > 1 fires more readily (divides the median gate).
        float sensitivity = 1.0f;
        /// Phase-reset amount handed back on detection (0..1).
        float strength = 1.0f;
        /// Refractory period: after a reset fires, suppress further resets for
        /// this many analysis frames. A transient is ONE event — without this
        /// the detector re-fires on every frame of a hit's decay/ring (broadband
        /// flux stays above threshold), and each full-spectrum reset discards the
        /// vocoder's accumulated synthesis-phase lead. Sustained re-firing drives
        /// the PV toward raw overlap-add at the synthesis hop, which both pitches
        /// every partial DOWN by the stretch factor and breaks phase coherence
        /// (the "wobble"). 0 disables (legacy behavior). At hop≈128 (offline
        /// drums) 3 frames ≈ 8 ms — below any real inter-onset gap.
        int refractory_frames = 3;
    };

    /// RT contract: prepare() allocates the previous-magnitude buffer and is
    /// not audio-thread safe. After prepare(), analyze() and reset() are
    /// allocation-free for the prepared FFT size.
    void prepare(const Config& config) {
        assert(config.fft_size >= 256 && (config.fft_size & (config.fft_size - 1)) == 0);
        config_ = config;
        num_bins_ = config.fft_size / 2 + 1;
        prev_mag_.assign(static_cast<size_t>(num_bins_), SampleType{0});
        reset();
    }

    void reset() {
        std::fill(prev_mag_.begin(), prev_mag_.end(), SampleType{0});
        std::memset(flux_history_, 0, sizeof(flux_history_));
        history_count_ = 0;
        history_pos_ = 0;
        refractory_remaining_ = 0;
    }

    /// Analyze one frame group (all channels of the same time index) and
    /// return the phase-reset confidence: `strength` when a transient is
    /// detected, 0 otherwise.
    SampleType analyze(const std::complex<SampleType>* const* frames, int channels, int num_bins) {
        assert(num_bins == num_bins_);
        double flux = 0.0;
        double total = 0.0;
        for (int k = 0; k < num_bins_; ++k) {
            std::complex<SampleType> sum(SampleType{0}, SampleType{0});
            for (int ch = 0; ch < channels; ++ch) sum += frames[ch][k];
            const SampleType mag = std::abs(sum);
            const SampleType d = mag - prev_mag_[static_cast<size_t>(k)];
            if (d > SampleType{0}) flux += static_cast<double>(d);
            total += static_cast<double>(mag);
            prev_mag_[static_cast<size_t>(k)] = mag;
        }

        flux_history_[history_pos_] = static_cast<SampleType>(flux);
        history_pos_ = (history_pos_ + 1) % kHistory;
        if (history_count_ < kHistory) ++history_count_;

        if (history_count_ < 9 || total < 1e-9)
            return SampleType{0};

        SampleType sorted[kHistory];
        std::copy(flux_history_, flux_history_ + history_count_, sorted);
        std::nth_element(sorted, sorted + history_count_ / 2, sorted + history_count_);
        const double median = static_cast<double>(sorted[history_count_ / 2]);

        // Median-relative AND a significant fraction of frame energy.
        const bool transient =
            flux * static_cast<double>(config_.sensitivity) > 3.0 * median
            && flux > 0.05 * total;

        // Refractory gate: count every analyzed frame down, and only let a fresh
        // transient fire once the guard has elapsed. This collapses a hit's
        // multi-frame decay into a SINGLE reset at its onset instead of one reset
        // per ringing frame — the over-firing that bled the synthesis-phase lead.
        if (refractory_remaining_ > 0) --refractory_remaining_;
        if (transient && refractory_remaining_ == 0) {
            refractory_remaining_ = config_.refractory_frames;
            return static_cast<SampleType>(config_.strength);
        }
        return SampleType{0};
    }

private:
    static constexpr int kHistory = 32;

    Config config_;
    int num_bins_ = 0;
    std::vector<SampleType> prev_mag_;
    SampleType flux_history_[kHistory] = {};
    int history_count_ = 0;
    int history_pos_ = 0;
    int refractory_remaining_ = 0;  // frames left in the post-reset suppression window
};

using TransientPhasePolicy = TransientPhasePolicyT<float>;
using TransientPhasePolicy64 = TransientPhasePolicyT<double>;

} // namespace pulp::signal
