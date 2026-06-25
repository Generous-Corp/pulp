#pragma once

/// @file perf_counters.hpp
/// Benchmark perf counters for copy/upload/readback costs.
///
/// This header is compiled out entirely when `PULP_BENCHMARK` is not defined,
/// so the production audio/UI paths see zero overhead by default.
///
/// Counters accumulate microseconds (or byte counts) via
/// `std::atomic<double>::fetch_add`. Writers on different threads (audio
/// thread, UI thread, GPU poll thread) can all accumulate concurrently
/// without a lock. A periodic consumer calls `snapshot_and_reset()` to
/// grab the rolling totals and zero them for the next window.
///
/// Usage:
///   #ifdef PULP_BENCHMARK
///   using namespace pulp::render::bench;
///   double t0 = now_us();
///   // ... do work ...
///   counters.audio_copy_total_us.fetch_add(
///       now_us() - t0, std::memory_order_relaxed);
///   #endif

#ifdef PULP_BENCHMARK

#include <atomic>
#include <chrono>

namespace pulp::render::bench {

/// High-resolution wall clock reading, in microseconds, as a double.
/// Resolution is platform-dependent; on macOS/iOS this backs onto
/// `mach_absolute_time` via `std::chrono::high_resolution_clock`.
static inline double now_us() {
    using Clock = std::chrono::high_resolution_clock;
    return static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch()).count()) / 1000.0;
}

/// Atomic accumulators for per-frame copy and upload costs.
///
/// All fields are `std::atomic<double>` (C++20 adds `fetch_add` for
/// floating-point atomics). Writers use `std::memory_order_relaxed` —
/// these are monitoring counters, not synchronization primitives.
struct PerfCounters {
    // Per-frame latency buckets (microseconds, accumulated across samples).
    std::atomic<double> audio_copy_total_us{0.0};
    std::atomic<double> triplebuffer_publish_total_us{0.0};
    std::atomic<double> gpu_upload_total_us{0.0};
    std::atomic<double> gpu_readback_total_us{0.0};
    std::atomic<double> gpu_dispatch_total_us{0.0};
    std::atomic<double> total_frame_total_us{0.0};

    // Per-frame bandwidth buckets (bytes, accumulated across samples).
    std::atomic<double> cpu_to_gpu_bytes_total{0.0};
    std::atomic<double> gpu_to_cpu_bytes_total{0.0};

    // Number of samples (frames) rolled into the above totals.
    std::atomic<double> sample_count{0.0};

    // JS→GPU upload instrumentation. Captures the JS-driven WebGPU resource
    // setup path in `core/view/src/widget_bridge.cpp`: base64 decode cost,
    // the number of WriteBuffer calls (so per-upload cost is derivable from
    // `gpu_upload_total_us / gpu_buffer_upload_count`), and the peak resident
    // GPU-buffer memory so we can tell whether a workload actually stresses
    // the bridge.
    //
    // The peak is tracked via a `compare_exchange` loop in
    // `observe_resident_peak` so concurrent writers stay monotonic
    // without a lock.
    std::atomic<double> base64_decode_total_us{0.0};
    std::atomic<double> gpu_buffer_upload_count{0.0};
    std::atomic<double> gpu_buffer_bytes_resident_peak{0.0};

    /// Zero every counter. Primarily for tests.
    void reset() {
        audio_copy_total_us.store(0.0, std::memory_order_relaxed);
        triplebuffer_publish_total_us.store(0.0, std::memory_order_relaxed);
        gpu_upload_total_us.store(0.0, std::memory_order_relaxed);
        gpu_readback_total_us.store(0.0, std::memory_order_relaxed);
        gpu_dispatch_total_us.store(0.0, std::memory_order_relaxed);
        total_frame_total_us.store(0.0, std::memory_order_relaxed);
        cpu_to_gpu_bytes_total.store(0.0, std::memory_order_relaxed);
        gpu_to_cpu_bytes_total.store(0.0, std::memory_order_relaxed);
        sample_count.store(0.0, std::memory_order_relaxed);
        base64_decode_total_us.store(0.0, std::memory_order_relaxed);
        gpu_buffer_upload_count.store(0.0, std::memory_order_relaxed);
        gpu_buffer_bytes_resident_peak.store(0.0, std::memory_order_relaxed);
    }

    /// Monotonic peak update for `gpu_buffer_bytes_resident_peak`. Safe
    /// to call concurrently from multiple threads. No-op if `candidate`
    /// is not larger than the current peak.
    void observe_resident_peak(double candidate) {
        double prev = gpu_buffer_bytes_resident_peak.load(std::memory_order_relaxed);
        while (candidate > prev &&
               !gpu_buffer_bytes_resident_peak.compare_exchange_weak(
                   prev, candidate, std::memory_order_relaxed)) {
            // `prev` is refreshed by `compare_exchange_weak` on failure;
            // loop until we either succeed or the candidate is no
            // longer larger.
        }
    }

    /// Plain-old-data snapshot of the counters. Returned by
    /// `snapshot_and_reset()`; safe to copy, serialize, or inspect in
    /// tests.
    struct Snapshot {
        double audio_copy_total_us = 0.0;
        double triplebuffer_publish_total_us = 0.0;
        double gpu_upload_total_us = 0.0;
        double gpu_readback_total_us = 0.0;
        double gpu_dispatch_total_us = 0.0;
        double total_frame_total_us = 0.0;
        double cpu_to_gpu_bytes_total = 0.0;
        double gpu_to_cpu_bytes_total = 0.0;
        double sample_count = 0.0;
        double base64_decode_total_us = 0.0;
        double gpu_buffer_upload_count = 0.0;
        double gpu_buffer_bytes_resident_peak = 0.0;
    };

    /// Grab the current totals and zero the underlying atomics. Uses
    /// `exchange` so no samples are lost between the read and reset.
    /// `gpu_buffer_bytes_resident_peak` is exchanged as well, so a
    /// fresh window starts from zero and tracks its own peak.
    Snapshot snapshot_and_reset() {
        Snapshot s;
        s.audio_copy_total_us =
            audio_copy_total_us.exchange(0.0, std::memory_order_relaxed);
        s.triplebuffer_publish_total_us =
            triplebuffer_publish_total_us.exchange(0.0, std::memory_order_relaxed);
        s.gpu_upload_total_us =
            gpu_upload_total_us.exchange(0.0, std::memory_order_relaxed);
        s.gpu_readback_total_us =
            gpu_readback_total_us.exchange(0.0, std::memory_order_relaxed);
        s.gpu_dispatch_total_us =
            gpu_dispatch_total_us.exchange(0.0, std::memory_order_relaxed);
        s.total_frame_total_us =
            total_frame_total_us.exchange(0.0, std::memory_order_relaxed);
        s.cpu_to_gpu_bytes_total =
            cpu_to_gpu_bytes_total.exchange(0.0, std::memory_order_relaxed);
        s.gpu_to_cpu_bytes_total =
            gpu_to_cpu_bytes_total.exchange(0.0, std::memory_order_relaxed);
        s.sample_count =
            sample_count.exchange(0.0, std::memory_order_relaxed);
        s.base64_decode_total_us =
            base64_decode_total_us.exchange(0.0, std::memory_order_relaxed);
        s.gpu_buffer_upload_count =
            gpu_buffer_upload_count.exchange(0.0, std::memory_order_relaxed);
        s.gpu_buffer_bytes_resident_peak =
            gpu_buffer_bytes_resident_peak.exchange(0.0, std::memory_order_relaxed);
        return s;
    }
};

} // namespace pulp::render::bench

#endif // PULP_BENCHMARK
