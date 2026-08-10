#pragma once

#include <cstddef>
#include <cstdint>
#include <compare>
#include <span>

#include <pulp/timeline_editor/viewport_projection.hpp>
#include <pulp/timebase/compiled_meter_map.hpp>

namespace pulp::timeline_editor {

enum class GridLineLevel : std::uint8_t {
    Bar,
    Beat,
};

struct GridLine {
    timebase::TickPosition tick{};
    float x = 0.0f;
    GridLineLevel level = GridLineLevel::Beat;
    constexpr auto operator<=>(const GridLine&) const = default;
};

enum class GridLineError : std::uint8_t {
    None,
    NonFiniteSpacing,
    NonPositiveSpacing,
    RangeOverflow,
    OutputTooSmall,
};

struct GridLineResult {
    GridLineError error = GridLineError::None;
    std::size_t count = 0;

    constexpr explicit operator bool() const noexcept {
        return error == GridLineError::None;
    }
};

/// Emits visible bar lines and beat lines that are at least `minimum_spacing_px`
/// from their enclosing bar line. The kernel deliberately returns screen
/// coordinates but knows nothing about a view, canvas, or rendering policy.
GridLineResult generate_grid_lines(const TickProjection& projection,
                                   const timebase::CompiledMeterMap& meter,
                                   float minimum_spacing_px,
                                   std::span<GridLine> output) noexcept;

} // namespace pulp::timeline_editor
