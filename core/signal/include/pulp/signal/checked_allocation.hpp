#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace pulp::signal {

inline constexpr std::uint64_t kTargetAddressMaximumBytes =
    static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max());

inline bool checked_capacity_product(std::uint64_t lhs,
                                     std::uint64_t rhs,
                                     std::uint64_t maximum,
                                     std::uint64_t& product) noexcept {
    if (lhs != 0 && rhs > maximum / lhs) return false;
    product = lhs * rhs;
    return true;
}

inline bool checked_capacity_sum(std::uint64_t lhs,
                                 std::uint64_t rhs,
                                 std::uint64_t maximum,
                                 std::uint64_t& sum) noexcept {
    if (lhs > maximum || rhs > maximum - lhs) return false;
    sum = lhs + rhs;
    return true;
}

template <typename Element>
inline bool checked_allocation_bytes(std::uint64_t elements,
                                     std::uint64_t target_max_bytes,
                                     std::uint64_t* bytes = nullptr) noexcept {
    std::uint64_t allocation_bytes = 0;
    if (!checked_capacity_product(elements, sizeof(Element),
                                  target_max_bytes, allocation_bytes))
        return false;
    if (bytes) *bytes = allocation_bytes;
    return true;
}

} // namespace pulp::signal
