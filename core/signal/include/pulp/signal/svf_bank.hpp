#pragma once

/// @file svf_bank.hpp
/// A fixed number of independent state-variable-filter voices processed in
/// lockstep.

#include <pulp/signal/denormal.hpp>
#include <pulp/signal/fast_math.hpp>
#include <pulp/signal/svf.hpp>

#include <algorithm>
#include <cstddef>
#include <iterator>

namespace pulp::signal {

/// `Voices` independent SvfT voices advanced together.
///
/// Contract:
///   - Each voice's output is bit-identical to an SvfT<SampleType> with the
///     same mode, sample rate, frequency and resonance, driven with the same
///     inputs through SvfT::process(SampleType) from the same reset state,
///     including its per-sample denormal snap. Setters have SvfT's
///     semantics: coefficients come from SvfT::design() and are recomputed by
///     every rate, frequency or resonance change; a default-constructed voice
///     has all-zero coefficients until one of them is set, as SvfT does.
///   - Voices share nothing but storage, and each has its own mode.
///   - process() advances every voice by one sample; process_block() advances
///     every voice by a block, one buffer per voice.
///
/// Why it is faster: the voices' state is held in structure-of-arrays form
/// and every step is a loop over independent voices, which the compiler
/// vectorizes; the per-voice mode becomes a per-lane select instead of a
/// branch.
///
/// RT contract: storage is fixed-size; every member function allocates no
/// memory.
template <typename SampleType = float, std::size_t Voices = 16> class SvfBankT {
    static_assert(Voices > 0, "SvfBankT needs at least one voice");

  public:
    using Filter = SvfT<SampleType>;
    using Mode = typename Filter::Mode;
    static constexpr std::size_t voice_count = Voices;

    SvfBankT() {
        std::fill(std::begin(sample_rate_), std::end(sample_rate_), SampleType{44100.0f});
        std::fill(std::begin(frequency_), std::end(frequency_), SampleType{1000.0f});
        std::fill(std::begin(resonance_), std::end(resonance_), SampleType{0.707f});
    }

    /// Install a coherent mode, rate, cutoff and resonance for one voice
    /// (SvfT::configure()).
    void configure(std::size_t voice, Mode mode, SampleType sample_rate, SampleType frequency,
                   SampleType resonance) {
        if (voice >= Voices)
            return;
        mode_[voice] = mode_index(mode);
        sample_rate_[voice] = sample_rate;
        frequency_[voice] = frequency;
        resonance_[voice] = resonance;
        update(voice);
    }

    void set_sample_rate(std::size_t voice, SampleType sample_rate) {
        if (voice >= Voices)
            return;
        sample_rate_[voice] = sample_rate;
        update(voice);
    }
    void set_frequency(std::size_t voice, SampleType hz) {
        if (voice >= Voices)
            return;
        frequency_[voice] = hz;
        update(voice);
    }
    void set_resonance(std::size_t voice, SampleType q) {
        if (voice >= Voices)
            return;
        resonance_[voice] = q;
        update(voice);
    }
    void set_mode(std::size_t voice, Mode mode) {
        if (voice >= Voices)
            return;
        mode_[voice] = mode_index(mode);
    }

    /// The voice's coefficients (SvfT::design() for its current settings).
    typename Filter::Coefficients coefficients(std::size_t voice) const {
        if (voice >= Voices)
            return {};
        return {g_[voice], k_[voice], a1_[voice], a2_[voice], a3_[voice]};
    }

    /// Advance every voice by one sample: out[v] = voice v's output for in[v].
    /// `in` and `out` may be the same array.
    void process(const SampleType* in, SampleType* out) {
        for (std::size_t v = 0; v < Voices; ++v) {
            const SampleType input = in[v];
            const SampleType v3 = input - ic2_[v];
            const SampleType v1 = a1_[v] * ic1_[v] + a2_[v] * v3;
            const SampleType v2 = ic2_[v] + a2_[v] * ic1_[v] + a3_[v] * v3;
            ic1_[v] = detail::snap_to_zero_bits(SampleType{2.0f} * v1 - ic1_[v]);
            ic2_[v] = detail::snap_to_zero_bits(SampleType{2.0f} * v2 - ic2_[v]);
            const SampleType highpass = input - k_[v] * v1 - v2;
            const SampleType notch = input - k_[v] * v1;
            const int mode = mode_[v];
            // Bit-operation selects (same values as conditionals) keep this
            // loop free of control flow, so it vectorizes under GCC too.
            const SampleType low_or_high = detail::select_bits(mode == kHighpass, highpass, v2);
            const SampleType band_or_notch = detail::select_bits(mode == kNotch, notch, v1);
            const bool band_family = (mode == kBandpass) | (mode == kNotch);
            out[v] = detail::select_bits(band_family, band_or_notch, low_or_high);
        }
    }

    /// Advance every voice by `num_samples` samples in place, one buffer per
    /// voice (buffers[v] holds voice v's input and receives its output).
    void process_block(SampleType* const* buffers, int num_samples) {
        SampleType frame[Voices];
        for (int n = 0; n < num_samples; ++n) {
            for (std::size_t v = 0; v < Voices; ++v)
                frame[v] = buffers[v][n];
            process(frame, frame);
            for (std::size_t v = 0; v < Voices; ++v)
                buffers[v][n] = frame[v];
        }
    }

    void reset(std::size_t voice) {
        if (voice >= Voices)
            return;
        ic1_[voice] = SampleType{0};
        ic2_[voice] = SampleType{0};
    }

    void reset() {
        for (std::size_t v = 0; v < Voices; ++v)
            reset(v);
    }

  private:
    static constexpr int kLowpass = 0, kHighpass = 1, kBandpass = 2, kNotch = 3;

    static int mode_index(Mode mode) {
        switch (mode) {
        case Mode::lowpass:
            return kLowpass;
        case Mode::highpass:
            return kHighpass;
        case Mode::bandpass:
            return kBandpass;
        case Mode::notch:
            return kNotch;
        }
        return kLowpass;
    }

    void update(std::size_t voice) {
        const auto c = Filter::design(sample_rate_[voice], frequency_[voice], resonance_[voice]);
        g_[voice] = c.g;
        k_[voice] = c.k;
        a1_[voice] = c.a1;
        a2_[voice] = c.a2;
        a3_[voice] = c.a3;
    }

    SampleType sample_rate_[Voices];
    SampleType frequency_[Voices];
    SampleType resonance_[Voices];
    int mode_[Voices] = {};
    SampleType g_[Voices] = {};
    SampleType k_[Voices] = {};
    SampleType a1_[Voices] = {};
    SampleType a2_[Voices] = {};
    SampleType a3_[Voices] = {};
    SampleType ic1_[Voices] = {};
    SampleType ic2_[Voices] = {};
};

template <std::size_t Voices = 16> using SvfBank = SvfBankT<float, Voices>;

} // namespace pulp::signal
