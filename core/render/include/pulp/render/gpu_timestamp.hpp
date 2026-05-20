#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace pulp::render {

/// GPU timestamp-query resolution — Phase 6.5 of the inspector roadmap
/// (planning/2026-05-19-inspector-phase6-gpu-perf-spike.md § Phase 6.5).
///
/// Phase 6.1 surfaced CPU wall-time around each render pass's draw calls
/// (`PassStats::cpu_time_ms`). That number is an honest *upper bound* on
/// GPU cost but hides the actual GPU-side execution time — the place perf
/// bugs usually hide. This module adds the real GPU clock.
///
/// WebGPU exposes GPU timing via `wgpu::QueryType::Timestamp`. The flow:
///
///   1. Request the `timestamp-query` feature at device creation. It may
///      be unavailable on some adapters — degrade gracefully (report
///      "unavailable", keep CPU numbers) rather than crashing.
///   2. Allocate a `wgpu::QuerySet` with two timestamp slots per pass
///      (beginning + end of pass), wired via `PassTimestampWrites` on the
///      render-pass descriptor.
///   3. `CommandEncoder::ResolveQuerySet` copies the raw GPU ticks into a
///      buffer; map-read it back to host memory the *following* frame
///      (one frame of lag is unavoidable — the GPU hasn't finished the
///      current frame when we want to read it).
///   4. Convert raw ticks → nanoseconds → milliseconds.
///
/// `GpuTimestampResolver` below is the **pure, GPU-free** half: it owns
/// the tick → millisecond conversion math and the validity rules. It is
/// deliberately decoupled from Dawn so the resolution logic is unit-
/// testable without a live device (feed it a mocked resolved buffer).
/// The Dawn-coupled query-set lifecycle lives in `GpuPassTimer`
/// (gpu_timestamp.cpp, compiled only when WebGPU is present).

/// A resolved GPU timing for one render pass, in milliseconds.
struct GpuPassTiming {
    /// GPU-side duration of the pass. Only meaningful when `valid`.
    double gpu_time_ms = 0.0;
    /// False when the timestamp pair could not be resolved this frame
    /// (feature absent, query slot never written, or non-monotonic
    /// ticks — see `GpuTimestampResolver::resolve_pair`).
    bool valid = false;
};

/// Converts raw WebGPU timestamp-query ticks into millisecond durations.
///
/// WebGPU timestamps are reported in *nanoseconds* per the spec
/// (`timestamp-query` feature: "the values are in nanoseconds"), so the
/// per-tick period is fixed at 1.0 ns. Some native backends historically
/// reported in device-specific ticks and needed a `queue.timestampPeriod`
/// scale factor; the resolver keeps that factor configurable so a backend
/// that does not normalise to nanoseconds can still be handled, and so the
/// conversion is independently testable.
class GpuTimestampResolver {
public:
    /// `nanoseconds_per_tick` scales a raw tick delta to nanoseconds.
    /// WebGPU normalises timestamps to nanoseconds, so the default is 1.0.
    explicit GpuTimestampResolver(double nanoseconds_per_tick = 1.0)
        : ns_per_tick_(nanoseconds_per_tick > 0.0 ? nanoseconds_per_tick
                                                  : 1.0) {}

    /// The active tick → nanosecond scale factor.
    double nanoseconds_per_tick() const { return ns_per_tick_; }

    /// Whether the GPU `timestamp-query` feature is available. When false,
    /// every `resolve_*` call returns an invalid timing and callers should
    /// fall back to the CPU numbers from Phase 6.1.
    bool feature_available() const { return feature_available_; }
    void set_feature_available(bool available) {
        feature_available_ = available;
    }

    /// Convert a raw tick count to milliseconds. Pure arithmetic; does not
    /// consult `feature_available()` — use `resolve_pair` for the gated
    /// path. Provided so the conversion is directly unit-testable.
    double ticks_to_ms(uint64_t ticks) const {
        return (static_cast<double>(ticks) * ns_per_tick_) / 1.0e6;
    }

    /// Resolve a begin/end timestamp pair into a pass duration.
    ///
    /// Returns an invalid timing when:
    ///   - the `timestamp-query` feature is unavailable, OR
    ///   - `end_ticks < begin_ticks` (non-monotonic — a query slot was
    ///     never written, or the GPU clock wrapped; the spec does not
    ///     guarantee monotonicity across a wrap so we treat it as a miss).
    ///
    /// A zero-length pass (`end == begin`) is valid: GPU passes can resolve
    /// to 0 when the draw work is trivially cheap.
    GpuPassTiming resolve_pair(uint64_t begin_ticks,
                               uint64_t end_ticks) const {
        GpuPassTiming timing;
        if (!feature_available_) {
            return timing;  // valid = false
        }
        if (end_ticks < begin_ticks) {
            return timing;  // non-monotonic → miss
        }
        timing.gpu_time_ms = ticks_to_ms(end_ticks - begin_ticks);
        timing.valid = true;
        return timing;
    }

    /// Resolve a buffer of interleaved [begin, end, begin, end, ...]
    /// timestamps — the exact layout `wgpu::CommandEncoder::ResolveQuerySet`
    /// produces when each pass writes a beginning + end query.
    ///
    /// `resolved_ticks` must hold `pass_count * 2` entries. A short or
    /// odd-length buffer yields all-invalid timings for the missing tail
    /// rather than reading out of bounds.
    std::vector<GpuPassTiming> resolve_passes(
        const std::vector<uint64_t>& resolved_ticks,
        std::size_t pass_count) const {
        std::vector<GpuPassTiming> out(pass_count);
        for (std::size_t i = 0; i < pass_count; ++i) {
            const std::size_t begin_idx = i * 2;
            const std::size_t end_idx = begin_idx + 1;
            if (end_idx >= resolved_ticks.size()) {
                break;  // remaining entries stay invalid
            }
            out[i] = resolve_pair(resolved_ticks[begin_idx],
                                  resolved_ticks[end_idx]);
        }
        return out;
    }

private:
    double ns_per_tick_;
    bool feature_available_ = false;
};

/// The WebGPU feature name a device must request for GPU timestamp
/// queries. Kept as a plain string so non-render code (the inspector,
/// tests) can refer to it without pulling in Dawn headers.
inline constexpr const char* kTimestampQueryFeature = "timestamp-query";

/// Owns the Dawn `wgpu::QuerySet` + resolve/readback buffers for one
/// frame's worth of per-pass GPU timestamps.
///
/// This is the Dawn-coupled half of Phase 6.5 — the half that cannot be
/// unit-tested without a live device. It is implemented in
/// gpu_timestamp.cpp and only built when WebGPU is present; on a
/// WebGPU-less build the factory returns nullptr and callers fall back
/// to CPU timings. The handles are intentionally opaque (`void*`,
/// interpreted as Dawn C++ types inside the .cpp) so this header has no
/// Dawn dependency — the same pattern `GpuSurface` uses.
///
/// Lifecycle per frame:
///   - `timestamp_writes_for_pass(i)` → a `wgpu::PassTimestampWrites*`
///     to attach to render-pass descriptor i (begin + end slots).
///   - after encoding all passes: `resolve(encoder)` records a
///     `ResolveQuerySet` + copy into the map-readable buffer.
///   - `read_back(resolver)` map-reads the *previous* frame's buffer and
///     returns resolved `GpuPassTiming`s (one frame of lag).
class GpuPassTimer {
public:
    virtual ~GpuPassTimer() = default;

    /// Number of render passes this timer was sized for.
    virtual std::size_t pass_capacity() const = 0;

    /// Whether the device actually supports timestamp queries. When
    /// false, every other method is a safe no-op and `read_back` returns
    /// all-invalid timings.
    virtual bool supported() const = 0;

    /// Opaque `wgpu::PassTimestampWrites*` for render pass `pass_index`,
    /// or nullptr when unsupported / out of range. Lives as long as the
    /// timer; do not free.
    virtual void* timestamp_writes_for_pass(std::size_t pass_index) = 0;

    /// Record `ResolveQuerySet` + a copy into the readback buffer on the
    /// given opaque `wgpu::CommandEncoder*`. No-op when unsupported.
    virtual void resolve(void* command_encoder) = 0;

    /// Map-read the previous frame's resolved buffer and convert to
    /// per-pass GPU durations. Returns `pass_capacity()` entries; all
    /// invalid when unsupported or when the buffer has not yet been
    /// populated (first frame). Safe to call every frame.
    virtual std::vector<GpuPassTiming> read_back(
        const GpuTimestampResolver& resolver) = 0;

    /// Construct a timer for `pass_count` passes against the opaque
    /// `wgpu::Device*` / `wgpu::Queue*` handles from `GpuSurface`.
    /// Returns nullptr on a WebGPU-less build. The returned timer
    /// reports `supported() == false` when the device lacks the
    /// `timestamp-query` feature — callers must check.
    static std::unique_ptr<GpuPassTimer> create(void* dawn_device,
                                                 void* dawn_queue,
                                                 std::size_t pass_count);
};

}  // namespace pulp::render
