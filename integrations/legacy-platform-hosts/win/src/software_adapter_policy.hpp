#pragma once

#include <pulp/render/gpu_surface.hpp>

namespace pulp::view::win_render {

// WARP can report AdapterType::integrated_gpu. Use its native identity rather
// than treating every integrated adapter (or every virtual machine) as software.
// The measured Windows editor workload is much faster on Skia raster than on
// WARP's software D3D12 queue. Other adapters retain their normal GPU path.
inline bool prefer_raster(const render::GpuSurface::AdapterInfo& adapter,
                          bool allow_software_gpu) noexcept {
    return !allow_software_gpu && adapter.available && !adapter.null_backend &&
           adapter.backend_type == "D3D12" && adapter.vendor == "microsoft" &&
           adapter.architecture == "warp";
}

} // namespace pulp::view::win_render
