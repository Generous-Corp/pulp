#pragma once

#include <cmath>
#include <cstdint>
#include <numbers>
#include <type_traits>

namespace pulp::signal {

/// Output shape for a parent-phase-locked sub-oscillator.
enum class SubOscillatorWaveform {
    square,
    sine,
};

/// A one- or two-octave divider derived from an explicit parent phase.
///
/// This class deliberately owns no free-running clock. The caller supplies the
/// normalized parent phase for every sample and reports each parent-cycle edge.
/// The sub phase is `(completed_parent_cycles + parent_phase) / 2^octave`, so a
/// parent reset or phase jump cannot leave a second oscillator drifting beside
/// it. Call `reset()` when the parent cycle count is reset, and call
/// `on_parent_cycle()` before rendering the first sample after each wrap.
///
/// Every operation is fixed-state, allocation-free, lock-free, and `noexcept`.
/// The output has normalized amplitude, zero latency, and no tail.
template <typename SampleType = float> class SubOscillatorT {
  public:
    static_assert(std::is_same_v<SampleType, float> || std::is_same_v<SampleType, double>);

    /// Select one or two octaves below the parent. Failure leaves the previous
    /// setting and phase relationship unchanged.
    [[nodiscard]] bool set_octave(int octave) noexcept {
        if (octave != 1 && octave != 2)
            return false;
        octave_ = octave;
        return true;
    }

    int octave() const noexcept {
        return octave_;
    }

    void set_waveform(SubOscillatorWaveform waveform) noexcept {
        waveform_ = waveform;
    }
    SubOscillatorWaveform waveform() const noexcept {
        return waveform_;
    }

    /// Re-anchor the divider to a known completed parent-cycle count.
    void reset(std::uint32_t completed_parent_cycles = 0) noexcept {
        cycle_index_ = completed_parent_cycles % max_divisor;
        phase_ = SampleType{0};
    }

    /// Report one completed parent cycle. Call before `next(0)` on a wrap.
    void on_parent_cycle() noexcept {
        cycle_index_ = (cycle_index_ + 1u) % max_divisor;
    }

    /// Render from the parent's current normalized phase in [0, 1).
    /// Non-finite or out-of-range input is absorbed as silence and re-anchors
    /// the divider, preventing invalid arithmetic from poisoning later output.
    SampleType next(SampleType parent_phase) noexcept {
        if (!std::isfinite(parent_phase) || parent_phase < SampleType{0} ||
            parent_phase >= SampleType{1}) {
            reset();
            return SampleType{0};
        }

        phase_ = (static_cast<SampleType>(cycle_index_ % divisor()) + parent_phase) /
                 static_cast<SampleType>(divisor());
        if (waveform_ == SubOscillatorWaveform::square)
            return phase_ < SampleType{0.5} ? SampleType{1} : SampleType{-1};
        return std::sin(SampleType{2} * std::numbers::pi_v<SampleType> * phase_);
    }

    /// Convenience form for a wrap and render in one call. The edge is applied
    /// first, matching the first sample of the new parent cycle.
    SampleType next(SampleType parent_phase, bool parent_cycle_edge) noexcept {
        if (parent_cycle_edge)
            on_parent_cycle();
        return next(parent_phase);
    }

    /// The phase used for the most recently rendered valid sample.
    SampleType phase() const noexcept {
        return phase_;
    }

  private:
    static constexpr std::uint32_t max_divisor = 4;

    std::uint32_t divisor() const noexcept {
        return std::uint32_t{1} << octave_;
    }

    std::uint32_t cycle_index_ = 0;
    int octave_ = 1;
    SampleType phase_ = SampleType{0};
    SubOscillatorWaveform waveform_ = SubOscillatorWaveform::square;
};

using SubOscillator = SubOscillatorT<float>;
using SubOscillator64 = SubOscillatorT<double>;

} // namespace pulp::signal
