#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace pulp::signal::detail {
struct CapacityLayout {
    std::size_t n{}, m{}, wcap{}, e{}, q{}, a{}, g{}, l{}, f{}, bytes{};
    static bool add(std::size_t a, std::size_t b, std::size_t& result) noexcept {
        if (b > std::numeric_limits<std::size_t>::max() - a)
            return false;
        result = a + b;
        return true;
    }
    static bool multiply(std::size_t a, std::size_t b, std::size_t& result) noexcept {
        if (a != 0 && b > std::numeric_limits<std::size_t>::max() / a)
            return false;
        result = a * b;
        return true;
    }
    static bool make(double sample_rate, std::size_t n, std::size_t m, double normalize_ratio,
                     std::size_t repeat_limit, std::size_t rotate_limit, std::size_t slot_count,
                     std::size_t sample_bytes, std::size_t held_bytes, std::size_t token_bytes,
                     std::size_t program_bytes, std::size_t step_bytes,
                     CapacityLayout& result) noexcept {
        CapacityLayout candidate{};
        candidate.n = n;
        candidate.m = m;
        candidate.wcap = std::min(n, rotate_limit);
        std::size_t repeat_bound{}, normalize_bound{};
        if (!multiply(m, repeat_limit, repeat_bound))
            return false;
        const long double normalized = static_cast<long double>(m) * normalize_ratio;
        if (!std::isfinite(normalized) ||
            normalized > static_cast<long double>(std::numeric_limits<std::size_t>::max()))
            return false;
        normalize_bound = static_cast<std::size_t>(std::ceil(normalized));
        candidate.e = std::max(repeat_bound, normalize_bound);
        const long double fade_product = static_cast<long double>(sample_rate) * 0.020L;
        if (!std::isfinite(fade_product) ||
            fade_product > static_cast<long double>(std::numeric_limits<std::size_t>::max()))
            return false;
        candidate.f = static_cast<std::size_t>(std::ceil(fade_product));
        if ((candidate.f & 1u) != 0u && !add(candidate.f, 1u, candidate.f))
            return false;
        std::size_t n1{};
        if (!add(n, 1u, n1) || !multiply(n1, candidate.e, candidate.q) ||
            !add(candidate.q, candidate.f, candidate.q) ||
            !multiply(candidate.wcap, m, candidate.a) || !multiply(n, m, candidate.g) ||
            !add(candidate.a, candidate.g, candidate.l))
            return false;
        std::size_t sample_count{}, term{}, count{};
        if (!add(m, candidate.e, sample_count) || !add(sample_count, candidate.q, sample_count) ||
            !add(sample_count, candidate.g, sample_count) ||
            !multiply(sample_count, sample_bytes, candidate.bytes) ||
            !multiply(n, held_bytes, term) || !add(candidate.bytes, term, candidate.bytes) ||
            !multiply(n, token_bytes, term) || !add(candidate.bytes, term, candidate.bytes) ||
            !multiply(slot_count, program_bytes, term) ||
            !add(candidate.bytes, term, candidate.bytes) || !multiply(slot_count, n, count) ||
            !multiply(count, step_bytes, term) || !add(candidate.bytes, term, candidate.bytes) ||
            !multiply(slot_count, candidate.wcap, count) ||
            !multiply(count, sizeof(std::uint16_t), term) ||
            !add(candidate.bytes, term, candidate.bytes))
            return false;
        result = candidate;
        return true;
    }
};

} // namespace pulp::signal::detail
