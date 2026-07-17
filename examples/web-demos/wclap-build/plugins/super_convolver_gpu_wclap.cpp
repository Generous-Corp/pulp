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

#include <cstdint>
#include <memory>
#include <vector>

#if !defined(PULP_WEB_GPU_AUDIO)
#error "super_convolver_gpu_wclap.cpp must be built with -DPULP_WEB_GPU_AUDIO (see wclap-build/CMakeLists.txt)"
#endif

namespace {

// A distinct CLAP id and name so a host (or a demo gallery) can hold both this
// module and the CPU-only SuperConvolver at once without them colliding.
class SuperConvolverGpuProcessor final : public pulp::examples::SuperConvolverProcessor {
public:
    SuperConvolverGpuProcessor() { s_live = this; }
    ~SuperConvolverGpuProcessor() override { if (s_live == this) s_live = nullptr; }

    pulp::format::PluginDescriptor descriptor() const override {
        auto d = SuperConvolverProcessor::descriptor();
        d.name = "SuperConvolver GPU";
        d.bundle_id = "com.pulp.superconvolver.gpu";
        return d;
    }

    /// The instance the IR exports below read from. A WebCLAP module is instantiated
    /// once per AudioWorklet and hosts exactly one plugin, so "the live instance" is
    /// well defined here in a way it would not be in a native host — hence this seam
    /// lives in the demo's entry point and not in the adapter. Cleared on destruction,
    /// so an export called after teardown reports "no IR" instead of reading freed
    /// memory.
    static SuperConvolverGpuProcessor* live() { return s_live; }

private:
    static SuperConvolverGpuProcessor* s_live;
};

SuperConvolverGpuProcessor* SuperConvolverGpuProcessor::s_live = nullptr;

// The snapshot the exports hand out. Held module-side (rather than malloc'd per call)
// so the page can read it straight out of wasm memory with zero copies on this side
// and no ownership question about who frees it.
std::vector<float> g_ir_snapshot;

std::unique_ptr<pulp::format::Processor> create_super_convolver_gpu() {
    return std::make_unique<SuperConvolverGpuProcessor>();
}

}  // namespace

// ── The IR handoff: plugin → worklet → page → GPU worker ────────────────────────
//
// The GPU worker has to convolve with the SAME impulse response the CPU convolver is
// using, or the two engines are different reverbs and the CPU net stops being a
// sample-for-sample substitute for a missed GPU block. That IR is built in C++
// (build_base_ir, keyed off Size) and then NORMALIZED and WINDOWED, so handing the page
// a raw IR to give to both sides is NOT equivalent — the plugin transforms what it is
// given. The page has to be told what the plugin actually ended up with.
//
// So the module publishes its live, post-transform IR (`worker_base_ir_`, via
// impulse_response_snapshot()) and a generation counter that bumps whenever it changes.
// The worklet polls the counter on the non-realtime tick — never inside process() — and
// forwards the samples to the page, which hands them to the worker.
//
// These are plain wasm exports rather than a new CLAP extension on purpose: a CLAP
// extension would be a permanent ABI surface invented for one demo, and every host that
// is not this page would carry it forever. An export is opt-in and invisible — the
// shared worklet checks whether the module has it and does nothing when it does not.
extern "C" {

/// Bumps whenever the plugin's IR changes. Cheap enough to poll; the snapshot is not.
__attribute__((export_name("pulp_ir_generation")))
std::uint32_t pulp_ir_generation() {
    auto* p = SuperConvolverGpuProcessor::live();
    return p ? p->ir_generation() : 0u;
}

/// Copies the live IR into module memory and returns its length in FLOATS (0 if there
/// is none yet). Call before pulp_ir_data(); the pointer is only valid until the next
/// call to this function.
__attribute__((export_name("pulp_ir_snapshot")))
std::uint32_t pulp_ir_snapshot() {
    auto* p = SuperConvolverGpuProcessor::live();
    if (!p) { g_ir_snapshot.clear(); return 0u; }
    g_ir_snapshot = p->impulse_response_snapshot();
    return static_cast<std::uint32_t>(g_ir_snapshot.size());
}

/// Where the last pulp_ir_snapshot() landed.
__attribute__((export_name("pulp_ir_data")))
const float* pulp_ir_data() { return g_ir_snapshot.data(); }

/// Set the GPU pipeline depth (round-trip latency, in internal blocks). This is the
/// JS side of per-device adaptive depth (adaptive-depth.mjs picks it from the measured
/// round trip): the page sets it so the plugin's L matches the ring's latencyBlocks.
/// It sets the latency the delay lines are sized to at prepare(), so to CHANGE it live
/// the caller must trigger a re-prepare (the thread-safe apply is the remaining step —
/// see planning/2026-07-16-adaptive-gpu-pipeline-depth.md inc 3). Opt-in wasm export,
/// invisible to any host that is not this page (same rationale as the pulp_ir_* exports).
__attribute__((export_name("pulp_sc_set_pipeline_depth")))
void pulp_sc_set_pipeline_depth(std::uint32_t blocks) {
    auto* p = SuperConvolverGpuProcessor::live();
    if (p) p->set_web_gpu_latency_blocks(static_cast<std::size_t>(blocks));
}

}  // extern "C"

PULP_WCLAP_PLUGIN(create_super_convolver_gpu)
