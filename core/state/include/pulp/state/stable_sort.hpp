#pragma once

// Stable, in-place, allocation-free sort for audio-thread event lists.
//
// std::stable_sort may allocate a temporary buffer and std::sort reorders
// equal keys, so neither suits a render callback that must keep a host's
// order for coincident events. This sorts blocks of 20 by insertion and
// merges them with the SymMerge algorithm (Kim and Kutzner): O(n log^2 n)
// compares and moves, recursion depth O(log n), no scratch storage. Input
// already in order is detected in one pass and left untouched.

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <utility>

namespace pulp::state {

namespace stable_sort_detail {

template <typename It, typename Less>
void insertion_sort(It base, std::size_t a, std::size_t b, Less& less) {
    for (std::size_t i = a + 1; i < b; ++i)
        for (std::size_t j = i; j > a && less(base[j], base[j - 1]); --j)
            std::iter_swap(base + j, base + (j - 1));
}

// Merges the sorted ranges [a, m) and [m, b) of base, stably.
template <typename It, typename Less>
void sym_merge(It base, std::size_t a, std::size_t m, std::size_t b, Less& less) {
    if (m - a == 1) {
        std::size_t lo = m, hi = b;
        while (lo < hi) {
            const std::size_t h = (lo + hi) / 2;
            if (less(base[h], base[a])) lo = h + 1; else hi = h;
        }
        for (std::size_t k = a; k + 1 < lo; ++k) std::iter_swap(base + k, base + (k + 1));
        return;
    }
    if (b - m == 1) {
        std::size_t lo = a, hi = m;
        while (lo < hi) {
            const std::size_t h = (lo + hi) / 2;
            if (!less(base[m], base[h])) lo = h + 1; else hi = h;
        }
        for (std::size_t k = m; k > lo; --k) std::iter_swap(base + k, base + (k - 1));
        return;
    }
    const std::size_t mid = (a + b) / 2;
    const std::size_t n = mid + m;
    std::size_t start = m > mid ? n - b : a;
    std::size_t r = m > mid ? mid : m;
    const std::size_t p = n - 1;
    while (start < r) {
        const std::size_t c = (start + r) / 2;
        if (!less(base[p - c], base[c])) start = c + 1; else r = c;
    }
    const std::size_t end = n - start;
    if (start < m && m < end) std::rotate(base + start, base + m, base + end);
    if (a < start && start < mid) sym_merge(base, a, start, mid, less);
    if (mid < end && end < b) sym_merge(base, mid, end, b, less);
}

}  // namespace stable_sort_detail

/// Sorts [first, last) by `less`, keeping equal elements in their original
/// order, without allocating. Requires random-access iterators.
template <typename It, typename Less>
void stable_sort_in_place(It first, It last, Less less) {
    const auto n = static_cast<std::size_t>(std::distance(first, last));
    if (n < 2 || std::is_sorted(first, last, less)) return;
    constexpr std::size_t kBlock = 20;
    for (std::size_t a = 0; a < n; a += kBlock)
        stable_sort_detail::insertion_sort(first, a, std::min(a + kBlock, n), less);
    for (std::size_t width = kBlock; width < n; width *= 2)
        for (std::size_t a = 0; a + width < n; a += 2 * width)
            stable_sort_detail::sym_merge(first, a, a + width, std::min(a + 2 * width, n), less);
}

}  // namespace pulp::state
