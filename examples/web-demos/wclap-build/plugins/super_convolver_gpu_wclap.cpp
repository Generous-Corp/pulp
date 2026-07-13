// WebCLAP entry point for the SuperConvolver GPU demo — the SAME processor as
// super_convolver_wclap.cpp, compiled with PULP_WEB_GPU_AUDIO so its browser GPU
// engine (WebGPU compute shaders in a DedicatedWorker, reached over the
// pulp_gpu_xfer import) is available behind an Engine CPU/GPU toggle.
//
// It is a SECOND artifact rather than a flag on the first one because a WAM/CLAP
// host reads a module's parameter list once at mount: growing that list at
// runtime is ABI-hostile, so the CPU-only module keeps declaring exactly its four
// parameters and stays byte-identical. This module declares two more (Engine and
// "GPU only"), and is the only one that imports pulp_gpu_xfer — a host that
// cannot supply that import (no cross-origin isolation → no SharedArrayBuffer →
// no worker) simply fails to instantiate this module and keeps the CPU one.
//
// The GPU is a CAPABILITY demonstration, not a speed claim: CPU is the default
// engine here exactly as it is natively, and a missed GPU block is covered by the
// latency-aligned CPU convolver unless the user explicitly removes the net.
#include "super_convolver.hpp"

#include <pulp/format/clap_entry.hpp>
#include <pulp/format/web/wclap_adapter.hpp>

#include <memory>

#if !defined(PULP_WEB_GPU_AUDIO)
#error "super_convolver_gpu_wclap.cpp must be built with -DPULP_WEB_GPU_AUDIO (see wclap-build/CMakeLists.txt)"
#endif

namespace {

// A distinct CLAP id and name so a host (or a demo gallery) can hold both this
// module and the CPU-only SuperConvolver at once without them colliding.
class SuperConvolverGpuProcessor final : public pulp::examples::SuperConvolverProcessor {
public:
    pulp::format::PluginDescriptor descriptor() const override {
        auto d = SuperConvolverProcessor::descriptor();
        d.name = "SuperConvolver GPU";
        d.bundle_id = "com.pulp.superconvolver.gpu";
        return d;
    }
};

std::unique_ptr<pulp::format::Processor> create_super_convolver_gpu() {
    return std::make_unique<SuperConvolverGpuProcessor>();
}

}  // namespace

PULP_WCLAP_PLUGIN(create_super_convolver_gpu)
