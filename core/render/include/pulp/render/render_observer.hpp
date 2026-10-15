#pragma once

#include <cstdint>
#include <string>

namespace pulp::render {

class GpuSurface;

/// Provider identity recorded by diagnostic render receipts.
///
/// This is intentionally a value-only seam: consumers can report which
/// renderer toolchain produced evidence without including Skia, Dawn, or
/// WebGPU headers in their translation unit.
struct RenderToolchainVersions {
    std::int32_t skia_milestone = 0;
    std::string dawn_version;
};

/// Narrow diagnostics seam for development-only render observers.
///
/// These operations are not part of the frame loop or product rendering API.
/// Device-loss injection is for seeded negative controls and must never be
/// called by a realtime or production host.
namespace render_observer {

/// Return the versions compiled into the active render provider. An unavailable
/// provider returns milestone 0 and an empty Dawn version.
RenderToolchainVersions toolchain_versions();

/// Destroy the active Dawn device for a seeded device-loss test. Returns false
/// when the surface has no provider device to destroy.
bool inject_device_loss(GpuSurface& surface);

/// Poll whether the active provider has reported device loss. Returns false if
/// no provider device exists or if its lost future has not completed.
bool device_lost(const GpuSurface& surface);

} // namespace render_observer
} // namespace pulp::render
