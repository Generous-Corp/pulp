#pragma once

/// @file rise_fall_generator.hpp
/// Two-segment modulation source built on the breakpoint envelope engine.
///
/// Rise and fall times are milliseconds. Levels are finite values in the
/// caller's real unit and are not normalized. One-shot mode holds the low level
/// after the fall. Looping mode is continuous because its first and final
/// breakpoints share the same low level.

#include <pulp/signal/breakpoint_envelope.hpp>

#include <array>
#include <cmath>
#include <cstdint>

namespace pulp::signal {

enum class RiseFallStage : std::uint8_t { idle, rising, falling };

template <typename SampleType = float> class RiseFallGeneratorT {
  public:
    using Engine = BreakpointEnvelopeT<SampleType, 3>;

    RiseFallGeneratorT() noexcept {
        rebuild_();
    }

    [[nodiscard]] BreakpointEnvelopeStatus prepare(double sample_rate) noexcept {
        return engine_.prepare(sample_rate);
    }

    [[nodiscard]] bool set_levels(SampleType low, SampleType high) noexcept {
        if (!std::isfinite(static_cast<double>(low)) || !std::isfinite(static_cast<double>(high)))
            return false;
        low_ = low;
        high_ = high;
        rebuild_();
        return true;
    }

    [[nodiscard]] bool set_times_ms(double rise_ms, double fall_ms) noexcept {
        if (!valid_time_(rise_ms) || !valid_time_(fall_ms) ||
            rise_ms + fall_ms > Engine::kMaxProgramTimeMs)
            return false;
        rise_ms_ = rise_ms;
        fall_ms_ = fall_ms;
        rebuild_();
        return true;
    }

    void set_rise_curve(ModulationCurve curve) noexcept {
        rise_curve_ = sanitize_modulation_curve(curve);
        rebuild_();
    }

    void set_fall_curve(ModulationCurve curve) noexcept {
        fall_curve_ = sanitize_modulation_curve(curve);
        rebuild_();
    }

    void set_looping(bool looping) noexcept {
        looping_ = looping;
        apply_loop_();
    }

    void trigger() noexcept {
        engine_.trigger();
    }
    void reset() noexcept {
        engine_.reset();
    }
    SampleType next() noexcept {
        return engine_.next();
    }
    void process(std::span<SampleType> output) noexcept {
        engine_.process(output);
    }

    SampleType current() const noexcept {
        return engine_.current();
    }
    bool active() const noexcept {
        return engine_.active();
    }
    bool looping() const noexcept {
        return looping_;
    }
    double rise_ms() const noexcept {
        return rise_ms_;
    }
    double fall_ms() const noexcept {
        return fall_ms_;
    }
    SampleType low() const noexcept {
        return low_;
    }
    SampleType high() const noexcept {
        return high_;
    }

    RiseFallStage stage() const noexcept {
        if (!engine_.active())
            return RiseFallStage::idle;
        return engine_.current_segment() == 0 ? RiseFallStage::rising : RiseFallStage::falling;
    }

  private:
    static bool valid_time_(double time_ms) noexcept {
        return std::isfinite(time_ms) && time_ms >= 0.0 && time_ms <= Engine::kMaxProgramTimeMs;
    }

    void rebuild_() noexcept {
        const std::array<typename Engine::Point, 3> points{{
            {0.0, low_, rise_curve_},
            {rise_ms_, high_, fall_curve_},
            {rise_ms_ + fall_ms_, low_, {}},
        }};
        (void)engine_.configure(points);
        apply_loop_();
    }

    void apply_loop_() noexcept {
        if (looping_)
            (void)engine_.set_loop(0, 2, Engine::kLoopForever);
        else
            engine_.clear_loop();
    }

    Engine engine_{};
    double rise_ms_ = 10.0;
    double fall_ms_ = 100.0;
    SampleType low_ = SampleType{0};
    SampleType high_ = SampleType{1};
    ModulationCurve rise_curve_{};
    ModulationCurve fall_curve_{};
    bool looping_ = false;
};

using RiseFallGenerator = RiseFallGeneratorT<float>;
using RiseFallGenerator64 = RiseFallGeneratorT<double>;

} // namespace pulp::signal
