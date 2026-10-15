#include <pulp/render/render_observer.hpp>
#include <pulp/render/gpu_surface.hpp>

#if defined(PULP_HAS_SKIA)
#include "include/core/SkMilestone.h"
#include "dawn/dawn_version.h"
#include "webgpu/webgpu_cpp.h"
#endif

namespace pulp::render::render_observer {

RenderToolchainVersions toolchain_versions() {
    RenderToolchainVersions versions;
#if defined(PULP_HAS_SKIA)
    versions.skia_milestone = SK_MILESTONE;
    static constexpr char digits[] = "0123456789abcdef";
    for (const auto byte : dawn::kDawnVersion) {
        versions.dawn_version += digits[byte >> 4];
        versions.dawn_version += digits[byte & 0x0f];
    }
#endif
    return versions;
}

bool inject_device_loss(GpuSurface& surface) {
#if defined(PULP_HAS_SKIA)
    auto* device = static_cast<wgpu::Device*>(surface.dawn_device_handle());
    if (!device || !*device)
        return false;
    device->Destroy();
    return true;
#else
    (void)surface;
    return false;
#endif
}

bool device_lost(const GpuSurface& surface) {
#if defined(PULP_HAS_SKIA)
    auto* device = static_cast<wgpu::Device*>(surface.dawn_device_handle());
    auto* instance = static_cast<wgpu::Instance*>(surface.dawn_instance_handle());
    if (!device || !*device || !instance || !*instance)
        return false;
    wgpu::FutureWaitInfo wait{device->GetLostFuture()};
    return instance->WaitAny(1, &wait, 0) == wgpu::WaitStatus::Success && wait.completed;
#else
    (void)surface;
    return false;
#endif
}

} // namespace pulp::render::render_observer
