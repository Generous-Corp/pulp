// Pulp Live Kernel — S0 spike — C ABI entry point (lk_* symbols).
//
// A NEW routing surface with its OWN C ABI (the lk_* prefix), entirely separate
// from the shipped wam_* four-file ABI (which this spike does not touch). One
// process-global Kernel owns two preallocated Plan slots and the equal-power
// crossfade. The audio thread calls lk_process(); edits arrive as either a
// binary graph blob (lk_load_plan + lk_swap) or a param poke (lk_set_param).
//
// ZERO-ALLOC PROOF: every C++ heap allocation goes through the replaced global
// operator new below, which bumps g_alloc_count. The harness reads
// lk_alloc_count() before and after a long render / a swap and asserts the delta
// is zero — process() and swap-fade rendering allocate nothing. (Allocation is
// EXPECTED once, in lk_init -> prepare_pool, for the DelayLineT rings.)

#include "crossfade.hpp"

#include <pulp/signal/fast_math.hpp>  // FastMath::tanh — the ladder's saturator

#include <cmath>
#include <cstdlib>
#include <cstddef>
#include <new>

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#define LK_EXPORT extern "C" EMSCRIPTEN_KEEPALIVE
#else
#define LK_EXPORT extern "C"
#endif

namespace {
uint64_t g_alloc_count = 0;
pulp::live_kernel::Kernel g_kernel;
}

// Replaced global allocation operators (the zero-alloc instrument). Built with
// -fno-exceptions, so these are the non-throwing shape (abort on OOM).
void* operator new(std::size_t n) {
    ++g_alloc_count;
    void* p = std::malloc(n ? n : 1);
    if (!p) std::abort();
    return p;
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void  operator delete(void* p) noexcept { std::free(p); }
void  operator delete[](void* p) noexcept { std::free(p); }
void  operator delete(void* p, std::size_t) noexcept { std::free(p); }
void  operator delete[](void* p, std::size_t) noexcept { std::free(p); }

LK_EXPORT void lk_init(double sample_rate, int max_block) {
    g_kernel.init(sample_rate, max_block);
}

// Decode + build a graph blob into the inactive plan. Returns 0 on success,
// negative on error (see DecodeError). Zero-alloc.
LK_EXPORT int lk_load_plan(const unsigned char* bytes, int len) {
    return g_kernel.load_plan(bytes, len);
}

// Arm the equal-power crossfade to the freshly-built plan.
LK_EXPORT void lk_swap(float fade_ms) { g_kernel.swap(fade_ms); }

// Zero-interruption param edit on the active plan.
LK_EXPORT void lk_set_param(int node, int param_id, float value) {
    g_kernel.set_param(node, param_id, value);
}

// Render `n` frames of mono audio into dst. Zero-alloc.
LK_EXPORT void lk_process(float* dst, int n) { g_kernel.process(dst, n); }

LK_EXPORT int    lk_is_fading()    { return g_kernel.is_fading() ? 1 : 0; }
LK_EXPORT int    lk_active_valid() { return g_kernel.active_valid() ? 1 : 0; }
LK_EXPORT double lk_sample_rate()  { return g_kernel.sample_rate(); }

// Copy the current graph's per-node output RMS into dst[0..max). Returns the
// node count written. Alloc-free: dst is a preallocated wasm buffer, node_rms is
// filled in-place by render_block, so this never touches the heap (the signal-
// flow graph's live readout keeps lk_alloc_count flat).
LK_EXPORT int lk_node_levels(float* dst, int max) { return g_kernel.node_levels(dst, max); }

// Toggle the per-node level tap. 0 = measurement mode (skip the tap so CPU
// benchmarks exclude the meter cost); nonzero = on (the worklet default).
LK_EXPORT void lk_set_meter(int on) { g_kernel.set_meter(on != 0); }

// The zero-alloc instrument: total C++ heap allocations since load.
LK_EXPORT double lk_alloc_count()  { return (double)g_alloc_count; }

// ── F2 libm bridge ────────────────────────────────────────────────────────────
// The F2 graph→wasm emitter (examples/web-demos/live-kernel-spike/f2-emitter.js)
// folds derived parameters (gain dB→linear, ladder g, biquad/SVF coefficients)
// at emit time and calls per-sample transcendentals (ladder tanh, sine osc) at
// run time THROUGH these exports — i.e. through the exact same emcc-linked musl
// libm bits the interpreter itself executes. That is what makes the emitted
// module bit-exact to the kernel VM and the AOT twins: every non-rational
// operation routes to the same compiled code; every rational f32 op is IEEE-754
// exact by definition. Pure float→float, no memory access, alloc-free.
LK_EXPORT float f2_tanhf(float x)          { return std::tanh(x); }
// The ladder's saturator is NOT libm tanh: LadderFilterT<float> uses
// signal::FastMath::tanh (Padé) on the real-time path. The emitter must route
// the ladder — and only the ladder — through this export, or the emitted module
// diverges from the interpreter by ~1 ulp and the bit-exact null test fails.
// The waveshaper's tanh_clip curve still wants real libm tanh above.
LK_EXPORT float f2_ladder_tanhf(float x)   { return pulp::signal::FastMath::tanh(x); }
LK_EXPORT float f2_sinf(float x)           { return std::sin(x); }
LK_EXPORT float f2_cosf(float x)           { return std::cos(x); }
LK_EXPORT float f2_expf(float x)           { return std::exp(x); }
LK_EXPORT float f2_tanf(float x)           { return std::tan(x); }
LK_EXPORT float f2_powf(float x, float y)  { return std::pow(x, y); }
LK_EXPORT float f2_fmodf(float x, float y) { return std::fmod(x, y); }
