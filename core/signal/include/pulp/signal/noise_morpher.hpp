#pragma once

/// @file noise_morpher.hpp
/// Time-stretch the noise component of a sound by morphing, not by
/// phase-vocoder propagation.
///
/// A phase vocoder cannot stretch noise correctly: moving analysis frames
/// distorts the temporal correlation of the STFT coefficients, and phase
/// propagation cannot restore a noise signal's statistics, so stretched
/// noise sounds "tonalized" / phasey (Liao & Röbel, "On Stretching
/// Gaussian Noises with the Phase Vocoder," DAFx 2012). The fix is to
/// regenerate the noise from a fresh white-noise excitation shaped by the
/// time-interpolated noise magnitude envelope, so every synthesis frame is
/// decorrelated from its neighbours while preserving the spectral shape
/// (Moliner, Fierro, Wright, Hämäläinen & Välimäki, "Noise Morphing for
/// Audio Time Stretching," 2023).
///
/// This primitive produces, per synthesis frame, a complex spectrum whose
/// magnitude is the (interpolated) noise envelope and whose phase is fresh
/// random — a magnitude-locked white-noise excitation. Fed through
/// SpectralFrameEngine's overlap-add synthesis, successive random-phase
/// frames sum to decorrelated noise of the right color, stretching
/// transparently at any ratio. Deterministic (seeded xorshift); no
/// allocation after prepare().

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <complex>
#include <vector>

namespace pulp::signal {

/// RT contract: `prepare()` and `prepare_masked_scratch()` allocate storage and
/// must run off the audio thread. After preparation, `reset()`,
/// `push_envelope()`, `advance()`, `synthesize()`, and
/// `push_masked_envelope()` allocate no memory for the prepared bin count.
template <typename SampleType = float>
class NoiseMorpherT {
public:
    void prepare(int num_bins, std::uint64_t seed = 0x9e3779b97f4a7c15ull) {
        num_bins_ = num_bins;
        seed_ = seed ? seed : 0x9e3779b97f4a7c15ull;
        rng_ = seed_;
        env_a_.assign(static_cast<size_t>(num_bins), SampleType{0});
        env_b_.assign(static_cast<size_t>(num_bins), SampleType{0});
        have_prev_ = false;
    }

    int num_bins() const { return num_bins_; }

    void reset() {
        rng_ = seed_;
        have_prev_ = false;
    }

    /// Set the noise magnitude envelope for the current analysis frame.
    /// Successive calls advance the interpolation window: the previous
    /// envelope becomes the "from" endpoint and this one the "to".
    void push_envelope(const SampleType* noise_mag) {
        std::copy(noise_mag, noise_mag + num_bins_, env_b_.begin());
        if (!have_prev_) {
            std::copy(env_b_.begin(), env_b_.end(), env_a_.begin());
            have_prev_ = true;
        }
    }

    /// Commit the current "to" envelope as the next "from" endpoint.
    /// Call once per analysis frame after emitting that frame's synthesis
    /// output (so interpolation walks from frame to frame).
    void advance() { std::swap(env_a_, env_b_); }

    /// Emit one synthesis frame at interpolation position `frac` in [0,1]
    /// between the previous and current envelopes (frac=0 → previous,
    /// frac=1 → current). `out` holds num_bins complex bins (DC..Nyquist)
    /// with magnitude = interpolated envelope and fresh random phase.
    /// Compensate the random-phase WOLA energy loss. Random-phase synthesis
    /// frames overlap-add INCOHERENTLY (powers add) while the engine's WOLA
    /// normalizes for COHERENT summation (amplitudes add), so the rendered
    /// noise comes out several dB too quiet — and more so at denser overlap
    /// (time compression). Callers that know the synthesis hop set this so the
    /// morphed noise hits the target envelope level. Default 1 = uncompensated.
    void set_synthesis_gain(SampleType g) {
        synth_gain_ = g > SampleType{0} ? g : SampleType{1};
    }

    void synthesize(SampleType frac, std::complex<SampleType>* out) {
        const SampleType f = frac < SampleType{0}
            ? SampleType{0}
            : (frac > SampleType{1} ? SampleType{1} : frac);
        for (int k = 0; k < num_bins_; ++k) {
            const SampleType mag = (env_a_[static_cast<size_t>(k)] * (SampleType{1} - f)
                            + env_b_[static_cast<size_t>(k)] * f) * synth_gain_;
            const SampleType phase = next_phase();
            out[k] = std::polar(mag, phase);
        }
    }

    /// Convenience: pull the noise component out of a full magnitude frame
    /// using an STN noise mask, then push it as the envelope.
    void push_masked_envelope(const SampleType* full_mag, const SampleType* noise_mask) {
        for (int k = 0; k < num_bins_; ++k)
            scratch_[static_cast<size_t>(k)] = full_mag[k] * noise_mask[k];
        push_envelope(scratch_.data());
    }

    void prepare_masked_scratch() {
        scratch_.assign(static_cast<size_t>(num_bins_), SampleType{0});
    }

private:
    SampleType next_phase() {
        rng_ ^= rng_ >> 12;
        rng_ ^= rng_ << 25;
        rng_ ^= rng_ >> 27;
        const std::uint64_t r = rng_ * 0x2545f4914f6cdd1dull;
        // Uniform phase in [-pi, pi).
        const double u = static_cast<double>(r >> 11) * (1.0 / 9007199254740992.0);
        return static_cast<SampleType>((u * 2.0 - 1.0) * 3.14159265358979323846);
    }

    int num_bins_ = 0;
    SampleType synth_gain_ = SampleType{1};
    std::uint64_t seed_ = 0;
    std::uint64_t rng_ = 0;
    bool have_prev_ = false;
    std::vector<SampleType> env_a_;
    std::vector<SampleType> env_b_;
    std::vector<SampleType> scratch_;
};

using NoiseMorpher = NoiseMorpherT<float>;
using NoiseMorpher64 = NoiseMorpherT<double>;

} // namespace pulp::signal
