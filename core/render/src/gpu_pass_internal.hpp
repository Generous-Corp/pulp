#pragma once

// Implementation state behind the handles in <pulp/render/gpu_pass.hpp>.
// Private to pulp-render; tests that need the Dawn objects include it with
// the render source directory on their include path.

#include <pulp/render/gpu_pass.hpp>

#if defined(PULP_HAS_SKIA) && defined(PULP_HAS_WEBGPU) && !defined(__EMSCRIPTEN__)
#define PULP_GPU_PASS_HAS_DAWN 1
#include "webgpu/webgpu_cpp.h"
#else
#define PULP_GPU_PASS_HAS_DAWN 0
#endif

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace pulp::render::detail {

#if PULP_GPU_PASS_HAS_DAWN

struct GpuDeviceState {
    wgpu::Device device;
    wgpu::Queue queue;
    wgpu::Instance instance;
    wgpu::Future lost_future{};
    bool has_lost_future = false;
    mutable bool lost = false;
    GpuDeviceCapabilities capabilities;

    /// Non-blocking: polls the device-lost future once. Sticky.
    bool is_lost() const;
};

struct GpuTextureState {
    std::shared_ptr<GpuDeviceState> device;
    wgpu::Texture texture;
    wgpu::TextureView view;
    GpuTextureDesc desc;
    /// Unsubmitted frames that write this texture as an attachment. A
    /// texture with pending writes cannot be composited or read back yet.
    int unsubmitted_writes = 0;
};

struct GpuBufferState {
    std::shared_ptr<GpuDeviceState> device;
    wgpu::Buffer buffer;
    GpuBufferDesc desc;
};

struct GpuSamplerState {
    std::shared_ptr<GpuDeviceState> device;
    wgpu::Sampler sampler;
};

struct GpuShaderModuleState {
    std::shared_ptr<GpuDeviceState> device;
    wgpu::ShaderModule module;
    std::vector<WgslDiagnostic> warnings;
};

struct GpuRenderPipelineState {
    std::shared_ptr<GpuDeviceState> device;
    wgpu::RenderPipeline pipeline;
};

struct GpuComputePipelineState {
    std::shared_ptr<GpuDeviceState> device;
    wgpu::ComputePipeline pipeline;
};

struct GpuBindGroupState {
    std::shared_ptr<GpuDeviceState> device;
    wgpu::BindGroup group;
};

struct GpuTargetState {
    std::shared_ptr<GpuDeviceState> device;
    GpuTexture color;
    std::optional<GpuTexture> depth;
};

struct GpuSubmissionState {
    std::shared_ptr<GpuDeviceState> device;
    std::mutex mutex;
    bool work_done = false;
    bool work_ok = false;
    GpuSubmissionStatus status = GpuSubmissionStatus::pending;
    std::optional<GpuError> error;
};

struct GpuFrameState {
    std::shared_ptr<GpuDeviceState> device;
    std::string label;
    wgpu::CommandEncoder encoder;
    wgpu::RenderPassEncoder render_pass;
    wgpu::ComputePassEncoder compute_pass;
    /// Id of the open pass, 0 when none is open.
    std::uint64_t open_pass = 0;
    std::uint64_t next_pass = 1;
    bool scopes_open = false;
    bool submitted = false;
    /// First error recorded by the API itself (misuse, a failed begin).
    std::optional<GpuError> error;
    /// Textures this frame writes as attachments; each counted once.
    std::vector<std::shared_ptr<GpuTextureState>> writes;

    void record_error(GpuError::Kind kind, std::string message);
    void end_open_pass();
    void release_writes();
};

#else // !PULP_GPU_PASS_HAS_DAWN

struct GpuDeviceState {
    GpuDeviceCapabilities capabilities;
};
struct GpuTextureState {
    GpuTextureDesc desc;
};
struct GpuBufferState {
    GpuBufferDesc desc;
};
struct GpuSamplerState {};
struct GpuShaderModuleState {
    std::vector<WgslDiagnostic> warnings;
};
struct GpuRenderPipelineState {};
struct GpuComputePipelineState {};
struct GpuBindGroupState {};
struct GpuTargetState {
    GpuTexture color;
    std::optional<GpuTexture> depth;
};
struct GpuSubmissionState {
    GpuSubmissionStatus status = GpuSubmissionStatus::failed;
    std::optional<GpuError> error;
};
struct GpuFrameState {
    bool submitted = false;
};

#endif // PULP_GPU_PASS_HAS_DAWN

/// Reaches the private state of the public handles.
struct GpuAccess {
    template <typename Handle> static auto& state(Handle& handle) {
        return handle.state_;
    }
    template <typename Handle> static const auto& state(const Handle& handle) {
        return handle.state_;
    }
};

} // namespace pulp::render::detail
