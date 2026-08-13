#pragma once

/// @file loop_range.hpp
/// Canonical loop-region construction for sequencer edits.

#include <cstdint>

#include <pulp/runtime/result.hpp>
#include <pulp/timebase/loop_region.hpp>

namespace pulp::timeline_editor {

/** @addtogroup timeline_editing
 * @{
 */

/// Why an already-snapped loop range could not be constructed.
enum class LoopRangeError : std::uint8_t {
    /// Both endpoints resolve to the same document tick.
    CollapsedSpan,
};

/// Build an enabled, structurally canonical loop candidate.
///
/// `first` and `second` must already contain the caller's explicit snapping
/// decision. Their order describes gesture direction only; the returned range
/// always has `start < end`. Equal endpoints fail instead of fabricating a
/// duration. This function validates tick structure only. A transport can
/// still reject the candidate when its endpoints map to the same sample or its
/// sample span is shorter than the prepared maximum block.
[[nodiscard]] inline runtime::Result<timebase::LoopRegion, LoopRangeError>
loop_region_from_snapped_endpoints(timebase::TickPosition first,
                                   timebase::TickPosition second) noexcept {
    if (first == second)
        return runtime::Err(LoopRangeError::CollapsedSpan);

    const auto start = first < second ? first : second;
    const auto end = first < second ? second : first;
    return runtime::Ok(timebase::LoopRegion{true, start, end});
}

/// Return `loop` with only its enabled state changed.
///
/// Both document bounds are preserved exactly, so disabling and later
/// re-enabling a loop restores the same authored range without re-snapping.
[[nodiscard]] constexpr timebase::LoopRegion
with_loop_enabled(timebase::LoopRegion loop, bool enabled) noexcept {
    loop.enabled = enabled;
    return loop;
}

/// @}

} // namespace pulp::timeline_editor
