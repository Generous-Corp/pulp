#pragma once

/// @file multichannel_phase_coordinator.hpp
/// Phase-vocoder phase propagation for coherent channel groups.
///
/// Computes ONE phase decision per bin per frame from an energy-weighted
/// reference (the channel sum) and applies the same phase *rotation* to
/// every channel. Per-channel phase differences — the stereo image, the
/// surround field — are preserved exactly by construction, and identical
/// channels remain bitwise identical.
///
/// Phase propagation follows Laroche & Dolson, "Improved Phase Vocoder
/// Time-Scale Modification of Audio" (IEEE TSAP 7(3), 1999): per-peak
/// heterodyned phase increments with identity phase locking of each
/// peak's region. A `reset` control supports transient handling
/// (Röbel, DAFx 2003): 1 snaps synthesis phases to analysis phases,
/// fractional values blend toward them.
///
/// Phase state is kept in double precision — neutrality at unity stretch
/// depends on the synthesis phase tracking the analysis phase exactly
/// over arbitrarily long streams.

#include <algorithm>
#include <cassert>
#include <cmath>
#include <complex>
#include <vector>

namespace pulp::signal {

/// RT contract: `prepare()` allocates internal phase/magnitude storage and must
/// run off the audio thread. After preparation, `reset()`, queries, and
/// `process_group()` allocate no memory for the prepared bin/channel counts.
template <typename SampleType = float>
class MultichannelPhaseCoordinatorT {
public:
    MultichannelPhaseCoordinatorT() = default;

    void prepare(int fft_size, int channels) {
        assert(fft_size >= 256 && (fft_size & (fft_size - 1)) == 0);
        assert(channels >= 1);
        fft_size_ = fft_size;
        channels_ = channels;
        num_bins_ = fft_size / 2 + 1;
        prev_phase_.assign(static_cast<size_t>(num_bins_), 0.0);
        synth_phase_.assign(static_cast<size_t>(num_bins_), 0.0);
        ref_mag_.assign(static_cast<size_t>(num_bins_), SampleType{0});
        ref_phase_.assign(static_cast<size_t>(num_bins_), 0.0);
        peaks_.assign(static_cast<size_t>(num_bins_), 0);
        reset();
    }

    void reset() {
        std::fill(prev_phase_.begin(), prev_phase_.end(), 0.0);
        std::fill(synth_phase_.begin(), synth_phase_.end(), 0.0);
        first_frame_ = true;
    }

    /// Sum of reference magnitudes from the last processed frame — cheap
    /// energy proxy callers (transient detectors) can reuse.
    const SampleType* reference_magnitudes() const { return ref_mag_.data(); }
    int num_bins() const { return num_bins_; }

    /// Propagate phases for one frame group and rotate every channel's
    /// bins in place. `analysis_hop` and `synthesis_hop` are the actual
    /// integer hops used for this frame; `reset` in [0, 1] blends the
    /// synthesis phases toward the analysis phases (1 = full transient
    /// reset). At unity hop ratio with reset 0 the rotation vanishes to
    /// double-precision rounding error, preserving transparency.
    void process_group(std::complex<SampleType>* const* frames, int num_bins,
                       int analysis_hop, int synthesis_hop, SampleType reset_amount = SampleType{0}) {
        assert(num_bins == num_bins_);
        assert(analysis_hop > 0 && synthesis_hop > 0);

        // Energy-weighted reference: the complex channel sum.
        for (int k = 0; k < num_bins_; ++k) {
            std::complex<SampleType> sum(SampleType{0}, SampleType{0});
            for (int ch = 0; ch < channels_; ++ch) sum += frames[ch][k];
            ref_mag_[static_cast<size_t>(k)] = std::abs(sum);
            ref_phase_[static_cast<size_t>(k)] = static_cast<double>(std::arg(sum));
        }

        if (first_frame_ || reset_amount >= SampleType{1}) {
            std::copy(ref_phase_.begin(), ref_phase_.end(), synth_phase_.begin());
            std::copy(ref_phase_.begin(), ref_phase_.end(), prev_phase_.begin());
            first_frame_ = false;
            return; // rotation is identically zero — nothing to apply
        }

        // Peak picking on the reference magnitude (local maximum over two
        // neighbors each side, per Laroche-Dolson region locking).
        num_peaks_ = 0;
        for (int k = 2; k <= num_bins_ - 3; ++k) {
            const SampleType m = ref_mag_[static_cast<size_t>(k)];
            if (m > ref_mag_[static_cast<size_t>(k - 1)] && m > ref_mag_[static_cast<size_t>(k - 2)]
                && m > ref_mag_[static_cast<size_t>(k + 1)] && m > ref_mag_[static_cast<size_t>(k + 2)])
                peaks_[static_cast<size_t>(num_peaks_++)] = k;
        }
        if (num_peaks_ == 0) peaks_[num_peaks_++] = 1;

        // Propagate each peak's phase: instantaneous frequency from the
        // heterodyned phase increment, advanced by the synthesis hop.
        const double ha = static_cast<double>(analysis_hop);
        const double hs = static_cast<double>(synthesis_hop);
        for (int i = 0; i < num_peaks_; ++i) {
            const int p = peaks_[static_cast<size_t>(i)];
            const double omega = two_pi_ * p / fft_size_;
            const double delta = princarg(ref_phase_[static_cast<size_t>(p)]
                                          - prev_phase_[static_cast<size_t>(p)] - omega * ha);
            const double inst = omega + delta / ha;
            synth_phase_[static_cast<size_t>(p)] += inst * hs;
        }

        // Identity phase locking: every bin inherits its nearest peak's
        // rotation, preserving the analysis phase relationships within
        // each peak region. Then apply the rotation to all channels.
        int pi_idx = 0;
        for (int k = 0; k < num_bins_; ++k) {
            while (pi_idx + 1 < num_peaks_
                   && std::abs(peaks_[static_cast<size_t>(pi_idx + 1)] - k)
                      < std::abs(peaks_[static_cast<size_t>(pi_idx)] - k))
                ++pi_idx;
            const int p = peaks_[static_cast<size_t>(pi_idx)];
            if (k != p)
                synth_phase_[static_cast<size_t>(k)] =
                    synth_phase_[static_cast<size_t>(p)]
                    + (ref_phase_[static_cast<size_t>(k)] - ref_phase_[static_cast<size_t>(p)]);

            // Work with the principal rotation: scaling a raw 2*pi-multiple
            // during a partial reset would manufacture a spurious rotation.
            double rotation = princarg(synth_phase_[static_cast<size_t>(k)]
                                       - ref_phase_[static_cast<size_t>(k)]);
            if (reset_amount > SampleType{0})
                rotation *= 1.0 - static_cast<double>(reset_amount);
            synth_phase_[static_cast<size_t>(k)] =
                ref_phase_[static_cast<size_t>(k)] + rotation;

            const SampleType rot = static_cast<SampleType>(rotation);
            if (rot != SampleType{0}) {
                const std::complex<SampleType> r(std::cos(rot), std::sin(rot));
                for (int ch = 0; ch < channels_; ++ch)
                    frames[ch][k] *= r;
            }
        }

        std::copy(ref_phase_.begin(), ref_phase_.end(), prev_phase_.begin());
    }

private:
    static double princarg(double p) {
        return p - two_pi_ * std::round(p / two_pi_);
    }

    static constexpr double two_pi_ = 6.28318530717958647692;

    int fft_size_ = 0;
    int channels_ = 0;
    int num_bins_ = 0;
    bool first_frame_ = true;

    std::vector<double> prev_phase_;
    std::vector<double> synth_phase_;
    std::vector<SampleType> ref_mag_;
    std::vector<double> ref_phase_;
    std::vector<int> peaks_;
    int num_peaks_ = 0;
};

using MultichannelPhaseCoordinator = MultichannelPhaseCoordinatorT<float>;
using MultichannelPhaseCoordinator64 = MultichannelPhaseCoordinatorT<double>;

} // namespace pulp::signal
