#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <vector>

namespace pulp::render {

/// Whole-recording GPU *render* time.
///
/// True per-pass GPU timing tied to Pulp's logical render passes is
/// architecturally blocked: Skia Graphite owns the Dawn command encoder and
/// every render-pass descriptor, so Pulp cannot inject per-pass
/// `timestampWrites`, and the encoder-level `WriteTimestamp` is gated behind
/// Dawn's `allow_unsafe_apis` toggle and disabled by Skia's `DawnCaps` on
/// Metal/Apple-silicon (Pulp's primary platform).
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

/// Nanoseconds per millisecond. Skia reports `results[1] - results[0]`
/// verbatim, but Dawn is not a passthrough: it scales raw ticks by a
/// per-device period and then, unless the toggle below is disabled, masks the
/// low 16 bits of every endpoint.
inline constexpr double kGpuRenderNanosecondsPerMillisecond = 1.0e6;

/// Dawn's `timestamp_quantization` toggle, which defaults to ON in native Dawn
/// (not only in Chrome) and ANDs every resolved timestamp with 0xFFFF0000 —
/// truncating each endpoint to a multiple of 65536 ns. It exists to stop
/// untrusted web content building a high-resolution timer; Pulp runs its own
/// shaders in its own process, so the mitigation buys nothing here and costs
/// the metric roughly three orders of magnitude of resolution.
///
/// Two consequences worth knowing if you ever read a sample taken with it on:
///   * Both endpoints truncate independently, so the error has zero mean. The
///     MEAN of many frames converges on the true duration; a min or a median
///     does not, at any sample count.
///   * A frame faster than 65536 ns reports exactly 0 and is discarded below
///     as "no sample", which preferentially drops the FAST frames and biases
///     even the mean upward.
///
/// Apple contributes no coarsening of its own: Apple Silicon GPU timestamps
/// are already nanoseconds, with a 41.67 ns (24 MHz) hardware granularity.
inline constexpr const char* kDawnTimestampQuantizationToggle = "timestamp_quantization";

/// Dawn toggles that must be DISABLED for the requested device features.
///
/// Requesting timestamp queries without disabling quantization yields samples
/// that are technically present and numerically useless.
[[nodiscard]] inline std::vector<const char*>
gpu_surface_disabled_toggles(bool timestamp_query_requested) {
    std::vector<const char*> disabled;
    if (timestamp_query_requested) {
        disabled.push_back(kDawnTimestampQuantizationToggle);
    }
    return disabled;
}

/// Convert a Graphite `GpuStats::elapsedTime` sample into a millisecond
/// duration. Returns `std::nullopt` — meaning "no usable sample this frame" —
/// when the finished-with-stats callback did not succeed, or when the elapsed
/// time is zero (Skia surfaces 0 when no pass was timestamped or the timer
/// setup failed — never a real sample). This guard is only correct while
/// quantization is disabled; with it on, a genuinely fast frame also reports
/// 0 and is wrongly discarded.
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
