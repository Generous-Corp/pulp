#pragma once

#include <pulp/timeline/command.hpp>

#include <cstddef>
#include <span>

namespace pulp::timeline {

struct UndoLimits {
    std::size_t max_groups = 128;
    std::size_t max_retained_bytes = 8 * 1024 * 1024;
};

std::size_t retained_size(std::span<const Command> commands) noexcept;

} // namespace pulp::timeline
