#pragma once

/// @file gpu_acquire_diagnostics.hpp
/// What a window host records about a swapchain acquire, so a trace can say
/// why `gpu_acquire` waited.
///
/// Acquire blocks when every drawable is held: on Metal, `nextDrawable` waits
/// for one of the layer's drawables (three by default) to come back from the
/// compositor. That happens for two different reasons, and they need opposite
/// fixes:
///
///   * the CPU presented frames faster than the display consumed them (two
///     frames landing in one refresh interval fill the queue), which shows as
///     a frame that started early or on time relative to its display-link
///     target with few frames still on the GPU;
///   * the GPU is behind (a frame's GPU work outruns the refresh interval),
///     which shows as two or more frames still executing on the GPU.
///
/// The host fills this in immediately before acquiring and attaches it to the
/// `gpu_acquire` trace span. Nothing reads it to make a decision.

#include <string_view>

namespace pulp::view {

struct GpuAcquireDiagnostics {
    /// True when the frame was dispatched by a display-link tick. Resize,
    /// capture and first-show frames are rendered outside the link and carry
    /// no target.
    bool vsync_driven = false;
    /// The display's refresh interval, or 0 when unknown.
    double refresh_period_ms = 0.0;
    /// How long after the display-link target presentation time the acquire
    /// started. Negative means the frame is being rendered ahead of the
    /// target. 0 when not display-link driven.
    double late_ms = 0.0;
    /// Frames submitted to the GPU whose work has not finished, or -1 when the
    /// surface does not track it.
    int frames_in_flight = -1;
};

/// Build the record for one acquire. `now_s` and `vsync_target_s` share one
/// monotonic timebase (seconds); a non-positive target means the frame was
/// not display-link driven. A non-positive or non-finite period reads as
/// unknown.
GpuAcquireDiagnostics gpu_acquire_diagnostics(double now_s,
                                              double vsync_target_s,
                                              double refresh_period_s,
                                              int frames_in_flight) noexcept;

/// Whether an environment value asks for GPU timing: "1", "true", "yes" or
/// "on", case-insensitively. Absent or anything else is off, because GPU
/// timestamp queries relax the device's validation (see
/// `GpuSurface::Config::enable_gpu_timing`) and must be a deliberate choice.
bool gpu_timing_requested(const char* env_value) noexcept;

/// The environment variable a standalone GPU window reads to enable GPU
/// render timing.
inline constexpr std::string_view kGpuTimingEnvVar = "PULP_GPU_TIMING";  // null-terminated

} // namespace pulp::view
