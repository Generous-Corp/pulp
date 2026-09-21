#pragma once

#include <pulp/runtime/result.hpp>
#include <pulp/timebase/groove_kernel.hpp>
#include <pulp/timebase/quantize.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>

namespace pulp::timebase {

// Sixteen entries hold one repeat of a sixteenth-grid groove, which is the
// granularity a groove table is authored at. The capacity is deliberately two
// orders of magnitude below kMaximumGrooveKernelSteps: the whole point of this
// type is to fit inside a value a realtime consumer copies wholesale, and an
// inline table of a thousand entries is large enough to be the reason it does
// not. A table needing more repeats than this belongs in the canonical model,
// which owns persistence and has no such bound.
inline constexpr std::size_t kMaximumInlineGrooveSteps = 16;

// Construction input. The numeric domains are the canonical groove domains --
// timing strength and velocity strength in per-mille, velocity scale in
// per-mille up to kMaximumGrooveKernelVelocityScale, offsets in ticks -- so a
// table authored against timeline::GrooveTemplate converts across losslessly.
struct InlineGrooveInput {
    // Zero disables swing. Swing and table grids are deliberately independent.
    TickDuration swing_grid{};
    SwingRatio swing = kStraightSwing;
    // Zero exactly when steps is empty.
    TickDuration table_grid{};
    std::span<const GrooveKernelStep> steps{};
    std::int32_t timing_strength = kGrooveKernelUnitScale;
    std::int32_t velocity_strength = kGrooveKernelUnitScale;
};

enum class InlineGrooveError {
    None,
    InvalidSwingGrid,
    InvalidSwing,
    InvalidTable,
    TooManySteps,
    InvalidStrength,
    InvalidVelocityScale,
    RangeExceeded,
};

// A trivially copyable projection of the canonical authored groove model onto a
// realtime path: timeline::GrooveTemplate's arithmetic carried in the
// fixed-capacity inline storage that a spec struct can hold and a realtime
// consumer can hot-swap by value.
//
// ORDER PRESERVATION IS EXPLICITLY NOT PROVIDED, AND THAT IS THE CONTRACT.
// Adjacent entries may lean in opposite directions, so material at the end of
// one entry can be displaced past material at the start of the next. That is
// what a groove table is, and the canonical model states the same thing:
// offsets are bounded to less than one entry rather than constrained into
// monotonicity. Callers that genuinely require a non-reordering feel want
// OrderPreservingGrooveKernel, which is the strict subset of this model and
// refuses at create() any table whose scaled offsets fall by more than one tick
// between adjacent entries. Both types exist because those are two different
// questions: a sequencer emitting by displaced position needs no ordering
// guarantee, while a consumer that walks events in authored order and cannot
// re-sort them does. Asking the strict kernel for an ordinary groove gets an
// error rather than a feel, and asking this type for an ordering guarantee gets
// silence rather than one.
//
// Swing and the table are two independent displacements of the same authored
// position, and the table is indexed by where a note was written rather than by
// where swing moved it, so changing the swing setting never re-assigns material
// to a different entry.
//
// A timing strength of zero is exact identity on every position, swing
// included, which is what lets a consumer define bypass as the zero-strength
// result rather than as a separate code path.
//
// Validation happens once in create(), on the control thread. Projection reads
// inline storage and allocates nothing.
class InlineGrooveProjector {
  public:
    // The default value states no feel: it projects every position onto itself
    // and leaves every velocity unscaled, so a spec struct holding one needs no
    // separate "groove absent" flag.
    constexpr InlineGrooveProjector() noexcept = default;

    static runtime::Result<InlineGrooveProjector, InlineGrooveError>
    create(InlineGrooveInput input) noexcept {
        if (input.swing_grid.value < 0 ||
            (input.swing_grid.value != 0 && !valid_swing_grid(input.swing_grid)))
            return runtime::Err(InlineGrooveError::InvalidSwingGrid);
        if (!valid_swing_ratio(input.swing))
            return runtime::Err(InlineGrooveError::InvalidSwing);
        // A table width and a table imply each other: a width with no entries
        // names nothing, and entries with no width have no position to be read
        // at.
        if ((input.table_grid.value != 0) != !input.steps.empty() || input.table_grid.value < 0 ||
            input.table_grid.value > kMaxSwingGridTicks)
            return runtime::Err(InlineGrooveError::InvalidTable);
        if (input.steps.size() > kMaximumInlineGrooveSteps)
            return runtime::Err(InlineGrooveError::TooManySteps);
        if (input.timing_strength < 0 || input.timing_strength > kGrooveKernelUnitScale ||
            input.velocity_strength < 0 || input.velocity_strength > kGrooveKernelUnitScale)
            return runtime::Err(InlineGrooveError::InvalidStrength);
        for (const auto& step : input.steps) {
            // An offset of a whole entry or more would move material past the
            // entry beyond its neighbour, which is a different table written
            // wrong rather than an extreme feel. Anything inside that bound is
            // admitted, reordering included.
            if (step.timing_offset.value <= -input.table_grid.value ||
                step.timing_offset.value >= input.table_grid.value)
                return runtime::Err(InlineGrooveError::InvalidTable);
            if (step.velocity_scale < 0 || step.velocity_scale > kMaximumGrooveKernelVelocityScale)
                return runtime::Err(InlineGrooveError::InvalidVelocityScale);
        }
        return runtime::Ok(InlineGrooveProjector(input));
    }

    constexpr TickDuration swing_grid() const noexcept {
        return swing_grid_;
    }
    constexpr SwingRatio swing() const noexcept {
        return swing_;
    }
    constexpr TickDuration table_grid() const noexcept {
        return table_grid_;
    }
    constexpr std::span<const GrooveKernelStep> steps() const noexcept {
        return {steps_.data(), size_};
    }
    constexpr std::int32_t timing_strength() const noexcept {
        return timing_strength_;
    }
    constexpr std::int32_t velocity_strength() const noexcept {
        return velocity_strength_;
    }

    // Whether timing and velocity projection are both the identity.
    constexpr bool states_no_feel() const noexcept {
        return swing_grid_.value == 0 && size_ == 0;
    }

    // The sounding position for an authored one. Swing and table offsets can
    // oppose one another, so their bounded deltas combine before the single
    // position add and cancellation is preserved.
    //
    // The result is checked rather than saturating: a position close enough to
    // the signed limit for the displacement to leave the domain reports
    // RangeExceeded instead of clamping onto the rail, which is the same
    // contract OrderPreservingGrooveKernel states. This is the one place the
    // projection differs from timeline::GrooveTemplate, whose position add
    // saturates; every position where that add does not rail projects
    // identically in both.
    runtime::Result<TickPosition, InlineGrooveError>
    apply_timing(TickPosition authored) const noexcept {
        std::int64_t displacement = 0;
        if (swing_grid_.value != 0)
            displacement = scaled_by_strength(
                swing_displacement(authored, swing_grid_, swing_).value, timing_strength_);
        if (const auto* step = step_at(authored))
            displacement += scaled_by_strength(step->timing_offset.value, timing_strength_);
        std::int64_t transformed = 0;
        if (!checked_add(authored.value, displacement, transformed))
            return runtime::Err(InlineGrooveError::RangeExceeded);
        return runtime::Ok(TickPosition{transformed});
    }

    // The accent multiplier at an authored position, in per-mille.
    constexpr std::int32_t velocity_scale_at(TickPosition authored) const noexcept {
        const auto* step = step_at(authored);
        if (step == nullptr)
            return kGrooveKernelUnitScale;
        const auto deviation =
            static_cast<std::int64_t>(step->velocity_scale) - kGrooveKernelUnitScale;
        return static_cast<std::int32_t>(kGrooveKernelUnitScale +
                                         scaled_by_strength(deviation, velocity_strength_));
    }

  private:
    explicit constexpr InlineGrooveProjector(InlineGrooveInput input) noexcept
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

    // Scale by a per-mille strength, rounding halves away from zero so a
    // positive and a negative offset of the same size are attenuated equally;
    // truncation would bias every groove toward zero displacement. A strength
    // of zero therefore scales every offset to exactly zero, which is what
    // makes the zero-strength projection the exact identity.
    //
    // create() bounds both call sites -- an offset smaller than a table entry,
    // a swing displacement smaller than a swing grid, each at most
    // kMaxSwingGridTicks, or a velocity deviation smaller than the scale
    // ceiling -- so the product with a strength of at most
    // kGrooveKernelUnitScale stays far inside the signed domain.
    static constexpr std::int64_t scaled_by_strength(std::int64_t value,
                                                     std::int32_t strength) noexcept {
        const auto magnitude = value < 0 ? -value : value;
        const auto scaled =
            (magnitude * strength + kGrooveKernelUnitScale / 2) / kGrooveKernelUnitScale;
        return value < 0 ? -scaled : scaled;
    }

    // The entry an authored position falls in, or null when there is no table.
    // The table repeats in both directions, so the index floors toward negative
    // infinity rather than inheriting the sign of the dividend.
    constexpr const GrooveKernelStep* step_at(TickPosition authored) const noexcept {
        if (size_ == 0 || table_grid_.value <= 0)
            return nullptr;
        auto index = authored.value / table_grid_.value;
        if (authored.value % table_grid_.value < 0)
            --index;
        auto slot = index % static_cast<std::int64_t>(size_);
        if (slot < 0)
            slot += static_cast<std::int64_t>(size_);
        return &steps_[static_cast<std::size_t>(slot)];
    }

    TickDuration swing_grid_{};
    SwingRatio swing_{};
    TickDuration table_grid_{};
    std::array<GrooveKernelStep, kMaximumInlineGrooveSteps> steps_{};
    std::size_t size_ = 0;
    std::int32_t timing_strength_ = kGrooveKernelUnitScale;
    std::int32_t velocity_strength_ = kGrooveKernelUnitScale;
};

// The reason this type exists rather than the canonical model: a realtime
// consumer hot-swaps its configuration as one trivially copyable value, and
// timeline::GrooveTemplate holds a shared_ptr over heap storage, so it cannot
// live in such a value at any size.
static_assert(std::is_trivially_copyable_v<InlineGrooveProjector>);

} // namespace pulp::timebase
