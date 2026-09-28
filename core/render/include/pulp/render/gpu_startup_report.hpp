// gpu_startup_report.hpp — the always-on GPU bring-up lines a support read
// greps for.
//
// A shipped app's stdout/stderr is often the only evidence a remote support
// read gets. These formatters produce a handful of stable, greppable,
// info-level lines — the adapter Dawn actually picked, how long each bring-up
// stage took, and whether the Skia log bridge was wired — so a log that says
// "Graphite initialized" also says on WHAT and how fast. None of them is
// per-frame.
//
// The line shapes are a contract with downstream tooling: add fields at the
// end, never rename or reorder existing ones.
#pragma once

#include <pulp/render/gpu_diagnostics.hpp>
#include <pulp/render/gpu_surface.hpp>

#include <string>

namespace pulp::render {

/// Stable lowercase label for an adapter type: "integrated", "discrete",
/// "cpu", or "unknown".
const char* adapter_type_label(GpuSurface::AdapterType type) noexcept;

/// One line naming the adapter a surface came up on, e.g.
///   GpuSurface: adapter name="Apple M3 Max" vendor="apple" architecture="metal-3"
///   description="..." type=integrated backend=Metal null=false
/// (a single line; wrapped here for width). String fields are quoted with `"`
/// and `\` escaped and control characters replaced by spaces, so one field can
/// never break the line or impersonate another.
std::string format_gpu_adapter_line(const GpuSurface::AdapterInfo& info);

/// Bring-up stage timings from GpuSurface::initialize() plus the two stages
/// the owning host measures around it.
struct GpuStartupReport {
    GpuSurface::StartupTimings surface;
    /// SkiaSurface (Graphite context) creation on the shared Dawn device.
    double graphite_ms = -1.0;
    /// From the start of GPU bring-up to the end of the first rendered frame
    /// (submitted and presented, or read back when there is no drawable).
    double first_frame_ms = -1.0;
};

/// One line of stage timings in milliseconds with one decimal, e.g.
///   GpuSurface: startup_ms instance=0.4 adapter=12.1 device=8.3 surface=1.0
///   graphite=20.5 first_frame=95.2
/// (a single line). A stage that was not measured prints `n/a`.
std::string format_gpu_startup_line(const GpuStartupReport& report);

/// One line saying whether the Skia log bridge is wired and what reached it, e.g.
///   GpuDiagnostics: skia_bridge=installed skia_records=3
///   gpu_diagnostics_emitted=5 truncated=0 skia_text_suppressed=0
/// (a single line), so "nothing was logged" is distinguishable from "nothing
/// was wired".
std::string format_gpu_diagnostics_line(const SkiaLogBridgeState& bridge,
                                        const GpuDiagnosticsStats& stats);

/// Log the GpuDiagnostics line for this process at info level, preceded by
/// `skia: N more records suppressed` when the bridge's text budget ran out.
/// Intended for the end of a bounded run (a headless screenshot), not per frame.
void log_gpu_diagnostics_summary();

} // namespace pulp::render
