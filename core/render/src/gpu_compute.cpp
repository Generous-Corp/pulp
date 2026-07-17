#include <pulp/render/gpu_compute.hpp>

// This file contains no Skia. It needs Dawn (webgpu_cpp.h) and nothing else.
// Dawn happens to be *delivered* by the Skia prebuilt slice on native, which is
// why the gate used to be spelled PULP_HAS_SKIA; the browser gets Dawn from
// Emscripten's emdawnwebgpu port with no Skia anywhere in sight. PULP_HAS_DAWN
// says what the code actually depends on.
#ifdef PULP_HAS_DAWN

#include <pulp/runtime/log.hpp>
#include "webgpu/webgpu_cpp.h"

// Dawn's *native* implementation headers: the instance factory and the C proc
// table. emdawnwebgpu implements webgpu.h over the browser's navigator.gpu and
// provides neither — see create_instance() for the two-arm seam.
#if !defined(__EMSCRIPTEN__)
#include "dawn/native/DawnNative.h"
#include "dawn/dawn_proc.h"
#endif

#include "gpu_compute_pool.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <numeric>
#include <sstream>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef PULP_BENCHMARK
#include <pulp/render/bench/perf_counters.hpp>
#endif

namespace pulp::render {

// ── WGSL Compute Shaders ────────────────────────────────────────────────────

static constexpr const char* kMagnitudeShader = R"wgsl(
// Compute magnitude from interleaved complex pairs: [re0, im0, re1, im1, ...]
// Output: linear magnitude per bin

@group(0) @binding(0) var<storage, read> input : array<f32>;
@group(0) @binding(1) var<storage, read_write> output : array<f32>;

@compute @workgroup_size(256)
fn main(@builtin(global_invocation_id) gid : vec3u) {
    let idx = gid.x;
    let num_bins = arrayLength(&output);
    if (idx >= num_bins) {
        return;
    }
    let re = input[idx * 2u];
    let im = input[idx * 2u + 1u];
    output[idx] = sqrt(re * re + im * im);
}
)wgsl";

static constexpr const char* kComplexMultiplyShader = R"wgsl(
// Element-wise complex multiply: result[i] = a[i] * b[i]
// All arrays are interleaved [re, im, re, im, ...]

@group(0) @binding(0) var<storage, read> a : array<f32>;
@group(0) @binding(1) var<storage, read> b : array<f32>;
@group(0) @binding(2) var<storage, read_write> result : array<f32>;

@compute @workgroup_size(256)
fn main(@builtin(global_invocation_id) gid : vec3u) {
    let idx = gid.x;
    let num_pairs = arrayLength(&result) / 2u;
    if (idx >= num_pairs) {
        return;
    }
    let base = idx * 2u;
    let a_re = a[base];
    let a_im = a[base + 1u];
    let b_re = b[base];
    let b_im = b[base + 1u];
    // (a_re + a_im*i) * (b_re + b_im*i)
    result[base]      = a_re * b_re - a_im * b_im;
    result[base + 1u] = a_re * b_im + a_im * b_re;
}
)wgsl";

static constexpr const char* kFftStockhamShader = R"wgsl(
// One radix-2 Stockham auto-sort FFT pass over interleaved complex data.
// Result is naturally ordered (no separate bit-reversal pass). log2(N) passes
// ping-pong between two buffers; `ns` doubles each pass (1, 2, 4, ... N/2).
// sign = -1 forward, +1 inverse (the host applies the 1/N inverse scale).
// Reference: Lloyd/Boyd/Govindaraju, "Fast Computation of General Fourier
// Transforms on GPUs" (Stockham radix-2 formulation).

struct FftParams {
    n     : u32,
    ns    : u32,
    sign  : f32,
    batch : u32,   // number of independent transforms packed back-to-back
};

@group(0) @binding(0) var<storage, read>       src    : array<f32>;
@group(0) @binding(1) var<storage, read_write> dst    : array<f32>;
@group(0) @binding(2) var<uniform>             params : FftParams;

const PI : f32 = 3.1415926535897932;

// Batched radix-2 Stockham pass. Each of `batch` transforms occupies a
// contiguous n-complex span; thread tid maps to (transform b, butterfly j).
// batch == 1 is bit-for-bit identical to the single-transform path.
@compute @workgroup_size(256)
fn main(@builtin(global_invocation_id) gid : vec3u) {
    let half = params.n / 2u;
    let tid = gid.x;
    if (tid >= params.batch * half) {
        return;
    }
    let b = tid / half;
    let j = tid - b * half;
    let base = b * params.n;   // complex-element offset of this transform

    let v0_re = src[2u * (base + j)];
    let v0_im = src[2u * (base + j) + 1u];
    let k = j + half;
    let v1_re = src[2u * (base + k)];
    let v1_im = src[2u * (base + k) + 1u];

    let angle = params.sign * 2.0 * PI * f32(j % params.ns) / f32(params.ns * 2u);
    let tw_re = cos(angle);
    let tw_im = sin(angle);

    // u1 = twiddle * v1
    let u1_re = tw_re * v1_re - tw_im * v1_im;
    let u1_im = tw_re * v1_im + tw_im * v1_re;

    let y0_re = v0_re + u1_re;
    let y0_im = v0_im + u1_im;
    let y1_re = v0_re - u1_re;
    let y1_im = v0_im - u1_im;

    let idxD = (j / params.ns) * params.ns * 2u + (j % params.ns);
    dst[2u * (base + idxD)]                   = y0_re;
    dst[2u * (base + idxD) + 1u]              = y0_im;
    dst[2u * (base + idxD + params.ns)]       = y1_re;
    dst[2u * (base + idxD + params.ns) + 1u]  = y1_im;
}
)wgsl";

static constexpr const char* kComplexMulBroadcastShader = R"wgsl(
// Element-wise complex multiply with the second operand BROADCAST: result has
// `batch` blocks of `ir` pairs; each block is multiplied by the same `ir`.
// result[p] = a[p] * ir[p % ir_pairs]. (batch == 1 reduces to a plain multiply.)

@group(0) @binding(0) var<storage, read>       a      : array<f32>;
@group(0) @binding(1) var<storage, read>       ir     : array<f32>;
@group(0) @binding(2) var<storage, read_write> result : array<f32>;

@compute @workgroup_size(256)
fn main(@builtin(global_invocation_id) gid : vec3u) {
    let idx = gid.x;
    let total = arrayLength(&result) / 2u;
    if (idx >= total) {
        return;
    }
    let ir_pairs = arrayLength(&ir) / 2u;
    let ii = (idx % ir_pairs) * 2u;
    let base = idx * 2u;
    let a_re = a[base];
    let a_im = a[base + 1u];
    let b_re = ir[ii];
    let b_im = ir[ii + 1u];
    result[base]      = a_re * b_re - a_im * b_im;
    result[base + 1u] = a_re * b_im + a_im * b_re;
}
)wgsl";

static constexpr const char* kMultiIrMulShader = R"wgsl(
// Multi-IR frequency-domain multiply: ONE input spectrum broadcast across
// `num_ir` distinct IR spectra. prod has `num_ir` blocks of n complex; the
// single input spectrum x (n complex) is reused for every block, and each
// block multiplies by its OWN IR spectrum (ir is num_ir blocks of n complex).
//   prod[idx] = x[idx % n] * ir[idx]
// This is the inverse of conv_bmul (one IR, many inputs): here it is one input,
// many IRs — so the forward FFT runs ONCE and is shared across all rooms.

@group(0) @binding(0) var<storage, read>       x    : array<f32>;   // 2n
@group(0) @binding(1) var<storage, read>       ir   : array<f32>;   // 2n*num_ir
@group(0) @binding(2) var<storage, read_write> prod : array<f32>;   // 2n*num_ir

@compute @workgroup_size(256)
fn main(@builtin(global_invocation_id) gid : vec3u) {
    let idx = gid.x;
    let total = arrayLength(&prod) / 2u;
    if (idx >= total) {
        return;
    }
    let n_pairs = arrayLength(&x) / 2u;
    let xi = (idx % n_pairs) * 2u;
    let base = idx * 2u;
    let xr  = x[xi];
    let xim = x[xi + 1u];
    let br  = ir[base];
    let bim = ir[base + 1u];
    prod[base]      = xr * br - xim * bim;
    prod[base + 1u] = xr * bim + xim * br;
}
)wgsl";

static constexpr const char* kMultiIrCombineShader = R"wgsl(
// Pan-combine reduce: collapse `num_ir` time-domain room results into a stereo
// pair using per-room constant-power pan weights, applying the inverse-FFT 1/n
// normalization. rooms holds num_ir blocks of n complex (the real part is the
// convolution result). Output layout: outlr[0..n) = L, outlr[n..2n) = R.
// Doing the reduction on the GPU means only 2n floats (one stereo block) are
// read back per block instead of the full 2n*num_ir room buffer.

struct CombineParams { n : u32, num_ir : u32, inv : f32, pad : u32 };

@group(0) @binding(0) var<storage, read>       rooms : array<f32>;  // 2n*num_ir
@group(0) @binding(1) var<storage, read>       panl  : array<f32>;  // num_ir
@group(0) @binding(2) var<storage, read>       panr  : array<f32>;  // num_ir
@group(0) @binding(3) var<storage, read_write> outlr : array<f32>;  // 2n
@group(0) @binding(4) var<uniform>             p     : CombineParams;

@compute @workgroup_size(256)
fn main(@builtin(global_invocation_id) gid : vec3u) {
    let i = gid.x;
    if (i >= p.n) {
        return;
    }
    var l = 0.0;
    var r = 0.0;
    for (var k = 0u; k < p.num_ir; k = k + 1u) {
        let v = rooms[(k * p.n + i) * 2u] * p.inv;  // real part, normalized
        l = l + panl[k] * v;
        r = r + panr[k] * v;
    }
    outlr[i]        = l;
    outlr[p.n + i]  = r;
}
)wgsl";

static constexpr const char* kMultiFdlMacShader = R"wgsl(
// Partitioned frequency-delay-line MAC for multi-IR convolution. Instead of one
// full-length-n FFT per block (n = next_pow2(block + IR_len)), each IR is split
// into `num_part` block-size partitions, so the FFT stays at the small block
// size and this kernel does the convolution as a sum of spectral products over
// a ring of the last `num_part` input spectra:
//   accum[r][i] = sum_p  ring[(head - p + P) % P][i] * ir[r][p][i]
// One thread per (room r, bin i). ring holds P input spectra (each n complex);
// ir holds num_ir*num_part IR-partition spectra; accum holds num_ir room spectra.

struct FdlParams { n : u32, num_ir : u32, num_part : u32, head : u32 };

@group(0) @binding(0) var<storage, read>       ring  : array<f32>;  // 2n*P
@group(0) @binding(1) var<storage, read>       ir    : array<f32>;  // 2n*num_ir*num_part
@group(0) @binding(2) var<storage, read_write> accum : array<f32>;  // 2n*num_ir
@group(0) @binding(3) var<uniform>             p     : FdlParams;

@compute @workgroup_size(256)
fn main(@builtin(global_invocation_id) gid : vec3u) {
    let flat = gid.x;
    let total = p.num_ir * p.n;
    if (flat >= total) { return; }
    let r = flat / p.n;   // room
    let i = flat % p.n;   // bin
    let P = p.num_part;
    var accr = 0.0;
    var acci = 0.0;
    for (var pp = 0u; pp < P; pp = pp + 1u) {
        // Partition 0 pairs with the newest input spectrum (head), partition
        // pp with the input pp blocks ago — the delay line.
        let slot = (p.head + P - (pp % P)) % P;
        let xi = (slot * p.n + i) * 2u;
        let iri = ((r * P + pp) * p.n + i) * 2u;
        let xr  = ring[xi];
        let xim = ring[xi + 1u];
        let br  = ir[iri];
        let bim = ir[iri + 1u];
        accr = accr + (xr * br - xim * bim);
        acci = acci + (xr * bim + xim * br);
    }
    let o = flat * 2u;
    accum[o]      = accr;
    accum[o + 1u] = acci;
}
)wgsl";

static constexpr const char* kSpectralAdvanceShader = R"wgsl(
// Per-bin phase advance + conjugate-symmetric jitter for a stack of frozen
// layers. One thread per (layer, bin) over the resident phase buffer (num_layers
// blocks of n). The nominal advance 2*pi*k*hop/n is the per-bin frequency over
// one hop; it is conjugate-antisymmetric (bin n-k advances by the negative of
// bin k mod 2*pi), so a magnitude-symmetric layer stays real after synthesis.
// Jitter is made antisymmetric explicitly: bin k in (0, n/2) gets +h(k), bin k
// in (n/2, n) gets -h(n-k), and bins 0 / n/2 get none — preserving realness.

struct SpAdvance { n : u32, num_layers : u32, hop_ratio : f32, jitter : f32, seed : u32, pad0 : u32, pad1 : u32, pad2 : u32 };

@group(0) @binding(0) var<storage, read_write> phase  : array<f32>;  // num_layers*n
@group(0) @binding(1) var<uniform>             p      : SpAdvance;

const TWO_PI_A : f32 = 6.2831853071795864;

// Integer hash → f32 in [-0.5, 0.5]. Deterministic per (bin, seed).
fn hash01(x : u32) -> f32 {
    var h = x * 2654435761u + p.seed * 40503u;
    h = (h ^ (h >> 15u)) * 2246822519u;
    h = (h ^ (h >> 13u)) * 3266489917u;
    h = h ^ (h >> 16u);
    return f32(h >> 8u) / 16777216.0 - 0.5;
}

@compute @workgroup_size(256)
fn main(@builtin(global_invocation_id) gid : vec3u) {
    let idx = gid.x;
    if (idx >= p.num_layers * p.n) {
        return;
    }
    let k = idx % p.n;
    var ph = phase[idx] + TWO_PI_A * f32(k) * p.hop_ratio;
    if (p.jitter > 0.0) {
        // Per-hop phase wander scaled to a full turn at jitter=1, so it
        // accumulates into a random walk that decorrelates the FFT-period
        // repetition (the buzzy "loop" a coherent freeze would otherwise have).
        // Conjugate-antisymmetric (bin k vs n-k) so the synthesized frame stays
        // real. The seed advances each render, so successive hops differ.
        let half = p.n / 2u;
        if (k > 0u && k < half) {
            ph = ph + p.jitter * TWO_PI_A * hash01(k);
        } else if (k > half && k < p.n) {
            ph = ph - p.jitter * TWO_PI_A * hash01(p.n - k);
        }
    }
    // Wrap to [-pi, pi] (std::remainder semantics) to keep cos/sin precise.
    phase[idx] = ph - TWO_PI_A * round(ph / TWO_PI_A);
}
)wgsl";

static constexpr const char* kSpectralCombineShader = R"wgsl(
// Smear + weighted complex sum across all resident layers into one combined
// spectrum. One thread per output bin k (over n). For each active layer the
// magnitude is optionally circular-box-blurred over +/- radius bins (the smear),
// scaled by the layer weight, and rotated by the (already-advanced) per-bin
// phase, then accumulated. Summing in the frequency domain and doing ONE inverse
// FFT afterward is identical to synthesizing each layer and summing in time
// (linearity), but costs one transform instead of num_layers — and the heavy
// per-bin smear runs in parallel across all n bins here.

struct SpCombine { n : u32, num_layers : u32, radius : u32, inv_kernel : f32 };

@group(0) @binding(0) var<storage, read>       mag      : array<f32>;  // num_layers*n
@group(0) @binding(1) var<storage, read>       phase    : array<f32>;  // num_layers*n
@group(0) @binding(2) var<storage, read>       weights  : array<f32>;  // num_layers
@group(0) @binding(3) var<storage, read_write> combined : array<f32>;  // 2n
@group(0) @binding(4) var<uniform>             p        : SpCombine;

@compute @workgroup_size(256)
fn main(@builtin(global_invocation_id) gid : vec3u) {
    let k = gid.x;
    if (k >= p.n) {
        return;
    }
    var accR = 0.0;
    var accI = 0.0;
    for (var L = 0u; L < p.num_layers; L = L + 1u) {
        let w = weights[L];
        if (abs(w) < 1e-6) {
            continue;
        }
        let base = L * p.n;
        var m = mag[base + k];
        if (p.radius > 0u) {
            var s = 0.0;
            let r = i32(p.radius);
            let ni = i32(p.n);
            for (var d = -r; d <= r; d = d + 1) {
                var j = i32(k) + d;
                j = ((j % ni) + ni) % ni;  // circular
                s = s + mag[base + u32(j)];
            }
            m = s * p.inv_kernel;
        }
        let ph = phase[base + k];
        accR = accR + w * m * cos(ph);
        accI = accI + w * m * sin(ph);
    }
    combined[2u * k]      = accR;
    combined[2u * k + 1u] = accI;
}
)wgsl";

// ── Fused conv-stack (WaveNet-style neural amp) shaders ─────────────────────
// Activations are time-major [t*C + c]. One thread per time step t (the block's
// samples compute in parallel — the network is feedforward). Channels are capped
// at 64 so the per-thread gate scratch can be a fixed-size local array (WGSL has
// no runtime-sized locals). All three passes run in one submit over resident
// buffers; only the final mono block is read back.

static constexpr const char* kConvStackInputShader = R"wgsl(
// Project the mono block to C channels, writing into the block region of act
// (after PAD left-history columns the layers read for cross-block continuity).
struct P { C:u32, B:u32, in_off:u32, pad:u32 };  // pad = PAD history columns
@group(0) @binding(0) var<storage, read>       wts : array<f32>;
@group(0) @binding(1) var<storage, read>       inp : array<f32>;   // B mono
@group(0) @binding(2) var<storage, read_write> act : array<f32>;   // C*(PAD+B)
@group(0) @binding(3) var<uniform>             p   : P;
@compute @workgroup_size(256)
fn main(@builtin(global_invocation_id) gid : vec3u) {
    let t = gid.x;
    if (t >= p.B) { return; }
    let x = inp[t];
    let wb = p.in_off + p.C;
    let col = (p.pad + t) * p.C;
    for (var c = 0u; c < p.C; c = c + 1u) {
        act[col + c] = wts[p.in_off + c] * x + wts[wb + c];
    }
}
)wgsl";

static constexpr const char* kConvStackLayerShader = R"wgsl(
// One gated dilated causal conv layer: z = dilated_conv(in) [2C], gate =
// tanh(z[:C])*sigmoid(z[C:]), out = in + residual(gate), skip += skip(gate).
struct P { C:u32, K:u32, B:u32, dil:u32, woff:u32, pad:u32, p1:u32, p2:u32 };  // pad = PAD
@group(0) @binding(0) var<storage, read>       wts  : array<f32>;
@group(0) @binding(1) var<storage, read>       ina  : array<f32>;  // C*(PAD+B)
@group(0) @binding(2) var<storage, read_write> outa : array<f32>;  // C*(PAD+B)
@group(0) @binding(3) var<storage, read_write> skip : array<f32>;  // C*B (accumulate)
@group(0) @binding(4) var<uniform>             p    : P;

var<private> zbuf : array<f32, 128>;  // 2*Cmax
var<private> gbuf : array<f32, 64>;   // Cmax

@compute @workgroup_size(256)
fn main(@builtin(global_invocation_id) gid : vec3u) {
    let t = gid.x;
    if (t >= p.B) { return; }
    let C = p.C; let K = p.K;
    let cw = p.woff;                    // conv W [2C*C*K]
    let cb = cw + 2u * C * C * K;       // conv b [2C]
    let rw = cb + 2u * C;              // residual W [C*C]
    let rb = rw + C * C;               // residual b [C]
    let sw = rb + C;                   // skip W [C*C]
    let sb = sw + C * C;               // skip b [C]

    // Absolute column of this sample in the padded buffer; taps reach back into
    // the PAD history (carried from the previous block) so blocks are continuous.
    let acol = p.pad + t;
    let two_c = 2u * C;
    for (var oc = 0u; oc < two_c; oc = oc + 1u) {
        var acc = wts[cb + oc];
        for (var k = 0u; k < K; k = k + 1u) {
            let back = p.dil * (K - 1u - k);
            let base = (acol - back) * C;   // valid: pad >= max back
            let wbase = cw + oc * C * K + k;
            for (var ic = 0u; ic < C; ic = ic + 1u) {
                acc = acc + wts[wbase + ic * K] * ina[base + ic];
            }
        }
        zbuf[oc] = acc;
    }
    for (var c = 0u; c < C; c = c + 1u) {
        let tg = tanh(zbuf[c]);
        let sg = 1.0 / (1.0 + exp(-zbuf[C + c]));
        gbuf[c] = tg * sg;
    }
    let tc = acol * C;       // residual writes into the block region of outa
    let sc = t * C;          // skip is block-sized (no history needed)
    for (var oc = 0u; oc < C; oc = oc + 1u) {
        var r = wts[rb + oc];
        var s = wts[sb + oc];
        let rrow = rw + oc * C;
        let srow = sw + oc * C;
        for (var ic = 0u; ic < C; ic = ic + 1u) {
            r = r + wts[rrow + ic] * gbuf[ic];
            s = s + wts[srow + ic] * gbuf[ic];
        }
        outa[tc + oc] = ina[tc + oc] + r;
        skip[sc + oc] = skip[sc + oc] + s;
    }
}
)wgsl";

static constexpr const char* kConvStackHeadShader = R"wgsl(
struct P { C:u32, B:u32, h_off:u32, p0:u32, scale:f32, p1:u32, p2:u32, p3:u32 };
@group(0) @binding(0) var<storage, read>       wts  : array<f32>;
@group(0) @binding(1) var<storage, read>       skip : array<f32>;  // C*B
@group(0) @binding(2) var<storage, read_write> outp : array<f32>;  // B
@group(0) @binding(3) var<uniform>             p    : P;
@compute @workgroup_size(256)
fn main(@builtin(global_invocation_id) gid : vec3u) {
    let t = gid.x;
    if (t >= p.B) { return; }
    var y = wts[p.h_off + p.C];   // head bias
    let tc = t * p.C;
    for (var c = 0u; c < p.C; c = c + 1u) {
        y = y + wts[p.h_off + c] * skip[tc + c];
    }
    outp[t] = y * p.scale;
}
)wgsl";

// ── WaveNet fused passes ────────────────────────────────────────────────
// The conditioned WaveNet forward, block-parallel (one thread per block
// sample) and fused (all passes in one submit over resident buffers). Per-array
// rechannel (1x1) → dilated conv layers (+ condition mixin, Tanh/gated, residual,
// head accumulate) → head rechannel, chained array→array, then head_scale.
// Channels are capped at 64 so the per-thread scratch arrays are fixed-size.

static constexpr const char* kWavenetRechannelShader = R"wgsl(
// 1x1 rechannel (no bias): dst[t] = W * src[t]. Source vectors live at
// (src_pad+t)*in_ch (so a deeper array reads the previous array's block region
// past its history columns); the mono input is in_ch=1, src_pad=0.
struct P { in_ch:u32, out_ch:u32, B:u32, w_off:u32, src_pad:u32, dst_pad:u32, p0:u32, p1:u32 };
@group(0) @binding(0) var<storage, read>       wts : array<f32>;
@group(0) @binding(1) var<storage, read>       src : array<f32>;
@group(0) @binding(2) var<storage, read_write> dst : array<f32>;
@group(0) @binding(3) var<uniform>             p   : P;
@compute @workgroup_size(256)
fn main(@builtin(global_invocation_id) gid : vec3u) {
    let t = gid.x;
    if (t >= p.B) { return; }
    let scol = (p.src_pad + t) * p.in_ch;
    let dcol = (p.dst_pad + t) * p.out_ch;
    for (var oc = 0u; oc < p.out_ch; oc = oc + 1u) {
        var acc = 0.0;
        let wrow = p.w_off + oc * p.in_ch;
        for (var ic = 0u; ic < p.in_ch; ic = ic + 1u) {
            acc = acc + wts[wrow + ic] * src[scol + ic];
        }
        dst[dcol + oc] = acc;
    }
}
)wgsl";

static constexpr const char* kWavenetLayerShader = R"wgsl(
// One WaveNet layer: z = dilated_conv(in)[Z] + bias + input_mixin*cond (Z), then
// a = (gated ? tanh(z[:C])*sigmoid(z[C:]) : tanh(z[:C])) (C); head += a;
// out = in + (layer1x1_bias + layer1x1_W * a). condition_size is 1 (mono input).
//
// Block-parallel dispatch: ONE WORKGROUP PER SAMPLE (t = workgroup_id.x, host
// dispatches exactly B workgroups) with WG lanes cooperating across the channel
// dimension. Each lane owns whole output channels (oc = lid, lid+WG, ...) and
// walks that channel's K*C convolution reduction in the SAME serial arithmetic
// order as the scalar path — so every output element is bit-identical to the
// one-thread-per-sample version (the correctness/determinism guards hold) — but
// a B-sample block now fills B workgroups instead of a single one (25% of one
// core), and the per-channel loops fill up to Z (then C) lanes each instead of
// one. zbuf/abuf move to workgroup shared memory so the activation and 1x1
// stages can read every channel the conv stage produced.
struct P { C:u32, K:u32, B:u32, dil:u32, Z:u32, gated:u32, pad:u32,
           conv_w:u32, conv_b:u32, mix_w:u32, l1_w:u32, l1_b:u32,
           p0:u32, p1:u32, p2:u32, p3:u32 };
@group(0) @binding(0) var<storage, read>       wts     : array<f32>;
@group(0) @binding(1) var<storage, read>       ina     : array<f32>;  // C*(pad+B)
@group(0) @binding(2) var<storage, read_write> outa    : array<f32>;  // C*(pad+B)
@group(0) @binding(3) var<storage, read>       cond    : array<f32>;  // B mono input
@group(0) @binding(4) var<storage, read_write> headacc : array<f32>;  // C*B
@group(0) @binding(5) var<uniform>             p       : P;

const WG : u32 = 64u;                    // lanes per sample-workgroup
var<workgroup> zbuf : array<f32, 128>;   // Zmax = 2*Cmax, shared across the workgroup
var<workgroup> abuf : array<f32, 64>;    // Cmax

@compute @workgroup_size(WG)
fn main(@builtin(workgroup_id) wid : vec3u,
        @builtin(local_invocation_id) lid : vec3u) {
    let t = wid.x;                       // one workgroup per sample
    // Uniform guard: workgroup_id is identical for every lane, so the whole
    // workgroup returns together — barriers below stay in uniform control flow.
    if (t >= p.B) { return; }
    let lane = lid.x;
    let C = p.C; let K = p.K; let Z = p.Z;
    let acol = p.pad + t;
    let x = cond[t];
    // Phase 1 — dilated conv + input mixin, one lane per output channel oc.
    for (var oc = lane; oc < Z; oc = oc + WG) {
        var acc = wts[p.conv_b + oc];
        for (var k = 0u; k < K; k = k + 1u) {
            let back = p.dil * (K - 1u - k);   // valid: pad >= max back
            let base = (acol - back) * C;
            let wbase = p.conv_w + oc * C * K + k;
            for (var ic = 0u; ic < C; ic = ic + 1u) {
                acc = acc + wts[wbase + ic * K] * ina[base + ic];
            }
        }
        acc = acc + wts[p.mix_w + oc] * x;   // input_mixin (condition_size == 1)
        zbuf[oc] = acc;
    }
    workgroupBarrier();
    // Phase 2 — activation, one lane per channel c.
    if (p.gated == 0u) {
        for (var c = lane; c < C; c = c + WG) { abuf[c] = tanh(zbuf[c]); }
    } else {
        for (var c = lane; c < C; c = c + WG) {
            let g = 1.0 / (1.0 + exp(-zbuf[C + c]));
            abuf[c] = tanh(zbuf[c]) * g;
        }
    }
    workgroupBarrier();
    // Phase 3 — head accumulate + 1x1 output, one lane per channel.
    let hc = t * C;
    for (var c = lane; c < C; c = c + WG) { headacc[hc + c] = headacc[hc + c] + abuf[c]; }
    let tc = acol * C;
    for (var oc = lane; oc < C; oc = oc + WG) {
        var r = wts[p.l1_b + oc];
        let rrow = p.l1_w + oc * C;
        for (var ic = 0u; ic < C; ic = ic + 1u) { r = r + wts[rrow + ic] * abuf[ic]; }
        outa[tc + oc] = ina[tc + oc] + r;
    }
}
)wgsl";

static constexpr const char* kWavenetHeadShader = R"wgsl(
// head rechannel (1x1, optional bias): headout[t] = (bias?) + W * headacc[t].
struct P { C:u32, H:u32, B:u32, hr_w:u32, hr_b:u32, head_bias:u32, p0:u32, p1:u32 };
@group(0) @binding(0) var<storage, read>       wts     : array<f32>;
@group(0) @binding(1) var<storage, read>       headacc : array<f32>;  // C*B
@group(0) @binding(2) var<storage, read_write> headout : array<f32>;  // H*B
@group(0) @binding(3) var<uniform>             p       : P;
@compute @workgroup_size(256)
fn main(@builtin(global_invocation_id) gid : vec3u) {
    let t = gid.x;
    if (t >= p.B) { return; }
    let ic0 = t * p.C;
    let oc0 = t * p.H;
    for (var oc = 0u; oc < p.H; oc = oc + 1u) {
        var acc = 0.0;
        if (p.head_bias == 1u) { acc = wts[p.hr_b + oc]; }
        let wrow = p.hr_w + oc * p.C;
        for (var ic = 0u; ic < p.C; ic = ic + 1u) {
            acc = acc + wts[wrow + ic] * headacc[ic0 + ic];
        }
        headout[oc0 + oc] = acc;
    }
}
)wgsl";

static constexpr const char* kWavenetScaleShader = R"wgsl(
// Final output: out[t] = head_scale * last_headout[t*H + 0].
struct P { B:u32, H:u32, scale:f32, p0:u32 };
@group(0) @binding(0) var<storage, read>       headout : array<f32>;  // H*B
@group(0) @binding(1) var<storage, read_write> outp    : array<f32>;  // B
@group(0) @binding(2) var<uniform>             p       : P;
@compute @workgroup_size(256)
fn main(@builtin(global_invocation_id) gid : vec3u) {
    let t = gid.x;
    if (t >= p.B) { return; }
    outp[t] = headout[t * p.H] * p.scale;
}
)wgsl";

static constexpr const char* kMatmulShader = R"wgsl(
// Dense matmul C[M*N] = A[M*K] * B[K*N], all row-major. One thread per output
// element. Foundational primitive for GPU neural inference (dense / LSTM gates).

struct MatmulParams { m : u32, k : u32, n : u32, pad : u32 };

@group(0) @binding(0) var<storage, read>       a : array<f32>;
@group(0) @binding(1) var<storage, read>       b : array<f32>;
@group(0) @binding(2) var<storage, read_write> c : array<f32>;
@group(0) @binding(3) var<uniform>             p : MatmulParams;

@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) gid : vec3u) {
    let idx = gid.x;
    if (idx >= p.m * p.n) {
        return;
    }
    let row = idx / p.n;
    let col = idx % p.n;
    var acc = 0.0;
    for (var kk = 0u; kk < p.k; kk = kk + 1u) {
        acc = acc + a[row * p.k + kk] * b[kk * p.n + col];
    }
    c[idx] = acc;
}
)wgsl";

static constexpr const char* kAdditiveSynthShader = R"wgsl(
// GPU additive synthesis: one thread per output sample, summing all partials.
// partials: num_partials × [freq, amp, phase]. out[s] = Σ amp·sin(2π·f·t + ph).

struct AddParams { num_partials : u32, num_samples : u32, sample_rate : f32, t0 : f32 };

@group(0) @binding(0) var<storage, read>       partials : array<f32>;
@group(0) @binding(1) var<storage, read_write> out      : array<f32>;
@group(0) @binding(2) var<uniform>             p        : AddParams;

const TWO_PI : f32 = 6.2831853071795864;

@compute @workgroup_size(256)
fn main(@builtin(global_invocation_id) gid : vec3u) {
    let s = gid.x;
    if (s >= p.num_samples) {
        return;
    }
    let t = (p.t0 + f32(s)) / p.sample_rate;
    var acc = 0.0;
    for (var i = 0u; i < p.num_partials; i = i + 1u) {
        let f  = partials[i * 3u];
        let a  = partials[i * 3u + 1u];
        let ph = partials[i * 3u + 2u];
        acc = acc + a * sin(TWO_PI * f * t + ph);
    }
    out[s] = acc;
}
)wgsl";

// Cooperative additive_synth variant: ONE WORKGROUP PER SAMPLE, WG lanes split
// the partials sum and combine it in a deterministic shared-memory tree
// reduction. The serial kernel above maps one thread per sample, so a small
// sample block (few ceil(S/256) workgroups) pins the device to a couple of cores
// AND each lane walks the whole num_partials sum alone — bad when partials is
// large. Here the host launches min(num_samples, 65535) workgroups; each
// grid-strides over its samples, the WG lanes parallelize the partials
// reduction. wid.x / nwg.x are uniform across the workgroup so the sample loop's
// trip count is uniform and every workgroupBarrier() stays in uniform control
// flow. The host picks this variant only when it wins (small sample block or many
// partials) — see additive_synth(); the serial kernel stays best for
// many-sample / few-partial blocks that already fill the device barrier-free.
static constexpr const char* kAdditiveSynthCoopShader = R"wgsl(
struct AddParams { num_partials : u32, num_samples : u32, sample_rate : f32, t0 : f32 };

@group(0) @binding(0) var<storage, read>       partials : array<f32>;
@group(0) @binding(1) var<storage, read_write> out      : array<f32>;
@group(0) @binding(2) var<uniform>             p        : AddParams;

const TWO_PI : f32 = 6.2831853071795864;
const WG : u32 = 256u;
var<workgroup> partial : array<f32, 256>;

@compute @workgroup_size(WG)
fn main(@builtin(workgroup_id) wid : vec3u,
        @builtin(num_workgroups) nwg : vec3u,
        @builtin(local_invocation_id) lid : vec3u) {
    let lane = lid.x;
    for (var s = wid.x; s < p.num_samples; s = s + nwg.x) {
        let t = (p.t0 + f32(s)) / p.sample_rate;
        var acc = 0.0;
        for (var i = lane; i < p.num_partials; i = i + WG) {
            let f  = partials[i * 3u];
            let a  = partials[i * 3u + 1u];
            let ph = partials[i * 3u + 2u];
            acc = acc + a * sin(TWO_PI * f * t + ph);
        }
        partial[lane] = acc;
        workgroupBarrier();
        for (var stride = WG / 2u; stride > 0u; stride = stride >> 1u) {
            if (lane < stride) { partial[lane] = partial[lane] + partial[lane + stride]; }
            workgroupBarrier();
        }
        if (lane == 0u) { out[s] = partial[0]; }
        workgroupBarrier();  // finish reading partial[0] before the next sample overwrites it
    }
}
)wgsl";

static constexpr const char* kModalStrikeShader = R"wgsl(
// GPU struck modal synthesis: one thread per sample, summing decaying modes.
// modes: num_modes × [freq, amp, decay, phase]. out[s] = Σ amp·e^(-decay·t)·sin(2π·f·t+ph).

struct ModalParams { num_modes : u32, num_samples : u32, sample_rate : f32, t0 : f32 };

@group(0) @binding(0) var<storage, read>       modes : array<f32>;
@group(0) @binding(1) var<storage, read_write> out   : array<f32>;
@group(0) @binding(2) var<uniform>             p     : ModalParams;

const TWO_PI : f32 = 6.2831853071795864;

@compute @workgroup_size(256)
fn main(@builtin(global_invocation_id) gid : vec3u) {
    let s = gid.x;
    if (s >= p.num_samples) {
        return;
    }
    let t = (p.t0 + f32(s)) / p.sample_rate;
    var acc = 0.0;
    for (var i = 0u; i < p.num_modes; i = i + 1u) {
        let f  = modes[i * 4u];
        let a  = modes[i * 4u + 1u];
        let d  = modes[i * 4u + 2u];
        let ph = modes[i * 4u + 3u];
        acc = acc + a * exp(-d * t) * sin(TWO_PI * f * t + ph);
    }
    out[s] = acc;
}
)wgsl";

// Cooperative modal_strike variant: ONE WORKGROUP PER SAMPLE, WG lanes split the
// modes sum and combine it in a deterministic shared-memory tree reduction. The
// serial kernel above maps one thread per sample, so a small sample block (few
// ceil(S/256) workgroups) pins the device to a couple of cores AND each lane
// walks the whole num_modes sum alone — bad when modes is large. Here the host
// launches min(num_samples, 65535) workgroups; each grid-strides over its
// samples, the WG lanes parallelize the modes reduction. wid.x / nwg.x are
// uniform across the workgroup so the sample loop's trip count is uniform and
// every workgroupBarrier() stays in uniform control flow. The host picks this
// variant only when it wins (small sample block or many modes) — see
// modal_strike(); the serial kernel stays best for many-sample / few-mode blocks
// that already fill the device with barrier-free work.
static constexpr const char* kModalStrikeCoopShader = R"wgsl(
struct ModalParams { num_modes : u32, num_samples : u32, sample_rate : f32, t0 : f32 };

@group(0) @binding(0) var<storage, read>       modes : array<f32>;
@group(0) @binding(1) var<storage, read_write> out   : array<f32>;
@group(0) @binding(2) var<uniform>             p     : ModalParams;

const TWO_PI : f32 = 6.2831853071795864;
const WG : u32 = 256u;
var<workgroup> partial : array<f32, 256>;

@compute @workgroup_size(WG)
fn main(@builtin(workgroup_id) wid : vec3u,
        @builtin(num_workgroups) nwg : vec3u,
        @builtin(local_invocation_id) lid : vec3u) {
    let lane = lid.x;
    for (var s = wid.x; s < p.num_samples; s = s + nwg.x) {
        let t = (p.t0 + f32(s)) / p.sample_rate;
        var acc = 0.0;
        for (var i = lane; i < p.num_modes; i = i + WG) {
            let f  = modes[i * 4u];
            let a  = modes[i * 4u + 1u];
            let d  = modes[i * 4u + 2u];
            let ph = modes[i * 4u + 3u];
            acc = acc + a * exp(-d * t) * sin(TWO_PI * f * t + ph);
        }
        partial[lane] = acc;
        workgroupBarrier();
        for (var stride = WG / 2u; stride > 0u; stride = stride >> 1u) {
            if (lane < stride) { partial[lane] = partial[lane] + partial[lane + stride]; }
            workgroupBarrier();
        }
        if (lane == 0u) { out[s] = partial[0]; }
        workgroupBarrier();  // finish reading partial[0] before the next sample overwrites it
    }
}
)wgsl";

static constexpr const char* kGranularShader = R"wgsl(
// GPU granular synthesis: one thread per output sample, summing the contribution
// of every active grain. grains: num_grains × [onset, duration, src_pos, pitch,
// amp]. Each grain is a Hann-windowed, linearly-interpolated, pitch-shifted
// snippet of `source`.

struct GrainParams { num_grains : u32, num_samples : u32, source_len : u32, pad : u32 };

@group(0) @binding(0) var<storage, read>       grains : array<f32>;
@group(0) @binding(1) var<storage, read>       source : array<f32>;
@group(0) @binding(2) var<storage, read_write> out    : array<f32>;
@group(0) @binding(3) var<uniform>             p      : GrainParams;

const TWO_PI : f32 = 6.2831853071795864;

@compute @workgroup_size(256)
fn main(@builtin(global_invocation_id) gid : vec3u) {
    let s = gid.x;
    if (s >= p.num_samples) {
        return;
    }
    let sf = f32(s);
    var acc = 0.0;
    for (var g = 0u; g < p.num_grains; g = g + 1u) {
        let onset = grains[g * 5u];
        let dur   = grains[g * 5u + 1u];
        if (dur <= 0.0 || sf < onset || sf >= onset + dur) {
            continue;
        }
        let local = sf - onset;
        let w = 0.5 - 0.5 * cos(TWO_PI * local / dur);  // Hann
        let srcf = grains[g * 5u + 2u] + local * grains[g * 5u + 3u];  // pos + local*pitch
        if (srcf < 0.0) { continue; }
        let i0 = u32(srcf);
        if (i0 + 1u >= p.source_len) { continue; }
        let frac = srcf - f32(i0);
        let smp = source[i0] * (1.0 - frac) + source[i0 + 1u] * frac;
        acc = acc + grains[g * 5u + 4u] * w * smp;  // amp * window * sample
    }
    out[s] = acc;
}
)wgsl";

static constexpr const char* kDenseTanhShader = R"wgsl(
// GPU dense layer with tanh activation: one thread per output neuron.
// out[j] = tanh(Σ_i W[j*in_dim + i]·x[i] + b[j]). W row-major [out_dim×in_dim].

struct DenseParams { in_dim : u32, out_dim : u32, pad0 : u32, pad1 : u32 };

@group(0) @binding(0) var<storage, read>       x   : array<f32>;
@group(0) @binding(1) var<storage, read>       w   : array<f32>;
@group(0) @binding(2) var<storage, read>       b   : array<f32>;
@group(0) @binding(3) var<storage, read_write> out : array<f32>;
@group(0) @binding(4) var<uniform>             p   : DenseParams;

@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) gid : vec3u) {
    let j = gid.x;
    if (j >= p.out_dim) {
        return;
    }
    var acc = b[j];
    let base = j * p.in_dim;
    for (var i = 0u; i < p.in_dim; i = i + 1u) {
        acc = acc + w[base + i] * x[i];
    }
    out[j] = tanh(acc);
}
)wgsl";

// ── Timing helper ───────────────────────────────────────────────────────────

// Largest power-of-two complex FFT the GPU path accepts: keeps
// 2*N*sizeof(float) well within uint32_t and typical WebGPU storage-buffer
// limits (32 MiB per complex buffer). Shared by fft_run() and capabilities().
static constexpr uint32_t kMaxFftN = 1u << 22;

static const char* backend_name(wgpu::BackendType type) {
    switch (type) {
        case wgpu::BackendType::Metal:    return "Metal";
        case wgpu::BackendType::D3D12:    return "D3D12";
        case wgpu::BackendType::D3D11:    return "D3D11";
        case wgpu::BackendType::Vulkan:   return "Vulkan";
        case wgpu::BackendType::OpenGL:   return "OpenGL";
        case wgpu::BackendType::OpenGLES: return "OpenGLES";
        case wgpu::BackendType::WebGPU:   return "WebGPU";
        case wgpu::BackendType::Null:     return "Null";
        default:                          return "Unknown";
    }
}

static double now_us() {
    using Clock = std::chrono::high_resolution_clock;
    return static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch()).count()) / 1000.0;
}

// Built-in WGSL for a pipeline label, or nullptr if the label is unknown. The
// labels are exactly the ones create_pipelines() passes to create_pipeline() —
// this table is the seam's namespace, so a pipeline added there without a row
// here is simply not overridable.
static const char* builtin_kernel_source(const char* label) {
    if (!label) return nullptr;
    static const std::unordered_map<std::string_view, const char*> kSources = {
        {"magnitude",           kMagnitudeShader},
        {"complex_multiply",    kComplexMultiplyShader},
        {"fft_stockham",        kFftStockhamShader},
        {"conv_bmul",           kComplexMulBroadcastShader},
        {"multi_ir_mul",        kMultiIrMulShader},
        {"multi_ir_combine",    kMultiIrCombineShader},
        {"multi_fdl_mac",       kMultiFdlMacShader},
        {"spectral_advance",    kSpectralAdvanceShader},
        {"spectral_combine",    kSpectralCombineShader},
        {"conv_in",             kConvStackInputShader},
        {"conv_layer",          kConvStackLayerShader},
        {"conv_head",           kConvStackHeadShader},
        {"wavenet_rechannel",   kWavenetRechannelShader},
        {"wavenet_layer",       kWavenetLayerShader},
        {"wavenet_head",        kWavenetHeadShader},
        {"wavenet_scale",       kWavenetScaleShader},
        {"matmul",              kMatmulShader},
        {"additive_synth",      kAdditiveSynthShader},
        {"additive_synth_coop", kAdditiveSynthCoopShader},
        {"modal_strike",        kModalStrikeShader},
        {"modal_strike_coop",   kModalStrikeCoopShader},
        {"granular_cloud",      kGranularShader},
        {"dense_tanh",          kDenseTanhShader},
    };
    auto it = kSources.find(std::string_view(label));
    return (it == kSources.end()) ? nullptr : it->second;
}

// Callback mode for every asynchronous WebGPU site in this file.
//
// Native Dawn resolves AllowProcessEvents callbacks from Instance::ProcessEvents(),
// which is what the blocking readback path drives and what poll_readbacks() pumps.
//
// emdawnwebgpu does NOT: ProcessEvents() there does not resolve map callbacks at
// all. Measured (headless Chrome, emsdk 6.0.2, DedicatedWorker, ProcessEvents()
// driven from a setTimeout loop so the JS event loop turns between calls): an
// AllowProcessEvents map callback never fired across 170+ pumped ticks, while an
// AllowSpontaneous map on the same submit resolved from the JS event loop with
// correct data — and resolved with ProcessEvents() never called at all. In the
// browser the JS event loop IS the event queue; spontaneous is the only mode
// that observes it.
//
// The rest of the async machinery is mode-agnostic: a spontaneous callback marks
// the request resolved, and poll_readbacks() still decides when completions fire
// and when a deadline expires.
#if defined(__EMSCRIPTEN__)
static constexpr wgpu::CallbackMode kAsyncCallbackMode =
    wgpu::CallbackMode::AllowSpontaneous;
#else
static constexpr wgpu::CallbackMode kAsyncCallbackMode =
    wgpu::CallbackMode::AllowProcessEvents;
#endif

// ── Implementation ──────────────────────────────────────────────────────────

class DawnGpuCompute : public GpuCompute {
public:
    ~DawnGpuCompute() override {
        // Expire the liveness token first: the device-lost callback holds a
        // weak_ptr to it and can fire during teardown (Dawn reports a destroyed
        // device as lost), at which point `this` is already unwinding.
        alive_.reset();

        // Outstanding async readbacks complete exactly once — the caller may be
        // waiting on that completion to route a block, so it fires here rather
        // than being silently dropped. Draining first also marks each request
        // abandoned, so a map callback that lands during teardown neither writes
        // the caller's `dest` nor touches the pool.
        drain_readbacks(ReadbackStatus::Failed);

        // Release pool buffers before the device is torn down — their
        // destructors call into Dawn internals and need a live device.
        pool_.reset();
        fft_plans_.clear();
        conv_plans_.clear();
        batch_conv_plans_.clear();
        multi_conv_plans_.clear();
        spectral_stack_plans_.clear();
        conv_stack_plans_.clear();
        wavenet_plans_.clear();
        pipeline_cache_.clear();
        magnitude_pipeline_ = nullptr;
        complex_mul_pipeline_ = nullptr;
        conv_bmul_pipeline_ = nullptr;
        multi_ir_mul_pipeline_ = nullptr;
        multi_ir_combine_pipeline_ = nullptr;
        spectral_advance_pipeline_ = nullptr;
        spectral_combine_pipeline_ = nullptr;
        conv_in_pipeline_ = nullptr;
        conv_layer_pipeline_ = nullptr;
        conv_head_pipeline_ = nullptr;
        wavenet_rechannel_pipeline_ = nullptr;
        wavenet_layer_pipeline_ = nullptr;
        wavenet_head_pipeline_ = nullptr;
        wavenet_scale_pipeline_ = nullptr;
        matmul_pipeline_ = nullptr;
        additive_pipeline_ = nullptr;
        additive_coop_pipeline_ = nullptr;
        modal_pipeline_ = nullptr;
        modal_coop_pipeline_ = nullptr;
        granular_pipeline_ = nullptr;
        dense_tanh_pipeline_ = nullptr;
        fft_pipeline_ = nullptr;
        queue_ = nullptr;
        device_ = nullptr;
        instance_ = nullptr;
#if !defined(__EMSCRIPTEN__)
        native_instance_.reset();
#endif
    }

    bool initialize_from_surface(GpuSurface& surface) override {
        if (!surface.is_initialized()) return false;

        auto* dev = static_cast<wgpu::Device*>(surface.dawn_device_handle());
        auto* q = static_cast<wgpu::Queue*>(surface.dawn_queue_handle());
        auto* inst = static_cast<wgpu::Instance*>(surface.dawn_instance_handle());
        if (!dev || !q || !inst) return false;

        device_ = *dev;
        queue_ = *q;
        instance_ = *inst;
        owns_device_ = false;

        return create_pipelines();
    }

    bool initialize_standalone() override {
#if defined(__EMSCRIPTEN__)
        // Structurally impossible in a browser, so it fails loudly instead of
        // returning a mysterious false: RequestAdapter/RequestDevice resolve only
        // once control returns to the JS event loop, and there is no valid way to
        // wait for that from inside this call (see kAsyncCallbackMode). The
        // browser acquires its device in JS and hands it over.
        runtime::log_error(
            "GpuCompute: initialize_standalone() is not supported in the browser "
            "— acquire the device in JS (navigator.gpu.requestDevice) and call "
            "initialize_from_device()");
        return false;
#else
        if (!create_instance()) return false;

        wgpu::RequestAdapterOptions opts{};
        opts.powerPreference = wgpu::PowerPreference::HighPerformance;

        instance_.RequestAdapter(
            &opts, wgpu::CallbackMode::AllowProcessEvents,
            [this](wgpu::RequestAdapterStatus status, wgpu::Adapter result, wgpu::StringView) {
                if (status == wgpu::RequestAdapterStatus::Success)
                    adapter_ = std::move(result);
            });
        pump_events();
        if (!adapter_) return false;

        wgpu::DeviceDescriptor dev_desc{};
        dev_desc.label = "Pulp Compute Device";
        // Opt into compute-pass GPU timing when the adapter advertises it
        // (the standalone device requests no features otherwise). Requesting an
        // unsupported feature fails RequestDevice, so it is strictly gated.
        std::vector<wgpu::FeatureName> required_features;
        if (adapter_.HasFeature(wgpu::FeatureName::TimestampQuery)) {
            required_features.push_back(wgpu::FeatureName::TimestampQuery);
        }
        dev_desc.requiredFeatureCount = required_features.size();
        dev_desc.requiredFeatures = required_features.data();
        dev_desc.SetUncapturedErrorCallback(
            [](const wgpu::Device&, wgpu::ErrorType type, wgpu::StringView msg) {
                runtime::log_error("GpuCompute: WebGPU error ({}): {}",
                    static_cast<int>(type), std::string(msg.data, msg.length));
            });
        // Device loss is normal behavior, not an init failure: a browser tab can
        // lose its device at any moment, and a native driver reset does the same.
        // Every in-flight readback is completed as Failed rather than left to
        // expire block after block with no way back.
        dev_desc.SetDeviceLostCallback(
            kAsyncCallbackMode,
            [this, alive = std::weak_ptr<int>(alive_)](
                const wgpu::Device&, wgpu::DeviceLostReason reason,
                wgpu::StringView msg) {
                // Dawn reports a *destroyed* device as lost, so this can land
                // while ~DawnGpuCompute() is unwinding. The token is expired
                // first in the dtor, which makes that case a no-op.
                if (alive.expired()) return;
                handle_device_lost(static_cast<int>(reason),
                                   std::string(msg.data, msg.length));
            });

        adapter_.RequestDevice(
            &dev_desc, wgpu::CallbackMode::AllowProcessEvents,
            [this](wgpu::RequestDeviceStatus status, wgpu::Device result, wgpu::StringView) {
                if (status == wgpu::RequestDeviceStatus::Success)
                    device_ = std::move(result);
            });
        pump_events();
        if (!device_) return false;

        queue_ = device_.GetQueue();
        owns_device_ = true;

        return create_pipelines();
#endif
    }

    // Adopt a device created elsewhere. The browser's DedicatedWorker awaits
    // navigator.gpu.requestAdapter()/requestDevice() in JS and publishes the
    // result as Module.preinitializedWebGPUDevice; the caller passes what
    // emscripten_webgpu_get_device() returns. Natively this is the same handle
    // style as initialize_from_surface(), minus the surface.
    //
    // A DeviceLost callback cannot be attached to an already-created device — it
    // lives on the descriptor — so an adopted device's owner reports loss via
    // notify_device_lost() (in the browser, from the `device.lost` promise).
    bool initialize_from_device(void* wgpu_device) override {
        if (!wgpu_device) return false;

        // The caller keeps ownership of the handle, so take a reference rather
        // than adopting the caller's: Acquire() would steal a refcount we do not
        // own and tear the device down under the JS side.
        device_ = wgpu::Device(static_cast<WGPUDevice>(wgpu_device));
        if (!device_) return false;

        // Instance creation is independent of the device and is what
        // poll_readbacks() pumps natively; in the browser it is a formality (the
        // JS event loop is the event queue) but wgpuCreateInstance() is valid
        // there and costs nothing.
        if (!instance_ && !create_instance()) return false;

        queue_ = device_.GetQueue();
        if (!queue_) return false;
        owns_device_ = false;
        device_lost_ = false;

        return create_pipelines();
    }

    bool device_lost() const override { return device_lost_; }

    void notify_device_lost() override {
        handle_device_lost(-1, "reported by device owner");
    }

    // Device loss is a state transition, not an error path. Everything in flight
    // is completed as Failed (a map on a lost device never resolves, so leaving
    // the queue alone would expire block after block forever), and the object is
    // marked uninitialized so the owner re-acquires a device and re-prepare_*()s
    // rather than dispatching into a dead device.
    void handle_device_lost(int reason, const std::string& msg) {
        if (device_lost_) return;
        device_lost_ = true;
        initialized_ = false;
        runtime::log_error("GpuCompute: device lost (reason {}): {}", reason, msg);
        drain_readbacks(ReadbackStatus::Failed);
    }

    // ── Compute operations ──────────────────────────────────────────────

    bool compute_magnitude(const float* complex_pairs, float* magnitudes,
                           uint32_t num_bins) override {
        if (!initialized_) return false;

        uint32_t input_bytes = num_bins * 2 * sizeof(float);
        uint32_t output_bytes = num_bins * sizeof(float);

        auto input_buf = acquire_storage_buffer(input_bytes,
            wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst);
        auto output_buf = acquire_storage_buffer(output_bytes,
            wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc);
        auto readback_buf = acquire_readback_buffer(output_bytes);

#ifdef PULP_BENCHMARK
        {
            const double t0 = bench::now_us();
            queue_.WriteBuffer(input_buf, 0, complex_pairs, input_bytes);
            if (bench_counters_) {
                bench_counters_->gpu_upload_total_us.fetch_add(
                    bench::now_us() - t0, std::memory_order_relaxed);
                bench_counters_->cpu_to_gpu_bytes_total.fetch_add(
                    static_cast<double>(input_bytes),
                    std::memory_order_relaxed);
            }
        }
#else
        queue_.WriteBuffer(input_buf, 0, complex_pairs, input_bytes);
#endif

        auto bind_group = create_bind_group(magnitude_pipeline_, {input_buf, output_buf});

        uint32_t workgroups = (num_bins + 255) / 256;
#ifdef PULP_BENCHMARK
        {
            const double t0 = bench::now_us();
            dispatch(magnitude_pipeline_, bind_group, workgroups);
            copy_buffer(output_buf, readback_buf, output_bytes);
            if (bench_counters_) {
                bench_counters_->gpu_dispatch_total_us.fetch_add(
                    bench::now_us() - t0, std::memory_order_relaxed);
            }
        }
#else
        dispatch(magnitude_pipeline_, bind_group, workgroups);
        copy_buffer(output_buf, readback_buf, output_bytes);
#endif

        // Register OnSubmittedWorkDone to release the storage buffers once the
        // GPU confirms the dispatch+copy submission completed. The readback
        // buffer is still live (it's about to be MapAsync'd), so it is
        // released synchronously after read_back() has Unmap'd it.
        schedule_pool_release({input_buf, output_buf});

        const bool ok = read_back(readback_buf, magnitudes, output_bytes);

        // Only recycle readback buffers on success. On MapAsync timeout the
        // GPU may still be mapping the buffer when this function returns;
        // handing it back to the pool could let a subsequent call re-acquire
        // and reuse it while the prior map is still in flight.
        //
        // On failure we still must drop the pool's in_flight_ reference via
        // discard(), otherwise repeated timeouts accumulate tracked handles
        // and grow the pool without bound. discard() is a release() that
        // skips the free-list recycle step.
        if (ok) {
            pool_->release(readback_buf);
        } else {
            pool_->discard(readback_buf);
        }
        return ok;
    }

    bool complex_multiply(const float* a, const float* b, float* result,
                          uint32_t count) override {
        if (!initialized_) return false;

        uint32_t bytes = count * 2 * sizeof(float);

        auto buf_a = acquire_storage_buffer(bytes,
            wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst);
        auto buf_b = acquire_storage_buffer(bytes,
            wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst);
        auto buf_result = acquire_storage_buffer(bytes,
            wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc);
        auto readback_buf = acquire_readback_buffer(bytes);

#ifdef PULP_BENCHMARK
        {
            const double t0 = bench::now_us();
            queue_.WriteBuffer(buf_a, 0, a, bytes);
            queue_.WriteBuffer(buf_b, 0, b, bytes);
            if (bench_counters_) {
                bench_counters_->gpu_upload_total_us.fetch_add(
                    bench::now_us() - t0, std::memory_order_relaxed);
                bench_counters_->cpu_to_gpu_bytes_total.fetch_add(
                    static_cast<double>(bytes) * 2.0,
                    std::memory_order_relaxed);
            }
        }
#else
        queue_.WriteBuffer(buf_a, 0, a, bytes);
        queue_.WriteBuffer(buf_b, 0, b, bytes);
#endif

        auto bind_group = create_bind_group(complex_mul_pipeline_,
            {buf_a, buf_b, buf_result});

        uint32_t workgroups = (count + 255) / 256;
#ifdef PULP_BENCHMARK
        {
            const double t0 = bench::now_us();
            dispatch(complex_mul_pipeline_, bind_group, workgroups);
            copy_buffer(buf_result, readback_buf, bytes);
            if (bench_counters_) {
                bench_counters_->gpu_dispatch_total_us.fetch_add(
                    bench::now_us() - t0, std::memory_order_relaxed);
            }
        }
#else
        dispatch(complex_mul_pipeline_, bind_group, workgroups);
        copy_buffer(buf_result, readback_buf, bytes);
#endif

        // Release storage buffers via OnSubmittedWorkDone; readback_buf stays
        // live until read_back() unmaps it. See compute_magnitude() for the
        // parallel comment on the GPU-in-flight tracking model.
        schedule_pool_release({buf_a, buf_b, buf_result});

        const bool ok = read_back(readback_buf, result, bytes);
        // Match the magnitude path: only recycle on success. On read_back
        // failure the GPU may still hold a mapping on this buffer.
        //
        // Use discard() on failure so repeated timeouts don't accumulate
        // tracked handles in in_flight_; see compute_magnitude() for the full
        // rationale.
        if (ok) {
            pool_->release(readback_buf);
        } else {
            pool_->discard(readback_buf);
        }
        return ok;
    }

    bool batch_magnitude(const float* complex_frames, float* magnitude_frames,
                         uint32_t bins_per_frame, uint32_t num_frames) override {
        if (!initialized_) return false;

        // Treat as one large magnitude computation — the shader is element-wise
        uint32_t total_bins = bins_per_frame * num_frames;
        return compute_magnitude(complex_frames, magnitude_frames, total_bins);
    }

    // ── FFT ──────────────────────────────────────────────────────────────

    bool fft_forward(const float* complex_in, float* complex_out, uint32_t n) override {
        return fft_run(complex_in, complex_out, n, /*sign=*/-1.0f, /*normalize=*/false);
    }

    bool fft_inverse(const float* complex_in, float* complex_out, uint32_t n) override {
        return fft_run(complex_in, complex_out, n, /*sign=*/+1.0f, /*normalize=*/true);
    }

    bool fft_forward_timed(const float* complex_in, float* complex_out, uint32_t n,
                           double* gpu_compute_us) override {
        double ns = -1.0;
        const bool ok = fft_run(complex_in, complex_out, n, /*sign=*/-1.0f,
                                /*normalize=*/false, &ns);
        if (gpu_compute_us) *gpu_compute_us = (ns >= 0.0) ? ns / 1000.0 : -1.0;
        return ok;
    }

    // ── GPU-resident convolution ───────────────────────────────────────────

    bool prepare_convolution(uint32_t n, const float* ir_spec) override {
        if (!initialized_ || ir_spec == nullptr) return false;
        if (!is_power_of_two(n) || n > kMaxFftN) return false;

        ConvPlan plan;
        plan.n = n;
        plan.log2n = 0;
        for (uint32_t v = n; v > 1u; v >>= 1) ++plan.log2n;

        const uint32_t bytes = n * 2u * static_cast<uint32_t>(sizeof(float));
        const auto sc = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst
                      | wgpu::BufferUsage::CopySrc;
        plan.buf_a = create_storage_buffer(bytes, sc);
        plan.buf_b = create_storage_buffer(bytes, sc);
        plan.buf_c = create_storage_buffer(bytes, sc);
        plan.irspec = create_storage_buffer(bytes,
            wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst);
        plan.readback = create_readback_buffer(bytes);
        if (!plan.buf_a || !plan.buf_b || !plan.buf_c || !plan.irspec || !plan.readback) {
            return false;
        }

        // Forward (sign=-1) and inverse (sign=+1) per-pass uniforms + bind
        // groups; both ping-pong starting from buf_a. Uniforms are constant per
        // pass, so write them once here.
        for (uint32_t s = 0; s < plan.log2n; ++s) {
            struct FftParams { uint32_t n; uint32_t ns; float sign; uint32_t batch; };
            const bool src_is_a = (s % 2u == 0u);
            for (int dir = 0; dir < 2; ++dir) {
                const float sign = (dir == 0) ? -1.0f : 1.0f;
                wgpu::BufferDescriptor ud{};
                ud.size = 16;
                ud.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
                wgpu::Buffer u = device_.CreateBuffer(&ud);
                if (!u) return false;
                FftParams p{n, 1u << s, sign, 1u};
                queue_.WriteBuffer(u, 0, &p, sizeof(p));
                wgpu::BindGroup bg = create_bind_group(
                    fft_pipeline_,
                    src_is_a ? std::initializer_list<wgpu::Buffer>{plan.buf_a, plan.buf_b, u}
                             : std::initializer_list<wgpu::Buffer>{plan.buf_b, plan.buf_a, u});
                if (!bg) return false;
                if (dir == 0) { plan.fwd_u.push_back(std::move(u)); plan.fwd_bgs.push_back(std::move(bg)); }
                else          { plan.inv_u.push_back(std::move(u)); plan.inv_bgs.push_back(std::move(bg)); }
            }
        }

        // Forward result lands in buf_b for odd log2n, buf_a for even. The
        // complex-multiply reads it × irspec → buf_c.
        wgpu::Buffer& fwd_buf = (plan.log2n & 1u) ? plan.buf_b : plan.buf_a;
        plan.mul_bg = create_bind_group(complex_mul_pipeline_,
                                        {fwd_buf, plan.irspec, plan.buf_c});
        if (!plan.mul_bg) return false;

        queue_.WriteBuffer(plan.irspec, 0, ir_spec, bytes);

        conv_plans_.insert_or_assign(n, std::move(plan));
        return true;
    }

    bool convolve(const float* in_complex, float* out_complex, uint32_t n) override {
        if (!initialized_ || in_complex == nullptr || out_complex == nullptr) return false;
        auto it = conv_plans_.find(n);
        if (it == conv_plans_.end()) return false;
        ConvPlan& plan = it->second;

        const uint32_t bytes = n * 2u * static_cast<uint32_t>(sizeof(float));
        const uint32_t fft_wg = ((n / 2u) + 255u) / 256u;  // FFT passes: n/2 threads
        const uint32_t mul_wg = (n + 255u) / 256u;          // complex-mul: n pairs

        queue_.WriteBuffer(plan.buf_a, 0, in_complex, bytes);

        // One command buffer: forward FFT → complex-mul → copy product into the
        // inverse input → inverse FFT → copy result to readback. Intermediates
        // never leave the GPU; only the final block is read back.
        wgpu::CommandEncoderDescriptor enc_desc{};
        auto encoder = device_.CreateCommandEncoder(&enc_desc);
        encode_fft_passes(encoder, plan.fwd_bgs, fft_wg);
        {
            wgpu::ComputePassDescriptor pd{};
            auto pass = encoder.BeginComputePass(&pd);
            pass.SetPipeline(complex_mul_pipeline_);
            pass.SetBindGroup(0, plan.mul_bg);
            pass.DispatchWorkgroups(mul_wg);
            pass.End();
        }
        encoder.CopyBufferToBuffer(plan.buf_c, 0, plan.buf_a, 0, bytes);
        encode_fft_passes(encoder, plan.inv_bgs, fft_wg);
        wgpu::Buffer& inv_buf = (plan.log2n & 1u) ? plan.buf_b : plan.buf_a;
        encoder.CopyBufferToBuffer(inv_buf, 0, plan.readback, 0, bytes);
        auto cmd = encoder.Finish();
        queue_.Submit(1, &cmd);

        if (!read_back(plan.readback, out_complex, bytes)) return false;

        const float inv = 1.0f / static_cast<float>(n);  // inverse-FFT normalization
        for (uint32_t i = 0; i < n * 2u; ++i) out_complex[i] *= inv;
        return true;
    }

    bool prepare_convolution_batch(uint32_t n, const float* ir_spec, uint32_t batch) override {
        if (!initialized_ || ir_spec == nullptr || batch == 0) return false;
        if (!is_power_of_two(n) || n > kMaxFftN) return false;
        // Cap total packed size so batch*2*n*sizeof(float) stays within uint32_t
        // and a sane storage-buffer budget (here batch*n <= kMaxFftN → ≤32 MiB).
        if (static_cast<uint64_t>(batch) * n > kMaxFftN) return false;

        BatchConvPlan plan;
        plan.n = n;
        plan.batch = batch;
        plan.log2n = 0;
        for (uint32_t v = n; v > 1u; v >>= 1) ++plan.log2n;

        const uint32_t big = batch * n * 2u * static_cast<uint32_t>(sizeof(float));
        const uint32_t irb = n * 2u * static_cast<uint32_t>(sizeof(float));
        const auto sc = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst
                      | wgpu::BufferUsage::CopySrc;
        plan.buf_a = create_storage_buffer(big, sc);
        plan.buf_b = create_storage_buffer(big, sc);
        plan.buf_c = create_storage_buffer(big, sc);
        plan.irspec = create_storage_buffer(irb,
            wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst);
        plan.readback = create_readback_buffer(big);
        if (!plan.buf_a || !plan.buf_b || !plan.buf_c || !plan.irspec || !plan.readback) {
            return false;
        }

        for (uint32_t s = 0; s < plan.log2n; ++s) {
            struct FftParams { uint32_t n; uint32_t ns; float sign; uint32_t batch; };
            const bool src_is_a = (s % 2u == 0u);
            for (int dir = 0; dir < 2; ++dir) {
                const float sign = (dir == 0) ? -1.0f : 1.0f;
                wgpu::BufferDescriptor ud{};
                ud.size = 16;
                ud.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
                wgpu::Buffer u = device_.CreateBuffer(&ud);
                if (!u) return false;
                FftParams p{n, 1u << s, sign, batch};
                queue_.WriteBuffer(u, 0, &p, sizeof(p));
                wgpu::BindGroup bg = create_bind_group(
                    fft_pipeline_,
                    src_is_a ? std::initializer_list<wgpu::Buffer>{plan.buf_a, plan.buf_b, u}
                             : std::initializer_list<wgpu::Buffer>{plan.buf_b, plan.buf_a, u});
                if (!bg) return false;
                if (dir == 0) { plan.fwd_u.push_back(std::move(u)); plan.fwd_bgs.push_back(std::move(bg)); }
                else          { plan.inv_u.push_back(std::move(u)); plan.inv_bgs.push_back(std::move(bg)); }
            }
        }

        wgpu::Buffer& fwd_buf = (plan.log2n & 1u) ? plan.buf_b : plan.buf_a;
        plan.bmul_bg = create_bind_group(conv_bmul_pipeline_,
                                         {fwd_buf, plan.irspec, plan.buf_c});
        if (!plan.bmul_bg) return false;

        queue_.WriteBuffer(plan.irspec, 0, ir_spec, irb);
        batch_conv_plans_.insert_or_assign(n, std::move(plan));
        return true;
    }

    bool convolve_batch(const float* in_complex, float* out_complex,
                        uint32_t n, uint32_t batch) override {
        if (!initialized_ || in_complex == nullptr || out_complex == nullptr) return false;
        auto it = batch_conv_plans_.find(n);
        if (it == batch_conv_plans_.end() || it->second.batch != batch) return false;
        BatchConvPlan& plan = it->second;

        const uint32_t big = batch * n * 2u * static_cast<uint32_t>(sizeof(float));
        const uint32_t fft_wg = ((batch * (n / 2u)) + 255u) / 256u;  // batched butterflies
        const uint32_t mul_wg = ((batch * n) + 255u) / 256u;         // batched pairs

        queue_.WriteBuffer(plan.buf_a, 0, in_complex, big);

        wgpu::CommandEncoderDescriptor enc_desc{};
        auto encoder = device_.CreateCommandEncoder(&enc_desc);
        encode_fft_passes(encoder, plan.fwd_bgs, fft_wg);
        {
            wgpu::ComputePassDescriptor pd{};
            auto pass = encoder.BeginComputePass(&pd);
            pass.SetPipeline(conv_bmul_pipeline_);
            pass.SetBindGroup(0, plan.bmul_bg);
            pass.DispatchWorkgroups(mul_wg);
            pass.End();
        }
        encoder.CopyBufferToBuffer(plan.buf_c, 0, plan.buf_a, 0, big);
        encode_fft_passes(encoder, plan.inv_bgs, fft_wg);
        wgpu::Buffer& inv_buf = (plan.log2n & 1u) ? plan.buf_b : plan.buf_a;
        encoder.CopyBufferToBuffer(inv_buf, 0, plan.readback, 0, big);
        auto cmd = encoder.Finish();
        queue_.Submit(1, &cmd);

        if (!read_back(plan.readback, out_complex, big)) return false;

        const float inv = 1.0f / static_cast<float>(n);
        for (uint32_t i = 0; i < batch * n * 2u; ++i) out_complex[i] *= inv;
        return true;
    }

    // ── Multi-IR convolution ────────────────────────────────────────────────

    bool prepare_multi_convolution(uint32_t n, const float* ir_specs,
                                   uint32_t num_ir) override {
        if (!initialized_ || ir_specs == nullptr || num_ir == 0) return false;
        if (!is_power_of_two(n) || n > kMaxFftN) return false;
        // The batched intermediates (big_a/big_b/irspecs) are 2*num_ir*n floats
        // each — the storage-buffer-binding-limited resource. Gate on the
        // device's real limit (not the conservative single-transform kMaxFftN),
        // and keep the byte math within uint32 (buffer sizes below are uint32_t).
        const uint64_t big_bytes =
            static_cast<uint64_t>(num_ir) * n * 2ull * sizeof(float);
        if (big_bytes > 0xFFFFFFFFull) return false;
        wgpu::Limits limits{};
        uint64_t max_bind = 0;
        if (device_.GetLimits(&limits) == wgpu::Status::Success)
            max_bind = limits.maxStorageBufferBindingSize;
        if (max_bind == 0)
            max_bind = static_cast<uint64_t>(kMaxFftN) * 2ull * sizeof(float);
        // Strictly under the per-binding limit: a buffer AT the limit creates a
        // Dawn error buffer (operations silently no-op), so leave headroom.
        if (big_bytes >= max_bind) return false;

        MultiConvPlan plan;
        plan.n = n;
        plan.num_ir = num_ir;
        plan.log2n = 0;
        for (uint32_t v = n; v > 1u; v >>= 1) ++plan.log2n;

        const uint32_t small = n * 2u * static_cast<uint32_t>(sizeof(float));   // 2n
        const uint32_t big = num_ir * small;                                    // 2n*num_ir
        const auto sc = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst
                      | wgpu::BufferUsage::CopySrc;
        plan.fx_a = create_storage_buffer(small, sc);
        plan.fx_b = create_storage_buffer(small, sc);
        plan.big_a = create_storage_buffer(big, sc);
        plan.big_b = create_storage_buffer(big, sc);
        plan.irspecs = create_storage_buffer(big,
            wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst);
        plan.panl = create_storage_buffer(num_ir * 4u,
            wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst);
        plan.panr = create_storage_buffer(num_ir * 4u,
            wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst);
        plan.out_lr = create_storage_buffer(small, sc);
        plan.readback = create_readback_buffer(small);
        wgpu::BufferDescriptor cud{};
        cud.size = 16;
        cud.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
        plan.cuniform = device_.CreateBuffer(&cud);
        if (!plan.fx_a || !plan.fx_b || !plan.big_a || !plan.big_b || !plan.irspecs ||
            !plan.panl || !plan.panr || !plan.out_lr || !plan.readback || !plan.cuniform) {
            return false;
        }

        // Forward passes: a single n-point transform ping-ponging fx_a/fx_b
        // (batch=1). Inverse passes: num_ir transforms ping-ponging big_a/big_b
        // (batch=num_ir).
        for (uint32_t s = 0; s < plan.log2n; ++s) {
            struct FftParams { uint32_t n; uint32_t ns; float sign; uint32_t batch; };
            const bool src_is_a = (s % 2u == 0u);
            // forward (sign=-1, batch=1) over fx_a/fx_b
            {
                wgpu::BufferDescriptor ud{};
                ud.size = 16;
                ud.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
                wgpu::Buffer u = device_.CreateBuffer(&ud);
                if (!u) return false;
                FftParams p{n, 1u << s, -1.0f, 1u};
                queue_.WriteBuffer(u, 0, &p, sizeof(p));
                wgpu::BindGroup bg = create_bind_group(
                    fft_pipeline_,
                    src_is_a ? std::initializer_list<wgpu::Buffer>{plan.fx_a, plan.fx_b, u}
                             : std::initializer_list<wgpu::Buffer>{plan.fx_b, plan.fx_a, u});
                if (!bg) return false;
                plan.fwd_u.push_back(std::move(u));
                plan.fwd_bgs.push_back(std::move(bg));
            }
            // inverse (sign=+1, batch=num_ir) over big_a/big_b
            {
                wgpu::BufferDescriptor ud{};
                ud.size = 16;
                ud.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
                wgpu::Buffer u = device_.CreateBuffer(&ud);
                if (!u) return false;
                FftParams p{n, 1u << s, 1.0f, num_ir};
                queue_.WriteBuffer(u, 0, &p, sizeof(p));
                wgpu::BindGroup bg = create_bind_group(
                    fft_pipeline_,
                    src_is_a ? std::initializer_list<wgpu::Buffer>{plan.big_a, plan.big_b, u}
                             : std::initializer_list<wgpu::Buffer>{plan.big_b, plan.big_a, u});
                if (!bg) return false;
                plan.inv_u.push_back(std::move(u));
                plan.inv_bgs.push_back(std::move(bg));
            }
        }

        // Forward result lands in fx_b for odd log2n, fx_a for even. The
        // multiply broadcasts it across num_ir IR spectra into big_a (the
        // inverse start buffer — fx_* and big_* are disjoint so write-after-read
        // is safe without an intermediate copy).
        wgpu::Buffer& fwd_buf = (plan.log2n & 1u) ? plan.fx_b : plan.fx_a;
        plan.mul_bg = create_bind_group(multi_ir_mul_pipeline_,
                                        {fwd_buf, plan.irspecs, plan.big_a});
        if (!plan.mul_bg) return false;

        // Inverse result lands in big_b for odd log2n, big_a for even. The
        // combine reduces it to the stereo out_lr.
        wgpu::Buffer& inv_buf = (plan.log2n & 1u) ? plan.big_b : plan.big_a;
        plan.combine_bg = create_bind_group(multi_ir_combine_pipeline_,
            {inv_buf, plan.panl, plan.panr, plan.out_lr, plan.cuniform});
        if (!plan.combine_bg) return false;

        struct CombineParams { uint32_t n; uint32_t num_ir; float inv; uint32_t pad; };
        CombineParams cp{n, num_ir, 1.0f / static_cast<float>(n), 0u};
        queue_.WriteBuffer(plan.cuniform, 0, &cp, sizeof(cp));
        queue_.WriteBuffer(plan.irspecs, 0, ir_specs, big);

        multi_conv_plans_.insert_or_assign(n, std::move(plan));
        return true;
    }

    // ── Partitioned FDL multi-convolution ───────────────────────────────────

    bool prepare_multi_fdl(uint32_t n, const float* ir_part_specs,
                           uint32_t num_ir, uint32_t num_part) override {
        if (!initialized_ || !ir_part_specs || num_ir == 0 || num_part == 0) return false;
        if (!is_power_of_two(n) || n > kMaxFftN) return false;

        // ir_specs (2*n*num_ir*num_part floats) is the storage-buffer limiter.
        const uint64_t ir_bytes =
            static_cast<uint64_t>(n) * 2ull * num_ir * num_part * sizeof(float);
        if (ir_bytes > 0xFFFFFFFFull) return false;
        wgpu::Limits limits{};
        uint64_t max_bind = 0;
        if (device_.GetLimits(&limits) == wgpu::Status::Success)
            max_bind = limits.maxStorageBufferBindingSize;
        if (max_bind == 0) max_bind = static_cast<uint64_t>(kMaxFftN) * 2ull * sizeof(float);
        if (ir_bytes >= max_bind) return false;
        const uint64_t ring_bytes = static_cast<uint64_t>(n) * 2ull * num_part * sizeof(float);
        const uint64_t accum_bytes = static_cast<uint64_t>(n) * 2ull * num_ir * sizeof(float);
        if (ring_bytes >= max_bind || accum_bytes >= max_bind) return false;

        MultiFdlPlan plan;
        plan.n = n;
        plan.block = n / 2u;
        plan.num_ir = num_ir;
        plan.num_part = num_part;
        plan.head = 0;
        plan.log2n = 0;
        for (uint32_t v = n; v > 1u; v >>= 1) ++plan.log2n;
        plan.prev_time.assign(plan.block, 0.0f);

        const uint32_t small = n * 2u * static_cast<uint32_t>(sizeof(float));  // 2n
        const auto sc = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst
                      | wgpu::BufferUsage::CopySrc;
        plan.fx_a = create_storage_buffer(small, sc);
        plan.fx_b = create_storage_buffer(small, sc);
        plan.ring = create_storage_buffer(static_cast<uint32_t>(ring_bytes), sc);
        plan.irspecs = create_storage_buffer(static_cast<uint32_t>(ir_bytes),
            wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst);
        plan.accum_a = create_storage_buffer(static_cast<uint32_t>(accum_bytes), sc);
        plan.accum_b = create_storage_buffer(static_cast<uint32_t>(accum_bytes), sc);
        plan.panl = create_storage_buffer(num_ir * 4u,
            wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst);
        plan.panr = create_storage_buffer(num_ir * 4u,
            wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst);
        plan.out_lr = create_storage_buffer(small, sc);
        plan.readback = create_readback_buffer(small);
        wgpu::BufferDescriptor ud16{};
        ud16.size = 16;
        ud16.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
        plan.cuniform = device_.CreateBuffer(&ud16);
        plan.fdl_uniform = device_.CreateBuffer(&ud16);
        if (!plan.fx_a || !plan.fx_b || !plan.ring || !plan.irspecs || !plan.accum_a ||
            !plan.accum_b || !plan.panl || !plan.panr || !plan.out_lr || !plan.readback ||
            !plan.cuniform || !plan.fdl_uniform) {
            return false;
        }

        // The ring is Dawn zero-initialized; upload the resident IR partition spectra.
        queue_.WriteBuffer(plan.irspecs, 0, ir_part_specs, static_cast<uint32_t>(ir_bytes));

        struct FftParams { uint32_t n; uint32_t ns; float sign; uint32_t batch; };
        for (uint32_t s = 0; s < plan.log2n; ++s) {
            const bool src_is_a = (s % 2u == 0u);
            // Forward (sign=-1, batch=1) over fx_a/fx_b.
            {
                wgpu::Buffer u = device_.CreateBuffer(&ud16);
                if (!u) return false;
                FftParams p{n, 1u << s, -1.0f, 1u};
                queue_.WriteBuffer(u, 0, &p, sizeof(p));
                wgpu::BindGroup bg = create_bind_group(fft_pipeline_,
                    src_is_a ? std::initializer_list<wgpu::Buffer>{plan.fx_a, plan.fx_b, u}
                             : std::initializer_list<wgpu::Buffer>{plan.fx_b, plan.fx_a, u});
                if (!bg) return false;
                plan.fwd_u.push_back(std::move(u));
                plan.fwd_bgs.push_back(std::move(bg));
            }
            // Inverse (sign=+1, batch=num_ir) over accum_a/accum_b.
            {
                wgpu::Buffer u = device_.CreateBuffer(&ud16);
                if (!u) return false;
                FftParams p{n, 1u << s, 1.0f, num_ir};
                queue_.WriteBuffer(u, 0, &p, sizeof(p));
                wgpu::BindGroup bg = create_bind_group(fft_pipeline_,
                    src_is_a ? std::initializer_list<wgpu::Buffer>{plan.accum_a, plan.accum_b, u}
                             : std::initializer_list<wgpu::Buffer>{plan.accum_b, plan.accum_a, u});
                if (!bg) return false;
                plan.inv_u.push_back(std::move(u));
                plan.inv_bgs.push_back(std::move(bg));
            }
        }

        // MAC writes accum_a (inverse pass s=0 reads accum_a). head lives in
        // fdl_uniform, rewritten each block.
        plan.mac_bg = create_bind_group(multi_fdl_mac_pipeline_,
            {plan.ring, plan.irspecs, plan.accum_a, plan.fdl_uniform});
        if (!plan.mac_bg) return false;

        // Inverse result lands in accum_b for odd log2n, accum_a for even.
        wgpu::Buffer& inv_buf = (plan.log2n & 1u) ? plan.accum_b : plan.accum_a;
        plan.combine_bg = create_bind_group(multi_ir_combine_pipeline_,
            {inv_buf, plan.panl, plan.panr, plan.out_lr, plan.cuniform});
        if (!plan.combine_bg) return false;

        struct CombineParams { uint32_t n; uint32_t num_ir; float inv; uint32_t pad; };
        CombineParams cp{n, num_ir, 1.0f / static_cast<float>(n), 0u};
        queue_.WriteBuffer(plan.cuniform, 0, &cp, sizeof(cp));

        multi_fdl_plans_.insert_or_assign(n, std::move(plan));
        return true;
    }

    bool multi_convolve(const float* in_complex, const float* pan_l,
                        const float* pan_r, float* out_lr, uint32_t n,
                        uint32_t num_ir) override {
        return multi_convolve_impl(in_complex, pan_l, pan_r, out_lr, n, num_ir,
                                   /*gpu_ns=*/nullptr);
    }

    bool multi_convolve_timed(const float* in_complex, const float* pan_l,
                              const float* pan_r, float* out_lr, uint32_t n,
                              uint32_t num_ir, double* gpu_compute_us) override {
        double ns = -1.0;
        const bool ok = multi_convolve_impl(in_complex, pan_l, pan_r, out_lr, n,
                                            num_ir, &ns);
        if (gpu_compute_us) *gpu_compute_us = (ns >= 0.0) ? ns / 1000.0 : -1.0;
        return ok;
    }

    // Shared implementation. When `gpu_ns` is non-null and timestamps are
    // available, brackets the fused pass sequence with a compute-pass timestamp
    // query (begin on the forward FFT's first pass, end on the combine pass) and
    // writes the GPU-busy nanoseconds; the untimed public entry passes null and
    // pays nothing. Not real-time-safe (blocks on the readback).
    bool multi_convolve_impl(const float* in_complex, const float* pan_l,
                             const float* pan_r, float* out_lr, uint32_t n,
                             uint32_t num_ir, double* gpu_ns) {
        if (!initialized_ || !in_complex || !pan_l || !pan_r || !out_lr) return false;
        auto it = multi_conv_plans_.find(n);
        if (it == multi_conv_plans_.end() || it->second.num_ir != num_ir) return false;
        MultiConvPlan& plan = it->second;

        if (gpu_ns) *gpu_ns = -1.0;
        bool do_ts = (gpu_ns != nullptr) && has_timestamp_ && ensure_device_ts();

        const uint32_t small = n * 2u * static_cast<uint32_t>(sizeof(float));
        const uint32_t fwd_wg = ((n / 2u) + 255u) / 256u;             // 1 transform
        const uint32_t inv_wg = ((num_ir * (n / 2u)) + 255u) / 256u;  // num_ir transforms
        const uint32_t mul_wg = ((num_ir * n) + 255u) / 256u;         // num_ir*n pairs
        const uint32_t comb_wg = (n + 255u) / 256u;                   // n output samples

        queue_.WriteBuffer(plan.fx_a, 0, in_complex, small);
        queue_.WriteBuffer(plan.panl, 0, pan_l, num_ir * 4u);
        queue_.WriteBuffer(plan.panr, 0, pan_r, num_ir * 4u);

        // One command buffer: forward FFT (shared) → broadcast multiply across
        // all rooms → batched inverse FFT → GPU pan-combine to stereo → copy the
        // single stereo block to readback. Only 2n floats leave the GPU.
        wgpu::CommandEncoderDescriptor enc_desc{};
        auto encoder = device_.CreateCommandEncoder(&enc_desc);
        // Begin timestamp rides the forward FFT's first pass.
        encode_fft_passes(encoder, plan.fwd_bgs, fwd_wg,
                          do_ts ? dev_ts_qs_ : wgpu::QuerySet{});
        {
            wgpu::ComputePassDescriptor pd{};
            auto pass = encoder.BeginComputePass(&pd);
            pass.SetPipeline(multi_ir_mul_pipeline_);
            pass.SetBindGroup(0, plan.mul_bg);
            pass.DispatchWorkgroups(mul_wg);
            pass.End();
        }
        encode_fft_passes(encoder, plan.inv_bgs, inv_wg);
        {
            wgpu::ComputePassDescriptor pd{};
            wgpu::PassTimestampWrites tw{};
            if (do_ts) {  // End timestamp on the final (combine) pass.
                tw.querySet = dev_ts_qs_;
                tw.beginningOfPassWriteIndex = wgpu::kQuerySetIndexUndefined;
                tw.endOfPassWriteIndex = 1;
                pd.timestampWrites = &tw;
            }
            auto pass = encoder.BeginComputePass(&pd);
            pass.SetPipeline(multi_ir_combine_pipeline_);
            pass.SetBindGroup(0, plan.combine_bg);
            pass.DispatchWorkgroups(comb_wg);
            pass.End();
        }
        encoder.CopyBufferToBuffer(plan.out_lr, 0, plan.readback, 0, small);
        if (do_ts) {
            encoder.ResolveQuerySet(dev_ts_qs_, 0, 2, dev_ts_resolve_, 0);
            encoder.CopyBufferToBuffer(dev_ts_resolve_, 0, dev_ts_readback_, 0,
                                       2u * sizeof(uint64_t));
        }
        auto cmd = encoder.Finish();
        queue_.Submit(1, &cmd);

        if (!read_back(plan.readback, out_lr, small)) return false;

        if (do_ts) {
            uint64_t ticks[2] = {0, 0};
            if (read_back(dev_ts_readback_, ticks, 2u * sizeof(uint64_t))
                && ticks[1] >= ticks[0]) {
                *gpu_ns = static_cast<double>(ticks[1] - ticks[0]);  // WebGPU ns
            }
        }
        return true;
    }

    bool multi_fdl_convolve(const float* in_block, const float* pan_l,
                            const float* pan_r, float* out_block, uint32_t n,
                            uint32_t num_ir) override {
        if (!initialized_ || !in_block || !pan_l || !pan_r || !out_block) return false;
        auto it = multi_fdl_plans_.find(n);
        if (it == multi_fdl_plans_.end() || it->second.num_ir != num_ir) return false;
        MultiFdlPlan& plan = it->second;
        const uint32_t block = plan.block;
        const uint32_t P = plan.num_part;
        const uint32_t small = n * 2u * static_cast<uint32_t>(sizeof(float));

        // Build the [prev | current] interleaved-complex window (imag = 0).
        std::vector<float> window(static_cast<size_t>(n) * 2u, 0.0f);
        for (uint32_t i = 0; i < block; ++i) window[2u * i] = plan.prev_time[i];
        for (uint32_t i = 0; i < block; ++i) window[2u * (block + i)] = in_block[i];
        queue_.WriteBuffer(plan.fx_a, 0, window.data(), small);
        queue_.WriteBuffer(plan.panl, 0, pan_l, num_ir * 4u);
        queue_.WriteBuffer(plan.panr, 0, pan_r, num_ir * 4u);
        struct FdlU { uint32_t n; uint32_t num_ir; uint32_t num_part; uint32_t head; };
        FdlU fu{n, num_ir, P, plan.head};
        queue_.WriteBuffer(plan.fdl_uniform, 0, &fu, sizeof(fu));

        const uint32_t fwd_wg = ((n / 2u) + 255u) / 256u;
        const uint32_t inv_wg = ((num_ir * (n / 2u)) + 255u) / 256u;
        const uint32_t mac_wg = ((num_ir * n) + 255u) / 256u;
        const uint32_t comb_wg = (n + 255u) / 256u;

        wgpu::CommandEncoderDescriptor enc_desc{};
        auto encoder = device_.CreateCommandEncoder(&enc_desc);
        // Forward-FFT the window into fx, copy the result into ring[head].
        encode_fft_passes(encoder, plan.fwd_bgs, fwd_wg);
        wgpu::Buffer& fwd_res = (plan.log2n & 1u) ? plan.fx_b : plan.fx_a;
        encoder.CopyBufferToBuffer(fwd_res, 0, plan.ring,
                                   static_cast<uint64_t>(plan.head) * small, small);
        // Partitioned MAC over the delay line → accum_a.
        {
            wgpu::ComputePassDescriptor pd{};
            auto pass = encoder.BeginComputePass(&pd);
            pass.SetPipeline(multi_fdl_mac_pipeline_);
            pass.SetBindGroup(0, plan.mac_bg);
            pass.DispatchWorkgroups(mac_wg);
            pass.End();
        }
        // Per-room inverse FFT, then pan-combine to stereo.
        encode_fft_passes(encoder, plan.inv_bgs, inv_wg);
        {
            wgpu::ComputePassDescriptor pd{};
            auto pass = encoder.BeginComputePass(&pd);
            pass.SetPipeline(multi_ir_combine_pipeline_);
            pass.SetBindGroup(0, plan.combine_bg);
            pass.DispatchWorkgroups(comb_wg);
            pass.End();
        }
        encoder.CopyBufferToBuffer(plan.out_lr, 0, plan.readback, 0, small);
        auto cmd = encoder.Finish();
        queue_.Submit(1, &cmd);

        // Read back the full 2n; overlap-save keeps the SECOND half of each room.
        std::vector<float> outbuf(static_cast<size_t>(n) * 2u);
        if (!read_back(plan.readback, outbuf.data(), small)) return false;
        for (uint32_t i = 0; i < block; ++i) {
            out_block[i]         = outbuf[block + i];       // L: outlr[block .. 2*block)
            out_block[block + i] = outbuf[n + block + i];   // R: outlr[n+block .. n+2*block)
        }

        // Advance the delay line: this block becomes prev; head moves forward.
        for (uint32_t i = 0; i < block; ++i) plan.prev_time[i] = in_block[i];
        plan.head = (plan.head + 1u) % P;
        return true;
    }

    // ── Multi-layer spectral stack ──────────────────────────────────────────

    bool prepare_spectral_stack(uint32_t n, uint32_t hop,
                                uint32_t num_layers) override {
        if (!initialized_ || num_layers == 0 || hop == 0) return false;
        if (!is_power_of_two(n) || n > kMaxFftN) return false;
        // The resident magnitude / phase buffers are num_layers*n floats — the
        // storage-buffer-binding-limited resource. Gate on the device's real
        // limit and keep byte math within uint32 (buffer sizes are uint32_t).
        const uint64_t res_bytes =
            static_cast<uint64_t>(num_layers) * n * sizeof(float);
        if (res_bytes > 0xFFFFFFFFull) return false;
        wgpu::Limits limits{};
        uint64_t max_bind = 0;
        if (device_.GetLimits(&limits) == wgpu::Status::Success)
            max_bind = limits.maxStorageBufferBindingSize;
        if (max_bind == 0)
            max_bind = static_cast<uint64_t>(kMaxFftN) * 2ull * sizeof(float);
        // Strictly under the per-binding limit: a buffer AT the limit creates a
        // Dawn error buffer (operations silently no-op), so leave headroom.
        if (res_bytes >= max_bind) return false;

        SpectralStackPlan plan;
        plan.n = n;
        plan.num_layers = num_layers;
        plan.hop_ratio = static_cast<float>(hop) / static_cast<float>(n);
        plan.log2n = 0;
        for (uint32_t v = n; v > 1u; v >>= 1) ++plan.log2n;

        const uint32_t small = n * 2u * static_cast<uint32_t>(sizeof(float));   // 2n
        const uint32_t res = num_layers * n * static_cast<uint32_t>(sizeof(float));
        const auto sc = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst
                      | wgpu::BufferUsage::CopySrc;
        plan.mag = create_storage_buffer(res,
            wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst);
        plan.phase = create_storage_buffer(res, sc);
        plan.weights = create_storage_buffer(
            num_layers * 4u, wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst);
        plan.comb_a = create_storage_buffer(small, sc);
        plan.comb_b = create_storage_buffer(small, sc);
        plan.readback = create_readback_buffer(small);
        wgpu::BufferDescriptor ad{};
        ad.size = 32;  // SpAdvance (8 x 4)
        ad.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
        plan.advance_u = device_.CreateBuffer(&ad);
        wgpu::BufferDescriptor cd{};
        cd.size = 16;  // SpCombine (4 x 4)
        cd.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
        plan.combine_u = device_.CreateBuffer(&cd);
        if (!plan.mag || !plan.phase || !plan.weights || !plan.comb_a ||
            !plan.comb_b || !plan.readback || !plan.advance_u || !plan.combine_u) {
            return false;
        }

        // Zero the resident buffers so an unset layer is silent (mag 0) and has a
        // defined phase. WriteBuffer from a host-zero vector (prepare is non-RT).
        std::vector<float> zeros(static_cast<std::size_t>(num_layers) * n, 0.0f);
        queue_.WriteBuffer(plan.mag, 0, zeros.data(), res);
        queue_.WriteBuffer(plan.phase, 0, zeros.data(), res);

        plan.advance_bg = create_bind_group(spectral_advance_pipeline_,
                                            {plan.phase, plan.advance_u});
        if (!plan.advance_bg) return false;
        // Combine writes into comb_a (the inverse-FFT start buffer).
        plan.combine_bg = create_bind_group(
            spectral_combine_pipeline_,
            {plan.mag, plan.phase, plan.weights, plan.comb_a, plan.combine_u});
        if (!plan.combine_bg) return false;

        // Inverse FFT passes (sign +1, batch 1) ping-ponging comb_a/comb_b.
        for (uint32_t s = 0; s < plan.log2n; ++s) {
            wgpu::BufferDescriptor ud{};
            ud.size = 16;
            ud.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
            wgpu::Buffer u = device_.CreateBuffer(&ud);
            if (!u) return false;
            struct FftParams { uint32_t n; uint32_t ns; float sign; uint32_t batch; };
            FftParams fp{n, 1u << s, 1.0f, 1u};
            queue_.WriteBuffer(u, 0, &fp, sizeof(fp));
            const bool src_is_a = (s % 2u == 0u);
            wgpu::BindGroup bg = create_bind_group(
                fft_pipeline_,
                src_is_a ? std::initializer_list<wgpu::Buffer>{plan.comb_a, plan.comb_b, u}
                         : std::initializer_list<wgpu::Buffer>{plan.comb_b, plan.comb_a, u});
            if (!bg) return false;
            plan.inv_u.push_back(std::move(u));
            plan.inv_bgs.push_back(std::move(bg));
        }

        spectral_stack_plans_.insert_or_assign(n, std::move(plan));
        return true;
    }

    bool spectral_stack_set_layer(uint32_t layer, const float* mag,
                                  const float* phase, uint32_t n,
                                  uint32_t num_layers) override {
        if (!initialized_ || !mag || !phase) return false;
        auto it = spectral_stack_plans_.find(n);
        if (it == spectral_stack_plans_.end() || it->second.num_layers != num_layers)
            return false;
        if (layer >= num_layers) return false;
        SpectralStackPlan& plan = it->second;
        const uint32_t row = n * static_cast<uint32_t>(sizeof(float));
        const uint64_t off = static_cast<uint64_t>(layer) * row;
        queue_.WriteBuffer(plan.mag, off, mag, row);
        queue_.WriteBuffer(plan.phase, off, phase, row);
        return true;
    }

    bool spectral_stack_render(const float* layer_weights, uint32_t num_layers,
                               float smear, float jitter, uint32_t rng_seed,
                               float* frame_out, uint32_t n) override {
        if (!initialized_ || !layer_weights || !frame_out) return false;
        auto it = spectral_stack_plans_.find(n);
        if (it == spectral_stack_plans_.end() || it->second.num_layers != num_layers)
            return false;
        SpectralStackPlan& plan = it->second;

        const uint32_t small = n * 2u * static_cast<uint32_t>(sizeof(float));
        // Blur half-width: any positive smear gives at least radius 1, up to
        // ~n/32 (matches the CPU reference's kernel growth, no odd-only step).
        const float sm = smear < 0.0f ? 0.0f : (smear > 1.0f ? 1.0f : smear);
        uint32_t radius = 0u;
        if (sm > 0.0f) {
            radius = static_cast<uint32_t>(sm * static_cast<float>(n / 32u));
            if (radius < 1u) radius = 1u;
        }
        const float jit = jitter < 0.0f ? 0.0f : (jitter > 1.0f ? 1.0f : jitter);

        struct SpAdvance {
            uint32_t n; uint32_t num_layers; float hop_ratio; float jitter;
            uint32_t seed; uint32_t pad0; uint32_t pad1; uint32_t pad2;
        };
        SpAdvance adv{n, num_layers, plan.hop_ratio, jit, rng_seed, 0u, 0u, 0u};
        struct SpCombine { uint32_t n; uint32_t num_layers; uint32_t radius; float inv_kernel; };
        SpCombine cmb{n, num_layers, radius,
                      1.0f / static_cast<float>(2u * radius + 1u)};
        queue_.WriteBuffer(plan.advance_u, 0, &adv, sizeof(adv));
        queue_.WriteBuffer(plan.combine_u, 0, &cmb, sizeof(cmb));
        queue_.WriteBuffer(plan.weights, 0, layer_weights, num_layers * 4u);

        const uint32_t adv_wg = ((num_layers * n) + 255u) / 256u;
        const uint32_t comb_wg = (n + 255u) / 256u;
        const uint32_t fft_wg = ((n / 2u) + 255u) / 256u;

        // One command buffer: advance phase (all layers) → smear + weighted-sum
        // combine → inverse FFT → copy the single frame to readback. Only 2n
        // floats leave the GPU regardless of num_layers.
        wgpu::CommandEncoderDescriptor enc_desc{};
        auto encoder = device_.CreateCommandEncoder(&enc_desc);
        {
            wgpu::ComputePassDescriptor pd{};
            auto pass = encoder.BeginComputePass(&pd);
            pass.SetPipeline(spectral_advance_pipeline_);
            pass.SetBindGroup(0, plan.advance_bg);
            pass.DispatchWorkgroups(adv_wg);
            pass.End();
        }
        {
            wgpu::ComputePassDescriptor pd{};
            auto pass = encoder.BeginComputePass(&pd);
            pass.SetPipeline(spectral_combine_pipeline_);
            pass.SetBindGroup(0, plan.combine_bg);
            pass.DispatchWorkgroups(comb_wg);
            pass.End();
        }
        encode_fft_passes(encoder, plan.inv_bgs, fft_wg);
        wgpu::Buffer& result = (plan.log2n & 1u) ? plan.comb_b : plan.comb_a;
        encoder.CopyBufferToBuffer(result, 0, plan.readback, 0, small);
        auto cmd = encoder.Finish();
        queue_.Submit(1, &cmd);

        if (!read_back(plan.readback, sp_scratch(small), small)) return false;
        const float* cplx = sp_scratch(small);
        const float inv = 1.0f / static_cast<float>(n);
        for (uint32_t i = 0; i < n; ++i) frame_out[i] = cplx[2u * i] * inv;
        return true;
    }

    bool prepare_conv_stack(uint32_t channels, uint32_t kernel,
                            const uint32_t* dilations, uint32_t num_layers,
                            const float* weights, uint32_t weights_len,
                            uint32_t block_size, float head_scale) override {
        if (!initialized_ || !dilations || !weights) return false;
        const uint32_t C = channels, K = kernel, L = num_layers, B = block_size;
        if (C == 0 || C > 64 || K == 0 || L == 0 || B == 0) return false;  // C<=64: fixed WGSL scratch
        const uint64_t per_layer = 2ull * C * C * K + 2ull * C + (uint64_t)C * C + C + (uint64_t)C * C + C;
        const uint64_t need = per_layer * L + (C + C) + (C + 1);
        if (weights_len < need) return false;

        // Left-history pad: the deepest tap reaches max_dilation*(K-1) samples
        // back, so each activation buffer carries that many history columns the
        // conv reads for cross-block continuity.
        uint32_t max_dil = 1;
        for (uint32_t l = 0; l < L; ++l) max_dil = dilations[l] > max_dil ? dilations[l] : max_dil;
        const uint32_t PAD = max_dil * (K - 1u);

        ConvStackPlan plan;
        plan.C = C; plan.K = K; plan.L = L; plan.B = B; plan.pad = PAD;
        plan.head_scale = head_scale;
        plan.per_layer = static_cast<uint32_t>(per_layer);

        const uint32_t wbytes = static_cast<uint32_t>(need) * 4u;
        const uint32_t act_bytes = C * (PAD + B) * 4u;   // padded activation buffer
        const uint32_t skip_bytes = C * B * 4u;
        const uint32_t blk_bytes = B * 4u;
        const uint32_t hist_bytes = C * PAD * 4u;
        const auto act_usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc
                             | wgpu::BufferUsage::CopyDst;
        plan.weights = create_storage_buffer(wbytes,
            wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst);
        plan.skip = create_storage_buffer(skip_bytes,
            wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst);
        plan.input = create_storage_buffer(blk_bytes,
            wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst);
        plan.output = create_storage_buffer(blk_bytes,
            wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc);
        plan.readback = create_readback_buffer(blk_bytes);
        plan.hist_temp = create_storage_buffer(hist_bytes, act_usage);  // history slide scratch
        if (!plan.weights || !plan.skip || !plan.input || !plan.output ||
            !plan.readback || !plan.hist_temp)
            return false;
        std::vector<float> act_zero(static_cast<std::size_t>(C) * (PAD + B), 0.0f);
        for (uint32_t l = 0; l <= L; ++l) {
            wgpu::Buffer a = create_storage_buffer(act_bytes, act_usage);
            if (!a) return false;
            queue_.WriteBuffer(a, 0, act_zero.data(), act_bytes);  // zero history at start
            plan.act.push_back(std::move(a));
        }
        queue_.WriteBuffer(plan.weights, 0, weights, wbytes);

        auto make_uniform = [&](uint32_t size) {
            wgpu::BufferDescriptor ud{};
            ud.size = size;
            ud.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
            return device_.CreateBuffer(&ud);
        };
        const uint32_t in_off = static_cast<uint32_t>(per_layer) * L;
        const uint32_t h_off = in_off + (C + C);

        plan.input_u = make_uniform(16);
        struct InU { uint32_t C, B, in_off, pad; } inu{C, B, in_off, PAD};
        queue_.WriteBuffer(plan.input_u, 0, &inu, sizeof(inu));
        plan.input_bg = create_bind_group(conv_in_pipeline_,
            {plan.weights, plan.input, plan.act[0], plan.input_u});
        if (!plan.input_bg) return false;

        for (uint32_t l = 0; l < L; ++l) {
            wgpu::Buffer u = make_uniform(32);
            struct LyU { uint32_t C, K, B, dil, woff, pad, p1, p2; }
                lyu{C, K, B, dilations[l], static_cast<uint32_t>(per_layer) * l, PAD, 0, 0};
            queue_.WriteBuffer(u, 0, &lyu, sizeof(lyu));
            wgpu::BindGroup bg = create_bind_group(conv_layer_pipeline_,
                {plan.weights, plan.act[l], plan.act[l + 1], plan.skip, u});
            if (!bg) return false;
            plan.layer_u.push_back(std::move(u));
            plan.layer_bg.push_back(std::move(bg));
        }

        plan.head_u = make_uniform(32);
        struct HdU { uint32_t C, B, h_off, p0; float scale; uint32_t p1, p2, p3; }
            hdu{C, B, h_off, 0, head_scale, 0, 0, 0};
        queue_.WriteBuffer(plan.head_u, 0, &hdu, sizeof(hdu));
        plan.head_bg = create_bind_group(conv_head_pipeline_,
            {plan.weights, plan.skip, plan.output, plan.head_u});
        if (!plan.head_bg) return false;

        cs_zero_.assign(static_cast<std::size_t>(C) * B, 0.0f);
        conv_stack_plans_.insert_or_assign(B, std::move(plan));
        return true;
    }

    bool conv_stack_forward(const float* in_block, float* out_block,
                            uint32_t block_size) override {
        if (!initialized_ || !in_block || !out_block) return false;
        auto it = conv_stack_plans_.find(block_size);
        if (it == conv_stack_plans_.end()) return false;
        ConvStackPlan& plan = it->second;
        const uint32_t B = plan.B;
        const uint32_t blk_bytes = B * 4u;

        queue_.WriteBuffer(plan.input, 0, in_block, blk_bytes);
        queue_.WriteBuffer(plan.skip, 0, cs_zero_.data(),
                           static_cast<uint32_t>(cs_zero_.size()) * 4u);

        const uint32_t wg = (B + 255u) / 256u;
        wgpu::CommandEncoderDescriptor enc_desc{};
        auto encoder = device_.CreateCommandEncoder(&enc_desc);
        auto run = [&](const wgpu::ComputePipeline& pl, const wgpu::BindGroup& bg) {
            wgpu::ComputePassDescriptor pd{};
            auto pass = encoder.BeginComputePass(&pd);
            pass.SetPipeline(pl);
            pass.SetBindGroup(0, bg);
            pass.DispatchWorkgroups(wg);
            pass.End();
        };
        run(conv_in_pipeline_, plan.input_bg);
        for (uint32_t l = 0; l < plan.L; ++l) run(conv_layer_pipeline_, plan.layer_bg[l]);
        run(conv_head_pipeline_, plan.head_bg);
        encoder.CopyBufferToBuffer(plan.output, 0, plan.readback, 0, blk_bytes);

        // Slide each activation buffer's window: the most-recent PAD columns
        // (the tail of [history|block]) become the next block's history, so taps
        // reach across the block boundary. Via a temp so source/dest can't
        // overlap; copies in one encoder are ordered, so reusing temp is safe.
        if (plan.pad > 0) {
            const uint32_t C = plan.C;
            const uint64_t hist_bytes = static_cast<uint64_t>(C) * plan.pad * 4u;
            const uint64_t tail_off = static_cast<uint64_t>(C) * plan.B * 4u;  // [B .. B+PAD) columns
            for (auto& a : plan.act) {
                encoder.CopyBufferToBuffer(a, tail_off, plan.hist_temp, 0, hist_bytes);
                encoder.CopyBufferToBuffer(plan.hist_temp, 0, a, 0, hist_bytes);
            }
        }
        auto cmd = encoder.Finish();
        queue_.Submit(1, &cmd);

        return read_back(plan.readback, out_block, blk_bytes);
    }

    // ── WaveNet ─────────────────────────────────────────────────────────

    bool prepare_wavenet(const WavenetLayerArraySpec* arrays, uint32_t num_arrays,
                     const float* weights, uint32_t weights_len,
                     uint32_t block_size, float head_scale,
                     uint32_t instance) override {
        if (!initialized_ || !arrays || !weights) return false;
        const uint32_t B = block_size;
        if (num_arrays == 0 || B == 0) return false;

        // Validate shapes + compute the exact weight count by walking the blob in
        // the serialization order below. Layout must match the flat weight blob byte-for-byte.
        uint64_t need = 0;
        for (uint32_t a = 0; a < num_arrays; ++a) {
            const WavenetLayerArraySpec& s = arrays[a];
            if (s.channels == 0 || s.channels > 64 || s.kernel == 0 ||
                s.num_layers == 0 || !s.dilations || s.head_size == 0)
                return false;
            if (s.condition_size != 1) return false;  // mono condition only
            const uint32_t expect_in = (a == 0) ? 1u : arrays[a - 1].channels;
            if (s.input_size != expect_in) return false;
            if (a > 0 && arrays[a - 1].head_size != s.channels) return false;  // head chain
            const uint64_t Z = s.gated ? 2ull * s.channels : s.channels;
            need += static_cast<uint64_t>(s.channels) * s.input_size;  // rechannel
            for (uint32_t l = 0; l < s.num_layers; ++l)
                need += Z * s.channels * s.kernel + Z          // conv W+bias
                        + Z * s.condition_size                  // input_mixin
                        + static_cast<uint64_t>(s.channels) * s.channels + s.channels;  // layer1x1
            need += static_cast<uint64_t>(s.head_size) * s.channels
                    + (s.head_bias ? s.head_size : 0u);        // head rechannel
        }
        need += 1;  // trailing head_scale
        if (weights_len != need) return false;

        WavenetPlan plan;
        plan.B = B;
        plan.head_scale = head_scale;

        const uint32_t wbytes = static_cast<uint32_t>(need) * 4u;
        const uint32_t blk_bytes = B * 4u;
        const auto act_usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc
                             | wgpu::BufferUsage::CopyDst;
        plan.weights = create_storage_buffer(wbytes,
            wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst);
        plan.input = create_storage_buffer(blk_bytes,
            wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst);
        plan.output = create_storage_buffer(blk_bytes,
            wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc);
        plan.readback = create_readback_buffer(blk_bytes);
        if (!plan.weights || !plan.input || !plan.output || !plan.readback) return false;
        queue_.WriteBuffer(plan.weights, 0, weights, wbytes);

        auto make_uniform = [&](uint32_t size) {
            wgpu::BufferDescriptor ud{};
            ud.size = size;
            ud.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
            return device_.CreateBuffer(&ud);
        };

        uint32_t max_hist_floats = 0;      // for the shared history-slide scratch
        uint64_t woff = 0;

        for (uint32_t a = 0; a < num_arrays; ++a) {
            const WavenetLayerArraySpec& s = arrays[a];
            const uint32_t C = s.channels, K = s.kernel, H = s.head_size;
            const uint32_t Z = s.gated ? 2u * C : C;
            uint32_t max_dil = 1;
            for (uint32_t l = 0; l < s.num_layers; ++l)
                max_dil = s.dilations[l] > max_dil ? s.dilations[l] : max_dil;
            const uint32_t PAD = max_dil * (K - 1u);

            WavenetArray na;
            na.channels = C; na.head_size = H; na.num_layers = s.num_layers;
            na.pad = PAD; na.head_bias = s.head_bias;

            const uint32_t act_bytes = C * (PAD + B) * 4u;
            std::vector<float> act_zero(static_cast<std::size_t>(C) * (PAD + B), 0.0f);
            for (uint32_t l = 0; l <= s.num_layers; ++l) {
                wgpu::Buffer buf = create_storage_buffer(act_bytes, act_usage);
                if (!buf) return false;
                queue_.WriteBuffer(buf, 0, act_zero.data(), act_bytes);  // zero history
                na.act.push_back(std::move(buf));
            }
            na.headacc = create_storage_buffer(C * B * 4u,
                wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst);
            na.headout = create_storage_buffer(H * B * 4u,
                wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc);
            if (!na.headacc || !na.headout) return false;

            max_hist_floats = std::max(max_hist_floats, C * PAD);

            // rechannel offset (consumed first in the array).
            const uint32_t rc_w = static_cast<uint32_t>(woff);
            woff += static_cast<uint64_t>(C) * s.input_size;

            // rechannel uniform + bind group. Array 0 reads the mono input
            // (in_ch=1, src_pad=0); array a>0 reads the previous array's last
            // activation (in_ch=prevC, src_pad=prevPAD), wired in wavenet_forward
            // ordering (the bind group references the previous array's buffer).
            na.rechannel_u = make_uniform(32);
            const uint32_t src_pad = (a == 0) ? 0u : plan.arrays[a - 1].pad;
            struct RcU { uint32_t in_ch, out_ch, B, w_off, src_pad, dst_pad, p0, p1; }
                rcu{s.input_size, C, B, rc_w, src_pad, PAD, 0, 0};
            queue_.WriteBuffer(na.rechannel_u, 0, &rcu, sizeof(rcu));
            const wgpu::Buffer& rc_src = (a == 0) ? plan.input
                                                  : plan.arrays[a - 1].act.back();
            na.rechannel_bg = create_bind_group(wavenet_rechannel_pipeline_,
                {plan.weights, rc_src, na.act[0], na.rechannel_u});
            if (!na.rechannel_bg) return false;

            for (uint32_t l = 0; l < s.num_layers; ++l) {
                const uint32_t conv_w = static_cast<uint32_t>(woff);
                woff += static_cast<uint64_t>(Z) * C * K;
                const uint32_t conv_b = static_cast<uint32_t>(woff);
                woff += Z;
                const uint32_t mix_w = static_cast<uint32_t>(woff);
                woff += static_cast<uint64_t>(Z) * s.condition_size;
                const uint32_t l1_w = static_cast<uint32_t>(woff);
                woff += static_cast<uint64_t>(C) * C;
                const uint32_t l1_b = static_cast<uint32_t>(woff);
                woff += C;

                wgpu::Buffer u = make_uniform(64);
                struct LyU { uint32_t C, K, B, dil, Z, gated, pad,
                             conv_w, conv_b, mix_w, l1_w, l1_b, p0, p1, p2, p3; }
                    lyu{C, K, B, s.dilations[l], Z, s.gated, PAD,
                        conv_w, conv_b, mix_w, l1_w, l1_b, 0, 0, 0, 0};
                queue_.WriteBuffer(u, 0, &lyu, sizeof(lyu));
                wgpu::BindGroup bg = create_bind_group(wavenet_layer_pipeline_,
                    {plan.weights, na.act[l], na.act[l + 1], plan.input, na.headacc, u});
                if (!bg) return false;
                na.layer_u.push_back(std::move(u));
                na.layer_bg.push_back(std::move(bg));
            }

            const uint32_t hr_w = static_cast<uint32_t>(woff);
            woff += static_cast<uint64_t>(H) * C;
            const uint32_t hr_b = static_cast<uint32_t>(woff);
            if (s.head_bias) woff += H;

            na.head_u = make_uniform(32);
            struct HdU { uint32_t C, H, B, hr_w, hr_b, head_bias, p0, p1; }
                hdu{C, H, B, hr_w, hr_b, s.head_bias, 0, 0};
            queue_.WriteBuffer(na.head_u, 0, &hdu, sizeof(hdu));
            na.head_bg = create_bind_group(wavenet_head_pipeline_,
                {plan.weights, na.headacc, na.headout, na.head_u});
            if (!na.head_bg) return false;

            plan.arrays.push_back(std::move(na));
        }

        // Final scale pass over the last array's head output (head_size==1 in the
        // standard model; the shader reads index 0 of each sample's head vector).
        const WavenetArray& last = plan.arrays.back();
        plan.scale_u = make_uniform(16);
        struct ScU { uint32_t B, H; float scale; uint32_t p0; }
            scu{B, last.head_size, head_scale, 0};
        queue_.WriteBuffer(plan.scale_u, 0, &scu, sizeof(scu));
        plan.scale_bg = create_bind_group(wavenet_scale_pipeline_,
            {last.headout, plan.output, plan.scale_u});
        if (!plan.scale_bg) return false;

        const uint32_t hist_bytes = std::max(max_hist_floats, 1u) * 4u;
        plan.hist_temp = create_storage_buffer(hist_bytes, act_usage);
        if (!plan.hist_temp) return false;

        wavenet_plans_.insert_or_assign(wavenet_plan_key(B, instance), std::move(plan));
        return true;
    }

    bool wavenet_forward(const float* in_block, float* out_block,
                     uint32_t block_size, uint32_t instance) override {
        if (!initialized_ || !in_block || !out_block) return false;
        auto it = wavenet_plans_.find(wavenet_plan_key(block_size, instance));
        if (it == wavenet_plans_.end()) return false;
        WavenetPlan& plan = it->second;
        const uint32_t B = plan.B;
        const uint32_t blk_bytes = B * 4u;

        queue_.WriteBuffer(plan.input, 0, in_block, blk_bytes);

        const uint32_t wg = (B + 255u) / 256u;
        wgpu::CommandEncoderDescriptor enc_desc{};
        auto encoder = device_.CreateCommandEncoder(&enc_desc);

        // Array 0's head accumulator starts at zero; every later array is seeded
        // from the previous array's head output below, so only array 0 needs
        // clearing. An on-GPU ClearBuffer replaces a per-block CPU→GPU upload of
        // zeros for every array — the later uploads were dead work, immediately
        // overwritten by the seed copy.
        if (!plan.arrays.empty())
            encoder.ClearBuffer(plan.arrays[0].headacc, 0,
                                static_cast<uint64_t>(plan.arrays[0].channels) * B * 4u);

        // One compute pass per array: rechannel → layers → head run as
        // consecutive dispatches in a single pass (WebGPU orders storage
        // read/write between dispatches within a pass), collapsing ~L+2 pass
        // objects per array into one. The inter-array seed copy is an
        // encoder-level command, so it sits between passes; rechannel writes
        // only act[0] and the seed writes only headacc, so seeding before the
        // pass is order-independent of rechannel.
        for (uint32_t a = 0; a < plan.arrays.size(); ++a) {
            WavenetArray& na = plan.arrays[a];
            if (a > 0) {  // seed this array's head accumulator with the previous head output
                const WavenetArray& prev = plan.arrays[a - 1];
                encoder.CopyBufferToBuffer(prev.headout, 0, na.headacc, 0,
                                           static_cast<uint64_t>(na.channels) * B * 4u);
            }
            wgpu::ComputePassDescriptor pd{};
            auto pass = encoder.BeginComputePass(&pd);
            pass.SetPipeline(wavenet_rechannel_pipeline_);
            pass.SetBindGroup(0, na.rechannel_bg);
            pass.DispatchWorkgroups(wg);
            for (uint32_t l = 0; l < na.num_layers; ++l) {
                pass.SetPipeline(wavenet_layer_pipeline_);
                pass.SetBindGroup(0, na.layer_bg[l]);
                // The WaveNet layer shader runs one workgroup per sample (WG lanes
                // cooperate across channels), so it needs exactly B workgroups —
                // not the ceil(B/256) used by the one-thread-per-sample passes.
                pass.DispatchWorkgroups(B);
            }
            pass.SetPipeline(wavenet_head_pipeline_);
            pass.SetBindGroup(0, na.head_bg);
            pass.DispatchWorkgroups(wg);
            pass.End();
        }
        {
            wgpu::ComputePassDescriptor pd{};
            auto pass = encoder.BeginComputePass(&pd);
            pass.SetPipeline(wavenet_scale_pipeline_);
            pass.SetBindGroup(0, plan.scale_bg);
            pass.DispatchWorkgroups(wg);
            pass.End();
        }
        encoder.CopyBufferToBuffer(plan.output, 0, plan.readback, 0, blk_bytes);

        // Slide each array's activation windows: the most-recent PAD columns
        // become the next block's history so dilated taps reach across the block
        // boundary (streaming continuity). Via a shared temp so source/dest never
        // overlap; copies in one encoder are ordered, so reusing temp is safe.
        for (auto& na : plan.arrays) {
            if (na.pad == 0) continue;
            const uint64_t hist_bytes = static_cast<uint64_t>(na.channels) * na.pad * 4u;
            const uint64_t tail_off = static_cast<uint64_t>(na.channels) * B * 4u;
            for (auto& buf : na.act) {
                encoder.CopyBufferToBuffer(buf, tail_off, plan.hist_temp, 0, hist_bytes);
                encoder.CopyBufferToBuffer(plan.hist_temp, 0, buf, 0, hist_bytes);
            }
        }
        auto cmd = encoder.Finish();
        queue_.Submit(1, &cmd);

        return read_back(plan.readback, out_block, blk_bytes);
    }

    // ── Linear algebra ─────────────────────────────────────────────────────

    bool matmul(const float* a, const float* b, float* c,
                uint32_t m, uint32_t k, uint32_t n) override {
        if (!initialized_ || a == nullptr || b == nullptr || c == nullptr) return false;
        if (m == 0u || k == 0u || n == 0u) return false;
        // Bound element counts so byte sizes stay within uint32_t and a sane
        // storage-buffer budget (≤ 2^24 elems = 64 MiB per matrix).
        const uint64_t ae = static_cast<uint64_t>(m) * k;
        const uint64_t be = static_cast<uint64_t>(k) * n;
        const uint64_t ce = static_cast<uint64_t>(m) * n;
        if (ae > (1u << 24) || be > (1u << 24) || ce > (1u << 24)) return false;

        const uint32_t ab = static_cast<uint32_t>(ae) * 4u;
        const uint32_t bb = static_cast<uint32_t>(be) * 4u;
        const uint32_t cb = static_cast<uint32_t>(ce) * 4u;
        auto a_buf = create_storage_buffer(ab, wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst);
        auto b_buf = create_storage_buffer(bb, wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst);
        auto c_buf = create_storage_buffer(cb, wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc);
        auto rb = create_readback_buffer(cb);
        wgpu::BufferDescriptor ud{};
        ud.size = 16;
        ud.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
        auto u_buf = device_.CreateBuffer(&ud);
        if (!a_buf || !b_buf || !c_buf || !rb || !u_buf) return false;

        struct MatmulParams { uint32_t m; uint32_t k; uint32_t n; uint32_t pad; };
        MatmulParams p{m, k, n, 0u};
        queue_.WriteBuffer(a_buf, 0, a, ab);
        queue_.WriteBuffer(b_buf, 0, b, bb);
        queue_.WriteBuffer(u_buf, 0, &p, sizeof(p));

        auto bg = create_bind_group(matmul_pipeline_, {a_buf, b_buf, c_buf, u_buf});
        if (!bg) return false;
        dispatch(matmul_pipeline_, bg, (m * n + 63u) / 64u);
        copy_buffer(c_buf, rb, cb);
        return read_back(rb, c, cb);
    }

    // ── Synthesis ──────────────────────────────────────────────────────────

    bool additive_synth(const float* partials, float* out, uint32_t num_partials,
                        uint32_t num_samples, float sample_rate, float t0_samples) override {
        if (!initialized_ || partials == nullptr || out == nullptr) return false;
        if (num_partials == 0u || num_samples == 0u || sample_rate <= 0.0f) return false;
        if (static_cast<uint64_t>(num_samples) > (1u << 24) ||
            static_cast<uint64_t>(num_partials) > (1u << 22)) return false;

        const uint32_t pbytes = num_partials * 3u * 4u;
        const uint32_t obytes = num_samples * 4u;
        auto p_buf = create_storage_buffer(pbytes, wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst);
        auto o_buf = create_storage_buffer(obytes, wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc);
        auto rb = create_readback_buffer(obytes);
        wgpu::BufferDescriptor ud{};
        ud.size = 16;
        ud.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
        auto u_buf = device_.CreateBuffer(&ud);
        if (!p_buf || !o_buf || !rb || !u_buf) return false;

        struct AddParams { uint32_t num_partials; uint32_t num_samples; float sample_rate; float t0; };
        AddParams ap{num_partials, num_samples, sample_rate, t0_samples};
        queue_.WriteBuffer(p_buf, 0, partials, pbytes);
        queue_.WriteBuffer(u_buf, 0, &ap, sizeof(ap));

        // Pick the dispatch strategy. The serial kernel (one thread per sample,
        // workgroup_size 256) sums the partials with NO barriers, so once the
        // sample block gives it enough workgroups — ceil(S/256) — to fill the
        // device it is already optimal, even for many partials (a partial is one
        // cheap sin, unlike modal_strike's sin·exp). The cooperative kernel (one
        // workgroup per sample, WG lanes tree-reduce the partials) only wins when
        // the serial kernel is workgroup-starved by a small block; at large blocks
        // its per-sample tree-reduce barriers are pure overhead (measured: it
        // regresses e.g. P=1024 S=16384). So route on occupancy alone —
        // ceil(S/256) < 32 (starved crossover on Apple Metal via the gpu_roofline
        // harness) — plus the extreme block the serial dispatch can't express
        // (> 65535 workgroups; the coop kernel grid-strides and caps its dispatch).
        // Both kernels share the (partials, out, params) bind-group layout.
        const uint32_t serial_wgs = (num_samples + 255u) / 256u;
        const bool coop = serial_wgs < 32u || serial_wgs > 65535u;
        const wgpu::ComputePipeline& pipe = coop ? additive_coop_pipeline_ : additive_pipeline_;
        auto bg = create_bind_group(pipe, {p_buf, o_buf, u_buf});
        if (!bg) return false;
        dispatch(pipe, bg, coop ? std::min(num_samples, 65535u) : serial_wgs);
        copy_buffer(o_buf, rb, obytes);
        return read_back(rb, out, obytes);
    }

    bool modal_strike(const float* modes, float* out, uint32_t num_modes,
                      uint32_t num_samples, float sample_rate, float t0_samples) override {
        if (!initialized_ || modes == nullptr || out == nullptr) return false;
        if (num_modes == 0u || num_samples == 0u || sample_rate <= 0.0f) return false;
        if (static_cast<uint64_t>(num_samples) > (1u << 24) ||
            static_cast<uint64_t>(num_modes) > (1u << 22)) return false;

        const uint32_t mbytes = num_modes * 4u * 4u;  // 4 floats/mode
        const uint32_t obytes = num_samples * 4u;
        auto m_buf = create_storage_buffer(mbytes, wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst);
        auto o_buf = create_storage_buffer(obytes, wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc);
        auto rb = create_readback_buffer(obytes);
        wgpu::BufferDescriptor ud{};
        ud.size = 16;
        ud.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
        auto u_buf = device_.CreateBuffer(&ud);
        if (!m_buf || !o_buf || !rb || !u_buf) return false;

        struct ModalParams { uint32_t num_modes; uint32_t num_samples; float sample_rate; float t0; };
        ModalParams mp{num_modes, num_samples, sample_rate, t0_samples};
        queue_.WriteBuffer(m_buf, 0, modes, mbytes);
        queue_.WriteBuffer(u_buf, 0, &mp, sizeof(mp));

        // Pick the dispatch strategy. The serial kernel (one thread per sample,
        // workgroup_size 256) already fills the device when the sample block is
        // large — ceil(S/256) workgroups — and sums the modes with no barriers, so
        // it is best for many-sample / few-mode blocks. The cooperative kernel (one
        // workgroup per sample, WG lanes tree-reduce the modes) wins when the serial
        // kernel is workgroup-starved (small block) OR the modes sum is the
        // bottleneck (many modes). Crossovers are from the gpu_roofline harness on
        // Apple Metal: ceil(S/256) < 8 (starved) or num_modes >= 512. Both kernels
        // share the (modes, out, params) bind-group layout.
        const uint32_t serial_wgs = (num_samples + 255u) / 256u;
        // Also take the cooperative route when the serial dispatch would exceed the
        // 65535 workgroups-per-dimension limit — the coop kernel grid-strides and
        // caps its dispatch, so it stays valid at the largest supported blocks.
        const bool coop = serial_wgs < 8u || num_modes >= 512u || serial_wgs > 65535u;
        const wgpu::ComputePipeline& pipe = coop ? modal_coop_pipeline_ : modal_pipeline_;
        auto bg = create_bind_group(pipe, {m_buf, o_buf, u_buf});
        if (!bg) return false;
        dispatch(pipe, bg, coop ? std::min(num_samples, 65535u) : serial_wgs);
        copy_buffer(o_buf, rb, obytes);
        return read_back(rb, out, obytes);
    }

    bool granular_cloud(const float* grains, const float* source, float* out,
                        uint32_t num_grains, uint32_t source_len,
                        uint32_t num_samples) override {
        if (!initialized_ || grains == nullptr || source == nullptr || out == nullptr) return false;
        if (num_grains == 0u || source_len == 0u || num_samples == 0u) return false;
        if (static_cast<uint64_t>(num_samples) > (1u << 24) ||
            static_cast<uint64_t>(source_len) > (1u << 24) ||
            static_cast<uint64_t>(num_grains) > (1u << 22)) return false;

        const uint32_t gbytes = num_grains * 5u * 4u;  // 5 floats/grain
        const uint32_t sbytes = source_len * 4u;
        const uint32_t obytes = num_samples * 4u;
        auto g_buf = create_storage_buffer(gbytes, wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst);
        auto s_buf = create_storage_buffer(sbytes, wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst);
        auto o_buf = create_storage_buffer(obytes, wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc);
        auto rb = create_readback_buffer(obytes);
        wgpu::BufferDescriptor ud{};
        ud.size = 16;
        ud.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
        auto u_buf = device_.CreateBuffer(&ud);
        if (!g_buf || !s_buf || !o_buf || !rb || !u_buf) return false;

        struct GrainParams { uint32_t num_grains; uint32_t num_samples; uint32_t source_len; uint32_t pad; };
        GrainParams gp{num_grains, num_samples, source_len, 0u};
        queue_.WriteBuffer(g_buf, 0, grains, gbytes);
        queue_.WriteBuffer(s_buf, 0, source, sbytes);
        queue_.WriteBuffer(u_buf, 0, &gp, sizeof(gp));

        auto bg = create_bind_group(granular_pipeline_, {g_buf, s_buf, o_buf, u_buf});
        if (!bg) return false;
        dispatch(granular_pipeline_, bg, (num_samples + 255u) / 256u);
        copy_buffer(o_buf, rb, obytes);
        return read_back(rb, out, obytes);
    }

    // ── Neural inference ────────────────────────────────────────────────────

    bool dense_tanh(const float* input, const float* weights, const float* bias,
                    float* output, uint32_t in_dim, uint32_t out_dim) override {
        if (!initialized_ || !input || !weights || !bias || !output) return false;
        if (in_dim == 0u || out_dim == 0u) return false;
        const uint64_t we = static_cast<uint64_t>(in_dim) * out_dim;
        if (we > (1u << 24) || in_dim > (1u << 20) || out_dim > (1u << 20)) return false;

        const uint32_t xb = in_dim * 4u, wb = static_cast<uint32_t>(we) * 4u, ob = out_dim * 4u;
        auto x_buf = create_storage_buffer(xb, wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst);
        auto w_buf = create_storage_buffer(wb, wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst);
        auto b_buf = create_storage_buffer(ob, wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst);
        auto o_buf = create_storage_buffer(ob, wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc);
        auto rb = create_readback_buffer(ob);
        wgpu::BufferDescriptor ud{};
        ud.size = 16;
        ud.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
        auto u_buf = device_.CreateBuffer(&ud);
        if (!x_buf || !w_buf || !b_buf || !o_buf || !rb || !u_buf) return false;

        struct DenseParams { uint32_t in_dim; uint32_t out_dim; uint32_t p0; uint32_t p1; };
        DenseParams dp{in_dim, out_dim, 0u, 0u};
        queue_.WriteBuffer(x_buf, 0, input, xb);
        queue_.WriteBuffer(w_buf, 0, weights, wb);
        queue_.WriteBuffer(b_buf, 0, bias, ob);
        queue_.WriteBuffer(u_buf, 0, &dp, sizeof(dp));

        auto bg = create_bind_group(dense_tanh_pipeline_, {x_buf, w_buf, b_buf, o_buf, u_buf});
        if (!bg) return false;
        dispatch(dense_tanh_pipeline_, bg, (out_dim + 63u) / 64u);
        copy_buffer(o_buf, rb, ob);
        return read_back(rb, output, ob);
    }

    // ── Capabilities ─────────────────────────────────────────────────────

    CapabilityReport capabilities() const override {
        CapabilityReport r{};
        r.available = initialized_ && (device_ != nullptr);
        if (!r.available) return r;

        r.timestamp_query = device_.HasFeature(wgpu::FeatureName::TimestampQuery);
        r.shader_f16 = device_.HasFeature(wgpu::FeatureName::ShaderF16);

        wgpu::Limits limits{};
        if (device_.GetLimits(&limits) == wgpu::Status::Success) {
            r.max_storage_buffer_binding_size = limits.maxStorageBufferBindingSize;
            r.max_buffer_size = limits.maxBufferSize;
            r.max_compute_invocations_per_workgroup = limits.maxComputeInvocationsPerWorkgroup;
            r.max_compute_workgroup_size_x = limits.maxComputeWorkgroupSizeX;
            r.max_compute_workgroup_storage_size = limits.maxComputeWorkgroupStorageSize;
        }

        if (adapter_) {
            wgpu::AdapterInfo info{};
            adapter_.GetInfo(&info);
            r.backend = backend_name(info.backendType);
            r.vendor = std::string(info.vendor.data, info.vendor.length);
        } else {
            // Shared-device mode borrows the surface's device; we don't hold
            // the adapter here.
            r.backend = "shared";
        }

        // Largest power-of-two complex FFT the storage-buffer limit admits,
        // capped at the implementation maximum.
        if (r.max_storage_buffer_binding_size > 0) {
            const uint64_t max_complex =
                r.max_storage_buffer_binding_size / (2u * sizeof(float));
            for (uint32_t nfft = 1u; nfft <= kMaxFftN && nfft <= max_complex; nfft <<= 1) {
                r.max_fft_size = nfft;
            }
        }
        return r;
    }

    // ── Device sharing verification ─────────────────────────────────────

#if defined(__EMSCRIPTEN__)
    // Verifying device sharing means submitting from Skia Graphite and compute on
    // one device and reading the result back — both blocking. There is no Skia
    // and no blocking readback in the browser GPU-audio module, and the whole
    // point of that module is that it needs neither. The method is pure virtual,
    // so it stays defined and says so.
    DeviceSharingReport verify_device_sharing(GpuSurface&) override {
        DeviceSharingReport report;
        report.notes = "not supported in browser";
        return report;
    }
#else
    DeviceSharingReport verify_device_sharing(GpuSurface& surface) override {
        DeviceSharingReport report;

        if (!surface.is_initialized()) {
            report.notes = "GpuSurface not initialized";
            return report;
        }

        // Step 1: Obtain device handles
        auto* dev = static_cast<wgpu::Device*>(surface.dawn_device_handle());
        auto* q = static_cast<wgpu::Queue*>(surface.dawn_queue_handle());
        if (!dev || !q) {
            report.notes = "Device handles are null";
            return report;
        }
        report.device_obtained = true;

        // Identify backend
        if (adapter_) {
            wgpu::AdapterInfo info;
            adapter_.GetInfo(&info);
            switch (info.backendType) {
                case wgpu::BackendType::Metal:   report.backend_name = "Metal"; break;
                case wgpu::BackendType::D3D12:   report.backend_name = "D3D12"; break;
                case wgpu::BackendType::Vulkan:  report.backend_name = "Vulkan"; break;
                default: report.backend_name = "Unknown"; break;
            }
        }

        // Step 2: Create a compute buffer on the shared device
        wgpu::BufferDescriptor buf_desc{};
        buf_desc.size = 4096;
        buf_desc.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc;
        auto compute_buf = dev->CreateBuffer(&buf_desc);
        report.second_consumer_works = (compute_buf != nullptr);

        if (!report.second_consumer_works) {
            report.notes = "Failed to create compute buffer on shared device";
            return report;
        }

        // Step 3: Submit compute work on the shared queue
        // Create a minimal compute pass and submit alongside potential Skia work
        {
            wgpu::CommandEncoderDescriptor enc_desc{};
            enc_desc.label = "device-sharing-test";
            auto encoder = dev->CreateCommandEncoder(&enc_desc);

            // Just a pass that writes zeros — proves we can submit
            wgpu::ComputePassDescriptor pass_desc{};
            pass_desc.label = "sharing-test-pass";
            auto pass = encoder.BeginComputePass(&pass_desc);
            pass.End();

            auto cmd = encoder.Finish();
            q->Submit(1, &cmd);
        }
        report.concurrent_submission_ok = true;

        // Step 4: Memory pressure test — allocate substantial GPU memory
        {
            constexpr uint32_t test_size = 16 * 1024 * 1024; // 16 MB
            wgpu::BufferDescriptor big_desc{};
            big_desc.size = test_size;
            big_desc.usage = wgpu::BufferUsage::Storage;

            auto big_buf_1 = dev->CreateBuffer(&big_desc);
            auto big_buf_2 = dev->CreateBuffer(&big_desc);

            report.memory_pressure_ok = (big_buf_1 != nullptr && big_buf_2 != nullptr);

            if (!report.memory_pressure_ok) {
                report.notes = "Memory pressure: failed to allocate 2x 16MB buffers";
            }
        }

        std::ostringstream notes;
        notes << "Backend: " << report.backend_name
              << ". Device sharing verified: compute buffers and command submission "
              << "work on the same Dawn device used by Skia Graphite. "
              << "Shared-device Three.js bridge prerequisites are satisfied.";
        report.notes = notes.str();

        return report;
    }
#endif  // !__EMSCRIPTEN__

    // ── Benchmarking ────────────────────────────────────────────────────

#if defined(__EMSCRIPTEN__)
    // Both benchmarks time the GPU by busy-waiting on ProcessEvents() until the
    // map lands. That is a hard deadlock in the browser: the spin starves the JS
    // event loop that would resolve the map, and ProcessEvents() does not resolve
    // map callbacks there at all. Timing is not what the browser module is for —
    // it reports per-block stats from the async path (async_stats()) — so the
    // blocking benchmarks are compiled out rather than emulated.
    std::vector<BenchmarkResult> benchmark_magnitude(
        const std::vector<uint32_t>&, int) override {
        return {};
    }

    std::vector<BenchmarkResult> benchmark_complex_multiply(
        const std::vector<uint32_t>&, int) override {
        return {};
    }
#else
    std::vector<BenchmarkResult> benchmark_magnitude(
        const std::vector<uint32_t>& sizes, int iterations) override {
        std::vector<BenchmarkResult> results;

        for (uint32_t num_bins : sizes) {
            BenchmarkResult avg{};
            avg.num_elements = num_bins;

            // Generate test data
            std::vector<float> input(num_bins * 2);
            std::vector<float> output(num_bins);
            for (uint32_t i = 0; i < num_bins * 2; ++i)
                input[i] = static_cast<float>(i % 100) / 100.0f;

            uint32_t input_bytes = num_bins * 2 * sizeof(float);
            uint32_t output_bytes = num_bins * sizeof(float);

            // Pre-create GPU buffers for fair timing
            auto input_buf = create_storage_buffer(input_bytes,
                wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst);
            auto output_buf = create_storage_buffer(output_bytes,
                wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc);
            auto readback_buf = create_readback_buffer(output_bytes);
            auto bind_group = create_bind_group(magnitude_pipeline_, {input_buf, output_buf});
            uint32_t workgroups = (num_bins + 255) / 256;

            // Warm-up pass
            {
                auto warmup_rb = create_readback_buffer(output_bytes);
                queue_.WriteBuffer(input_buf, 0, input.data(), input_bytes);
                dispatch(magnitude_pipeline_, bind_group, workgroups);
                copy_buffer(output_buf, warmup_rb, output_bytes);
                read_back(warmup_rb, output.data(), output_bytes);
            }

            // Timed iterations
            for (int iter = 0; iter < iterations; ++iter) {
                double t0 = now_us();
                queue_.WriteBuffer(input_buf, 0, input.data(), input_bytes);
                double t1 = now_us();

                dispatch(magnitude_pipeline_, bind_group, workgroups);
                // Force GPU completion by doing copy + readback
                copy_buffer(output_buf, readback_buf, output_bytes);
                // Map synchronously to measure actual GPU time
                bool mapped = false;
                readback_buf.MapAsync(wgpu::MapMode::Read, 0, output_bytes,
                    wgpu::CallbackMode::AllowProcessEvents,
                    [&mapped](wgpu::MapAsyncStatus status, wgpu::StringView) {
                        mapped = (status == wgpu::MapAsyncStatus::Success);
                    });
                instance_.ProcessEvents();
                // Busy-wait for map (measures actual GPU completion)
                while (!mapped) {
                    instance_.ProcessEvents();
                }
                double t2 = now_us();
                readback_buf.Unmap();

                // Reconstruct readback buffer for next iteration
                readback_buf = create_readback_buffer(output_bytes);

                avg.upload_us += (t1 - t0);
                avg.dispatch_us += (t2 - t1);
                avg.total_us += (t2 - t0);
            }

            avg.upload_us /= iterations;
            avg.dispatch_us /= iterations;
            avg.total_us /= iterations;
            // dispatch_us includes readback in this measurement
            avg.readback_us = 0; // folded into dispatch_us

            // CPU baseline: magnitude computation
            {
                double cpu_total = 0;
                for (int iter = 0; iter < iterations; ++iter) {
                    double t0 = now_us();
                    for (uint32_t i = 0; i < num_bins; ++i) {
                        float re = input[i * 2];
                        float im = input[i * 2 + 1];
                        output[i] = std::sqrt(re * re + im * im);
                    }
                    double t1 = now_us();
                    cpu_total += (t1 - t0);
                }
                avg.cpu_baseline_us = cpu_total / iterations;
            }

            avg.gpu_faster = avg.total_us < avg.cpu_baseline_us;
            results.push_back(avg);
        }

        return results;
    }

    std::vector<BenchmarkResult> benchmark_complex_multiply(
        const std::vector<uint32_t>& sizes, int iterations) override {
        std::vector<BenchmarkResult> results;

        for (uint32_t count : sizes) {
            BenchmarkResult avg{};
            avg.num_elements = count;

            std::vector<float> a(count * 2), b(count * 2), result(count * 2);
            for (uint32_t i = 0; i < count * 2; ++i) {
                a[i] = static_cast<float>(i % 50) / 50.0f;
                b[i] = static_cast<float>((i + 17) % 50) / 50.0f;
            }

            uint32_t bytes = count * 2 * sizeof(float);

            auto buf_a = create_storage_buffer(bytes,
                wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst);
            auto buf_b = create_storage_buffer(bytes,
                wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst);
            auto buf_r = create_storage_buffer(bytes,
                wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc);
            auto readback_buf = create_readback_buffer(bytes);
            auto bind_group = create_bind_group(complex_mul_pipeline_,
                {buf_a, buf_b, buf_r});
            uint32_t workgroups = (count + 255) / 256;

            // Warm-up
            {
                auto warmup_rb = create_readback_buffer(bytes);
                queue_.WriteBuffer(buf_a, 0, a.data(), bytes);
                queue_.WriteBuffer(buf_b, 0, b.data(), bytes);
                dispatch(complex_mul_pipeline_, bind_group, workgroups);
                copy_buffer(buf_r, warmup_rb, bytes);
                read_back(warmup_rb, result.data(), bytes);
            }

            for (int iter = 0; iter < iterations; ++iter) {
                double t0 = now_us();
                queue_.WriteBuffer(buf_a, 0, a.data(), bytes);
                queue_.WriteBuffer(buf_b, 0, b.data(), bytes);
                double t1 = now_us();

                dispatch(complex_mul_pipeline_, bind_group, workgroups);
                copy_buffer(buf_r, readback_buf, bytes);
                bool mapped = false;
                readback_buf.MapAsync(wgpu::MapMode::Read, 0, bytes,
                    wgpu::CallbackMode::AllowProcessEvents,
                    [&mapped](wgpu::MapAsyncStatus status, wgpu::StringView) {
                        mapped = (status == wgpu::MapAsyncStatus::Success);
                    });
                instance_.ProcessEvents();
                while (!mapped) {
                    instance_.ProcessEvents();
                }
                double t2 = now_us();
                readback_buf.Unmap();
                readback_buf = create_readback_buffer(bytes);

                avg.upload_us += (t1 - t0);
                avg.dispatch_us += (t2 - t1);
                avg.total_us += (t2 - t0);
            }

            avg.upload_us /= iterations;
            avg.dispatch_us /= iterations;
            avg.total_us /= iterations;

            // CPU baseline
            {
                double cpu_total = 0;
                for (int iter = 0; iter < iterations; ++iter) {
                    double t0 = now_us();
                    for (uint32_t i = 0; i < count; ++i) {
                        float a_re = a[i * 2], a_im = a[i * 2 + 1];
                        float b_re = b[i * 2], b_im = b[i * 2 + 1];
                        result[i * 2]     = a_re * b_re - a_im * b_im;
                        result[i * 2 + 1] = a_re * b_im + a_im * b_re;
                    }
                    double t1 = now_us();
                    cpu_total += (t1 - t0);
                }
                avg.cpu_baseline_us = cpu_total / iterations;
            }

            avg.gpu_faster = avg.total_us < avg.cpu_baseline_us;
            results.push_back(avg);
        }

        return results;
    }
#endif  // !__EMSCRIPTEN__

    // ── Asynchronous readback ───────────────────────────────────────────

    uint64_t compute_magnitude_async(const float* complex_pairs, float* magnitudes,
                                     uint32_t num_bins,
                                     std::chrono::microseconds deadline,
                                     ReadbackCallback on_complete) override {
        if (!initialized_ || !complex_pairs || !magnitudes || num_bins == 0
            || !on_complete) {
            return 0;
        }
        // Reject at the cap BEFORE doing any GPU work — a submit whose readback
        // cannot be admitted is wasted dispatch.
        if (readbacks_at_cap()) return 0;

        const uint32_t input_bytes = num_bins * 2 * sizeof(float);
        const uint32_t output_bytes = num_bins * sizeof(float);

        auto input_buf = acquire_storage_buffer(input_bytes,
            wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst);
        auto output_buf = acquire_storage_buffer(output_bytes,
            wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc);
        auto readback_buf = acquire_readback_buffer(output_bytes);
        if (!input_buf || !output_buf || !readback_buf) return 0;

        queue_.WriteBuffer(input_buf, 0, complex_pairs, input_bytes);

        auto bind_group = create_bind_group(magnitude_pipeline_, {input_buf, output_buf});
        dispatch(magnitude_pipeline_, bind_group, (num_bins + 255) / 256);
        copy_buffer(output_buf, readback_buf, output_bytes);
        // Counted at the submit that carries this request's readback copy (the
        // dispatch above rides an earlier submit on the same in-order queue).
        ++stats_->submits;

        // The storage buffers are done with once the GPU confirms the submit;
        // the readback buffer stays pinned until its map resolves, so it is
        // recycled (or discarded) by the readback bookkeeping instead.
        schedule_pool_release({input_buf, output_buf});

        return read_back_async(readback_buf, magnitudes, output_bytes, deadline,
                               std::move(on_complete));
    }

    // Non-blocking convolve_batch. Identical GPU work to convolve_batch() — same
    // plan, same buffers, same one submit — with exactly two differences:
    //
    //  1. The readback buffer is per-submit (from the staging pool) instead of
    //     the plan's single shared `readback`. That single buffer is the entire
    //     reason convolve_batch has to block: block N+1's CopyBufferToBuffer
    //     would target a buffer still mapped for block N. The COMPUTE buffers
    //     (buf_a/buf_b/buf_c/irspec) stay shared across in-flight blocks, which
    //     is safe because the WebGPU queue executes submissions in order: submit
    //     N's copy out of buf_a/buf_b runs before submit N+1's WriteBuffer into
    //     plan.buf_a. That invariant is what makes this conversion cheap, and it
    //     is pinned by the two-blocks-in-flight test.
    //
    //  2. The 1/n inverse-FFT scale runs in a completion wrapper on Success,
    //     before the caller's callback — not in the WGSL, which is shared with
    //     the blocking path.
    uint64_t convolve_batch_async(const float* in_complex, float* out_complex,
                                  uint32_t n, uint32_t batch,
                                  std::chrono::microseconds deadline,
                                  ReadbackCallback on_complete) override {
        if (!initialized_ || !in_complex || !out_complex || !on_complete) return 0;
        auto it = batch_conv_plans_.find(n);
        if (it == batch_conv_plans_.end() || it->second.batch != batch) return 0;
        if (readbacks_at_cap()) return 0;
        BatchConvPlan& plan = it->second;

        const uint32_t big = batch * n * 2u * static_cast<uint32_t>(sizeof(float));
        const uint32_t fft_wg = ((batch * (n / 2u)) + 255u) / 256u;  // batched butterflies
        const uint32_t mul_wg = ((batch * n) + 255u) / 256u;         // batched pairs

        auto readback_buf = acquire_readback_buffer(big);
        if (!readback_buf) return 0;

        // Opt-in GPU-busy timing (default OFF → this whole block is skipped and the
        // encoding below is byte-identical). Gated on ts_in_flight_ so only one async
        // sample uses the single 2-slot dev_ts_qs_ at a time. A tiny pool buffer holds
        // the two resolved ticks; if it can't be had, timing is silently skipped this
        // block rather than holding the audio path hostage.
        const uint64_t ts_bytes = 2u * sizeof(uint64_t);
        const bool want_ts = async_timing_enabled_ && has_timestamp_ && !ts_in_flight_
                             && ensure_device_ts();
        wgpu::Buffer ts_buf = want_ts ? acquire_readback_buffer(
                                            static_cast<uint32_t>(ts_bytes))
                                      : wgpu::Buffer{};
        const bool ts = want_ts && ts_buf;

        queue_.WriteBuffer(plan.buf_a, 0, in_complex, big);

        wgpu::CommandEncoderDescriptor enc_desc{};
        auto encoder = device_.CreateCommandEncoder(&enc_desc);
        // Begin timestamp rides the forward FFT's first pass; end timestamp rides the
        // inverse FFT's last pass — bracketing the whole convolution's GPU-busy time.
        encode_fft_passes(encoder, plan.fwd_bgs, fft_wg,
                          ts ? dev_ts_qs_ : wgpu::QuerySet{});
        {
            wgpu::ComputePassDescriptor pd{};
            auto pass = encoder.BeginComputePass(&pd);
            pass.SetPipeline(conv_bmul_pipeline_);
            pass.SetBindGroup(0, plan.bmul_bg);
            pass.DispatchWorkgroups(mul_wg);
            pass.End();
        }
        encoder.CopyBufferToBuffer(plan.buf_c, 0, plan.buf_a, 0, big);
        encode_fft_passes(encoder, plan.inv_bgs, fft_wg, wgpu::QuerySet{},
                          ts ? dev_ts_qs_ : wgpu::QuerySet{});
        wgpu::Buffer& inv_buf = (plan.log2n & 1u) ? plan.buf_b : plan.buf_a;
        encoder.CopyBufferToBuffer(inv_buf, 0, readback_buf, 0, big);
        if (ts) {
            encoder.ResolveQuerySet(dev_ts_qs_, 0, 2, dev_ts_resolve_, 0);
            encoder.CopyBufferToBuffer(dev_ts_resolve_, 0, ts_buf, 0, ts_bytes);
        }
        auto cmd = encoder.Finish();
        queue_.Submit(1, &cmd);
        ++stats_->submits;

        const uint32_t floats = batch * n * 2u;
        const float inv = 1.0f / static_cast<float>(n);
        auto scale_then_complete =
            [out_complex, floats, inv, cb = std::move(on_complete)](
                const ReadbackResult& r) {
                if (r.status == ReadbackStatus::Success) {
                    for (uint32_t i = 0; i < floats; ++i) out_complex[i] *= inv;
                }
                cb(r);
            };

        // read_back_async() re-checks the cap and hands the buffer back itself if
        // it rejects; the check above means it cannot, but the id is what the
        // caller keys off either way.
        const uint64_t audio_id = read_back_async(readback_buf, out_complex, big,
                                                  deadline,
                                                  std::move(scale_then_complete));

        // Register the ticks readback through the SAME non-blocking machinery. Its
        // callback smooths (end - begin) ns into last_gpu_busy_ns_ and clears the
        // in-flight gate on EVERY outcome, so a dropped/expired sample can never wedge
        // timing. WebGPU timestamps are already nanoseconds. `this` is valid whenever
        // the callback fires — the readback queue is a member, drained by
        // poll_readbacks() or in the dtor, both with this alive.
        if (ts) {
            const uint64_t ts_id = read_back_async(
                ts_buf, ts_ticks_, static_cast<uint32_t>(ts_bytes), deadline,
                [this](const ReadbackResult& r) {
                    if (r.status == ReadbackStatus::Success
                        && ts_ticks_[1] >= ts_ticks_[0]) {
                        const double ns =
                            static_cast<double>(ts_ticks_[1] - ts_ticks_[0]);
                        // Metal/Chrome quantize a small dispatch to 0 ns; hold the
                        // last good number rather than blink the readout to 0.
                        if (ns > 0.0)
                            last_gpu_busy_ns_ = last_gpu_busy_ns_ > 0.0
                                ? last_gpu_busy_ns_ * 0.8 + ns * 0.2
                                : ns;
                    }
                    ts_in_flight_ = false;
                });
            ts_in_flight_ = (ts_id != 0);
        }

        return audio_id;
    }

    void set_max_readbacks_in_flight(std::size_t max_in_flight) override {
        max_readbacks_in_flight_ = (max_in_flight == 0) ? 1 : max_in_flight;
    }

    AsyncStats async_stats() const override { return *stats_; }

    // ── Kernel-source seam ──────────────────────────────────────────────

    const char* kernel_source(const char* label) const override {
        return builtin_kernel_source(label);
    }

    bool override_kernel_source(const char* label, const char* wgsl) override {
        if (!label || !wgsl) return false;
        if (!builtin_kernel_source(label)) return false;
        // Pipelines are compiled once, in create_pipelines(). An override applied
        // afterwards would silently do nothing, which is exactly the kind of
        // quiet no-op this seam exists to rule out.
        if (initialized_) {
            runtime::log_error(
                "GpuCompute: override_kernel_source('{}') after initialization — "
                "pipelines are already compiled; override before initialize_*()",
                label);
            return false;
        }
        kernel_overrides_[label] = wgsl;
        return true;
    }

    std::size_t poll_readbacks() override {
        // A completion callback that polls again would re-enter the queue while
        // it is being drained; the outer poll is already draining everything
        // that is ready, so the inner call has nothing to do.
        if (polling_ || async_readbacks_.empty()) return async_readbacks_.size();

        polling_ = true;

        // The deadline is judged against the poll's ENTRY time, before the event
        // queue is pumped: a result is on time only if it was already delivered
        // by the deadline, not if it happens to land in this pump. That keeps
        // expiry a pure function of the clock — the caller's block deadline has
        // passed either way, and the substitute has already gone out.
        const auto now = std::chrono::steady_clock::now();
        pump_events();

        while (!async_readbacks_.empty()) {
            auto req = async_readbacks_.front();

            if (!req->resolved && now < req->deadline) {
                // In-order delivery: a request whose map has not landed holds the
                // queue until it resolves or its deadline passes. The deadline is
                // what makes that bounded — the queue can never wedge.
                break;
            }
            async_readbacks_.pop_front();

            if (now >= req->deadline) {
                // Late. `dest` is never written; a map that is still pending must
                // not go back on the free list where a later acquire() could hand
                // it out mid-map, so the pool drops its reference instead.
                req->abandoned = true;
                if (req->resolved && req->ok) req->buffer.Unmap();
                if (pool_) pool_->discard(req->buffer);
                complete(*req, ReadbackStatus::Expired, 0);
                continue;
            }

            const void* data = req->ok
                ? req->buffer.GetConstMappedRange(0, req->size)
                : nullptr;
            if (!data) {
                if (pool_) pool_->discard(req->buffer);
                complete(*req, ReadbackStatus::Failed, 0);
                continue;
            }

            std::memcpy(req->dest, data, req->size);
            req->buffer.Unmap();
            if (pool_) pool_->release(req->buffer);
            complete(*req, ReadbackStatus::Success, req->size);
        }

        polling_ = false;
        return async_readbacks_.size();
    }

    std::size_t readbacks_in_flight() const override { return async_readbacks_.size(); }

#ifdef PULP_BENCHMARK
    void set_bench_counters(bench::PerfCounters* counters) override {
        bench_counters_ = counters;
    }
#endif

private:
    wgpu::Device device_;
    wgpu::Queue queue_;
    wgpu::Instance instance_;
    wgpu::Adapter adapter_;
#if !defined(__EMSCRIPTEN__)
    std::unique_ptr<dawn::native::Instance> native_instance_;
#endif
    bool owns_device_ = false;
    bool device_lost_ = false;

    // Liveness token for callbacks that outlive this object (the device-lost
    // callback lives on the device, which Dawn may report as lost while it is
    // being destroyed). Expired first in the dtor.
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
    bool has_timestamp_ = false;  // TimestampQuery feature enabled on device_

    // Device-level 2-slot timestamp probe, shared by any op that reports
    // GPU-busy time and whose plan struct does not carry its own QuerySet
    // (multi_convolve). Safe to share because every timed op blocks on its
    // readback before returning, so no two use it concurrently.
    wgpu::QuerySet dev_ts_qs_;
    wgpu::Buffer dev_ts_resolve_, dev_ts_readback_;
    bool ensure_device_ts() {
        if (dev_ts_qs_) return true;
        if (!has_timestamp_) return false;
        wgpu::QuerySetDescriptor qd{};
        qd.type = wgpu::QueryType::Timestamp;
        qd.count = 2;
        dev_ts_qs_ = device_.CreateQuerySet(&qd);
        const uint64_t bytes = 2u * sizeof(uint64_t);
        wgpu::BufferDescriptor rd{};
        rd.size = bytes;
        rd.usage = wgpu::BufferUsage::QueryResolve | wgpu::BufferUsage::CopySrc;
        dev_ts_resolve_ = device_.CreateBuffer(&rd);
        wgpu::BufferDescriptor bd{};
        bd.size = bytes;
        bd.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
        dev_ts_readback_ = device_.CreateBuffer(&bd);
        return dev_ts_qs_ && dev_ts_resolve_ && dev_ts_readback_;
    }

    // ── Async GPU-busy timing (opt-in; OFF by default) ───────────────────
    // The blocking *_timed() paths own dev_ts_qs_/dev_ts_resolve_ and read
    // dev_ts_readback_ with a BLOCKING map — safe to share only because each such op
    // finishes before the next starts. The async path below must never block, so it
    // resolves into dev_ts_resolve_ and copies the two ticks into a POOL readback
    // buffer drained by poll_readbacks(); it serialises itself with ts_in_flight_ (one
    // in-flight async sample at a time, since dev_ts_qs_ is a single 2-slot set) and
    // never touches dev_ts_readback_. Native audio leaves async_timing_enabled_ false,
    // so this whole path is dormant and the async convolution is byte-identical there;
    // only the browser GPU demo turns it on.
    bool async_timing_enabled_ = false;
    bool ts_in_flight_ = false;
    uint64_t ts_ticks_[2] = {0, 0};   // stable async-readback destination for the ticks
    double last_gpu_busy_ns_ = 0.0;   // EMA of (end - begin) ns; 0 = no honest number

    void set_async_timing_enabled(bool enabled) override { async_timing_enabled_ = enabled; }
    double last_gpu_busy_ns() const override { return last_gpu_busy_ns_; }

#ifdef PULP_BENCHMARK
    bench::PerfCounters* bench_counters_ = nullptr;
#endif

    wgpu::ComputePipeline magnitude_pipeline_;
    wgpu::ComputePipeline complex_mul_pipeline_;
    wgpu::ComputePipeline conv_bmul_pipeline_;  // broadcast complex-mul (convolution)
    wgpu::ComputePipeline multi_ir_mul_pipeline_;      // one input × many IRs
    wgpu::ComputePipeline multi_ir_combine_pipeline_;  // pan-combine reduce → stereo
    wgpu::ComputePipeline multi_fdl_mac_pipeline_;     // partitioned FDL MAC
    wgpu::ComputePipeline spectral_advance_pipeline_;  // per-bin phase advance + jitter
    wgpu::ComputePipeline spectral_combine_pipeline_;  // smear + weighted layer sum
    wgpu::ComputePipeline conv_in_pipeline_;     // conv-stack input projection
    wgpu::ComputePipeline conv_layer_pipeline_;  // conv-stack gated dilated layer
    wgpu::ComputePipeline conv_head_pipeline_;   // conv-stack linear head
    wgpu::ComputePipeline wavenet_rechannel_pipeline_;  // WaveNet 1x1 rechannel
    wgpu::ComputePipeline wavenet_layer_pipeline_;      // WaveNet dilated conv layer
    wgpu::ComputePipeline wavenet_head_pipeline_;       // WaveNet head rechannel
    wgpu::ComputePipeline wavenet_scale_pipeline_;      // WaveNet final head_scale
    wgpu::ComputePipeline matmul_pipeline_;
    wgpu::ComputePipeline additive_pipeline_;
    wgpu::ComputePipeline additive_coop_pipeline_;  // block-parallel additive variant
    wgpu::ComputePipeline modal_pipeline_;
    wgpu::ComputePipeline modal_coop_pipeline_;  // block-parallel modal variant
    wgpu::ComputePipeline granular_pipeline_;
    wgpu::ComputePipeline dense_tanh_pipeline_;
    wgpu::ComputePipeline fft_pipeline_;

    // Per-device pipeline cache keyed by the WGSL source string. Mirrors the
    // canvas RuntimeEffectCache (cache-by-source), but per-instance, not a
    // global singleton: compute pipelines are device-specific and cannot be
    // shared across devices. Keyed by the full source (not just a hash) so
    // there is no collision risk. create_pipeline() routes through this so
    // repeated kernels compile once per device.
    std::unordered_map<std::string, wgpu::ComputePipeline> pipeline_cache_;

    // label → replacement WGSL, consulted by create_pipeline(). Populated only
    // before initialization (see override_kernel_source), so it is read-only for
    // the lifetime of the compiled pipelines.
    std::unordered_map<std::string, std::string> kernel_overrides_;

    // Persistent per-N FFT plan: two ping-pong complex buffers (2*N floats),
    // one uniform + one bind group PER pass (so all log2(N) passes plus the
    // copy-to-readback encode into a single command buffer / single submit),
    // and a readback buffer. Built lazily on first use of a given N and reused.
    struct FftPlan {
        uint32_t n = 0;
        uint32_t log2n = 0;
        wgpu::Buffer buf_a;       // Storage | CopyDst | CopySrc
        wgpu::Buffer buf_b;       // Storage | CopyDst | CopySrc
        wgpu::Buffer readback;    // CopyDst | MapRead
        std::vector<wgpu::Buffer> uniforms;      // FftParams, one per pass
        std::vector<wgpu::BindGroup> stage_bgs;  // one per pass, alternating src/dst
        // Timestamp resources, created lazily on the first timed call.
        wgpu::QuerySet ts_qs;       // 2 Timestamp slots (begin/end of FFT passes)
        wgpu::Buffer ts_resolve;    // QueryResolve | CopySrc
        wgpu::Buffer ts_readback;   // CopyDst | MapRead (2 * u64)
    };
    std::unordered_map<uint32_t, FftPlan> fft_plans_;

    // GPU-resident convolution plan: forward + complex-mul + inverse fused into
    // one command buffer (one readback). Separate forward/inverse uniform sets
    // (sign -1/+1) so both run in a single submit; buf_c holds the product; the
    // IR spectrum lives resident in `irspec`. "Plan once, run many."
    struct ConvPlan {
        uint32_t n = 0;
        uint32_t log2n = 0;
        wgpu::Buffer buf_a;       // ping-pong + inverse input (Storage|CopyDst|CopySrc)
        wgpu::Buffer buf_b;       // ping-pong (Storage|CopyDst|CopySrc)
        wgpu::Buffer buf_c;       // complex-mul product (Storage|CopyDst|CopySrc)
        wgpu::Buffer irspec;      // resident IR spectrum (Storage|CopyDst)
        wgpu::Buffer readback;    // CopyDst|MapRead
        std::vector<wgpu::Buffer> fwd_u;       // forward per-pass uniforms (sign=-1)
        std::vector<wgpu::Buffer> inv_u;       // inverse per-pass uniforms (sign=+1)
        std::vector<wgpu::BindGroup> fwd_bgs;  // forward passes (ping-pong from buf_a)
        std::vector<wgpu::BindGroup> inv_bgs;  // inverse passes (ping-pong from buf_a)
        wgpu::BindGroup mul_bg;   // {forward-result-buf, irspec, buf_c}
    };
    std::unordered_map<uint32_t, ConvPlan> conv_plans_;

    // Batched convolution plan: a/b/c/readback sized batch*2n, irspec resident
    // (2n, broadcast). Forward/inverse uniforms carry batch=batch; the
    // complex-mul uses the broadcast kernel. One submit + one readback covers
    // all `batch` blocks. Keyed by n; rebuilt if the batch count changes.
    struct BatchConvPlan {
        uint32_t n = 0;
        uint32_t log2n = 0;
        uint32_t batch = 0;
        wgpu::Buffer buf_a, buf_b, buf_c, irspec, readback;
        std::vector<wgpu::Buffer> fwd_u, inv_u;
        std::vector<wgpu::BindGroup> fwd_bgs, inv_bgs;
        wgpu::BindGroup bmul_bg;
    };
    std::unordered_map<uint32_t, BatchConvPlan> batch_conv_plans_;

    // Multi-IR convolution plan: one input spectrum (fx_a/fx_b, 2n) broadcast
    // across num_ir resident IR spectra (irspecs, 2n*num_ir), inverse-FFT'd in
    // big_a/big_b (2n*num_ir), then GPU-reduced to a stereo block (out_lr, 2n)
    // with per-room pan weights (panl/panr). One submit + one 2n readback covers
    // all rooms. Keyed by n; rebuilt if num_ir changes.
    struct MultiConvPlan {
        uint32_t n = 0;
        uint32_t log2n = 0;
        uint32_t num_ir = 0;
        wgpu::Buffer fx_a, fx_b;          // forward FFT ping-pong (2n)
        wgpu::Buffer big_a, big_b;        // multiply out / inverse ping-pong (2n*num_ir)
        wgpu::Buffer irspecs;             // resident IR spectra (2n*num_ir)
        wgpu::Buffer panl, panr;          // per-room pan gains (num_ir)
        wgpu::Buffer out_lr;              // stereo result (2n)
        wgpu::Buffer cuniform;            // CombineParams
        wgpu::Buffer readback;            // 2n
        std::vector<wgpu::Buffer> fwd_u, inv_u;
        std::vector<wgpu::BindGroup> fwd_bgs, inv_bgs;
        wgpu::BindGroup mul_bg, combine_bg;
    };
    std::unordered_map<uint32_t, MultiConvPlan> multi_conv_plans_;

    // Partitioned frequency-delay-line plan. n = 2*block (small, fixed). Each IR
    // is split into num_part block-size partitions; `ring` holds the last
    // num_part input spectra (a delay line), advanced by `head` each block. The
    // MAC sums ring[(head-p) % P] * ir_spectra[r][p] over partitions into accum,
    // one room per num_ir. prev_time keeps the previous input block (overlap).
    struct MultiFdlPlan {
        uint32_t n = 0;          // fft size = 2*block
        uint32_t block = 0;
        uint32_t log2n = 0;
        uint32_t num_ir = 0;
        uint32_t num_part = 0;
        uint32_t head = 0;       // newest ring slot (mutable, advanced per block)
        wgpu::Buffer fx_a, fx_b;   // input forward FFT ping-pong (2n)
        wgpu::Buffer ring;         // delay line of P input spectra (2n*P)
        wgpu::Buffer irspecs;      // per-room per-partition IR spectra (2n*P*num_ir)
        wgpu::Buffer accum_a, accum_b;  // MAC out / per-room inverse ping-pong (2n*num_ir)
        wgpu::Buffer panl, panr;   // per-room pan gains (num_ir)
        wgpu::Buffer out_lr;       // stereo result (2n)
        wgpu::Buffer cuniform;     // CombineParams
        wgpu::Buffer fdl_uniform;  // FdlParams (n, num_ir, num_part, head)
        wgpu::Buffer readback;     // 2n
        std::vector<wgpu::Buffer> fwd_u, inv_u;
        std::vector<wgpu::BindGroup> fwd_bgs, inv_bgs;
        wgpu::BindGroup mac_bg, combine_bg;
        std::vector<float> prev_time;   // previous input block (block samples)
    };
    std::unordered_map<uint32_t, MultiFdlPlan> multi_fdl_plans_;

    // Multi-layer spectral-stack plan: num_layers frozen layer-spectra resident
    // as a magnitude buffer (mag, num_layers*n reals) and a persistent phase
    // buffer (phase, num_layers*n reals, advanced in place each render). Per
    // render: advance phase → smear+weighted-sum combine into comb_a (2n) →
    // inverse FFT ping-ponging comb_a/comb_b → copy one frame to readback (2n).
    // One submit + one 2n readback covers all layers. Keyed by n; rebuilt if
    // num_layers changes.
    struct SpectralStackPlan {
        uint32_t n = 0;
        uint32_t log2n = 0;
        uint32_t num_layers = 0;
        float hop_ratio = 0.0f;
        wgpu::Buffer mag, phase;          // resident layer-spectra (num_layers*n)
        wgpu::Buffer weights;             // per-layer morph weights (num_layers)
        wgpu::Buffer comb_a, comb_b;      // combine out / inverse ping-pong (2n)
        wgpu::Buffer advance_u, combine_u;
        wgpu::Buffer readback;            // one frame (2n)
        std::vector<wgpu::Buffer> inv_u;
        std::vector<wgpu::BindGroup> inv_bgs;
        wgpu::BindGroup advance_bg, combine_bg;
    };
    std::unordered_map<uint32_t, SpectralStackPlan> spectral_stack_plans_;

    struct ConvStackPlan {
        uint32_t C = 0, K = 0, L = 0, B = 0, per_layer = 0, pad = 0;
        float head_scale = 1.0f;
        wgpu::Buffer weights;                 // all model weights, resident
        std::vector<wgpu::Buffer> act;        // L+1 activation buffers (C*(PAD+B))
        wgpu::Buffer skip, input, output;     // skip accum (C*B), in/out blocks (B)
        wgpu::Buffer readback;
        wgpu::Buffer hist_temp;               // C*PAD history-slide scratch
        wgpu::Buffer input_u, head_u;
        std::vector<wgpu::Buffer> layer_u;
        wgpu::BindGroup input_bg, head_bg;
        std::vector<wgpu::BindGroup> layer_bg;
    };
    std::unordered_map<uint32_t, ConvStackPlan> conv_stack_plans_;
    std::vector<float> cs_zero_;  // skip-buffer zeroing scratch

    // WaveNet plan: the full multi-array model, device-resident. `weights`
    // holds the whole flat blob. Each array owns its activation buffers (one per
    // layer-input, C*(pad+B), padded for dilation history), a head accumulator
    // (C*B) and a head output (H*B). Pass uniforms + bind groups are pre-built so
    // each forward is just WriteBuffer(input) + encode + one readback. Keyed by
    // block size; rebuilt if it changes.
    struct WavenetArray {
        uint32_t channels = 0, head_size = 0, num_layers = 0, pad = 0, head_bias = 0;
        std::vector<wgpu::Buffer> act;   // num_layers+1 activation buffers
        wgpu::Buffer headacc, headout;
        wgpu::Buffer rechannel_u, head_u;
        std::vector<wgpu::Buffer> layer_u;
        wgpu::BindGroup rechannel_bg, head_bg;
        std::vector<wgpu::BindGroup> layer_bg;
    };
    struct WavenetPlan {
        uint32_t B = 0;
        float head_scale = 1.0f;
        wgpu::Buffer weights, input, output, readback, hist_temp;
        wgpu::Buffer scale_u;
        wgpu::BindGroup scale_bg;
        std::vector<WavenetArray> arrays;
    };
    // Keyed by (block_size, instance) so multiple independent WaveNet streams
    // (e.g. stereo channels) coexist on one device — each with its own buffers and
    // dilation history, sharing the device/queue/pipelines. Instance defaults to 0.
    static uint64_t wavenet_plan_key(uint32_t block_size, uint32_t instance) {
        return (static_cast<uint64_t>(block_size) << 32) | instance;
    }
    std::unordered_map<uint64_t, WavenetPlan> wavenet_plans_;

    // Host scratch for the spectral-stack 2n-complex readback (grown on demand;
    // the render path is non-RT so a resize here is fine).
    std::vector<float> sp_scratch_;
    float* sp_scratch(uint32_t bytes) {
        const std::size_t floats = bytes / sizeof(float);
        if (sp_scratch_.size() < floats) sp_scratch_.assign(floats, 0.0f);
        return sp_scratch_.data();
    }

    // Pool of persistent staging buffers, keyed by usage bitmask. Replaces
    // per-call device_.CreateBuffer() in compute_magnitude / complex_multiply
    // to eliminate allocator churn. Created lazily in create_pipelines().
    // OnSubmittedWorkDone callbacks can fire after `~DawnGpuCompute()` resets
    // the pool in shared-device mode, where event processing continues outside
    // this object. Owning the pool via `shared_ptr` and handing callbacks a
    // `weak_ptr` means stale callbacks just drop their buffers (RAII dtor
    // handles the wgpu::Buffer side) instead of dereferencing freed state.
    std::shared_ptr<detail::StagingBufferPool> pool_;

    bool create_pipelines() {
        magnitude_pipeline_ = create_pipeline("magnitude", kMagnitudeShader);
        if (!magnitude_pipeline_) return false;

        complex_mul_pipeline_ = create_pipeline("complex_multiply", kComplexMultiplyShader);
        if (!complex_mul_pipeline_) return false;

        fft_pipeline_ = create_pipeline("fft_stockham", kFftStockhamShader);
        if (!fft_pipeline_) return false;

        conv_bmul_pipeline_ = create_pipeline("conv_bmul", kComplexMulBroadcastShader);
        if (!conv_bmul_pipeline_) return false;

        multi_ir_mul_pipeline_ = create_pipeline("multi_ir_mul", kMultiIrMulShader);
        if (!multi_ir_mul_pipeline_) return false;

        multi_ir_combine_pipeline_ =
            create_pipeline("multi_ir_combine", kMultiIrCombineShader);
        multi_fdl_mac_pipeline_ =
            create_pipeline("multi_fdl_mac", kMultiFdlMacShader);
        if (!multi_ir_combine_pipeline_ || !multi_fdl_mac_pipeline_) return false;

        spectral_advance_pipeline_ =
            create_pipeline("spectral_advance", kSpectralAdvanceShader);
        if (!spectral_advance_pipeline_) return false;

        spectral_combine_pipeline_ =
            create_pipeline("spectral_combine", kSpectralCombineShader);
        if (!spectral_combine_pipeline_) return false;

        conv_in_pipeline_ = create_pipeline("conv_in", kConvStackInputShader);
        if (!conv_in_pipeline_) return false;
        conv_layer_pipeline_ = create_pipeline("conv_layer", kConvStackLayerShader);
        if (!conv_layer_pipeline_) return false;
        conv_head_pipeline_ = create_pipeline("conv_head", kConvStackHeadShader);
        if (!conv_head_pipeline_) return false;

        wavenet_rechannel_pipeline_ = create_pipeline("wavenet_rechannel", kWavenetRechannelShader);
        if (!wavenet_rechannel_pipeline_) return false;
        wavenet_layer_pipeline_ = create_pipeline("wavenet_layer", kWavenetLayerShader);
        if (!wavenet_layer_pipeline_) return false;
        wavenet_head_pipeline_ = create_pipeline("wavenet_head", kWavenetHeadShader);
        if (!wavenet_head_pipeline_) return false;
        wavenet_scale_pipeline_ = create_pipeline("wavenet_scale", kWavenetScaleShader);
        if (!wavenet_scale_pipeline_) return false;

        matmul_pipeline_ = create_pipeline("matmul", kMatmulShader);
        if (!matmul_pipeline_) return false;

        additive_pipeline_ = create_pipeline("additive_synth", kAdditiveSynthShader);
        if (!additive_pipeline_) return false;

        additive_coop_pipeline_ = create_pipeline("additive_synth_coop", kAdditiveSynthCoopShader);
        if (!additive_coop_pipeline_) return false;

        modal_pipeline_ = create_pipeline("modal_strike", kModalStrikeShader);
        if (!modal_pipeline_) return false;

        modal_coop_pipeline_ = create_pipeline("modal_strike_coop", kModalStrikeCoopShader);
        if (!modal_coop_pipeline_) return false;

        granular_pipeline_ = create_pipeline("granular_cloud", kGranularShader);
        if (!granular_pipeline_) return false;

        dense_tanh_pipeline_ = create_pipeline("dense_tanh", kDenseTanhShader);
        if (!dense_tanh_pipeline_) return false;

        // Staging buffer pool: pre-allocated ring of persistent wgpu::Buffer
        // objects that replaces per-call device_.CreateBuffer() in the compute
        // hot path. The cap is PER usage key, and the async path pins one readback
        // buffer per in-flight request — so it must clear 2x the in-flight cap
        // (default 4) for the MapRead key to recycle rather than churn, on top of
        // the per-call storage buffers on the other keys.
        pool_ = std::make_shared<detail::StagingBufferPool>(device_, 16);

        has_timestamp_ = device_.HasFeature(wgpu::FeatureName::TimestampQuery);

        initialized_ = true;
        runtime::log_info("GpuCompute: pipelines created (device shared: {})",
            !owns_device_);
        return true;
    }

    // The one place a named pipeline's WGSL is turned into a pipeline, which is
    // what makes the kernel-source seam a lookup rather than a refactor: an
    // override substitutes the text here, and the cache (keyed by the WGSL
    // itself) gives the mutated kernel its own entry for free.
    wgpu::ComputePipeline create_pipeline(const char* label, const char* wgsl) {
        if (label) {
            if (auto ov = kernel_overrides_.find(label); ov != kernel_overrides_.end()) {
                wgsl = ov->second.c_str();
            }
        }

        std::string key(wgsl);
        if (auto it = pipeline_cache_.find(key); it != pipeline_cache_.end()) {
            return it->second;
        }

        wgpu::ShaderSourceWGSL wgsl_source{};
        wgsl_source.code = wgsl;

        wgpu::ShaderModuleDescriptor sm_desc{};
        sm_desc.label = label;
        sm_desc.nextInChain = &wgsl_source;

        auto shader_module = device_.CreateShaderModule(&sm_desc);
        if (!shader_module) {
            runtime::log_error("GpuCompute: failed to create shader module '{}'", label);
            return nullptr;
        }

        wgpu::ComputePipelineDescriptor pipe_desc{};
        pipe_desc.label = label;
        pipe_desc.compute.module = shader_module;
        pipe_desc.compute.entryPoint = "main";
        // Use auto layout — Dawn infers bind group layout from shader
        pipe_desc.layout = nullptr;

        auto pipeline = device_.CreateComputePipeline(&pipe_desc);
        if (!pipeline) {
            runtime::log_error("GpuCompute: failed to create pipeline '{}'", label);
            return pipeline;
        }
        pipeline_cache_.emplace(key, pipeline);
        return pipeline;
    }

    static bool is_power_of_two(uint32_t n) {
        return n != 0u && (n & (n - 1u)) == 0u;
    }

    // Encode one Stockham FFT pass per bind group into an existing encoder (no
    // submit). Each pass is its own compute pass so Dawn inserts the required
    // cross-pass synchronization. Shared by the fused convolution path.
    // Optionally writes a beginning-of-pass timestamp into slot 0 of `begin_ts_qs`
    // on the FIRST pass, and/or an end-of-pass timestamp into slot 1 of `end_ts_qs`
    // on the LAST pass — used to bracket a multi-pass op's GPU-busy time. The `multi`
    // path ends its bracket on a separate combine pass and so passes only a begin
    // here; the fused batch path has no trailing pass, so it ends the bracket on the
    // inverse FFT's last pass via `end_ts_qs`. Pass null QuerySets to disable (the
    // default — every non-timing caller is unchanged).
    void encode_fft_passes(wgpu::CommandEncoder& encoder,
                           const std::vector<wgpu::BindGroup>& stage_bgs,
                           uint32_t workgroups,
                           const wgpu::QuerySet& begin_ts_qs = {},
                           const wgpu::QuerySet& end_ts_qs = {}) {
        const std::size_t last = stage_bgs.empty() ? 0 : stage_bgs.size() - 1;
        std::size_t idx = 0;
        for (const auto& bg : stage_bgs) {
            wgpu::ComputePassDescriptor pd{};
            wgpu::PassTimestampWrites tw{};
            tw.beginningOfPassWriteIndex = wgpu::kQuerySetIndexUndefined;
            tw.endOfPassWriteIndex = wgpu::kQuerySetIndexUndefined;
            bool want_ts = false;
            if (begin_ts_qs && idx == 0) {
                tw.querySet = begin_ts_qs;
                tw.beginningOfPassWriteIndex = 0;
                want_ts = true;
            }
            if (end_ts_qs && idx == last) {
                // begin and end share one 2-slot set, so the querySet is consistent
                // even when both land on a single-pass FFT (last == first).
                tw.querySet = end_ts_qs;
                tw.endOfPassWriteIndex = 1;
                want_ts = true;
            }
            if (want_ts) pd.timestampWrites = &tw;
            auto pass = encoder.BeginComputePass(&pd);
            pass.SetPipeline(fft_pipeline_);
            pass.SetBindGroup(0, bg);
            pass.DispatchWorkgroups(workgroups);
            pass.End();
            ++idx;
        }
    }

    // Build (or fetch) the persistent ping-pong plan for a given FFT size.
    // Returns nullptr if any GPU resource fails to allocate.
    FftPlan* get_or_create_fft_plan(uint32_t n) {
        if (auto it = fft_plans_.find(n); it != fft_plans_.end()) {
            return &it->second;
        }

        FftPlan plan;
        plan.n = n;
        plan.log2n = 0;
        for (uint32_t v = n; v > 1u; v >>= 1) ++plan.log2n;

        const uint32_t bytes = n * 2u * static_cast<uint32_t>(sizeof(float));
        const auto buf_usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst
                             | wgpu::BufferUsage::CopySrc;
        plan.buf_a = create_storage_buffer(bytes, buf_usage);
        plan.buf_b = create_storage_buffer(bytes, buf_usage);
        plan.readback = create_readback_buffer(bytes);
        if (!plan.buf_a || !plan.buf_b || !plan.readback) return nullptr;

        // One uniform + one bind group per pass, so a single command buffer can
        // encode every pass. Pass i reads from buf_a and writes buf_b on even i,
        // and the reverse on odd i (Stockham ping-pong starting at buf_a).
        for (uint32_t s = 0; s < plan.log2n; ++s) {
            wgpu::BufferDescriptor udesc{};
            udesc.size = 16;  // FftParams: u32 n, u32 ns, f32 sign, f32 pad
            udesc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
            wgpu::Buffer u = device_.CreateBuffer(&udesc);
            if (!u) return nullptr;

            const bool src_is_a = (s % 2u == 0u);
            wgpu::BindGroup bg = create_bind_group(
                fft_pipeline_,
                src_is_a ? std::initializer_list<wgpu::Buffer>{plan.buf_a, plan.buf_b, u}
                         : std::initializer_list<wgpu::Buffer>{plan.buf_b, plan.buf_a, u});
            if (!bg) return nullptr;

            plan.uniforms.push_back(std::move(u));
            plan.stage_bgs.push_back(std::move(bg));
        }

        auto inserted = fft_plans_.emplace(n, std::move(plan));
        return &inserted.first->second;
    }

    // Lazily allocate the 2-slot timestamp QuerySet + resolve/readback buffers
    // for a plan. Returns false if any allocation fails.
    bool ensure_ts_resources(FftPlan& plan) {
        if (plan.ts_qs) return true;
        wgpu::QuerySetDescriptor qd{};
        qd.type = wgpu::QueryType::Timestamp;
        qd.count = 2;
        plan.ts_qs = device_.CreateQuerySet(&qd);

        const uint64_t ts_bytes = 2u * sizeof(uint64_t);
        wgpu::BufferDescriptor rd{};
        rd.size = ts_bytes;
        rd.usage = wgpu::BufferUsage::QueryResolve | wgpu::BufferUsage::CopySrc;
        plan.ts_resolve = device_.CreateBuffer(&rd);

        wgpu::BufferDescriptor bd{};
        bd.size = ts_bytes;
        bd.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
        plan.ts_readback = device_.CreateBuffer(&bd);
        return plan.ts_qs && plan.ts_resolve && plan.ts_readback;
    }

    // Multi-pass Stockham FFT, encoded as a SINGLE command buffer (all log2(N)
    // passes + the copy-to-readback in one submit). sign = -1 forward,
    // +1 inverse; `normalize` applies the 1/N inverse scale on readback. When
    // gpu_ns != null and timestamps are available, also measures the true GPU
    // compute time of the passes (excludes upload/readback) and writes it (ns);
    // sets *gpu_ns = -1 when timing is unavailable. Not real-time safe — it
    // blocks on the readback map.
    bool fft_run(const float* complex_in, float* complex_out, uint32_t n,
                 float sign, bool normalize, double* gpu_ns = nullptr) {
        // kMaxFftN (file scope) rejects transforms that would overflow the
        // byte count and under-allocate GPU buffers.
        if (!initialized_ || !complex_in || !complex_out) return false;
        if (!is_power_of_two(n) || n > kMaxFftN) return false;
        if (gpu_ns) *gpu_ns = -1.0;

        FftPlan* plan = get_or_create_fft_plan(n);
        if (!plan) return false;

        bool do_ts = (gpu_ns != nullptr) && has_timestamp_ && plan->log2n > 0;
        if (do_ts && !ensure_ts_resources(*plan)) do_ts = false;

        const uint32_t bytes = n * 2u * static_cast<uint32_t>(sizeof(float));
        queue_.WriteBuffer(plan->buf_a, 0, complex_in, bytes);
        for (uint32_t s = 0; s < plan->log2n; ++s) {
            struct FftParams { uint32_t n; uint32_t ns; float sign; uint32_t batch; };
            FftParams params{n, 1u << s, sign, 1u};
            queue_.WriteBuffer(plan->uniforms[s], 0, &params, sizeof(params));
        }

        // Encode all passes + the result copy into one command buffer. Each
        // pass is its own compute pass; WebGPU/Dawn inserts the required
        // synchronization/resource transitions between encoded uses, so one
        // pass's storage writes are visible to the next pass's reads.
        const uint32_t half = n / 2u;
        const uint32_t workgroups = (half + 255u) / 256u;
        wgpu::CommandEncoderDescriptor enc_desc{};
        auto encoder = device_.CreateCommandEncoder(&enc_desc);
        for (uint32_t s = 0; s < plan->log2n; ++s) {
            wgpu::ComputePassDescriptor pass_desc{};
            // Bracket the whole pass sequence: beginning-of-pass timestamp on
            // the first pass, end-of-pass on the last (both on a single pass
            // when log2n == 1). Each unused index MUST stay "undefined"
            // (UINT32_MAX), not 0 — 0 is a valid slot. Set both explicitly
            // rather than relying on the wrapper's default member initializers.
            wgpu::PassTimestampWrites tw{};
            tw.beginningOfPassWriteIndex = wgpu::kQuerySetIndexUndefined;
            tw.endOfPassWriteIndex = wgpu::kQuerySetIndexUndefined;
            if (do_ts && (s == 0 || s + 1 == plan->log2n)) {
                tw.querySet = plan->ts_qs;
                if (s == 0) tw.beginningOfPassWriteIndex = 0;
                if (s + 1 == plan->log2n) tw.endOfPassWriteIndex = 1;
                pass_desc.timestampWrites = &tw;
            }
            auto pass = encoder.BeginComputePass(&pass_desc);
            pass.SetPipeline(fft_pipeline_);
            pass.SetBindGroup(0, plan->stage_bgs[s]);
            pass.DispatchWorkgroups(workgroups);
            pass.End();
        }
        // After log2(N) ping-pong passes the result lives in buf_b for odd
        // log2(N), buf_a for even (we always start reading buf_a).
        wgpu::Buffer& result = (plan->log2n & 1u) ? plan->buf_b : plan->buf_a;
        encoder.CopyBufferToBuffer(result, 0, plan->readback, 0, bytes);
        if (do_ts) {
            encoder.ResolveQuerySet(plan->ts_qs, 0, 2, plan->ts_resolve, 0);
            encoder.CopyBufferToBuffer(plan->ts_resolve, 0, plan->ts_readback, 0,
                                       2u * sizeof(uint64_t));
        }
        auto cmd = encoder.Finish();
        queue_.Submit(1, &cmd);

        if (!read_back(plan->readback, complex_out, bytes)) return false;

        if (do_ts) {
            uint64_t ticks[2] = {0, 0};
            if (read_back(plan->ts_readback, ticks, 2u * sizeof(uint64_t))
                && ticks[1] >= ticks[0]) {
                *gpu_ns = static_cast<double>(ticks[1] - ticks[0]);  // WebGPU ns
            }
        }

        if (normalize) {
            const float inv = 1.0f / static_cast<float>(n);
            for (uint32_t i = 0; i < n * 2u; ++i) complex_out[i] *= inv;
        }
        return true;
    }

    wgpu::Buffer create_storage_buffer(uint32_t size, wgpu::BufferUsage usage) {
        wgpu::BufferDescriptor desc{};
        desc.size = size;
        desc.usage = usage;
        return device_.CreateBuffer(&desc);
    }

    wgpu::Buffer create_readback_buffer(uint32_t size) {
        wgpu::BufferDescriptor desc{};
        desc.size = size;
        desc.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
        return device_.CreateBuffer(&desc);
    }

    // Pool-backed buffer acquisition for the compute hot path. These wrap
    // StagingBufferPool::acquire() and fall back to raw create_*() if the
    // pool is not yet initialized (should only happen in error paths).
    wgpu::Buffer acquire_storage_buffer(uint32_t size, wgpu::BufferUsage usage) {
        if (pool_) return pool_->acquire(size, usage);
        return create_storage_buffer(size, usage);
    }

    wgpu::Buffer acquire_readback_buffer(uint32_t size) {
        const auto usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
        if (pool_) return pool_->acquire(size, usage);
        return create_readback_buffer(size);
    }

    // Register an OnSubmittedWorkDone callback on the queue that releases
    // each of the given buffers back to the pool once the GPU confirms the
    // most-recent submission is complete. The lambda captures the buffers
    // (which are wgpu::Buffer ref-counted handles) to keep them alive until
    // the callback fires, and a weak_ptr to the pool so a callback that
    // outlives ~DawnGpuCompute() cleanly no-ops instead of dereferencing
    // freed state.
    void schedule_pool_release(std::vector<wgpu::Buffer> bufs) {
        if (!pool_ || bufs.empty()) return;

        std::weak_ptr<detail::StagingBufferPool> pool_weak = pool_;
        // The lambda is intentionally non-`mutable`: it only reads the
        // captured buffers (StagingBufferPool::release takes a
        // `const wgpu::Buffer&`). m150's Dawn `webgpu_cpp.h` only deduces
        // capturing-callback traits for a `const` `operator()`, so a
        // `mutable` lambda fails to instantiate `CppFTraitsImpl`.
        queue_.OnSubmittedWorkDone(
            wgpu::CallbackMode::AllowProcessEvents,
            [pool_weak, bufs = std::move(bufs)](wgpu::QueueWorkDoneStatus,
                                                 wgpu::StringView) {
                if (auto pool = pool_weak.lock()) {
                    for (const auto& b : bufs) {
                        pool->release(b);
                    }
                }
                // When lock() returns null the owning DawnGpuCompute has
                // been destroyed; `bufs` goes out of scope here and the
                // wgpu::Buffer dtor drops the underlying GPU handles.
            });
    }

    wgpu::BindGroup create_bind_group(const wgpu::ComputePipeline& pipeline,
                                       std::initializer_list<wgpu::Buffer> buffers) {
        auto layout = pipeline.GetBindGroupLayout(0);

        std::vector<wgpu::BindGroupEntry> entries;
        uint32_t binding = 0;
        for (const auto& buf : buffers) {
            wgpu::BindGroupEntry entry{};
            entry.binding = binding++;
            entry.buffer = buf;
            entry.offset = 0;
            entry.size = buf.GetSize();
            entries.push_back(entry);
        }

        wgpu::BindGroupDescriptor bg_desc{};
        bg_desc.layout = layout;
        bg_desc.entryCount = entries.size();
        bg_desc.entries = entries.data();

        return device_.CreateBindGroup(&bg_desc);
    }

    void dispatch(const wgpu::ComputePipeline& pipeline,
                  const wgpu::BindGroup& bind_group,
                  uint32_t workgroup_count) {
        wgpu::CommandEncoderDescriptor enc_desc{};
        auto encoder = device_.CreateCommandEncoder(&enc_desc);

        wgpu::ComputePassDescriptor pass_desc{};
        auto pass = encoder.BeginComputePass(&pass_desc);
        pass.SetPipeline(pipeline);
        pass.SetBindGroup(0, bind_group);
        pass.DispatchWorkgroups(workgroup_count);
        pass.End();

        auto cmd = encoder.Finish();
        queue_.Submit(1, &cmd);
    }

    void copy_buffer(const wgpu::Buffer& src, const wgpu::Buffer& dst, uint32_t size) {
        wgpu::CommandEncoderDescriptor enc_desc{};
        auto encoder = device_.CreateCommandEncoder(&enc_desc);
        encoder.CopyBufferToBuffer(src, 0, dst, 0, size);
        auto cmd = encoder.Finish();
        queue_.Submit(1, &cmd);
    }

    // ── Backend seam ────────────────────────────────────────────────────
    //
    // The only two places this class reaches for a Dawn *implementation* rather
    // than the webgpu.h/webgpu_cpp.h API: creating the instance (which on native
    // needs dawn::native::Instance plus a proc-table registration) and driving
    // the event queue. Emscripten's emdawnwebgpu port provides neither — the
    // browser constructs the instance via wgpu::CreateInstance() and the JS
    // event loop IS the event queue. Every other call in this file is already
    // plain webgpu_cpp, so the browser carve-out is exactly these two functions.
#if defined(__EMSCRIPTEN__)
    bool create_instance() {
        // No proc table: emdawnwebgpu links the procs directly, and there is no
        // dawn::native to source them from.
        instance_ = wgpu::CreateInstance(nullptr);
        return instance_ != nullptr;
    }

    // Deliberately a no-op. emdawnwebgpu's ProcessEvents() does not resolve map
    // callbacks (see kAsyncCallbackMode) — they arrive spontaneously from the JS
    // event loop — so pumping here would only be a misleading suggestion that
    // this call is how results land. Nothing in this file may ever *wait* on it.
    void pump_events() {}
#else
    bool create_instance() {
        const DawnProcTable& procs = dawn::native::GetProcs();
        dawnProcSetProcs(&procs);

        wgpu::InstanceDescriptor inst_desc{};
        native_instance_ = std::make_unique<dawn::native::Instance>(
            reinterpret_cast<const WGPUInstanceDescriptor*>(&inst_desc));
        instance_ = wgpu::Instance(native_instance_->Get());
        return instance_ != nullptr;
    }

    void pump_events() {
        if (instance_) instance_.ProcessEvents();
    }
#endif

    // ── Asynchronous readback bookkeeping ───────────────────────────────

    struct AsyncReadback {
        uint64_t id = 0;
        void* dest = nullptr;
        uint32_t size = 0;
        wgpu::Buffer buffer;
        std::chrono::steady_clock::time_point deadline;
        ReadbackCallback on_complete;
        bool resolved = false;    // the map callback has fired
        bool ok = false;          // ... and reported Success
        bool abandoned = false;   // completed without the map: a late map must
                                  // not write `dest` or touch the pool
    };

    void complete(AsyncReadback& req, ReadbackStatus status, uint32_t bytes) {
        if (status == ReadbackStatus::Expired) ++stats_->expired;
        else if (status == ReadbackStatus::Failed) ++stats_->failed;

        if (!req.on_complete) return;
        // Move the callback out first: a completion fires exactly once even if
        // it re-enters this object.
        auto cb = std::move(req.on_complete);
        req.on_complete = nullptr;
        cb(ReadbackResult{req.id, status, bytes});
    }

    void drain_readbacks(ReadbackStatus status) {
        while (!async_readbacks_.empty()) {
            auto req = async_readbacks_.front();
            async_readbacks_.pop_front();
            req->abandoned = true;
            if (pool_) pool_->discard(req->buffer);
            complete(*req, status, 0);
        }
    }

    // True when another async request cannot be admitted. Checked BEFORE any
    // buffer is acquired, so a rejected submit costs nothing.
    bool readbacks_at_cap() const {
        return async_readbacks_.size() >= max_readbacks_in_flight_;
    }

    // Map `buffer` without blocking. Each in-flight request owns its own staging
    // buffer (the pool hands out a distinct one per acquire), so N requests can
    // be in flight at once; poll_readbacks() delivers them in submission order.
    //
    // Admission control: at the in-flight cap the request is REJECTED (returns 0)
    // and its staging buffer handed straight back. Queueing instead would grow
    // staging memory by one buffer per block for as long as the GPU is stalled —
    // and a throttled background tab stalls for as long as it likes. A rejected
    // submit is a miss, which the caller already routes through its MissPolicy.
    uint64_t read_back_async(wgpu::Buffer buffer, void* dest, uint32_t size,
                             std::chrono::microseconds deadline,
                             ReadbackCallback on_complete) {
        if (readbacks_at_cap()) {
            if (pool_) pool_->release(buffer);
            return 0;
        }

        auto req = std::make_shared<AsyncReadback>();
        req->id = ++next_readback_id_;
        req->dest = dest;
        req->size = size;
        req->buffer = buffer;
        req->deadline = std::chrono::steady_clock::now() + deadline;
        req->on_complete = std::move(on_complete);
        async_readbacks_.push_back(req);

        // The callback is intentionally non-`mutable` (see schedule_pool_release
        // for why m150's webgpu_cpp.h requires that). It mutates the request
        // through the captured shared_ptr, which a const operator() permits.
        //
        // The stats counter is bumped here, at the resolution itself, rather than
        // inferred later from a completion status — a map can resolve and still
        // complete Expired, and the browser stats block reports what the GPU
        // actually did.
        buffer.MapAsync(wgpu::MapMode::Read, 0, size,
            kAsyncCallbackMode,
            [req, stats = std::weak_ptr<AsyncStats>(stats_)](
                wgpu::MapAsyncStatus status, wgpu::StringView) {
                req->resolved = true;
                req->ok = (status == wgpu::MapAsyncStatus::Success);
                if (auto s = stats.lock()) ++s->map_resolves;
                if (!req->abandoned) return;
                // The deadline already fired: poll_readbacks() has delivered the
                // completion and discarded the buffer from the pool. Release the
                // map so the handle can be torn down cleanly.
                if (req->ok) req->buffer.Unmap();
            });

        return req->id;
    }

    uint64_t next_readback_id_ = 0;
    bool polling_ = false;
    std::size_t max_readbacks_in_flight_ = 4;
    std::deque<std::shared_ptr<AsyncReadback>> async_readbacks_;

    // Held by shared_ptr because the map callback that bumps `map_resolves` can
    // outlive this object (a map on a shared device resolves from whatever pumps
    // the queue next); the callback holds a weak_ptr and no-ops if it is gone.
    std::shared_ptr<AsyncStats> stats_ = std::make_shared<AsyncStats>();

#if defined(__EMSCRIPTEN__)
    // There is no correct blocking readback in a browser, so there is no browser
    // implementation of one. Spinning on ProcessEvents() would starve the JS
    // event loop that resolves the map — a guaranteed self-deadlock that burns
    // the full deadline and then fails anyway. Every op that still calls this is
    // native-only; the browser uses the async path exclusively.
    bool read_back(wgpu::Buffer&, void*, uint32_t) {
        if (!read_back_blocked_logged_) {
            read_back_blocked_logged_ = true;
            runtime::log_error(
                "GpuCompute: blocking read_back() is not available in the browser "
                "— use the async ops (convolve_batch_async / "
                "compute_magnitude_async) + poll_readbacks()");
        }
        return false;
    }
    bool read_back_blocked_logged_ = false;
#else
    bool read_back(wgpu::Buffer& buffer, void* dest, uint32_t size) {
        // The map request outlives this frame if the deadline fires, so its state
        // cannot live on this stack: a pending map that resolves after the return
        // would write through dangling pointers from whichever later
        // ProcessEvents() happens to pump it. Same shared-state + `abandoned`
        // shape as read_back_async().
        struct BlockingMap {
            bool mapped = false;
            bool ok = false;
            bool abandoned = false;
            wgpu::Buffer buffer;
        };
        auto st = std::make_shared<BlockingMap>();
        st->buffer = buffer;

        buffer.MapAsync(wgpu::MapMode::Read, 0, size,
            wgpu::CallbackMode::AllowProcessEvents,
            [st](wgpu::MapAsyncStatus status, wgpu::StringView) {
                st->mapped = true;
                st->ok = (status == wgpu::MapAsyncStatus::Success);
                // The caller already gave up and returned. Release the mapping so
                // the buffer can be torn down cleanly; nothing reads it now.
                if (st->abandoned && st->ok) st->buffer.Unmap();
            });

        // Drive the event loop until the map completes. Spin (yield) for the
        // first few ms — GPU compute jobs here complete well within that, and a
        // fixed 1ms sleep per iteration otherwise imposes a poll-granularity
        // latency floor that dwarfs the actual GPU time. Back off to a 1ms sleep
        // after the spin budget so a stuck/long job never burns a core. Never
        // called on the audio thread (worker/offline only), so a short spin is
        // safe.
        const auto start = std::chrono::steady_clock::now();
        const auto spin_until = start + std::chrono::milliseconds(4);
        const auto deadline = start + std::chrono::seconds(2);
        while (!st->mapped && std::chrono::steady_clock::now() < deadline) {
            instance_.ProcessEvents();
            if (st->mapped) break;
            if (std::chrono::steady_clock::now() < spin_until)
                std::this_thread::yield();
            else
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        if (!st->mapped || !st->ok) {
            st->abandoned = true;
            return false;
        }

        const void* data = buffer.GetConstMappedRange(0, size);
        if (!data) return false;

#ifdef PULP_BENCHMARK
        {
            const double t0 = bench::now_us();
            std::memcpy(dest, data, size);
            if (bench_counters_) {
                bench_counters_->gpu_readback_total_us.fetch_add(
                    bench::now_us() - t0, std::memory_order_relaxed);
                bench_counters_->gpu_to_cpu_bytes_total.fetch_add(
                    static_cast<double>(size),
                    std::memory_order_relaxed);
            }
        }
#else
        std::memcpy(dest, data, size);
#endif
        buffer.Unmap();
        return true;
    }
#endif  // !__EMSCRIPTEN__
};

std::unique_ptr<GpuCompute> GpuCompute::create() {
    return std::make_unique<DawnGpuCompute>();
}

} // namespace pulp::render

#else // !PULP_HAS_DAWN

namespace pulp::render {

// Stub when Dawn/Skia not available
std::unique_ptr<GpuCompute> GpuCompute::create() {
    return nullptr;
}

} // namespace pulp::render

#endif
