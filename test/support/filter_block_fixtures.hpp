#pragma once

/// @file filter_block_fixtures.hpp
/// Shared inputs for tests that compare a filter's block path with its
/// per-sample path: a busy program-like signal, an irregular block schedule
/// that straddles the 64-sample snap chunk, and a bitwise comparison.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <vector>

namespace pulp::test::filter_block {

inline std::vector<float> filters_busy_signal(std::size_t n, float sample_rate) {
    std::vector<float> s(n);
    std::uint32_t lcg = 0x13579bdu;
    for (std::size_t i = 0; i < n; ++i) {
        lcg = lcg * 1664525u + 1013904223u;
        const float noise = static_cast<float>(lcg >> 8) / 16777216.0f - 0.5f;
        const float t = static_cast<float>(i) / sample_rate;
        s[i] = 0.5f * std::sin(6.2831853f * 220.0f * t) +
               0.3f * std::sin(6.2831853f * 3150.0f * t) + 0.2f * noise;
    }
    return s;
}

inline std::vector<int> filters_split_schedule(std::size_t total) {
    static constexpr int kPattern[] = {1, 63, 64, 65, 7, 128, 333, 1024, 2, 512, 31};
    std::vector<int> blocks;
    std::size_t done = 0;
    for (std::size_t k = 0; done < total; ++k) {
        const int b = static_cast<int>(std::min<std::size_t>(
            static_cast<std::size_t>(kPattern[k % std::size(kPattern)]), total - done));
        blocks.push_back(b);
        done += static_cast<std::size_t>(b);
    }
    return blocks;
}

inline bool same_bits(const std::vector<float>& a, const std::vector<float>& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

} // namespace pulp::test::filter_block
