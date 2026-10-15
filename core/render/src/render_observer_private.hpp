#pragma once

#include <cstdint>
#include <string>

namespace pulp::render {

class GpuSurface;

/// Provider identity recorded by the development-only render observer.
///
/// This header lives under the render implementation source tree on purpose:
/// the observer can support importer diagnostics without becoming part of the
/// installed SDK or product rendering API.
struct RenderToolchainVersions {
    std::int32_t skia_milestone = 0;
    std::string dawn_version;
};

namespace render_observer {

RenderToolchainVersions toolchain_versions();
bool inject_device_loss(GpuSurface& surface);
bool device_lost(const GpuSurface& surface);

} // namespace render_observer
} // namespace pulp::render
