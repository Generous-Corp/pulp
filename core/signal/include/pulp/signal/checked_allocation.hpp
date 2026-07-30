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

/// Validate one allocation against its address ceiling, then compute the
/// aggregate retained charge for `repetitions` distinct allocations. The
/// aggregate is deliberately not compared with the per-allocation ceiling.
template <typename Element>
inline bool checked_repeated_allocation_bytes(std::uint64_t elements_per_allocation,
                                              std::uint64_t repetitions,
                                              std::uint64_t target_max_bytes,
                                              std::uint64_t& aggregate_bytes) noexcept {
    std::uint64_t one_allocation_bytes = 0;
    return checked_allocation_bytes<Element>(elements_per_allocation, target_max_bytes,
                                             &one_allocation_bytes)
        && checked_capacity_product(one_allocation_bytes, repetitions,
                                    std::numeric_limits<std::uint64_t>::max(), aggregate_bytes);
}

class CheckedRetainedByteCharge {
  public:
    explicit CheckedRetainedByteCharge(std::uint64_t allocation_ceiling) noexcept
        : allocation_ceiling_(allocation_ceiling) {}

    template <typename Element>
    bool add(std::uint64_t elements) noexcept {
        std::uint64_t bytes = 0;
        return checked_allocation_bytes<Element>(elements, allocation_ceiling_, &bytes)
            && add_retained_bytes(bytes);
    }

    template <typename Element>
    bool add_repeated(std::uint64_t elements_per_allocation,
                      std::uint64_t repetitions) noexcept {
        std::uint64_t bytes = 0;
        return checked_repeated_allocation_bytes<Element>(
                   elements_per_allocation, repetitions, allocation_ceiling_, bytes)
            && add_retained_bytes(bytes);
    }

    bool add_retained_bytes(std::uint64_t bytes) noexcept {
        return checked_capacity_sum(total_, bytes, std::numeric_limits<std::uint64_t>::max(),
                                    total_);
    }

    std::uint64_t total() const noexcept { return total_; }

  private:
    std::uint64_t allocation_ceiling_ = 0;
    std::uint64_t total_ = 0;
};

} // namespace pulp::signal
