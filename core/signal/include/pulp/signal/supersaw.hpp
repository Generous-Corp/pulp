#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <pulp/signal/oscillator.hpp>
#include <pulp/signal/unison.hpp>

namespace pulp::signal {

template <typename SampleType> struct StereoSampleT {
    SampleType left{};
    SampleType right{};
};

template <typename SampleType = float, std::size_t MaximumVoices = 16>
class SupersawT {
public:
    static_assert(std::is_floating_point_v<SampleType>);

    /// Control-side preparation. The sample rate is in hertz; invalid input
    /// leaves the previous configuration and render state unchanged.
    bool prepare(SampleType sample_rate, const UnisonSpec& spec) noexcept {
        if (!std::isfinite(sample_rate) || sample_rate <= SampleType{0}) return false;
        UnisonLayout<MaximumVoices> check;
        if (!check.configure(spec)) return false;
        sample_rate_ = sample_rate;
        spec_ = spec;
        prepared_ = true;
        active_ = false;
        return true;
    }

    /// Audio-thread-safe note start. Frequency is in hertz and must remain
    /// below Nyquist after the configured maximum detune and drift. Failure is
    /// atomic: the currently sounding note is unchanged.
    bool trigger(SampleType frequency, std::uint64_t seed,
                 std::uint64_t note_instance_id) noexcept {
        if (!prepared_ || !std::isfinite(frequency) || frequency <= SampleType{0})
            return false;
        UnisonLayout<MaximumVoices> next_layout;
        if (!next_layout.configure(spec_, seed, note_instance_id)) return false;
        const double worst_cents = spec_.detune_cents + spec_.drift_cents;
        const double maximum_frequency = static_cast<double>(frequency) *
                                         std::exp2(worst_cents / 1200.0);
        if (!std::isfinite(maximum_frequency) || maximum_frequency >= sample_rate_ * 0.5)
            return false;
        auto next_oscillators = oscillators_;
        for (std::size_t i = 0; i < next_layout.size(); ++i) {
            next_oscillators[i].set_sample_rate(sample_rate_);
            next_oscillators[i].set_waveform(OscillatorT<SampleType>::Waveform::saw);
            if (!next_oscillators[i].reset_phase(
                    static_cast<SampleType>(next_layout[i].phase))) return false;
        }
        layout_ = next_layout;
        oscillators_ = next_oscillators;
        base_frequency_ = frequency;
        active_ = true;
        return true;
    }

    /// Render one stereo sample for the supplied absolute audio-frame index.
    /// Calls are allocation-free. Consecutive frame indices produce output
    /// independent of how a host partitions those calls into callbacks.
    StereoSampleT<SampleType> next(std::uint64_t absolute_frame) noexcept {
        StereoSampleT<SampleType> output{};
        if (!active_) return output;
        constexpr double half_pi = 1.57079632679489661923;
        for (std::size_t i = 0; i < layout_.size(); ++i) {
            const auto& voice = layout_[i];
            const double cents = voice.detune_cents + layout_.drift_cents(i, absolute_frame);
            oscillators_[i].set_frequency(static_cast<SampleType>(
                static_cast<double>(base_frequency_) * std::exp2(cents / 1200.0)));
            const double sample = oscillators_[i].next() * voice.gain;
            const double angle = (voice.pan + 1.0) * 0.5 * half_pi;
            output.left += static_cast<SampleType>(sample * std::cos(angle));
            output.right += static_cast<SampleType>(sample * std::sin(angle));
        }
        return output;
    }

    /// Audio-thread-safe hard reset. Subsequent samples are silent until the
    /// next successful trigger.
    void reset() noexcept {
        for (auto& oscillator : oscillators_) oscillator.reset();
        active_ = false;
    }

    bool prepared() const noexcept { return prepared_; }
    bool active() const noexcept { return active_; }
    const UnisonLayout<MaximumVoices>& layout() const noexcept { return layout_; }

private:
    std::array<OscillatorT<SampleType>, MaximumVoices> oscillators_{};
    UnisonLayout<MaximumVoices> layout_{};
    UnisonSpec spec_{};
    SampleType sample_rate_ = SampleType{0};
    SampleType base_frequency_ = SampleType{0};
    bool prepared_ = false;
    bool active_ = false;
};

using Supersaw = SupersawT<float>;
using Supersaw64 = SupersawT<double>;

} // namespace pulp::signal
