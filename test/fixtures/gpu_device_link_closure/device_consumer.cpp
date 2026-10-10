// Links against pulp::gpu-device alone, with no 2D renderer in the link.
//
// pulp-gpu-audio depends on exactly this: the GPU compute and surface
// implementation without the Skia renderer above it. Every device translation
// unit is referenced here, so an undefined symbol that only pulp-render could
// satisfy fails this link instead of surfacing first in a GPU-audio consumer.
// Nothing creates a device: the test must pass on a machine with no GPU.

#include <pulp/render/gpu_compute.hpp>
#include <pulp/render/gpu_diagnostics.hpp>
#include <pulp/render/gpu_startup_report.hpp>
#include <pulp/render/gpu_surface.hpp>

#include <cstdio>
#include <cstring>
#include <memory>

namespace {

// Taking the address keeps the factory, and therefore the Dawn surface and
// compute objects, in the link without running any GPU bring-up.
using SurfaceFactory = std::unique_ptr<pulp::render::GpuSurface> (*)();
using ComputeFactory = std::unique_ptr<pulp::render::GpuCompute> (*)();
volatile SurfaceFactory surface_factory = &pulp::render::GpuSurface::create_dawn;
volatile ComputeFactory compute_factory = &pulp::render::GpuCompute::create;

} // namespace

int main() {
    using namespace pulp::render;

    if (surface_factory == nullptr || compute_factory == nullptr) {
        return 1;
    }
    if (std::strcmp(to_string(GpuDiagnosticSeverity::error), "error") != 0) {
        std::fprintf(stderr, "unexpected severity label\n");
        return 1;
    }
    const auto before = gpu_diagnostics_stats().emitted;
    emit_gpu_diagnostic(GpuDiagnosticSeverity::info, "gpu-device.link-closure",
                        "device-only consumer");
    if (gpu_diagnostics_stats().emitted != before + 1) {
        std::fprintf(stderr, "diagnostic sink did not count the record\n");
        return 1;
    }
    GpuStartupReport report;
    if (format_gpu_startup_line(report).empty()
        || format_gpu_adapter_line(GpuSurface::AdapterInfo{}).empty()) {
        std::fprintf(stderr, "startup report formatting returned nothing\n");
        return 1;
    }
    return 0;
}
