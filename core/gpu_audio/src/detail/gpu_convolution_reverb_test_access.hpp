#pragma once

namespace pulp::gpu_audio {
class GpuConvolutionReverb;
class GpuConvolver;
} // namespace pulp::gpu_audio

namespace pulp::gpu_audio::detail {

// Private fault-injection seam for focused tests. Host-only, with no callback
// running. Prepares the route exactly as prepare() does, except that each
// lane's convolver is prepared by `prepare_node` instead of the authenticated
// shared provider. The lane is still admitted only if its transport reports
// the authenticated shared path.
struct GpuConvolutionReverbTestAccess {
    static bool prepare_with(GpuConvolutionReverb&,
                             bool (*prepare_node)(GpuConvolver&, void*) noexcept,
                             void* context) noexcept;
};

} // namespace pulp::gpu_audio::detail
