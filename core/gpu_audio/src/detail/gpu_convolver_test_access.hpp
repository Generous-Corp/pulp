#pragma once

#include <memory>

namespace pulp::render {
class GpuCompute;
}

namespace pulp::gpu_audio {
class GpuConvolver;
}

namespace pulp::gpu_audio::detail {

class SharedIoConvolutionSession;

// Private fault-injection seam for focused tests. Host-only, on a prepared
// convolver with no transport or callback running.
struct GpuConvolverTestAccess {
    static render::GpuCompute* staged_compute(GpuConvolver&) noexcept;
    // Replace the staged GpuCompute device. Null leaves the CPU worker path.
    static void install_staged_compute(GpuConvolver&, std::unique_ptr<render::GpuCompute>) noexcept;
    // Replace the provider with an already prepared private session. False
    // when shared-I/O support is not compiled into this build.
    static bool install_shared_io_session(GpuConvolver&,
                                          std::unique_ptr<SharedIoConvolutionSession>) noexcept;
};

} // namespace pulp::gpu_audio::detail
