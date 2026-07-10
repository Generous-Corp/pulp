// Pulp Live Kernel — S0 spike — AOT twin (the CPU baseline + null-test oracle).
//
// Hand-fused, straight-line compiled equivalents of the three spike patches,
// built the SAME way as a WAM plugin (Emscripten). Each twin instantiates the
// exact same core/signal classes with the exact same parameters as the kernel-VM
// plan (it reuses the registry's init_node/set_node_param so the DSP config
// cannot drift), but runs them as ONE fused per-sample loop instead of the
// interpreter's block-per-node walk. So:
//   * null test  = kernel-VM render vs this twin  -> isolates execution
//                   equivalence (should be ~exact; same classes, same order).
//   * CPU ratio  = kernel time / this twin's time -> isolates interpreter
//                   overhead (buffer traffic + per-node dispatch vs fusion).
//
// aot_* C ABI, entirely separate from wam_* and lk_*.

#include "registry.hpp"

#include <cstring>
#include <cstdlib>
#include <cstddef>
#include <new>

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#define AOT_EXPORT extern "C" EMSCRIPTEN_KEEPALIVE
#else
#define AOT_EXPORT extern "C"
#endif

using namespace pulp::live_kernel;

// Share the zero-alloc instrument shape with lk_entry when linked together is not
// needed — the twin is its own module. Define plain new here.
namespace {

constexpr int MAXB = LK_MAX_BLOCK;

struct MusicalTwin {
    NodeInstance osc1, osc2, mix, adsr, ladder, biquad, delay, g7, g8, g9;
    Delay dl;
};
struct TrivialTwin {
    NodeInstance osc, g[9];
};
struct FeedbackTwin {
    NodeInstance osc, mix, delay, gain;
    Delay dl;
    float fb_prev[MAXB];
};

MusicalTwin  M;
TrivialTwin  T;
FeedbackTwin F;
double g_sr = 48000.0;

void setup_musical(double sr) {
    M.dl.prepare(48000);
    M.delay.delay = &M.dl;
    init_node(M.osc1, NodeType::Oscillator, sr);
    init_node(M.osc2, NodeType::Oscillator, sr);
    init_node(M.mix,  NodeType::Mixer, sr);
    init_node(M.adsr, NodeType::Adsr, sr);
    init_node(M.ladder, NodeType::Ladder, sr);
    init_node(M.biquad, NodeType::Biquad, sr);
    init_node(M.delay, NodeType::Delay, sr);
    init_node(M.g7, NodeType::Gain, sr);
    init_node(M.g8, NodeType::Gain, sr);
    init_node(M.g9, NodeType::Gain, sr);
    // params (MUST match the JS-built LKB0 musical blob)
    set_node_param(M.osc1, NodeType::Oscillator, 0, 110.0f, sr);
    set_node_param(M.osc1, NodeType::Oscillator, 1, 1.0f, sr); // saw
    set_node_param(M.osc1, NodeType::Oscillator, 2, 0.3f, sr);
    set_node_param(M.osc2, NodeType::Oscillator, 0, 110.77f, sr);
    set_node_param(M.osc2, NodeType::Oscillator, 1, 1.0f, sr);
    set_node_param(M.osc2, NodeType::Oscillator, 2, 0.3f, sr);
    set_node_param(M.mix, NodeType::Mixer, 0, 0.5f, sr);
    set_node_param(M.adsr, NodeType::Adsr, 0, 0.005f, sr);
    set_node_param(M.adsr, NodeType::Adsr, 1, 0.12f, sr);
    set_node_param(M.adsr, NodeType::Adsr, 2, 0.6f, sr);
    set_node_param(M.adsr, NodeType::Adsr, 3, 0.3f, sr);
    set_node_param(M.ladder, NodeType::Ladder, 0, 1200.0f, sr);
    set_node_param(M.ladder, NodeType::Ladder, 1, 0.4f, sr);
    set_node_param(M.biquad, NodeType::Biquad, 0, 5.0f, sr); // peaking
    set_node_param(M.biquad, NodeType::Biquad, 1, 2500.0f, sr);
    set_node_param(M.biquad, NodeType::Biquad, 2, 1.0f, sr);
    set_node_param(M.biquad, NodeType::Biquad, 3, 6.0f, sr);
    set_node_param(M.delay, NodeType::Delay, 0, 0.18f, sr);
    set_node_param(M.delay, NodeType::Delay, 1, 0.35f, sr);
    set_node_param(M.delay, NodeType::Delay, 2, 0.35f, sr);
    set_node_param(M.g7, NodeType::Gain, 0, -3.0f, sr);
    set_node_param(M.g8, NodeType::Gain, 0, 0.0f, sr);
    set_node_param(M.g9, NodeType::Gain, 0, -2.0f, sr);
    // gate on
    set_node_param(M.adsr, NodeType::Adsr, 4, 1.0f, sr);
}

void setup_trivial(double sr) {
    init_node(T.osc, NodeType::Oscillator, sr);
    set_node_param(T.osc, NodeType::Oscillator, 0, 220.0f, sr);
    set_node_param(T.osc, NodeType::Oscillator, 1, 1.0f, sr);
    set_node_param(T.osc, NodeType::Oscillator, 2, 0.3f, sr);
    for (int i = 0; i < 9; ++i) {
        init_node(T.g[i], NodeType::Gain, sr);
        set_node_param(T.g[i], NodeType::Gain, 0, 0.0f, sr);
    }
}

void setup_feedback(double sr) {
    F.dl.prepare(48000);
    F.delay.delay = &F.dl;
    std::memset(F.fb_prev, 0, sizeof(F.fb_prev));
    init_node(F.osc, NodeType::Oscillator, sr);
    init_node(F.mix, NodeType::Mixer, sr);
    init_node(F.delay, NodeType::Delay, sr);
    init_node(F.gain, NodeType::Gain, sr);
    set_node_param(F.osc, NodeType::Oscillator, 0, 110.0f, sr);
    set_node_param(F.osc, NodeType::Oscillator, 1, 1.0f, sr);
    set_node_param(F.osc, NodeType::Oscillator, 2, 0.25f, sr);
    set_node_param(F.mix, NodeType::Mixer, 0, 0.5f, sr);
    set_node_param(F.delay, NodeType::Delay, 0, 0.15f, sr);
    set_node_param(F.delay, NodeType::Delay, 1, 0.0f, sr);
    set_node_param(F.delay, NodeType::Delay, 2, 0.5f, sr);
    set_node_param(F.gain, NodeType::Gain, 0, -6.0f, sr);
}

} // namespace

void* operator new(std::size_t n) { void* p = std::malloc(n ? n : 1); if (!p) std::abort(); return p; }
void* operator new[](std::size_t n) { return ::operator new(n); }
void  operator delete(void* p) noexcept { std::free(p); }
void  operator delete[](void* p) noexcept { std::free(p); }
void  operator delete(void* p, std::size_t) noexcept { std::free(p); }
void  operator delete[](void* p, std::size_t) noexcept { std::free(p); }

AOT_EXPORT void aot_init(double sr) {
    g_sr = sr;
    setup_musical(sr);
    setup_trivial(sr);
    setup_feedback(sr);
}

// patch: 0=musical, 1=trivial, 2=feedback
AOT_EXPORT void aot_process(int patch, float* dst, int n) {
    if (n > MAXB) n = MAXB;
    if (patch == 0) {
        for (int i = 0; i < n; ++i) {
            float o1 = M.osc1.osc.next() * M.osc1.osc_amp;
            float o2 = M.osc2.osc.next() * M.osc2.osc_amp;
            float mx = M.mix.mixer.process(o1, o2);
            float e  = M.adsr.adsr.next();
            float a  = mx * e;
            float l  = M.ladder.ladder.process(a);
            float b  = M.biquad.biquad.process(l);
            float wet = M.delay.delay->read(M.delay.dl_samps);
            M.delay.delay->push(b + M.delay.dl_fb * wet);
            float d  = b * (1.f - M.delay.dl_mix) + wet * M.delay.dl_mix;
            float g  = M.g9.gain.process(M.g8.gain.process(M.g7.gain.process(d)));
            dst[i] = g;
        }
    } else if (patch == 1) {
        for (int i = 0; i < n; ++i) {
            float s = T.osc.osc.next() * T.osc.osc_amp;
            for (int k = 0; k < 9; ++k) s = T.g[k].gain.process(s);
            dst[i] = s;
        }
    } else {
        for (int i = 0; i < n; ++i) {
            float fb = F.fb_prev[i];
            float o  = F.osc.osc.next() * F.osc.osc_amp;
            float mx = F.mix.mixer.process(o, fb);
            float wet = F.delay.delay->read(F.delay.dl_samps);
            F.delay.delay->push(mx + F.delay.dl_fb * wet);
            float d  = mx * (1.f - F.delay.dl_mix) + wet * F.delay.dl_mix;
            float g  = F.gain.gain.process(d);
            dst[i] = g;
            F.fb_prev[i] = g; // captured for next block's feedback
        }
    }
}
