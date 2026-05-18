#pragma once

// Templated Range<T> — represents a [start, end) interval with set operations.

#include <algorithm>
#include <optional>
#include <type_traits>

namespace pulp::runtime {

template<typename T>
struct Range {
    T start{};
    T end{};

    constexpr Range() = default;
    constexpr Range(T s, T e) : start(s), end(e) {}

    /// Length of the range
    constexpr T length() const { return end - start; }

    /// Whether the range is empty (start >= end)
    constexpr bool empty() const { return start >= end; }

    /// Whether this range contains a value
    constexpr bool contains(T value) const { return value >= start && value < end; }

    /// Whether this range fully contains another range
    constexpr bool contains(const Range& other) const {
        return other.start >= start && other.end <= end;
    }

    /// Whether this range intersects another range
    constexpr bool intersects(const Range& other) const {
        return start < other.end && other.start < end;
    }

    /// Intersection of two ranges (empty if no overlap)
    constexpr Range intersection(const Range& other) const {
        T s = std::max(start, other.start);
        T e = std::min(end, other.end);
        return (s < e) ? Range(s, e) : Range{};
    }

    /// Union of two ranges (bounding box — may include a gap)
    constexpr Range enclosing_union(const Range& other) const {
        if (empty()) return other;
        if (other.empty()) return *this;
        return Range(std::min(start, other.start), std::max(end, other.end));
    }

    /// Constrain a value to this range [start, end)
    constexpr T constrain(T value) const {
        if constexpr (std::is_integral_v<T>) {
            return std::clamp(value, start, end > start ? end - T(1) : start);
        } else {
            return std::clamp(value, start, end > start ? end : start);
        }
    }

    /// Expand range to include a value
    constexpr Range expanded(T value) const {
        if (empty()) return Range(value, value + T(1));
        return Range(std::min(start, value), std::max(end, value + T(1)));
    }

    constexpr bool operator==(const Range& other) const {
        return start == other.start && end == other.end;
    }
    constexpr bool operator!=(const Range& other) const { return !(*this == other); }

    /// Create a range from start + length
    static constexpr Range from_start_length(T s, T len) { return Range(s, s + len); }
};

// Common type aliases
using IntRange = Range<int>;
using FloatRange = Range<float>;
using DoubleRange = Range<double>;
using SizeRange = Range<size_t>;

}  // namespace pulp::runtime
