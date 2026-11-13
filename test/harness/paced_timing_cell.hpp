#pragma once

// Fixed-origin paced callback cell shared by the graph paced-timing matrix.
//
// A matrix cell drives an already-prepared fixture from one callback thread on
// a fixed schedule: callback i targets origin + i * frames / rate and a late
// callback never resets the origin. The harness records raw steady-clock
// offsets into preallocated storage and prints them only after the last
// callback, so the timed window contains no I/O. Percentiles, deadline misses
// and the qualification verdict are computed by
// tools/graph_paced_timing_matrix.py from the raw rows, not here.
//
// The cell is configured through environment variables so one hidden Catch2
// case per fixture can serve every matrix cell:
//   PULP_PACED_VARIANT       fixture-specific variant name
//   PULP_PACED_RATE          sample rate in Hz (default 48000)
//   PULP_PACED_FRAMES        frames per callback
//   PULP_PACED_CALLBACKS     callback count, 1..100000 (default 32)
//   PULP_PACED_PLANTED_SLEEP_US  planted negative: sleep inside the timed
//                                callback window (default 0)
//   PULP_PACED_PLANTED_EVERY     apply the planted sleep to every Nth callback

#include <pulp/runtime/build_info.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace pulp::test::paced {

inline constexpr std::string_view kCellHeader = "pulp_paced_timing_cell_v1";
inline constexpr std::uint64_t kMaxCallbacks = 100000;

struct CellConfig {
    std::string variant;
    std::uint64_t rate = 48000;
    std::uint64_t frames = 0;
    std::uint64_t callbacks = 32;
    std::uint64_t planted_sleep_us = 0;
    std::uint64_t planted_every = 1;
};

struct CellRow {
    std::int64_t target_ns = 0;
    std::int64_t start_ns = 0;
    std::int64_t end_ns = 0;
    std::uint8_t ok = 0;
};

inline std::uint64_t env_integer(const char* key, std::uint64_t fallback) {
    const char* text = std::getenv(key);
    if (text == nullptr || *text == '\0')
        return fallback;
    char* end = nullptr;
    const auto value = std::strtoull(text, &end, 10);
    if (end == text || *end != '\0')
        throw std::invalid_argument(std::string("malformed ") + key);
    return static_cast<std::uint64_t>(value);
}

// Unset variables fall back to the fixture's defaults, so running a cell case
// directly (or through a tag filter) is a short smoke run rather than an error.
inline CellConfig read_config(std::string_view default_variant, std::uint64_t default_frames) {
    CellConfig config;
    const char* variant = std::getenv("PULP_PACED_VARIANT");
    config.variant = (variant == nullptr || *variant == '\0') ? std::string(default_variant)
                                                              : std::string(variant);
    config.rate = env_integer("PULP_PACED_RATE", 48000);
    config.frames = env_integer("PULP_PACED_FRAMES", default_frames);
    config.callbacks = env_integer("PULP_PACED_CALLBACKS", 32);
    config.planted_sleep_us = env_integer("PULP_PACED_PLANTED_SLEEP_US", 0);
    config.planted_every = env_integer("PULP_PACED_PLANTED_EVERY", 1);
    if (config.rate == 0 || config.frames == 0 || config.callbacks == 0 ||
        config.callbacks > kMaxCallbacks || config.planted_every == 0)
        throw std::invalid_argument("paced cell configuration out of range");
    return config;
}

// Runs `process(index, first_sample)` on a dedicated thread at the fixed-origin
// schedule and times only that call (plus any planted sleep). `verify(index,
// first_sample)` then checks the block outside the timed window and returns the
// fixture's own oracle verdict; it still shares the callback period, so keep
// it table-driven and cheap. Returns the raw rows.
template <class Process, class Verify>
std::vector<CellRow> run_cell(const CellConfig& config, Process&& process, Verify&& verify) {
    using clock = std::chrono::steady_clock;
    std::vector<CellRow> rows(static_cast<std::size_t>(config.callbacks));
    const auto planted = std::chrono::microseconds(config.planted_sleep_us);
    std::thread worker([&] {
        const auto origin = clock::now() + std::chrono::milliseconds(10);
        const auto offset = [origin](clock::time_point point) {
            return static_cast<std::int64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(point - origin).count());
        };
        for (std::uint64_t i = 0; i < config.callbacks; ++i) {
            // Integer schedule arithmetic: no accumulated floating drift.
            const auto target_ns = static_cast<std::int64_t>(
                i * config.frames * 1'000'000'000ull / config.rate);
            const auto target = origin + std::chrono::nanoseconds(target_ns);
            std::this_thread::sleep_until(target);
            const auto start = clock::now();
            process(static_cast<std::size_t>(i), i * config.frames);
            if (config.planted_sleep_us != 0 && i % config.planted_every == 0)
                std::this_thread::sleep_for(planted);
            const auto end = clock::now();
            const bool ok = verify(static_cast<std::size_t>(i), i * config.frames);
            auto& row = rows[static_cast<std::size_t>(i)];
            row.target_ns = target_ns;
            row.start_ns = offset(start);
            row.end_ns = offset(end);
            row.ok = ok ? 1 : 0;
        }
    });
    worker.join();
    return rows;
}

// Prints one cell in the line format parsed by tools/graph_paced_timing_matrix.py
// and returns the number of blocks whose fixture oracle failed.
inline std::uint64_t print_cell(std::string_view fixture, const CellConfig& config,
                       const std::vector<CellRow>& rows, std::string_view extra = {}) {
    std::uint64_t oracle_failures = 0;
    for (const auto& row : rows)
        oracle_failures += row.ok ? 0 : 1;
    std::cout << kCellHeader << '\n'
              << "PACED_CELL fixture=" << fixture << " variant=" << config.variant
              << " rate=" << config.rate << " frames=" << config.frames
              << " callbacks=" << config.callbacks
              << " planted_sleep_us=" << config.planted_sleep_us
              << " planted_every=" << config.planted_every
              << " compiled_source=" << pulp::runtime::kGitSha
              << " build_type=" << pulp::runtime::kBuildType
              << " build_dirty=" << pulp::runtime::kGitDirty;
    if (!extra.empty())
        std::cout << ' ' << extra;
    std::cout << '\n';
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& row = rows[i];
        std::cout << "P " << i << ' ' << row.target_ns << ' ' << row.start_ns << ' '
                  << row.end_ns << ' ' << static_cast<int>(row.ok) << '\n';
    }
    std::cout << "PACED_END rows=" << rows.size() << " oracle_failures=" << oracle_failures
              << '\n';
    return oracle_failures;
}

} // namespace pulp::test::paced
