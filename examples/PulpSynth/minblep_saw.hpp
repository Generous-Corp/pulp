#pragma once

// Opt-in PulpSynth voice primitive proving public minBLEP API consumption.

#include <pulp/signal/osc/minblep.hpp>
#include <pulp/signal/osc/phase.hpp>

#include <cstddef>

namespace pulp::examples {

class MinBlepSaw {
  public:
    void reset(double phase = 0.0) noexcept {
        phase_.reset(phase);
        correction_.reset();
        dropped_events_ = 0;
    }

    [[nodiscard]] double next(double increment) noexcept {
        const double output = shape(phase_.phase()) + correction_.next();
        phase_.advance(increment);
        for (const auto& event : phase_.events()) {
            const double height = shape_limit(event.phase_after) - shape_limit(event.phase_before);
            if (correction_.insert(event.frac, height) ==
                signal::osc::MinBlepInsertResult::capacity_exceeded)
                ++dropped_events_;
        }
        return output;
    }

    [[nodiscard]] std::size_t dropped_events() const noexcept {
        return dropped_events_;
    }

  private:
    static double shape(double phase) noexcept {
        return 2.0 * phase - 1.0;
    }

    static double shape_limit(double phase) noexcept {
        return phase == 1.0 ? 1.0 : shape(phase);
    }

    signal::osc::PhaseAccumulator phase_;
    signal::osc::MinBlepAccumulator<> correction_;
    std::size_t dropped_events_ = 0;
};

} // namespace pulp::examples
