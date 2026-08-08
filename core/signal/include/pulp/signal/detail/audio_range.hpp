#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace pulp::signal::detail {

template <typename SampleType>
inline bool audio_ranges_overlap(const SampleType* first_ptr, const SampleType* second_ptr,
                                 std::size_t frames) noexcept {
    if (frames == 0)
        return false;
    if (frames > std::numeric_limits<std::uintptr_t>::max() / sizeof(SampleType))
        return true;
    const auto bytes = static_cast<std::uintptr_t>(frames * sizeof(SampleType));
    const auto first = reinterpret_cast<std::uintptr_t>(first_ptr);
    const auto second = reinterpret_cast<std::uintptr_t>(second_ptr);
    if (first > std::numeric_limits<std::uintptr_t>::max() - bytes ||
        second > std::numeric_limits<std::uintptr_t>::max() - bytes)
        return true;
    return first < second + bytes && second < first + bytes;
}

} // namespace pulp::signal::detail
