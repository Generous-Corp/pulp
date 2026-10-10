// Compiles and links against pulp::gpu-device-api alone.
//
// The device API is the bottom of the GPU stack: canvas, the 2D renderer, the
// view layer and GPU audio all consume these headers. This translation unit
// includes every header the API target owns and uses only what they define
// inline, so it builds with no Dawn, WebGPU or Skia include directory and links
// with no Pulp archive. A header that starts naming a Dawn type, or an inline
// helper that starts calling an out-of-line device function, breaks this build.

#include <pulp/render/bench/perf_counters.hpp>
#include <pulp/render/gpu_compute.hpp>
#include <pulp/render/gpu_diagnostics.hpp>
#include <pulp/render/gpu_render_time.hpp>
#include <pulp/render/gpu_startup_report.hpp>
#include <pulp/render/gpu_surface.hpp>

#include <cstdio>

int main() {
#ifdef PULP_BENCHMARK
    pulp::render::bench::PerfCounters counters;
    if (counters.sample_count.load() != 0.0) {
        return 1;
    }
#endif
    pulp::render::GpuSurface::AdapterInfo info;
    pulp::render::GpuStartupReport report;
    pulp::render::GpuDiagnosticsStats stats;

    const bool ok = !info.available
        && report.graphite_ms < 0.0
        && stats.emitted == 0
        && pulp::render::kGpuRenderNanosecondsPerMillisecond == 1.0e6;
    if (!ok) {
        std::fprintf(stderr, "gpu-device-api defaults changed unexpectedly\n");
        return 1;
    }
    return 0;
}
