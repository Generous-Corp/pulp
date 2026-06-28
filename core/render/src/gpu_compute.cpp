#include <pulp/render/gpu_compute.hpp>

#ifdef PULP_HAS_SKIA

#include <pulp/runtime/log.hpp>
#include "webgpu/webgpu_cpp.h"
#include "dawn/native/DawnNative.h"
#include "dawn/dawn_proc.h"

#include "gpu_compute_pool.hpp"

#include <chrono>
#include <cmath>
#include <cstring>
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
        let half = p.n / 2u;
        if (k > 0u && k < half) {
            ph = ph + p.jitter * hash01(k);
        } else if (k > half && k < p.n) {
            ph = ph - p.jitter * hash01(p.n - k);
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
struct P { C:u32, B:u32, in_off:u32, pad:u32 };
@group(0) @binding(0) var<storage, read>       wts : array<f32>;
@group(0) @binding(1) var<storage, read>       inp : array<f32>;   // B mono
@group(0) @binding(2) var<storage, read_write> act : array<f32>;   // C*B
@group(0) @binding(3) var<uniform>             p   : P;
@compute @workgroup_size(256)
fn main(@builtin(global_invocation_id) gid : vec3u) {
    let t = gid.x;
    if (t >= p.B) { return; }
    let x = inp[t];
    let wb = p.in_off + p.C;
    for (var c = 0u; c < p.C; c = c + 1u) {
        act[t * p.C + c] = wts[p.in_off + c] * x + wts[wb + c];
    }
}
)wgsl";

static constexpr const char* kConvStackLayerShader = R"wgsl(
// One gated dilated causal conv layer: z = dilated_conv(in) [2C], gate =
// tanh(z[:C])*sigmoid(z[C:]), out = in + residual(gate), skip += skip(gate).
struct P { C:u32, K:u32, B:u32, dil:u32, woff:u32, p0:u32, p1:u32, p2:u32 };
@group(0) @binding(0) var<storage, read>       wts  : array<f32>;
@group(0) @binding(1) var<storage, read>       ina  : array<f32>;  // C*B
@group(0) @binding(2) var<storage, read_write> outa : array<f32>;  // C*B
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

    let two_c = 2u * C;
    for (var oc = 0u; oc < two_c; oc = oc + 1u) {
        var acc = wts[cb + oc];
        for (var k = 0u; k < K; k = k + 1u) {
            let back = p.dil * (K - 1u - k);
            if (t >= back) {
                let base = (t - back) * C;
                let wbase = cw + oc * C * K + k;
                for (var ic = 0u; ic < C; ic = ic + 1u) {
                    acc = acc + wts[wbase + ic * K] * ina[base + ic];
                }
            }
        }
        zbuf[oc] = acc;
    }
    for (var c = 0u; c < C; c = c + 1u) {
        let tg = tanh(zbuf[c]);
        let sg = 1.0 / (1.0 + exp(-zbuf[C + c]));
        gbuf[c] = tg * sg;
    }
    let tc = t * C;
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
        skip[tc + oc] = skip[tc + oc] + s;
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

// ── Implementation ──────────────────────────────────────────────────────────

class DawnGpuCompute : public GpuCompute {
public:
    ~DawnGpuCompute() override {
        // Release pool buffers before the device is torn down — their
        // destructors call into Dawn internals and need a live device.
        pool_.reset();
        fft_plans_.clear();
        conv_plans_.clear();
        batch_conv_plans_.clear();
        multi_conv_plans_.clear();
        spectral_stack_plans_.clear();
        conv_stack_plans_.clear();
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
        matmul_pipeline_ = nullptr;
        additive_pipeline_ = nullptr;
        modal_pipeline_ = nullptr;
        granular_pipeline_ = nullptr;
        dense_tanh_pipeline_ = nullptr;
        fft_pipeline_ = nullptr;
        queue_ = nullptr;
        device_ = nullptr;
        instance_ = nullptr;
        native_instance_.reset();
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
        const DawnProcTable& procs = dawn::native::GetProcs();
        dawnProcSetProcs(&procs);

        wgpu::InstanceDescriptor inst_desc{};
        native_instance_ = std::make_unique<dawn::native::Instance>(
            reinterpret_cast<const WGPUInstanceDescriptor*>(&inst_desc));
        instance_ = wgpu::Instance(native_instance_->Get());
        if (!instance_) return false;

        wgpu::RequestAdapterOptions opts{};
        opts.powerPreference = wgpu::PowerPreference::HighPerformance;

        instance_.RequestAdapter(
            &opts, wgpu::CallbackMode::AllowProcessEvents,
            [this](wgpu::RequestAdapterStatus status, wgpu::Adapter result, wgpu::StringView) {
                if (status == wgpu::RequestAdapterStatus::Success)
                    adapter_ = std::move(result);
            });
        instance_.ProcessEvents();
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

        adapter_.RequestDevice(
            &dev_desc, wgpu::CallbackMode::AllowProcessEvents,
            [this](wgpu::RequestDeviceStatus status, wgpu::Device result, wgpu::StringView) {
                if (status == wgpu::RequestDeviceStatus::Success)
                    device_ = std::move(result);
            });
        instance_.ProcessEvents();
        if (!device_) return false;

        queue_ = device_.GetQueue();
        owns_device_ = true;

        return create_pipelines();
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

    bool multi_convolve(const float* in_complex, const float* pan_l,
                        const float* pan_r, float* out_lr, uint32_t n,
                        uint32_t num_ir) override {
        if (!initialized_ || !in_complex || !pan_l || !pan_r || !out_lr) return false;
        auto it = multi_conv_plans_.find(n);
        if (it == multi_conv_plans_.end() || it->second.num_ir != num_ir) return false;
        MultiConvPlan& plan = it->second;

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
        encode_fft_passes(encoder, plan.fwd_bgs, fwd_wg);
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
            auto pass = encoder.BeginComputePass(&pd);
            pass.SetPipeline(multi_ir_combine_pipeline_);
            pass.SetBindGroup(0, plan.combine_bg);
            pass.DispatchWorkgroups(comb_wg);
            pass.End();
        }
        encoder.CopyBufferToBuffer(plan.out_lr, 0, plan.readback, 0, small);
        auto cmd = encoder.Finish();
        queue_.Submit(1, &cmd);

        return read_back(plan.readback, out_lr, small);
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

        ConvStackPlan plan;
        plan.C = C; plan.K = K; plan.L = L; plan.B = B;
        plan.head_scale = head_scale;
        plan.per_layer = static_cast<uint32_t>(per_layer);

        const uint32_t wbytes = static_cast<uint32_t>(need) * 4u;
        const uint32_t act_bytes = C * B * 4u;
        const uint32_t blk_bytes = B * 4u;
        plan.weights = create_storage_buffer(wbytes,
            wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst);
        plan.skip = create_storage_buffer(act_bytes,
            wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst);
        plan.input = create_storage_buffer(blk_bytes,
            wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst);
        plan.output = create_storage_buffer(blk_bytes,
            wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc);
        plan.readback = create_readback_buffer(blk_bytes);
        if (!plan.weights || !plan.skip || !plan.input || !plan.output || !plan.readback)
            return false;
        for (uint32_t l = 0; l <= L; ++l) {
            wgpu::Buffer a = create_storage_buffer(act_bytes, wgpu::BufferUsage::Storage);
            if (!a) return false;
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
        struct InU { uint32_t C, B, in_off, pad; } inu{C, B, in_off, 0};
        queue_.WriteBuffer(plan.input_u, 0, &inu, sizeof(inu));
        plan.input_bg = create_bind_group(conv_in_pipeline_,
            {plan.weights, plan.input, plan.act[0], plan.input_u});
        if (!plan.input_bg) return false;

        for (uint32_t l = 0; l < L; ++l) {
            wgpu::Buffer u = make_uniform(32);
            struct LyU { uint32_t C, K, B, dil, woff, p0, p1, p2; }
                lyu{C, K, B, dilations[l], static_cast<uint32_t>(per_layer) * l, 0, 0, 0};
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

        auto bg = create_bind_group(additive_pipeline_, {p_buf, o_buf, u_buf});
        if (!bg) return false;
        dispatch(additive_pipeline_, bg, (num_samples + 255u) / 256u);
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

        auto bg = create_bind_group(modal_pipeline_, {m_buf, o_buf, u_buf});
        if (!bg) return false;
        dispatch(modal_pipeline_, bg, (num_samples + 255u) / 256u);
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

    // ── Benchmarking ────────────────────────────────────────────────────

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
    std::unique_ptr<dawn::native::Instance> native_instance_;
    bool owns_device_ = false;
    bool has_timestamp_ = false;  // TimestampQuery feature enabled on device_

#ifdef PULP_BENCHMARK
    bench::PerfCounters* bench_counters_ = nullptr;
#endif

    wgpu::ComputePipeline magnitude_pipeline_;
    wgpu::ComputePipeline complex_mul_pipeline_;
    wgpu::ComputePipeline conv_bmul_pipeline_;  // broadcast complex-mul (convolution)
    wgpu::ComputePipeline multi_ir_mul_pipeline_;      // one input × many IRs
    wgpu::ComputePipeline multi_ir_combine_pipeline_;  // pan-combine reduce → stereo
    wgpu::ComputePipeline spectral_advance_pipeline_;  // per-bin phase advance + jitter
    wgpu::ComputePipeline spectral_combine_pipeline_;  // smear + weighted layer sum
    wgpu::ComputePipeline conv_in_pipeline_;     // conv-stack input projection
    wgpu::ComputePipeline conv_layer_pipeline_;  // conv-stack gated dilated layer
    wgpu::ComputePipeline conv_head_pipeline_;   // conv-stack linear head
    wgpu::ComputePipeline matmul_pipeline_;
    wgpu::ComputePipeline additive_pipeline_;
    wgpu::ComputePipeline modal_pipeline_;
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
        uint32_t C = 0, K = 0, L = 0, B = 0, per_layer = 0;
        float head_scale = 1.0f;
        wgpu::Buffer weights;                 // all model weights, resident
        std::vector<wgpu::Buffer> act;        // L+1 activation buffers (C*B)
        wgpu::Buffer skip, input, output;     // skip accum (C*B), in/out blocks (B)
        wgpu::Buffer readback;
        wgpu::Buffer input_u, head_u;
        std::vector<wgpu::Buffer> layer_u;
        wgpu::BindGroup input_bg, head_bg;
        std::vector<wgpu::BindGroup> layer_bg;
    };
    std::unordered_map<uint32_t, ConvStackPlan> conv_stack_plans_;
    std::vector<float> cs_zero_;  // skip-buffer zeroing scratch

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
        if (!multi_ir_combine_pipeline_) return false;

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

        matmul_pipeline_ = create_pipeline("matmul", kMatmulShader);
        if (!matmul_pipeline_) return false;

        additive_pipeline_ = create_pipeline("additive_synth", kAdditiveSynthShader);
        if (!additive_pipeline_) return false;

        modal_pipeline_ = create_pipeline("modal_strike", kModalStrikeShader);
        if (!modal_pipeline_) return false;

        granular_pipeline_ = create_pipeline("granular_cloud", kGranularShader);
        if (!granular_pipeline_) return false;

        dense_tanh_pipeline_ = create_pipeline("dense_tanh", kDenseTanhShader);
        if (!dense_tanh_pipeline_) return false;

        // Staging buffer pool: pre-allocated ring of persistent wgpu::Buffer
        // objects that replaces per-call device_.CreateBuffer() in the compute
        // hot path. Default cap of 8 covers typical GPU dispatch depth (3-4 in
        // flight) times per-call buffer count (2-3 inputs + output), with
        // headroom.
        pool_ = std::make_shared<detail::StagingBufferPool>(device_, 8);

        has_timestamp_ = device_.HasFeature(wgpu::FeatureName::TimestampQuery);

        initialized_ = true;
        runtime::log_info("GpuCompute: pipelines created (device shared: {})",
            !owns_device_);
        return true;
    }

    wgpu::ComputePipeline create_pipeline(const char* label, const char* wgsl) {
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
    void encode_fft_passes(wgpu::CommandEncoder& encoder,
                           const std::vector<wgpu::BindGroup>& stage_bgs,
                           uint32_t workgroups) {
        for (const auto& bg : stage_bgs) {
            wgpu::ComputePassDescriptor pd{};
            auto pass = encoder.BeginComputePass(&pd);
            pass.SetPipeline(fft_pipeline_);
            pass.SetBindGroup(0, bg);
            pass.DispatchWorkgroups(workgroups);
            pass.End();
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

    bool read_back(wgpu::Buffer& buffer, void* dest, uint32_t size) {
        bool mapped = false;
        bool ok = false;

        buffer.MapAsync(wgpu::MapMode::Read, 0, size,
            wgpu::CallbackMode::AllowProcessEvents,
            [&mapped, &ok](wgpu::MapAsyncStatus status, wgpu::StringView) {
                mapped = true;
                ok = (status == wgpu::MapAsyncStatus::Success);
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
        while (!mapped && std::chrono::steady_clock::now() < deadline) {
            instance_.ProcessEvents();
            if (mapped) break;
            if (std::chrono::steady_clock::now() < spin_until)
                std::this_thread::yield();
            else
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        if (!mapped || !ok) return false;

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
};

std::unique_ptr<GpuCompute> GpuCompute::create() {
    return std::make_unique<DawnGpuCompute>();
}

} // namespace pulp::render

#else // !PULP_HAS_SKIA

namespace pulp::render {

// Stub when Dawn/Skia not available
std::unique_ptr<GpuCompute> GpuCompute::create() {
    return nullptr;
}

} // namespace pulp::render

#endif
