#pragma once

/// @file processing_switch_crossfade.hpp
/// A gap-free, click-free switch between two realisations of the same effect
/// whose LATENCIES differ — a linear-phase and a minimum-phase filter, a
/// high-quality and a low-latency mode, a CPU and a GPU path.
///
/// Swapping such realisations at a block boundary is audible twice over:
///
///   - the incoming realisation starts with no history, so for its own
///     latency it emits silence (a dropout — 213 ms for an 8192/2048 WOLA
///     engine at 48 kHz) and for the length of its impulse a partial response;
///   - the outgoing realisation stops mid-waveform, leaving a step.
///
/// The switch therefore runs in two phases, counted in samples on the audio
/// thread:
///
///   1. WARM — both realisations process the same input; only the outgoing
///      one is heard. The incoming one fills its delay line and its impulse
///      history, so when it is first heard its output is already steady.
///   2. FADE — a `TransitionMixer` EqualPower fade (smoothstep-shaped, so both
///      ends are corner-free) from the outgoing output to the incoming one.
///      The two carry the same material at different delays, which is
///      uncorrelated over a fade, so equal POWER keeps the level constant.
///
/// What no fade can remove: the content moves in time by the difference of
/// the two latencies, because that is what changing the latency means. The
/// host's delay compensation re-aligns it; the fade makes the move smooth.
///
/// The caller owns both realisations and runs them; this type owns only the
/// sample schedule and the blend. `plan_processing_switch()` and
/// `processing_switch_gains_at()` are pure, so a test or a UI can state what a
/// switch will do without running audio through it.

#include "transition_mixer.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace pulp::signal {

/// How long each phase of a switch lasts, in samples.
struct ProcessingSwitchPlan {
    /// Samples the incoming realisation processes unheard before the fade.
    std::int64_t warm_samples = 0;
    /// Samples the fade lasts. At least 1: a switch always ends on the
    /// incoming realisation.
    std::int64_t fade_samples = 1;

    [[nodiscard]] constexpr std::int64_t total_samples() const noexcept {
        return warm_samples + fade_samples;
    }
};

/// The plan for switching INTO a realisation with `incoming_latency` samples
/// of delay that needs `incoming_history` samples of input before its output
/// is steady (an FIR's length; zero for a realisation that primes its own
/// stream start). The fade is `fade_seconds` at `sample_rate`. Pure.
[[nodiscard]] inline ProcessingSwitchPlan plan_processing_switch(std::int64_t incoming_latency,
                                                                 std::int64_t incoming_history,
                                                                 double sample_rate,
                                                                 double fade_seconds) noexcept {
    ProcessingSwitchPlan plan;
    plan.warm_samples =
        std::max<std::int64_t>(0, incoming_latency) + std::max<std::int64_t>(0, incoming_history);
    const double rate = (std::isfinite(sample_rate) && sample_rate > 0.0) ? sample_rate : 48000.0;
    const double fade = (std::isfinite(fade_seconds) && fade_seconds > 0.0) ? fade_seconds : 0.0;
    plan.fade_samples = std::max<std::int64_t>(1, std::llround(fade * rate));
    return plan;
}

/// Gains for the outgoing and incoming outputs at absolute `position` samples
/// into a switch following `plan`. Pure: the whole schedule as a function.
template <typename SampleType = float>
inline void processing_switch_gains_at(const ProcessingSwitchPlan& plan, std::int64_t position,
                                       SampleType& outgoing, SampleType& incoming) noexcept {
    const std::int64_t into_fade = position - plan.warm_samples;
    if (into_fade < 0) {
        outgoing = SampleType{1};
        incoming = SampleType{0};
        return;
    }
    if (into_fade >= plan.fade_samples) {
        outgoing = SampleType{0};
        incoming = SampleType{1};
        return;
    }
    TransitionMixerT<SampleType> mixer;
    mixer.configure(static_cast<std::size_t>(plan.fade_samples), TransitionCurve::EqualPower);
    mixer.gains_at(static_cast<std::size_t>(into_fade), outgoing, incoming);
}

/// The audio-thread state of one switch. Allocation-free and lock-free.
///
/// While `active()`, the caller runs BOTH realisations on the same input — the
/// outgoing one into `output`, the incoming one into a buffer of its own — and
/// calls `mix()`, which leaves the heard result in `output`. Once `finished()`
/// the caller drops the outgoing realisation and runs the incoming one alone;
/// `mix()` has by then already been returning the incoming output exactly.
template <typename SampleType = float> class ProcessingSwitchCrossfadeT {
  public:
    void begin(const ProcessingSwitchPlan& plan) noexcept {
        plan_ = plan;
        if (plan_.fade_samples < 1)
            plan_.fade_samples = 1;
        mixer_.configure(static_cast<std::size_t>(plan_.fade_samples), TransitionCurve::EqualPower);
        position_ = 0;
        active_ = true;
    }
    void cancel() noexcept {
        active_ = false;
        position_ = 0;
    }

    [[nodiscard]] bool active() const noexcept {
        return active_;
    }
    [[nodiscard]] bool finished() const noexcept {
        return active_ && position_ >= plan_.total_samples();
    }
    [[nodiscard]] bool warming() const noexcept {
        return active_ && position_ < plan_.warm_samples;
    }
    [[nodiscard]] std::int64_t position() const noexcept {
        return position_;
    }
    [[nodiscard]] const ProcessingSwitchPlan& plan() const noexcept {
        return plan_;
    }

    /// Blend `num_samples` of the outgoing output (in `output`) with the
    /// incoming output, in place, and advance. While warming `output` is left
    /// untouched; past the fade it becomes the incoming output exactly.
    void mix(SampleType* const* output, const SampleType* const* incoming, int channels,
             int num_samples) noexcept {
        if (!active_ || num_samples <= 0)
            return;
        for (int i = 0; i < num_samples; ++i) {
            const std::int64_t into_fade = position_ - plan_.warm_samples;
            if (into_fade >= plan_.fade_samples) {
                for (int ch = 0; ch < channels; ++ch)
                    output[ch][i] = incoming[ch][i];
            } else if (into_fade >= 0) {
                SampleType g_out{}, g_in{};
                mixer_.gains_at(static_cast<std::size_t>(into_fade), g_out, g_in);
                for (int ch = 0; ch < channels; ++ch)
                    output[ch][i] = g_out * output[ch][i] + g_in * incoming[ch][i];
            }
            ++position_;
        }
    }

  private:
    ProcessingSwitchPlan plan_{};
    TransitionMixerT<SampleType> mixer_{};
    std::int64_t position_ = 0;
    bool active_ = false;
};

using ProcessingSwitchCrossfade = ProcessingSwitchCrossfadeT<float>;
using ProcessingSwitchCrossfade64 = ProcessingSwitchCrossfadeT<double>;

} // namespace pulp::signal
