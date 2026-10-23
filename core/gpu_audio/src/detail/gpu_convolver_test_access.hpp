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
    // when shared-I/O support is not compiled into this build, in which case
    // `session` is left with the caller. Taken by rvalue reference so a build
    // that sees only the forward declaration never destroys the session:
    // under the MSVC ABI a by-value unique_ptr parameter is destroyed by the
    // callee, which needs the complete type (C2027).
    static bool install_shared_io_session(GpuConvolver&,
                                          std::unique_ptr<SharedIoConvolutionSession>&&) noexcept;
};

} // namespace pulp::gpu_audio::detail
