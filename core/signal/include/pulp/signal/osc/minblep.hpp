#pragma once

/// @file minblep.hpp
/// Fixed-capacity minimum-phase bandlimited-step correction accumulator.

#include "detail/minblep_table.hpp"

#include <array>
#include <cmath>
#include <cstddef>

namespace pulp::signal::osc {

enum class MinBlepInsertResult {
    inserted,
    zero_height,
    invalid_position,
    non_finite_height,
    capacity_exceeded,
};

/// Accumulates causal minBLEP corrections for discontinuities found between
/// adjacent output samples.
///
/// `insert(position, height)` receives a position in [0, 1], measured after the
/// sample most recently returned by `next()`, and a step height measured as
/// after minus before. The correction is added to the already-stepped trivial
/// waveform beginning with subsequent `next()` calls. Position 0 means the
/// event occurred immediately after the previous output sample; position 1
/// means the next output sample lands exactly on the event.
///
/// The table follows Brandt's minimum-phase construction: a Blackman-windowed
/// sinc is transformed through its real cepstrum, integrated, normalized, and
/// stored as the residual from an already-applied unit step. The reproducible
/// generator is `tools/scripts/generate_minblep_table.py`; the primary method
/// reference is https://www.cs.cmu.edu/~eli/papers/icmc01-hardsync.pdf.
///
/// Each insertion occupies one of `MaximumEvents` slots for at most 32 samples.
/// Slots are chosen and summed in ascending index order. If every slot is live,
/// the new event is dropped with `capacity_exceeded`; existing corrections are
/// unchanged. Per-sample work is therefore fixed at `MaximumEvents`, with no
/// allocation, locks, I/O, or table initialization on the audio thread.
template <std::size_t MaximumEvents = 8> class MinBlepAccumulator {
  public:
    static_assert(MaximumEvents > 0, "minBLEP event capacity must be positive");

    static constexpr std::size_t capacity = MaximumEvents;
    static constexpr std::size_t kernel_samples = detail::kMinBlepKernelSamples;
    static constexpr std::size_t phases_per_sample = detail::kMinBlepPhasesPerSample;

    void reset() noexcept {
        slots_ = {};
        active_events_ = 0;
    }

    [[nodiscard]] MinBlepInsertResult insert(double position, double height) noexcept {
        if (!(position >= 0.0 && position <= 1.0))
            return MinBlepInsertResult::invalid_position;
        if (!std::isfinite(height))
            return MinBlepInsertResult::non_finite_height;
        if (height == 0.0)
            return MinBlepInsertResult::zero_height;

        for (auto& slot : slots_) {
            if (slot.active)
                continue;
            slot.active = true;
            slot.table_position = (1.0 - position) * static_cast<double>(phases_per_sample);
            slot.height = height;
            ++active_events_;
            return MinBlepInsertResult::inserted;
        }
        return MinBlepInsertResult::capacity_exceeded;
    }

    [[nodiscard]] double next() noexcept {
        double correction = 0.0;
        constexpr auto last_index = detail::kMinBlepResidual.size() - 1;
        for (auto& slot : slots_) {
            if (!slot.active)
                continue;

            const auto index = static_cast<std::size_t>(slot.table_position);
            const double fraction = slot.table_position - static_cast<double>(index);
            const double first = static_cast<double>(detail::kMinBlepResidual[index]);
            const double second = static_cast<double>(detail::kMinBlepResidual[index + 1]);
            correction += slot.height * (first + fraction * (second - first));
            slot.table_position += static_cast<double>(phases_per_sample);
            if (slot.table_position >= static_cast<double>(last_index)) {
                slot = {};
                --active_events_;
            }
        }
        return correction;
    }

    [[nodiscard]] std::size_t active_events() const noexcept {
        return active_events_;
    }

  private:
    struct Slot {
        double table_position = 0.0;
        double height = 0.0;
        bool active = false;
    };

    std::array<Slot, MaximumEvents> slots_{};
    std::size_t active_events_ = 0;
};

} // namespace pulp::signal::osc
