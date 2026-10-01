#pragma once

/// @file rt_work_counter.hpp
/// Deterministic operation counters for audio-callback cost gates.
///
/// Offline renders have no deadline, so a callback that bursts expensive work
/// on a state transition (a latch, a mode switch, a preset load) renders
/// perfectly offline and drops a buffer in a DAW. Wall-clock timing can see
/// that burst but flakes on shared runners; counting the expensive operations
/// cannot flake. These counters are what a required-lane transition gate reads.
///
/// Counted operations:
/// - `fft`  — one per FftT / MultiBackendFft forward, inverse, or real-forward
///            transform, regardless of size.
/// - `trig` — one per call to the `rt::polar` / `rt::sin` / `rt::cos` /
///            `rt::exp` / `rt::arg` wrappers below. DSP that wants its
///            transcendental cost visible to the gate calls these instead of
///            the bare `std::` functions; the result is bit-identical.
/// - `bins` — per-bin accumulation work a spectral processor reports itself
///            through `count_bins(n)` (one unit per frame × channel × bin
///            visited), for work that is cheap per element but can be
///            multiplied by a long window in one callback.
///
/// Build contract: counting compiles in only when `PULP_RT_WORK_COUNTERS` is
/// non-zero. The root CMake option of the same name defines it for every
/// translation unit of a build at once (default: ON when tests are built, OFF
/// otherwise), so the inline hooks below never disagree between TUs. With the
/// option OFF every hook is an empty inline function and the wrappers are the
/// bare `std::` calls.
///
/// Storage is process-wide relaxed atomics rather than thread-locals: on Darwin
/// the first access to a `thread_local` from a new thread can allocate, which
/// would put a malloc on the first audio callback of a counted build. The
/// consequence is attribution: a delta measured around a callback includes
/// work done concurrently on other threads. Gates therefore measure a
/// single-threaded offline render, where the render thread is the only
/// thread doing counted work.
///
/// RT contract: every hook is one relaxed `fetch_add` (or nothing); no
/// allocation, no lock. `work_counts()` is a handful of relaxed loads.

#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>

#ifndef PULP_RT_WORK_COUNTERS
#define PULP_RT_WORK_COUNTERS 0
#endif

namespace pulp::signal::rt {

/// True when this build counts operations. A gate that reads counts must
/// check this first: with counting compiled out every delta is zero, which
/// would read as "no work" rather than "not measured".
inline constexpr bool kWorkCountersEnabled = PULP_RT_WORK_COUNTERS != 0;

/// A snapshot (or a difference of two snapshots) of the counted operations.
struct RtWorkCounts {
    std::uint64_t fft = 0;
    std::uint64_t trig = 0;
    std::uint64_t bins = 0;

    friend constexpr RtWorkCounts operator-(RtWorkCounts a,
                                            RtWorkCounts b) noexcept {
        return {a.fft - b.fft, a.trig - b.trig, a.bins - b.bins};
    }
    friend constexpr RtWorkCounts operator+(RtWorkCounts a,
                                            RtWorkCounts b) noexcept {
        return {a.fft + b.fft, a.trig + b.trig, a.bins + b.bins};
    }
    friend constexpr bool operator==(RtWorkCounts, RtWorkCounts) = default;
};

namespace detail {
#if PULP_RT_WORK_COUNTERS
inline std::atomic<std::uint64_t> g_fft_count{0};
inline std::atomic<std::uint64_t> g_trig_count{0};
inline std::atomic<std::uint64_t> g_bins_count{0};
#endif
} // namespace detail

/// Record one FFT execution.
inline void count_fft() noexcept {
#if PULP_RT_WORK_COUNTERS
    detail::g_fft_count.fetch_add(1, std::memory_order_relaxed);
#endif
}

/// Record `n` transcendental evaluations.
inline void count_trig(std::uint64_t n = 1) noexcept {
#if PULP_RT_WORK_COUNTERS
    detail::g_trig_count.fetch_add(n, std::memory_order_relaxed);
#else
    (void)n;
#endif
}

/// Record `n` units of per-bin accumulation work.
inline void count_bins(std::uint64_t n) noexcept {
#if PULP_RT_WORK_COUNTERS
    detail::g_bins_count.fetch_add(n, std::memory_order_relaxed);
#else
    (void)n;
#endif
}

/// Current process-wide totals. Take one before and one after the work of
/// interest and subtract.
inline RtWorkCounts work_counts() noexcept {
#if PULP_RT_WORK_COUNTERS
    return {detail::g_fft_count.load(std::memory_order_relaxed),
            detail::g_trig_count.load(std::memory_order_relaxed),
            detail::g_bins_count.load(std::memory_order_relaxed)};
#else
    return {};
#endif
}

/// Measures the counted work done between construction and `delta()`.
class RtWorkCounter {
public:
    RtWorkCounter() noexcept : start_(work_counts()) {}
    RtWorkCounts delta() const noexcept { return work_counts() - start_; }
    void restart() noexcept { start_ = work_counts(); }

private:
    RtWorkCounts start_;
};

/// Counted `std::polar`. Bit-identical result.
template <typename T>
inline std::complex<T> polar(T magnitude, T phase) noexcept {
    count_trig();
    return std::polar(magnitude, phase);
}

/// Counted `std::sin`. Bit-identical result.
template <typename T>
inline T sin(T x) noexcept {
    count_trig();
    return std::sin(x);
}

/// Counted `std::cos`. Bit-identical result.
template <typename T>
inline T cos(T x) noexcept {
    count_trig();
    return std::cos(x);
}

/// Counted `std::exp`. Bit-identical result.
template <typename T>
inline T exp(T x) noexcept {
    count_trig();
    return std::exp(x);
}

/// Counted `std::arg` (an atan2). Bit-identical result.
template <typename T>
inline T arg(const std::complex<T>& z) noexcept {
    count_trig();
    return std::arg(z);
}

} // namespace pulp::signal::rt
