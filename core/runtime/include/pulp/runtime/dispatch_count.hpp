#pragma once

/// @file dispatch_count.hpp
/// Turn a run-time count into a compile-time one.

#include <cstddef>
#include <type_traits>
#include <utility>

namespace pulp::runtime {

/// Call `fn(std::integral_constant<std::size_t, n>{})` for a run-time `n` in
/// [1, Max], so a kernel can be instantiated per count (fixed-size local
/// arrays, fully unrolled inner loops) and selected at run time without a
/// hand-written switch. Returns false, without calling `fn`, when `n` is 0 or
/// above `Max`.
///
/// @code
/// pulp::runtime::dispatch_count<16>(sections, [&](auto count) {
///     run_pass<decltype(count)::value>(state, buffer, frames);
/// });
/// @endcode
///
/// RT contract: no allocation, no locks; the selection is a chain of integer
/// compares that compilers lower to a jump table.
template <std::size_t Max, typename Fn> constexpr bool dispatch_count(std::size_t n, Fn&& fn) {
    static_assert(Max > 0, "dispatch_count needs at least one count");
    return [&]<std::size_t... I>(std::index_sequence<I...>) {
        return ((n == I + 1 ? (fn(std::integral_constant<std::size_t, I + 1>{}), true) : false) ||
                ...);
    }(std::make_index_sequence<Max>{});
}

} // namespace pulp::runtime
