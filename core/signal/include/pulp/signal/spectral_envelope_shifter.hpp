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
/// `warp` must be finite and positive; invalid values fail closed without
/// changing the frame. `warp == 1` is an exact bypass. No allocation after
/// `prepare()`.

#include <algorithm>
#include <cassert>
#include <cmath>
#include <complex>
#include <pulp/signal/source_filter_analysis.hpp>
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
    /// Finite, nonnegative gain clamp for the applied correction, in dB
    /// (safety bound for degenerate envelopes on sparse spectra).
    float max_gain_db = 60.0f;
};

template <typename SampleType = float> class SpectralEnvelopeShifterT {
  public:
    SpectralEnvelopeShifterT() = default;

    static bool checked_retained_bytes(int fft_size, std::uint64_t target_max_bytes,
                                       std::uint64_t& bytes) noexcept {
        const auto bins = static_cast<std::uint64_t>(fft_size / 2 + 1);
        CepstralEnvelopeConfigT<SampleType> analysis_config;
        analysis_config.fft_size = fft_size;
        analysis_config.order = fft_size / 16;
        std::uint64_t analysis_bytes = 0;
        CheckedRetainedByteCharge charge(target_max_bytes);
        if (!charge.add<SampleType>(bins) || !charge.add<SampleType>(bins) ||
            !CepstralEnvelopeAnalyzerT<SampleType>::checked_retained_bytes(
                analysis_config, target_max_bytes, analysis_bytes) ||
            !charge.add_retained_bytes(analysis_bytes))
            return false;
        bytes = charge.total();
        return true;
    }

    /// RT contract: prepare() allocates FFT and scratch/envelope storage and is
    /// not audio-thread safe. After prepare(), num_bins(), order(), and
    /// process_group() are allocation-free for the prepared FFT size; the frame
    /// pointers must reference exactly num_bins() bins.
    [[nodiscard]] SourceFilterAnalysisStatus prepare(const SpectralEnvelopeShifterConfig& config) {
        auto effective_config = config;
        if (!std::isfinite(effective_config.max_gain_db) || effective_config.max_gain_db < 0.0f)
            return SourceFilterAnalysisStatus::InvalidGain;
        if (effective_config.order <= 0)
            effective_config.order = effective_config.fft_size / 16;
        CepstralEnvelopeConfigT<SampleType> analysis_config;
        analysis_config.fft_size = effective_config.fft_size;
        analysis_config.order = effective_config.order;
        analysis_config.true_envelope_iterations = effective_config.true_envelope_iterations;
        analysis_config.convergence_tolerance = SampleType{0};
        CepstralEnvelopeAnalyzerT<SampleType> next_analyzer;
        const auto analysis_status = next_analyzer.prepare(analysis_config);
        if (analysis_status != SourceFilterAnalysisStatus::Ok)
            return analysis_status;

        const auto commit = [&]() {
            const auto next_num_bins = effective_config.fft_size / 2 + 1;
            std::vector<SampleType> next_log_mag(static_cast<size_t>(next_num_bins), SampleType{0});
            std::vector<SampleType> next_envelope(static_cast<size_t>(next_num_bins),
                                                  SampleType{0});
            config_ = effective_config;
            num_bins_ = next_num_bins;
            envelope_analyzer_ = std::move(next_analyzer);
            log_mag_ = std::move(next_log_mag);
            envelope_ = std::move(next_envelope);
            max_gain_ln_ = static_cast<SampleType>(effective_config.max_gain_db * 0.1151293f);
            return SourceFilterAnalysisStatus::Ok;
        };
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
        try {
            return commit();
        } catch (const std::bad_alloc&) {
            return SourceFilterAnalysisStatus::AllocationFailure;
        } catch (const std::length_error&) {
            return SourceFilterAnalysisStatus::AllocationFailure;
        }
#else
        return commit();
#endif
    }

    int num_bins() const {
        return num_bins_;
    }
    int order() const {
        return config_.order;
    }

    /// Estimate the group envelope and rescale all channels' bins by
    /// E(k * warp) / E(k). `frames` holds `channels` pointers to
    /// `num_bins()` bins (DC..Nyquist) of the same time index.
    void process_group(std::complex<SampleType>* const* frames, int channels, int num_bins,
                       SampleType warp) {
        assert(num_bins == num_bins_);
        assert(channels >= 1);
        if (!std::isfinite(warp) || warp <= SampleType{0})
            return;
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
        const SampleType floor_power =
            std::max(max_power * static_cast<SampleType>(1e-4), static_cast<SampleType>(1e-24));
        for (int k = 0; k < num_bins_; ++k)
            log_mag_[static_cast<size_t>(k)] =
                static_cast<SampleType>(0.5) *
                std::log(std::max(log_mag_[static_cast<size_t>(k)], floor_power));

        const auto envelope_result = envelope_analyzer_.estimate(log_mag_, envelope_);
        assert(envelope_result.ok());
        if (!envelope_result.ok())
            return;

        // Apply E(k*warp) / E(k) in the log domain with linear
        // interpolation, clamped at the spectrum edges and by max gain.
        double energy_after = 0.0;
        for (int k = 0; k < num_bins_; ++k) {
            const SampleType pos =
                std::min(static_cast<SampleType>(k) * warp, static_cast<SampleType>(num_bins_ - 1));
            const int i0 = static_cast<int>(pos);
            const int i1 = std::min(i0 + 1, num_bins_ - 1);
            const SampleType frac = pos - static_cast<SampleType>(i0);
            const SampleType warped = envelope_[static_cast<size_t>(i0)] * (SampleType{1} - frac) +
                                      envelope_[static_cast<size_t>(i1)] * frac;
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
            norm = std::clamp(norm, static_cast<SampleType>(1.0 / 256.0),
                              static_cast<SampleType>(256.0));
            if (std::isfinite(static_cast<double>(norm))) {
                for (int k = 0; k < num_bins_; ++k)
                    for (int ch = 0; ch < channels; ++ch)
                        frames[ch][k] *= norm;
            }
        }
    }

  private:
    SpectralEnvelopeShifterConfig config_;
    CepstralEnvelopeAnalyzerT<SampleType> envelope_analyzer_;
    int num_bins_ = 0;
    SampleType max_gain_ln_ = static_cast<SampleType>(6.9);

    std::vector<SampleType> log_mag_;
    std::vector<SampleType> envelope_;
};

using SpectralEnvelopeShifter = SpectralEnvelopeShifterT<float>;
using SpectralEnvelopeShifter64 = SpectralEnvelopeShifterT<double>;

} // namespace pulp::signal
