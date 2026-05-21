#pragma once

#include <atomic>
#include <cstdint>
#include <optional>

namespace pulp::render {

/// Phase 6.5 (re-scoped) — whole-recording GPU *render* time.
///
/// True per-pass GPU timing tied to Pulp's logical render passes is
/// architecturally blocked: Skia Graphite owns the Dawn command encoder and
/// every render-pass descriptor, so Pulp cannot inject per-pass
/// `timestampWrites`, and the encoder-level `WriteTimestamp` is gated behind
/// Dawn's `allow_unsafe_apis` toggle and disabled by Skia's `DawnCaps` on
/// Metal/Apple-silicon (Pulp's primary platform). See
/// `planning/2026-05-21-gpu-timestamp-readback-proposal.md`.
///
/// What IS available — and correct on every backend — is Skia Graphite's own
/// GPU-stats API: `InsertRecordingInfo::fGpuStatsFlags = kElapsedTime` plus a
/// finished-with-stats callback that hands back a `GpuStats{elapsedTime}` in
/// nanoseconds once the GPU finishes the recording. `SkiaSurface` wires that
/// callback; this header carries the *pure*, Dawn-free seam — the ns→ms
/// conversion + the cross-thread latest-sample holder — so it can be unit
/// tested without a live GPU device.
///
/// Naming note: it is "GPU render time", not total frame time. On the Metal
/// fallback Skia measures first-pass-begin → last-pass-end of the recording
/// and excludes non-pass work (texture uploads/copies) and present.

/// Nanoseconds per millisecond. WebGPU timestamps resolve in nanoseconds and
/// Skia reports `results[1] - results[0]` directly, so no per-device period.
inline constexpr double kGpuRenderNanosecondsPerMillisecond = 1.0e6;

/// Convert a Graphite `GpuStats::elapsedTime` sample into a millisecond
/// duration. Returns `std::nullopt` — meaning "no usable sample this frame" —
/// when the finished-with-stats callback did not succeed, or when the elapsed
/// time is zero (Skia surfaces 0 when no pass was timestamped, the timer setup
/// failed, or the work was effectively zero/quantized — never a real sample).
[[nodiscard]] inline std::optional<double>
gpu_render_ns_to_ms(std::uint64_t elapsed_ns, bool callback_ok) {
    if (!callback_ok || elapsed_ns == 0) {
        return std::nullopt;
    }
    return static_cast<double>(elapsed_ns) / kGpuRenderNanosecondsPerMillisecond;
}

/// Thread-safe holder for the latest GPU render-time sample.
///
/// The Graphite finished-with-stats callback fires on whatever thread pumps
/// GPU completion (the render thread, via `Context::submit`), while the
/// inspector reads from the UI thread. A scalar duration + a "have a sample
/// yet" flag are independent atomics; a stale read across the pair is
/// harmless (it only ever shows a slightly older valid duration). Relaxed
/// ordering is sufficient — there is no other state these guard.
class GpuRenderTimeTracker {
public:
    /// Store a sample from the finished-with-stats callback. Ignored when the
    /// callback failed or the elapsed time is zero (so the last good sample is
    /// retained rather than being clobbered by a "no sample" frame).
    void store(std::uint64_t elapsed_ns, bool callback_ok) {
        if (auto ms = gpu_render_ns_to_ms(elapsed_ns, callback_ok)) {
            last_ms_.store(*ms, std::memory_order_relaxed);
            have_sample_.store(true, std::memory_order_relaxed);
        }
    }

    /// True once at least one valid sample has landed. Lets the inspector
    /// distinguish "supported, waiting for the first sample" from "0 ms".
    [[nodiscard]] bool have_sample() const {
        return have_sample_.load(std::memory_order_relaxed);
    }

    /// The most recent valid GPU render duration in milliseconds (0 until the
    /// first sample lands).
    [[nodiscard]] double last_ms() const {
        return last_ms_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<double> last_ms_{0.0};
    std::atomic<bool> have_sample_{false};
};

} // namespace pulp::render
