#pragma once

#include <pulp/timeline/command.hpp>

#include <cstddef>
#include <span>

namespace pulp::timeline {

/** @addtogroup timeline_editing
 * @{
 */

/// Bounds the number and retained memory of undo groups in a session.
///
/// Closed oldest groups are evicted to admit new work. An open gesture that
/// cannot fit is rejected rather than partially discarded.
struct UndoLimits {
    std::size_t max_groups = 128;
    std::size_t max_retained_bytes = 8 * 1024 * 1024;
};

/// Returns a saturated retained-memory estimate for a command sequence.
std::size_t retained_size(std::span<const Command> commands) noexcept;

/// @}

} // namespace pulp::timeline
