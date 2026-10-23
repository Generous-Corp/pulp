#pragma once

// WGSL for one WaveNet layer, shared by GpuCompute's WaveNet passes and the
// shared-I/O provider's WaveNet program so both run the same kernel source.
// Bindings: 0 weights, 1 input ring, 2 output ring, 3 mono condition input,
// 4 head sum, 5 layer uniform, 6 stream state (read-only u32 storage:
// [0] stream position of the block's first sample, [1] floats per instance
// weight slice). A single stream is an instance group of one (workgroup_id.y
// is always 0).

namespace pulp::render::detail {

inline constexpr const char* kWavenetLayerWgsl = R"wgsl(
// One WaveNet layer: z = dilated_conv(in)[Z] + bias + input_mixin*cond (Z), then
// a = (gated ? tanh(z[:C])*sigmoid(z[C:]) : tanh(z[:C])) (C); head += a;
// out = in + (layer1x1_bias + layer1x1_W * a). condition_size is 1 (mono input).
// head_init = 1 assigns the head sum instead of accumulating (first layer of the
// first array), so the sum needs no per-block clear.
//
// Block-parallel dispatch: each workgroup handles S consecutive samples
// (S = p.S, chosen by the host so S*Z fills the WG lanes; S = 1 when Z > 32),
// and the host dispatches ceil(B/S) workgroups. Lane work items are
// (sample, channel) pairs: in each phase a lane owns whole output channels of
// one sample and walks that channel's K*C reduction in the SAME serial
// arithmetic order as the scalar path, so every output element is bit-identical
// whatever S is. Narrow models (C = 2..16) would otherwise leave most of the 64
// lanes idle with one sample per workgroup. zbuf/abuf are workgroup shared
// memory so the activation and 1x1 stages can read every channel the conv
// stage produced for their sample; S*Z <= 128 and S*C <= 64 by construction.
//
// Weight layout (p.wt): with wt = 1 the conv and 1x1 weights are stored
// output-channel-minor ([k][ic][oc] and [ic][oc]; the host transposes the blob
// at prepare), so the lanes of one sample, which own consecutive output
// channels, read consecutive weights on every step of their reductions. That
// wins once a large group keeps the GPU throughput-bound. With wt = 0 they keep
// the blob's [oc][ic][k] / [oc][ic] order, where each lane walks one contiguous
// row; that is faster when few instances leave each dispatch latency-bound.
// Either way every reduction runs in the same order, so results are identical.
//
// Activations are column rings (see the rechannel shader): ina holds this
// layer's input with at least dil*(K-1) columns of history before the block,
// outa receives the layer output at the block's columns. Ring sizes are powers
// of two, so the u32 wrap of the stream position keeps column indices exact.
struct P { C:u32, K:u32, B:u32, dil:u32, Z:u32, gated:u32, in_mask:u32,
           conv_w:u32, conv_b:u32, mix_w:u32, l1_w:u32, l1_b:u32,
           out_mask:u32, S:u32, head_init:u32, wt:u32 };
@group(0) @binding(0) var<storage, read>       wts     : array<f32>;
@group(0) @binding(1) var<storage, read>       ina     : array<f32>;  // C*(in_mask+1)
@group(0) @binding(2) var<storage, read_write> outa    : array<f32>;  // C*(out_mask+1)
@group(0) @binding(3) var<storage, read>       cond    : array<f32>;  // B mono input
@group(0) @binding(4) var<storage, read_write> headacc : array<f32>;  // C*B
@group(0) @binding(5) var<uniform>             p       : P;
@group(0) @binding(6) var<storage, read>       st      : array<u32>;  // [0] position, [1] weight slice

const WG : u32 = 64u;                    // lanes per workgroup
var<workgroup> zbuf : array<f32, 128>;   // S*Z <= 128 (Zmax = 2*Cmax at S = 1)
var<workgroup> abuf : array<f32, 64>;    // S*C <= 64

@compute @workgroup_size(WG)
fn main(@builtin(workgroup_id) wid : vec3u,
        @builtin(local_invocation_id) lid : vec3u) {
    let S = p.S;
    let t0 = wid.x * S;                  // first sample of this workgroup
    let inst = wid.y;                    // instance within the group
    // Uniform guard: workgroup_id is identical for every lane, so the whole
    // workgroup returns together — barriers below stay in uniform control flow.
    // Samples past B inside the last workgroup are masked per item instead.
    if (t0 >= p.B) { return; }
    let lane = lid.x;
    let C = p.C; let K = p.K; let Z = p.Z;
    let w0 = inst * st[1];                        // this instance's weight slice
    let in0 = inst * C * (p.in_mask + 1u);       // this instance's input ring
    let out0 = inst * C * (p.out_mask + 1u);     // this instance's output ring
    let h0 = inst * C * p.B;                     // this instance's head sum
    // Conv stage — dilated conv + input mixin, one item per (sample, output channel).
    for (var i = lane; i < S * Z; i = i + WG) {
        let s = i / Z;
        let oc = i - s * Z;
        let t = t0 + s;
        if (t < p.B) {
            let n = st[0] + t;
            var acc = wts[w0 + p.conv_b + oc];
            for (var k = 0u; k < K; k = k + 1u) {
                let back = p.dil * (K - 1u - k);   // valid: ring history >= max back
                let base = in0 + ((n - back) & p.in_mask) * C;
                // wt == 1: [k][ic][oc] (stride Z per ic); else [oc][ic][k] (stride K).
                let wbase = w0 + p.conv_w + select(oc * C * K + k, k * C * Z + oc, p.wt == 1u);
                let wstep = select(K, Z, p.wt == 1u);
                for (var ic = 0u; ic < C; ic = ic + 1u) {
                    acc = acc + wts[wbase + ic * wstep] * ina[base + ic];
                }
            }
            acc = acc + wts[w0 + p.mix_w + oc] * cond[inst * p.B + t];   // input_mixin (condition_size == 1)
            zbuf[i] = acc;
        }
    }
    workgroupBarrier();
    // Activation stage — one item per (sample, channel).
    for (var i = lane; i < S * C; i = i + WG) {
        let s = i / C;
        let c = i - s * C;
        if (t0 + s < p.B) {
            // WGSL tanh may return NaN for large |x| (Metal does); float tanh is
            // already exactly +-1 at |x| = 15, so the clamp changes no finite result.
            // Keep the sigmoid a named reciprocal multiplied after tanh: written
            // as a * (1 / x) the shader compiler contracts it differently and
            // gated outputs move by a few ULP.
            if (p.gated == 0u) {
                abuf[i] = tanh(clamp(zbuf[s * Z + c], -15.0, 15.0));
            } else {
                let g = 1.0 / (1.0 + exp(-zbuf[s * Z + C + c]));
                abuf[i] = tanh(clamp(zbuf[s * Z + c], -15.0, 15.0)) * g;
            }
        }
    }
    workgroupBarrier();
    // Output stage — head accumulate + 1x1 output, one item per (sample, channel).
    for (var i = lane; i < S * C; i = i + WG) {
        let s = i / C;
        let oc = i - s * C;
        let t = t0 + s;
        if (t < p.B) {
            // The first layer of the first array starts the head sum; every other
            // layer adds to it (a later array's sum arrives pre-seeded with the
            // previous array's head output).
            if (p.head_init == 1u) {
                headacc[h0 + t * C + oc] = abuf[i];
            } else {
                headacc[h0 + t * C + oc] = headacc[h0 + t * C + oc] + abuf[i];
            }
            let n = st[0] + t;
            var r = wts[w0 + p.l1_b + oc];
            let a0 = s * C;
            for (var ic = 0u; ic < C; ic = ic + 1u) {
                r = r + wts[w0 + p.l1_w + select(oc * C + ic, ic * C + oc, p.wt == 1u)] *
                            abuf[a0 + ic];
            }
            outa[out0 + (n & p.out_mask) * C + oc] = ina[in0 + (n & p.in_mask) * C + oc] + r;
        }
    }
}
)wgsl";

} // namespace pulp::render::detail
