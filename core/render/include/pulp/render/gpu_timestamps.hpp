#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "pulp/render/render_pass.hpp"

namespace pulp::render {

/// Phase 6.5 — Dawn GPU timestamp queries.
///
/// WebGPU exposes GPU-side timing through a `wgpu::QuerySet` of
/// `QueryType::Timestamp`. A render pass declares `timestampWrites`
/// (begin/end query indices); after the queue submits the encoder, the
/// timestamps are resolved into a buffer with `ResolveQuerySet` and
/// map-read back to the host as raw `uint64_t` GPU-clock ticks.
///
/// This header carries the *pure* part of that pipeline — the math that
/// turns resolved tick values into per-pass millisecond durations, plus
/// the feature-availability state machine. It deliberately has **no
/// Dawn dependency** so it can be unit-tested without a live GPU device:
/// a test feeds synthetic resolved-buffer values and asserts the
/// converted durations. The Dawn-specific QuerySet/buffer plumbing lives
/// in `gpu_timestamps.cpp` behind `PULP_HAS_WEBGPU`.
///
/// Two passes consume two timestamps each (begin + end), so a frame with
/// N passes needs a QuerySet of `2 * N` timestamp slots.

/// Number of timestamp-query slots a single render pass consumes
/// (one written at pass begin, one at pass end).
inline constexpr std::uint32_t kTimestampsPerPass = 2;

/// WebGPU timestamps are reported in **nanoseconds** — the spec fixes
/// the resolution at 1 ns regardless of backend. (Native WebGPU/Dawn
/// normalizes Metal/Vulkan/D3D12 hardware counters to ns for us, so
/// unlike raw Vulkan there is no per-device `timestampPeriod` to apply.)
inline constexpr double kNanosecondsPerMillisecond = 1.0e6;

/// Convert a raw begin/end GPU-timestamp pair (nanosecond ticks) into a
/// pass duration in milliseconds.
///
/// Returns `std::nullopt` — meaning "no usable sample" — when:
///   * `end < begin`. A backwards pair is never a real duration. It
///     happens when the query slot was never written (Dawn zero-fills
///     unwritten slots, but a stale resolve buffer can also surface a
///     half-written pair) or when the two timestamps straddle a GPU
///     counter wrap. Either way the value is garbage; the caller falls
///     back to the CPU number.
///   * either tick is zero. Slot zero-fill is Dawn's "not written"
///     sentinel; a genuine GPU clock is effectively never exactly 0.
[[nodiscard]] inline std::optional<double>
timestamp_pair_to_ms(std::uint64_t begin_ticks, std::uint64_t end_ticks) {
    if (begin_ticks == 0 || end_ticks == 0) {
        return std::nullopt;
    }
    if (end_ticks < begin_ticks) {
        return std::nullopt;
    }
    const std::uint64_t delta_ns = end_ticks - begin_ticks;
    return static_cast<double>(delta_ns) / kNanosecondsPerMillisecond;
}

/// Resolved GPU duration for a single pass.
struct PassGpuTiming {
    std::size_t pass_index = 0;   ///< Position within the frame's pass list.
    double      gpu_time_ms = 0;  ///< GPU-side duration (ms); valid only if `valid`.
    bool        valid = false;    ///< Whether `gpu_time_ms` is a real sample.
};

/// Convert a fully resolved timestamp buffer into per-pass durations.
///
/// `resolved` is the raw map-read of the timestamp buffer: `2 * N`
/// nanosecond ticks laid out as `[begin0, end0, begin1, end1, ...]`,
/// exactly the order `ResolveQuerySet` writes them. `pass_count` is the
/// number of passes the frame recorded.
///
/// The result always has `pass_count` entries (one per pass). A pass
/// whose pair fails `timestamp_pair_to_ms` gets `valid = false` rather
/// than being dropped, so the caller can still address passes by index.
/// If `resolved` is shorter than `2 * pass_count` (a truncated or
/// not-yet-ready readback), the missing tail passes are returned invalid.
[[nodiscard]] inline std::vector<PassGpuTiming>
resolve_pass_timings(const std::vector<std::uint64_t>& resolved,
                     std::size_t pass_count) {
    std::vector<PassGpuTiming> out;
    out.reserve(pass_count);
    for (std::size_t i = 0; i < pass_count; ++i) {
        PassGpuTiming t;
        t.pass_index = i;
        const std::size_t begin_idx = i * kTimestampsPerPass;
        const std::size_t end_idx = begin_idx + 1;
        if (end_idx < resolved.size()) {
            if (auto ms = timestamp_pair_to_ms(resolved[begin_idx],
                                               resolved[end_idx])) {
                t.gpu_time_ms = *ms;
                t.valid = true;
            }
        }
        out.push_back(t);
    }
    return out;
}

/// Apply resolved per-pass GPU durations onto a RenderPassManager.
///
/// The timestamps belong to the frame that submitted them, which — by
/// the time the readback completes — is the *previous* frame. The pass
/// list may have changed in the meantime; `RenderPassManager::
/// set_pass_gpu_time` silently ignores out-of-range indices, so a
/// shrunk pass list is safe. Invalid timings are skipped entirely so a
/// pass keeps `gpu_time_valid = false` and the inspector shows the
/// honest "unavailable" state for it.
inline void apply_pass_timings(RenderPassManager& rpm,
                               const std::vector<PassGpuTiming>& timings) {
    for (const auto& t : timings) {
        if (t.valid) {
            rpm.set_pass_gpu_time(t.pass_index,
                                  static_cast<float>(t.gpu_time_ms));
        }
    }
}

/// Lifecycle state of the GPU-timestamp subsystem.
///
/// `timestamp-query` is an *optional* WebGPU feature. It must be both
/// advertised by the adapter and requested at device creation; some
/// adapters (older mobile GPUs, software fallbacks) never offer it. The
/// resolver degrades gracefully: when the feature is absent the
/// inspector reports "GPU timestamps unavailable" and keeps showing CPU
/// time — it never crashes and never blocks rendering.
enum class GpuTimestampSupport {
    unknown,      ///< Not yet probed (device not created).
    unsupported,  ///< Adapter lacks the `timestamp-query` feature.
    supported,    ///< Feature present and the QuerySet was created.
};

/// Human-readable label for a support state — used in the inspector
/// perf readout and in log lines.
[[nodiscard]] inline const char* describe(GpuTimestampSupport s) {
    switch (s) {
        case GpuTimestampSupport::unknown:     return "GPU timestamps: not probed";
        case GpuTimestampSupport::unsupported: return "GPU timestamps unavailable";
        case GpuTimestampSupport::supported:   return "GPU timestamps active";
    }
    return "GPU timestamps: invalid state";
}

/// Returns true when GPU timestamps can actually produce numbers.
[[nodiscard]] inline bool is_usable(GpuTimestampSupport s) {
    return s == GpuTimestampSupport::supported;
}

/// Dawn-backed GPU timestamp collector for a frame's render passes.
///
/// Owns the `wgpu::QuerySet` and the resolve/map-read buffers. The
/// public surface here is Dawn-free; the implementation
/// (`gpu_timestamps.cpp`) is compiled with the real Dawn types only
/// when `PULP_HAS_WEBGPU` is defined. In a CPU-only build every method
/// is a safe no-op and `support()` stays `unsupported`.
///
/// Intended per-frame usage (driven by the GPU surface / render loop):
///   1. `begin_frame(pass_count)` — (re)size the QuerySet for the frame.
///   2. For each pass, `timestamp_writes(i)` supplies the
///      `timestampWrites` for that pass's render-pass descriptor.
///   3. `resolve(encoder_handle)` — append `ResolveQuerySet` + copy.
///   4. After the queue submits and the buffer maps, `read_back()`
///      returns the resolved ticks; feed them through
///      `resolve_pass_timings` + `apply_pass_timings`.
///
/// The 1-frame readback lag is inherent to GPU timestamp queries and is
/// accepted by the spike spec (Phase 6.5, "1-frame lag is unavoidable").
class GpuTimestamps {
public:
    GpuTimestamps();
    ~GpuTimestamps();

    GpuTimestamps(const GpuTimestamps&) = delete;
    GpuTimestamps& operator=(const GpuTimestamps&) = delete;

    /// Probe + initialize against a Dawn device handle (a `wgpu::Device*`
    /// erased to `void*`, matching `GpuSurface::dawn_device_handle()`).
    /// Returns true only when the `timestamp-query` feature is present
    /// and the QuerySet was created. Safe to call with `nullptr` (then
    /// it just reports `unsupported`).
    bool initialize(void* dawn_device_handle);

    /// Current support state — drives the inspector's CPU-vs-GPU UI.
    [[nodiscard]] GpuTimestampSupport support() const { return support_; }

    /// Convenience: are GPU timestamps live this run?
    [[nodiscard]] bool available() const { return is_usable(support_); }

    /// Number of passes the QuerySet is currently sized for.
    [[nodiscard]] std::size_t pass_capacity() const { return pass_count_; }

    /// (Re)size the QuerySet for a frame of `pass_count` passes. No-op
    /// when unsupported or when the size is unchanged.
    void begin_frame(std::size_t pass_count);

    /// Read back the most recently resolved timestamp buffer as raw
    /// nanosecond ticks (`[begin0, end0, begin1, end1, ...]`). Empty
    /// when unsupported or when no frame has resolved yet.
    [[nodiscard]] std::vector<std::uint64_t> read_back() const;

private:
    struct Impl;
    Impl* impl_ = nullptr;  ///< Dawn-owning state; nullptr in CPU-only builds.
    GpuTimestampSupport support_ = GpuTimestampSupport::unknown;
    std::size_t pass_count_ = 0;
};

} // namespace pulp::render
