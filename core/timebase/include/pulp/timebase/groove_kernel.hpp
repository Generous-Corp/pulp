#pragma once

#include <pulp/runtime/result.hpp>
#include <pulp/timebase/quantize.hpp>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <span>

namespace pulp::timebase {

// These numeric domains intentionally match timeline::GrooveTemplate so its
// authored values can feed this dependency-lower, allocation-free kernel.
inline constexpr std::int32_t kGrooveKernelUnitScale = 1'000;
inline constexpr std::int32_t kMaximumGrooveKernelVelocityScale = 4'000;
inline constexpr std::size_t kMaximumGrooveKernelSteps = 1'024;
inline constexpr std::size_t kMaximumGrooveValidationBoundaries = 65'536;

struct GrooveKernelStep {
    TickDuration timing_offset{};
    std::int32_t velocity_scale = kGrooveKernelUnitScale;
    constexpr auto operator<=>(const GrooveKernelStep&) const = default;
};

struct GrooveKernelInput {
    // Zero disables swing. Swing and table grids are deliberately independent.
    TickDuration swing_grid{};
    SwingRatio swing = kStraightSwing;
    // Zero exactly when steps is empty.
    TickDuration table_grid{};
    std::span<const GrooveKernelStep> steps{};
    std::int32_t timing_strength = kGrooveKernelUnitScale;
    std::int32_t velocity_strength = kGrooveKernelUnitScale;
};

enum class GrooveKernelError {
    None,
    InvalidSwingGrid,
    InvalidSwing,
    InvalidTable,
    TooManySteps,
    InvalidStrength,
    InvalidVelocityScale,
    ReordersEvents,
    ValidationLimitExceeded,
    RangeExceeded,
};

// This is not the canonical, named, sequence-owned GrooveTemplate. It is a
// fixed-capacity projection kernel for callers that require the stricter
// non-reordering subset of that authored model on a realtime path.
class OrderPreservingGrooveKernel {
  public:
    static runtime::Result<OrderPreservingGrooveKernel, GrooveKernelError>
    create(GrooveKernelInput input) noexcept {
        if (input.swing_grid.value < 0 ||
            (input.swing_grid.value != 0 && !valid_swing_grid(input.swing_grid)))
            return runtime::Err(GrooveKernelError::InvalidSwingGrid);
        if (!valid_swing_ratio(input.swing))
            return runtime::Err(GrooveKernelError::InvalidSwing);
        if ((input.table_grid.value != 0) != !input.steps.empty() || input.table_grid.value < 0 ||
            input.table_grid.value > kMaxSwingGridTicks)
            return runtime::Err(GrooveKernelError::InvalidTable);
        if (input.steps.size() > kMaximumGrooveKernelSteps)
            return runtime::Err(GrooveKernelError::TooManySteps);
        if (input.timing_strength < 0 || input.timing_strength > kGrooveKernelUnitScale ||
            input.velocity_strength < 0 || input.velocity_strength > kGrooveKernelUnitScale)
            return runtime::Err(GrooveKernelError::InvalidStrength);
        for (const auto& step : input.steps) {
            if (step.timing_offset.value <= -input.table_grid.value ||
                step.timing_offset.value >= input.table_grid.value)
                return runtime::Err(GrooveKernelError::InvalidTable);
            if (step.velocity_scale < 0 || step.velocity_scale > kMaximumGrooveKernelVelocityScale)
                return runtime::Err(GrooveKernelError::InvalidVelocityScale);
        }

        OrderPreservingGrooveKernel result(input);
        const auto order = result.validate_order();
        if (order != GrooveKernelError::None)
            return runtime::Err(order);
        return runtime::Ok(result);
    }

    TickDuration swing_grid() const noexcept {
        return swing_grid_;
    }
    SwingRatio swing() const noexcept {
        return swing_;
    }
    TickDuration table_grid() const noexcept {
        return table_grid_;
    }
    std::span<const GrooveKernelStep> steps() const noexcept {
        return {steps_.data(), size_};
    }
    std::int32_t timing_strength() const noexcept {
        return timing_strength_;
    }
    std::int32_t velocity_strength() const noexcept {
        return velocity_strength_;
    }

    runtime::Result<TickPosition, GrooveKernelError>
    apply_timing(TickPosition authored) const noexcept {
        std::int64_t swing_offset = 0;
        if (swing_grid_.value != 0)
            swing_offset = scaled_by_strength(
                swing_displacement(authored, swing_grid_, swing_).value, timing_strength_);
        std::int64_t table_offset = 0;
        if (const auto* step = step_at(authored)) {
            table_offset = scaled_by_strength(step->timing_offset.value, timing_strength_);
        }
        std::int64_t displacement = 0;
        if (!checked_add(swing_offset, table_offset, displacement))
            return runtime::Err(GrooveKernelError::RangeExceeded);
        std::int64_t transformed = 0;
        if (!checked_add(authored.value, displacement, transformed))
            return runtime::Err(GrooveKernelError::RangeExceeded);
        return runtime::Ok(TickPosition{transformed});
    }

    std::int32_t velocity_scale_at(TickPosition authored) const noexcept {
        const auto* step = step_at(authored);
        if (step == nullptr)
            return kGrooveKernelUnitScale;
        const auto deviation =
            static_cast<std::int64_t>(step->velocity_scale) - kGrooveKernelUnitScale;
        return static_cast<std::int32_t>(kGrooveKernelUnitScale +
                                         scaled_by_strength(deviation, velocity_strength_));
    }

  private:
    explicit OrderPreservingGrooveKernel(GrooveKernelInput input) noexcept
        : swing_grid_(input.swing_grid), swing_(input.swing), table_grid_(input.table_grid),
          size_(input.steps.size()), timing_strength_(input.timing_strength),
          velocity_strength_(input.velocity_strength) {
        for (std::size_t index = 0; index < size_; ++index)
            steps_[index] = input.steps[index];
    }

    static constexpr bool checked_add(std::int64_t lhs, std::int64_t rhs,
                                      std::int64_t& result) noexcept {
        constexpr auto min = std::numeric_limits<std::int64_t>::min();
        constexpr auto max = std::numeric_limits<std::int64_t>::max();
        if ((rhs > 0 && lhs > max - rhs) || (rhs < 0 && lhs < min - rhs))
            return false;
        result = lhs + rhs;
        return true;
    }

    static constexpr std::int64_t scaled_by_strength(std::int64_t value,
                                                     std::int32_t strength) noexcept {
        if (value == 0 || strength == 0)
            return 0;
        const auto negative = value < 0;
        const auto magnitude = negative ? static_cast<std::uint64_t>(-(value + 1)) + 1U
                                        : static_cast<std::uint64_t>(value);
        const auto whole = magnitude / kGrooveKernelUnitScale;
        const auto remainder = magnitude % kGrooveKernelUnitScale;
        const auto scaled =
            whole * static_cast<std::uint32_t>(strength) +
            (remainder * static_cast<std::uint32_t>(strength) + kGrooveKernelUnitScale / 2U) /
                kGrooveKernelUnitScale;
        if (!negative)
            return static_cast<std::int64_t>(scaled);
        if (scaled == (std::uint64_t{1} << 63U))
            return std::numeric_limits<std::int64_t>::min();
        return -static_cast<std::int64_t>(scaled);
    }

    const GrooveKernelStep* step_at(TickPosition authored) const noexcept {
        if (size_ == 0)
            return nullptr;
        auto index = authored.value / table_grid_.value;
        if (authored.value % table_grid_.value < 0)
            --index;
        auto slot = index % static_cast<std::int64_t>(size_);
        if (slot < 0)
            slot += static_cast<std::int64_t>(size_);
        return &steps_[static_cast<std::size_t>(slot)];
    }

    GrooveKernelError validate_order() const noexcept {
        if (timing_strength_ == 0)
            return GrooveKernelError::None;
        if (size_ == 0)
            return GrooveKernelError::None;

        const auto swing_period = swing_grid_.value == 0 ? std::int64_t{1} : 2 * swing_grid_.value;
        const auto phase_count = swing_period / std::gcd(swing_period, table_grid_.value);
        const auto step_count = static_cast<std::int64_t>(size_);
        const auto divisor = std::gcd(phase_count, step_count);
        if (phase_count >
            static_cast<std::int64_t>(kMaximumGrooveValidationBoundaries) / (step_count / divisor))
            return GrooveKernelError::ValidationLimitExceeded;
        const auto boundaries = phase_count * (step_count / divisor);

        for (std::int64_t index = 1; index <= boundaries; ++index) {
            const auto boundary = index * table_grid_.value;
            const auto before = apply_timing({boundary - 1});
            const auto after = apply_timing({boundary});
            if (!before || !after)
                return GrooveKernelError::RangeExceeded;
            if (before.value() > after.value())
                return GrooveKernelError::ReordersEvents;
        }
        return GrooveKernelError::None;
    }

    TickDuration swing_grid_{};
    SwingRatio swing_{};
    TickDuration table_grid_{};
    std::array<GrooveKernelStep, kMaximumGrooveKernelSteps> steps_{};
    std::size_t size_ = 0;
    std::int32_t timing_strength_ = kGrooveKernelUnitScale;
    std::int32_t velocity_strength_ = kGrooveKernelUnitScale;
};

} // namespace pulp::timebase
