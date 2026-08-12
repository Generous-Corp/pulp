#pragma once

#include <cstddef>
#include <cstdint>
#include <compare>
#include <span>

#include <pulp/timeline_editor/viewport_projection.hpp>
#include <pulp/timebase/compiled_meter_map.hpp>

namespace pulp::timeline_editor {

/// Identifies the musical boundary represented by a grid line.
enum class GridLineLevel : std::uint8_t {
    /// Marks the first tick of a bar.
    Bar,
    /// Marks a beat subdivision after the bar boundary.
    Beat,
};

/// Describes one visible musical boundary in projected screen coordinates.
struct GridLine {
    /// Musical position of the boundary.
    timebase::TickPosition tick{};
    /// Horizontal pixel coordinate produced by the projection.
    float x = 0.0f;
    /// Musical boundary level used to select its visual prominence.
    GridLineLevel level = GridLineLevel::Beat;
    constexpr auto operator<=>(const GridLine&) const = default;
};

/// Reports why grid-line generation stopped.
enum class GridLineError : std::uint8_t {
    /// Generation completed successfully.
    None,
    /// The requested minimum pixel spacing is not finite.
    NonFiniteSpacing,
    /// The requested minimum pixel spacing is zero or negative.
    NonPositiveSpacing,
    /// Meter or tick arithmetic could not advance safely through the range.
    RangeOverflow,
    /// The output span filled before generation completed.
    OutputTooSmall,
};

/// Carries the generation status and the number of lines written.
struct GridLineResult {
    /// Completion status for the generation attempt.
    GridLineError error = GridLineError::None;
    /// Number of initialized entries in the caller-provided output span.
    std::size_t count = 0;

    /// Returns true when generation completed without an error.
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
