#include <pulp/timeline/undo.hpp>

#include <limits>

namespace pulp::timeline {

std::size_t retained_size(std::span<const Command> commands) noexcept {
    std::size_t result = 0;
    for (const auto& command : commands) {
        const auto bytes = retained_size(command);
        if (bytes > std::numeric_limits<std::size_t>::max() - result)
            return std::numeric_limits<std::size_t>::max();
        result += bytes;
    }
    return result;
}

} // namespace pulp::timeline
