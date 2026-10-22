#pragma once

/// @file ladder_bank.hpp
/// A fixed number of independent ladder-filter voices processed in lockstep.

#include <pulp/signal/denormal.hpp>
#include <pulp/signal/ladder_filter.hpp>

#include <algorithm>
#include <cstddef>
#include <iterator>

namespace pulp::signal {

/// `Voices` independent LadderFilterT voices advanced together.
///
/// Contract:
///   - Each voice's output is bit-identical to a LadderFilterT<SampleType>
///     with the same sample rate, frequency and resonance, driven with the
///     same inputs through LadderFilterT::process() (the default saturator,
///     LadderSaturation / kDefaultLadderSaturation), from the same reset
///     state. Setters have LadderFilterT's semantics, including that
///     set_sample_rate() takes effect at the next set_frequency() or
///     set_resonance().
///   - Voices share nothing but storage: changing or resetting one voice
///     never affects another.
///   - process() advances every voice by one sample; process_block() advances
///     every voice by a block, one buffer per voice.
///   - The per-stage coefficient and the saturator are the ones LadderFilterT
///     exposes (stage_coefficient(), saturate()), so a widget or test can
///     reproduce a voice without running the bank.
///
/// Why it is faster: a ladder sample is a chain of five dependent
/// saturations, so one voice at a time is latency-bound. The bank computes
/// stage 1 of every voice, then stage 2 of every voice, and so on, with the
/// voices' state in structure-of-arrays form; each stage is then a loop over
/// independent voices that the compiler vectorizes and that keeps many
/// recursions in flight.
///
/// RT contract: storage is fixed-size; every member function allocates no
/// memory.
template <typename SampleType = float, std::size_t Voices = 16>
class LadderBankT {
    static_assert(Voices > 0, "LadderBankT needs at least one voice");

  public:
    using Filter = LadderFilterT<SampleType>;
    static constexpr std::size_t voice_count = Voices;

    LadderBankT() {
        std::fill(std::begin(sample_rate_), std::end(sample_rate_), SampleType{44100.0f});
        std::fill(std::begin(cutoff_), std::end(cutoff_), SampleType{1000.0f});
    }

    /// Set every voice's sample rate (applied by the next frequency or
    /// resonance change, as in LadderFilterT).
    void set_sample_rate(SampleType sample_rate) {
        std::fill(std::begin(sample_rate_), std::end(sample_rate_), sample_rate);
    }

    void set_frequency(std::size_t voice, SampleType hz) {
        if (voice >= Voices) return;
        cutoff_[voice] = hz;
        update(voice);
    }

    void set_resonance(std::size_t voice, SampleType resonance) {
        if (voice >= Voices) return;
        resonance_[voice] = std::clamp(resonance, SampleType{0.0f}, SampleType{1.0f});
        update(voice);
    }

    SampleType frequency(std::size_t voice) const { return voice < Voices ? cutoff_[voice] : SampleType{}; }
    SampleType resonance(std::size_t voice) const { return voice < Voices ? resonance_[voice] : SampleType{}; }

    /// The voice's per-stage one-pole coefficient (Filter::stage_coefficient()).
    SampleType stage_coefficient(std::size_t voice) const { return voice < Voices ? g_[voice] : SampleType{}; }

    /// Advance every voice by one sample: out[v] = voice v's output for in[v].
    /// `in` and `out` may be the same array.
    void process(const SampleType* in, SampleType* out) {
        SampleType prev[Voices];
        for (std::size_t v = 0; v < Voices; ++v) {
            const SampleType input = in[v];
            const SampleType feedback =
                resonance_[v] * SampleType{4.0f} * (stage_[3][v] - input * SampleType{0.5f});
            prev[v] = saturate(input - feedback);
        }
        for (int i = 0; i < 4; ++i) {
            SampleType* stage = stage_[i];
            SampleType* saturated = saturated_stage_[i];
            for (std::size_t v = 0; v < Voices; ++v) {
                stage[v] = snap_to_zero(stage[v] + g_[v] * (prev[v] - saturated[v]));
                saturated[v] = saturate(stage[v]);
                prev[v] = saturated[v];
            }
        }
        for (std::size_t v = 0; v < Voices; ++v)
            out[v] = stage_[3][v];
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

    /// Clear one voice's state.
    void reset(std::size_t voice) {
        if (voice >= Voices) return;
        for (int i = 0; i < 4; ++i) {
            stage_[i][voice] = SampleType{0};
            saturated_stage_[i][voice] = SampleType{0};
        }
    }

    /// Clear every voice's state.
    void reset() {
        for (std::size_t v = 0; v < Voices; ++v)
            reset(v);
    }

  private:
    static SampleType saturate(SampleType x) {
        return Filter::template saturate<kDefaultLadderSaturation>(x);
    }

    void update(std::size_t voice) {
        g_[voice] = Filter::stage_coefficient(cutoff_[voice], sample_rate_[voice]);
    }

    SampleType sample_rate_[Voices];
    SampleType cutoff_[Voices];
    SampleType resonance_[Voices] = {};
    SampleType g_[Voices] = {};
    SampleType stage_[4][Voices] = {};
    SampleType saturated_stage_[4][Voices] = {};
};

template <std::size_t Voices = 16>
using LadderBank = LadderBankT<float, Voices>;

} // namespace pulp::signal
