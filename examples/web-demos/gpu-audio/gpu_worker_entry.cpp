// pulp-gpu-dsp — the C ABI the DedicatedWorker's JS drives.
//
// WHY A WORKER AT ALL: an AudioWorkletProcessor cannot touch navigator.gpu
// (WebGPU is exposed on Window and DedicatedWorker only). So the audio thread
// stays in the worklet, the GPU work lives here, and the two exchange planar
// blocks through SharedArrayBuffer rings owned by the JS side — the ring cannot
// be a C++ object because the worklet's wasm and this wasm are SEPARATE linear
// memories.
//
// NOTHING HERE BLOCKS. pulp_gpu_poll() pumps the readback queue exactly once and
// returns; the JS event loop, not this module, is what lets the WebGPU map
// callbacks resolve. A poll that spun would starve the loop it is waiting on.
//
// THIS ABI IS PINNED — the worker JS codes against exactly these symbols. The
// export allowlist in examples/web-demos/gpu-audio/CMakeLists.txt must list every
// one of them.

#include <emscripten/emscripten.h>
// emdawnwebgpu declares emscripten_webgpu_get_device() in webgpu.h itself — it
// does NOT ship the legacy <emscripten/html5_webgpu.h>.
#include <webgpu/webgpu.h>

#include <pulp/gpu_audio/web/web_gpu_convolver.hpp>
#include <pulp/render/gpu_compute.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace {

// SuperConvolver's internal block (examples/super-convolver/super_convolver.hpp)
// and channel count. Fixed, not negotiated: the JS ring, the worklet's reblock
// FIFO, and this module's scratch are all sized from these, so a mismatch is a
// caller bug and is rejected rather than silently reshaped.
constexpr int kBlock = 512;
constexpr int kChannels = 2;

struct Module {
    std::unique_ptr<pulp::render::GpuCompute> gpu;
    pulp::gpu_audio::WebGpuConvolver convolver;

    std::vector<float> in_scratch =
        std::vector<float>(static_cast<std::size_t>(kBlock) * kChannels, 0.0f);
    std::vector<float> out_scratch =
        std::vector<float>(static_cast<std::size_t>(kBlock) * kChannels, 0.0f);

    std::string kernel_source_buf;  // owns the string pulp_gpu_kernel_source returns

    /// How many kernels have had their WGSL replaced via pulp_gpu_override_kernel.
    /// GpuCompute::kernel_source() deliberately always returns the BUILT-IN text (so
    /// a mutation cannot chain), which means there is no way to ask it "are you
    /// still running the shipped shaders?" — so the count is kept here. The worker
    /// needs it: its bring-up self-test asserts the module is an IDENTITY when given
    /// a unit-impulse IR, and that assertion is only meaningful while the kernels
    /// are the ones we shipped. See pulp_gpu_stat(6).
    int overridden_kernels = 0;
};

Module& mod() {
    static Module m;
    return m;
}

/// GpuCompute::override_kernel_source() must run BEFORE initialize_*, because the
/// pipelines are built there. So the object is created on first use — by
/// pulp_gpu_kernel_source / pulp_gpu_override_kernel just as readily as by
/// pulp_gpu_init — and only initialized by pulp_gpu_init.
pulp::render::GpuCompute* ensure_gpu() {
    Module& m = mod();
    if (!m.gpu) m.gpu = pulp::render::GpuCompute::create();
    return m.gpu.get();
}

}  // namespace

extern "C" {

/// Adopt the device the JS side already created (Module.preinitializedWebGPUDevice)
/// and build the compute pipelines. 1 = ok. Must precede every other call.
EMSCRIPTEN_KEEPALIVE int pulp_gpu_init(void) {
    Module& m = mod();
    if (m.gpu && m.gpu->is_initialized() && !m.gpu->device_lost()) return 1;

    // The device is created in JS (navigator.gpu.requestAdapter/requestDevice is
    // a promise, and this module has no Asyncify) and handed over through
    // Module.preinitializedWebGPUDevice.
    WGPUDevice device = emscripten_webgpu_get_device();
    if (device == nullptr) return 0;

    auto* gpu = ensure_gpu();
    if (gpu == nullptr || !gpu->initialize_from_device(static_cast<void*>(device))) {
        m.gpu.reset();
        return 0;
    }
    // Opt the async convolution path into GPU-busy timing so pulp_gpu_stat(4) can
    // report the shader's real execution time (this is the browser demo; the native
    // audio path leaves it off). A no-op when the device lacks timestamp-query.
    gpu->set_async_timing_enabled(true);
    return 1;
}

/// The WGSL of kernel `label`, for the live-kernel editor. Empty string if the
/// label is unknown. The returned pointer is owned by this module and stays valid
/// until the next call.
EMSCRIPTEN_KEEPALIVE const char* pulp_gpu_kernel_source(const char* label) {
    Module& m = mod();
    m.kernel_source_buf.clear();
    auto* gpu = ensure_gpu();
    if (gpu != nullptr && label != nullptr) {
        if (const char* src = gpu->kernel_source(label)) m.kernel_source_buf = src;
    }
    return m.kernel_source_buf.c_str();
}

/// Replace kernel `label`'s WGSL. MUST precede pulp_gpu_init(): the pipelines are
/// built at initialization, so a kernel swapped afterwards would not be the one
/// dispatching. To swap a kernel on a live module, override then re-init then
/// re-prepare. 1 = accepted.
EMSCRIPTEN_KEEPALIVE int pulp_gpu_override_kernel(const char* label, const char* wgsl) {
    auto* gpu = ensure_gpu();
    if (gpu == nullptr || label == nullptr || wgsl == nullptr) return 0;
    if (!gpu->override_kernel_source(label, wgsl)) return 0;
    ++mod().overridden_kernels;
    return 1;
}

/// Transform the IR (on the CPU — see WebGpuConvolver) and upload the resident IR
/// spectrum. No GPU readback happens here. 1 = ok.
EMSCRIPTEN_KEEPALIVE int pulp_gpu_prepare(int sample_rate, int block, int channels,
                                          const float* ir, int ir_len) {
    Module& m = mod();
    if (!m.gpu || !m.gpu->is_initialized()) return 0;
    if (block != kBlock || channels != kChannels) return 0;
    return m.convolver.prepare(m.gpu.get(), sample_rate, block, channels, ir, ir_len) ? 1
                                                                                      : 0;
}

/// Issue one block. `in_planar` holds kChannels * kBlock floats, channel-major
/// (normally pulp_gpu_in_buffer()). `deadline_ms` is measured from this call; a
/// block whose map has not resolved by then completes as Expired and is reported
/// to onBlockDone with ok = 0 — a missed GPU deadline is NORMAL in a browser
/// (a backgrounded tab throttles this worker while the worklet keeps running),
/// so the JS side must treat it as a miss to be covered, never as an error.
/// 1 = admitted, 0 = at the pipeline's depth cap or the device is gone.
EMSCRIPTEN_KEEPALIVE int pulp_gpu_submit(unsigned seq, const float* in_planar,
                                         double deadline_ms) {
    Module& m = mod();
    if (!m.convolver.prepared() || in_planar == nullptr) return 0;
    if (!(deadline_ms > 0.0)) return 0;

    const auto deadline = std::chrono::microseconds(
        static_cast<int64_t>(std::llround(deadline_ms * 1000.0)));
    return m.convolver.submit(static_cast<uint32_t>(seq), in_planar, kBlock, deadline) ? 1
                                                                                       : 0;
}

/// ONE non-blocking harvest. Invokes Module.onBlockDone(seq, outPtr, ok) for each
/// block that resolved, in submission order. Returns how many were delivered.
EMSCRIPTEN_KEEPALIVE int pulp_gpu_poll(void) {
    Module& m = mod();
    if (!m.convolver.prepared()) return 0;

    const uint32_t delivered = m.convolver.collect(
        [&m](uint32_t seq, const float* out_planar, bool ok) {
            std::copy(out_planar, out_planar + m.out_scratch.size(), m.out_scratch.begin());
            EM_ASM(
                {
                    if (Module['onBlockDone']) Module['onBlockDone']($0, $1, $2);
                },
                seq, m.out_scratch.data(), ok ? 1 : 0);
        });
    return static_cast<int>(delivered);
}

/// Block-pipeline telemetry, for the demo's live stats panel.
///   0 = submits, 1 = map_resolves, 2 = expired, 3 = failed,
///   4 = gpu_ns_last, 5 = device_lost, 6 = overridden_kernels.
///
/// 6 is not decoration: the worker's bring-up self-test convolves a known block
/// against a unit-impulse IR and requires wet == dry. That identity holds only for
/// the SHIPPED kernels — a caller that replaced a kernel's WGSL (the live-kernel
/// editor, or the browser proof's 0.5x tamper) has deliberately changed the maths,
/// and the self-test must relax to "the block came back, finite and non-silent"
/// rather than fail the whole lane. It stays STRICT whenever 6 == 0, which is every
/// shipped configuration.
///
/// case 4 is the async convolution shader's GPU-busy time in NANOSECONDS (the field
/// is gpuNsLast throughout the JS), or 0 when the device lacks timestamp-query or
/// quantized a small dispatch to 0 ns (Metal and Chrome do). GpuCompute::
/// convolve_batch_async() brackets its passes with a timestamp query and reads the
/// ticks back through the SAME non-blocking readback path as the audio — never a
/// blocking map — so this is honest and costs the audio path nothing (it is off
/// unless set_async_timing_enabled(true), which pulp_gpu_init does here). The JS
/// still must render 0 as "no timing", never as a stall.
EMSCRIPTEN_KEEPALIVE double pulp_gpu_stat(int idx) {
    Module& m = mod();
    switch (idx) {
        case 0: return static_cast<double>(m.convolver.submits());
        case 1: return static_cast<double>(m.convolver.resolves());
        case 2: return static_cast<double>(m.convolver.expired());
        case 3: return static_cast<double>(m.convolver.failed());
        case 4: return m.gpu ? m.gpu->last_gpu_busy_ns() : 0.0;  // ns (gpuNsLast)
        case 5: return (m.gpu && m.gpu->device_lost()) ? 1.0 : 0.0;
        case 6: return static_cast<double>(m.overridden_kernels);
        default: return 0.0;
    }
}

/// Report that the adopted device has been lost. ADDITIVE to the pinned ABI, and
/// not optional: a device this module did NOT create cannot observe its own loss
/// (the device-lost callback can only be installed on the device *descriptor*),
/// so without this call stat 5 could never become 1 and submits would keep being
/// issued into a dead device. The worker JS awaits `device.lost` and calls it.
EMSCRIPTEN_KEEPALIVE void pulp_gpu_notify_device_lost(void) {
    Module& m = mod();
    if (m.gpu) m.gpu->notify_device_lost();
}

/// Scratch the JS copies a planar input block into before pulp_gpu_submit().
/// kChannels * kBlock floats. Stable for the module's lifetime — but ALLOW_MEMORY_GROWTH
/// can detach the JS views, so re-read Module.HEAPF32 after any allocation.
EMSCRIPTEN_KEEPALIVE float* pulp_gpu_in_buffer(void) { return mod().in_scratch.data(); }

/// Scratch onBlockDone's outPtr points at. kChannels * kBlock floats, valid only
/// for the duration of the callback (the next delivered block overwrites it).
EMSCRIPTEN_KEEPALIVE float* pulp_gpu_out_buffer(void) { return mod().out_scratch.data(); }

}  // extern "C"
