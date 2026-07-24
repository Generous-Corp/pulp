#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace pulp::timeline::detail {

struct BoundedIncrement {
    bool within_limit = false;
    std::uint64_t actual = 0;

    explicit operator bool() const noexcept {
        return within_limit;
    }
};

inline BoundedIncrement bounded_increment(std::size_t& count, std::size_t limit) noexcept {
    if (count < limit) {
        ++count;
        return {true, count};
    }
    const auto actual = limit == std::numeric_limits<std::size_t>::max() ? limit : limit + 1;
    return {false, actual};
}

} // namespace pulp::timeline::detail
