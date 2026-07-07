#pragma once

/// @file spectral_envelope_shifter.hpp
/// Spectral-envelope estimation and formant warping for phase-vocoder
/// frame groups.
///
/// Estimates the spectral envelope of a frame group by cepstral
/// smoothing (homomorphic liftering per Oppenheim & Schafer,
/// *Discrete-Time Signal Processing*) optionally refined with
/// true-envelope iterations (Röbel & Rodet, "Efficient Spectral Envelope
/// Estimation and its Application to Pitch Shifting and Envelope
/// Preservation," DAFx 2005), then rescales every channel's bins by
/// `E(k * warp) / E(k)`.
///
/// `warp` semantics: features of the envelope at frequency f move to
/// f / warp. A resample-based pitch shifter that scales the spectrum by
/// `pitch_ratio` passes `warp = pitch_ratio` to preserve formants, and
/// `warp = pitch_ratio / formant_ratio` to additionally shift them; a
/// pure formant shift uses `warp = 1 / formant_ratio`.
///
/// The envelope is estimated once per frame from the channel group's
/// RMS magnitude and the same gain is applied to every channel, so
/// inter-channel relationships (and identical channels) are preserved.
///
/// `warp == 1` is an exact bypass. No allocation after `prepare()`.

#include <pulp/signal/fft.hpp>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <complex>
#include <vector>

namespace pulp::signal {

struct SpectralEnvelopeShifterConfig {
    int fft_size = 2048;
    /// Cepstral cutoff (quefrency bins kept). Smaller = smoother
    /// envelope. Defaulted from the frame geometry when <= 0.
    int order = 0;
    /// True-envelope refinement passes (0 = plain liftering). Each pass
    /// costs two FFTs per frame.
    int true_envelope_iterations = 3;
    /// Gain clamp for the applied correction, in dB (safety bound for
    /// degenerate envelopes on sparse spectra).
    float max_gain_db = 60.0f;
};

template <typename SampleType = float>
class SpectralEnvelopeShifterT {
public:
    SpectralEnvelopeShifterT() = default;

    /// RT contract: prepare() allocates FFT and scratch/envelope storage and is
    /// not audio-thread safe. After prepare(), num_bins(), order(), and
    /// process_group() are allocation-free for the prepared FFT size; the frame
    /// pointers must reference exactly num_bins() bins.
    void prepare(const SpectralEnvelopeShifterConfig& config) {
        assert(config.fft_size >= 256 && (config.fft_size & (config.fft_size - 1)) == 0);
        config_ = config;
        if (config_.order <= 0)
            config_.order = config_.fft_size / 16;
        num_bins_ = config_.fft_size / 2 + 1;
        fft_ = FftT<SampleType>(config_.fft_size);

        log_mag_.assign(static_cast<size_t>(num_bins_), SampleType{0});
        envelope_.assign(static_cast<size_t>(num_bins_), SampleType{0});
        smooth_in_.assign(static_cast<size_t>(num_bins_), SampleType{0});
        cepstrum_.assign(static_cast<size_t>(config_.fft_size),
                         std::complex<SampleType>(SampleType{0}, SampleType{0}));
        max_gain_ln_ = static_cast<SampleType>(config_.max_gain_db * 0.1151293f); // dB → ln
    }

    int num_bins() const { return num_bins_; }
    int order() const { return config_.order; }

    /// Estimate the group envelope and rescale all channels' bins by
    /// E(k * warp) / E(k). `frames` holds `channels` pointers to
    /// `num_bins()` bins (DC..Nyquist) of the same time index.
    void process_group(std::complex<SampleType>* const* frames, int channels,
                       int num_bins, SampleType warp) {
        assert(num_bins == num_bins_);
        assert(channels >= 1);
        if (warp == SampleType{1})
            return; // exact bypass — neutral by construction

        // Group RMS magnitude → log domain, floored 40 dB below the frame
        // peak: without the relative floor, the near-zero bins between
        // harmonics drag the cepstral envelope far below the harmonic
        // tops and the true-envelope refinement cannot recover the
        // contrast within a bounded iteration budget. Envelope detail
        // more than 40 dB under the frame peak has no audible effect on
        // the correction.
        SampleType max_power = SampleType{0};
        double energy_before = 0.0;
        for (int k = 0; k < num_bins_; ++k) {
            SampleType power = SampleType{0};
            for (int ch = 0; ch < channels; ++ch) {
                const SampleType re = frames[ch][k].real();
                const SampleType im = frames[ch][k].imag();
                power += re * re + im * im;
            }
            energy_before += static_cast<double>(power);
            log_mag_[static_cast<size_t>(k)] = power / static_cast<SampleType>(channels);
            max_power = std::max(max_power, log_mag_[static_cast<size_t>(k)]);
        }
        const SampleType floor_power = std::max(max_power * static_cast<SampleType>(1e-4),
                                                static_cast<SampleType>(1e-24));
        for (int k = 0; k < num_bins_; ++k)
            log_mag_[static_cast<size_t>(k)] =
                static_cast<SampleType>(0.5) * std::log(
                    std::max(log_mag_[static_cast<size_t>(k)], floor_power));

        // Plain cepstral lifter, then true-envelope refinement: re-smooth
        // the pointwise max of spectrum and current envelope so the
        // envelope rides harmonic peaks instead of averaging through them.
        cepstral_smooth(log_mag_.data(), envelope_.data());
        for (int it = 0; it < config_.true_envelope_iterations; ++it) {
            for (int k = 0; k < num_bins_; ++k)
                smooth_in_[static_cast<size_t>(k)] =
                    std::max(log_mag_[static_cast<size_t>(k)], envelope_[static_cast<size_t>(k)]);
            cepstral_smooth(smooth_in_.data(), envelope_.data());
        }

        // Apply E(k*warp) / E(k) in the log domain with linear
        // interpolation, clamped at the spectrum edges and by max gain.
        double energy_after = 0.0;
        for (int k = 0; k < num_bins_; ++k) {
            const SampleType pos = std::min(static_cast<SampleType>(k) * warp,
                                            static_cast<SampleType>(num_bins_ - 1));
            const int i0 = static_cast<int>(pos);
            const int i1 = std::min(i0 + 1, num_bins_ - 1);
            const SampleType frac = pos - static_cast<SampleType>(i0);
            const SampleType warped = envelope_[static_cast<size_t>(i0)] * (SampleType{1} - frac)
                               + envelope_[static_cast<size_t>(i1)] * frac;
            SampleType gain_ln = warped - envelope_[static_cast<size_t>(k)];
            gain_ln = std::clamp(gain_ln, -max_gain_ln_, max_gain_ln_);
            const SampleType gain = std::exp(gain_ln);
            for (int ch = 0; ch < channels; ++ch) {
                frames[ch][k] *= gain;
                energy_after += static_cast<double>(std::norm(frames[ch][k]));
            }
        }

        // Energy-preserving normalization. The correction RESHAPES the spectral
        // envelope (the whole point — moving formants) but must not change
        // overall loudness. Without this, a narrow-band input (e.g. a single
        // tone, whose cepstral envelope peaks AT the tone) is scaled by the
        // falling envelope sampled above the peak, so the level collapses
        // progressively as warp (= pitch ratio) grows — silent pitch-up with
        // formant preservation ON. Restoring the pre-correction energy keeps the
        // timbre change while holding loudness constant.
        //
        // SAFETY: only normalize frames with real energy, and CLAMP the gain.
        // On a near-silent frame (the gaps between instrument notes) the ratio
        // energy_before/energy_after is ill-conditioned and could explode to a
        // huge gain → Inf/NaN → which a host (Logic) treats as a dead channel
        // and mutes the whole signal path. A sane correction never needs more
        // than a few dB of overall trim, so clamp to +/-18 dB and leave silent
        // frames untouched.
        constexpr double kEnergyFloor = 1e-9; // below this the frame is silence
        if (energy_after > kEnergyFloor && energy_before > kEnergyFloor) {
            SampleType norm = static_cast<SampleType>(std::sqrt(energy_before / energy_after));
            // Generous bound: a narrow-band tone legitimately needs up to ~+30 dB
            // of correction (its cepstral envelope concentrates the energy), so
            // the guard rail must clear that with margin. It exists only to cap a
            // runaway toward Inf on a numerically-degenerate frame, NOT to limit
            // a real correction — a tight rail under-corrects and re-introduces
            // the quiet-pitch-up bug. +/-48 dB.
            norm = std::clamp(norm, static_cast<SampleType>(1.0 / 256.0), static_cast<SampleType>(256.0));
            if (std::isfinite(static_cast<double>(norm))) {
                for (int k = 0; k < num_bins_; ++k)
                    for (int ch = 0; ch < channels; ++ch)
                        frames[ch][k] *= norm;
            }
        }
    }

private:
    // log-spectrum (num_bins) → liftered log-envelope (num_bins) via the
    // real cepstrum: even-symmetric extension, inverse FFT, rectangular
    // lifter keeping quefrencies |q| <= order, forward FFT.
    void cepstral_smooth(const SampleType* log_spec, SampleType* env_out) {
        const int n = config_.fft_size;
        for (int k = 0; k < num_bins_; ++k)
            cepstrum_[static_cast<size_t>(k)] = {log_spec[k], SampleType{0}};
        for (int k = num_bins_; k < n; ++k)
            cepstrum_[static_cast<size_t>(k)] = {log_spec[n - k], SampleType{0}};
        fft_.inverse(cepstrum_.data());
        for (int q = config_.order + 1; q < n - config_.order; ++q)
            cepstrum_[static_cast<size_t>(q)] = {SampleType{0}, SampleType{0}};
        fft_.forward(cepstrum_.data());
        for (int k = 0; k < num_bins_; ++k)
            env_out[k] = cepstrum_[static_cast<size_t>(k)].real();
    }

    SpectralEnvelopeShifterConfig config_;
    FftT<SampleType> fft_{2048};
    int num_bins_ = 0;
    SampleType max_gain_ln_ = static_cast<SampleType>(6.9);

    std::vector<SampleType> log_mag_;
    std::vector<SampleType> envelope_;
    std::vector<SampleType> smooth_in_;
    std::vector<std::complex<SampleType>> cepstrum_;
};

using SpectralEnvelopeShifter = SpectralEnvelopeShifterT<float>;
using SpectralEnvelopeShifter64 = SpectralEnvelopeShifterT<double>;

} // namespace pulp::signal
