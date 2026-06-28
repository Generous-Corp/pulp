#pragma once

// A WaveNet-style neural amp inference model — the architecture behind modern
// "neural amp" captures: a stack of gated, dilated, causal 1-D convolutions with
// residual + skip connections feeding a linear head. This header is the CPU
// reference (and the always-available fallback); the GPU engine reproduces its
// math exactly and is validated against it.
//
// Layout of one layer's parameters (all row-major f32, in this order):
//   conv weights  [2C x C x K]   gated dilated conv (2C = tanh half + gate half)
//   conv bias     [2C]
//   residual W    [C x C]        1x1 conv folded back into the running activation
//   residual bias [C]
//   skip W        [C x C]        1x1 conv accumulated into the skip sum
//   skip bias     [C]
// Plus, once for the whole model:
//   input W [C x 1] + bias [C]   project the mono input to C channels
//   head  W [1 x C] + bias [1]   project the skip sum to one output sample
// followed by a scalar head_scale. This mirrors the standard neural-amp WaveNet
// shape closely enough to host real captures while staying a clean, readable
// reference.

#include <cmath>
#include <cstdint>
#include <vector>

namespace pulp::examples {

struct WaveNetConfig {
    uint32_t channels = 16;          // hidden channels C
    uint32_t kernel = 3;             // conv kernel K
    std::vector<uint32_t> dilations; // one per layer; size() == num_layers
    float head_scale = 1.0f;

    uint32_t num_layers() const { return static_cast<uint32_t>(dilations.size()); }
    uint32_t max_dilation() const {
        uint32_t m = 1;
        for (uint32_t d : dilations) m = d > m ? d : m;
        return m;
    }
    // Left history (past input samples) the deepest layer can reach.
    uint32_t receptive_pad() const { return max_dilation() * (kernel - 1) * num_layers(); }
};

// Flat parameter blob for one model, sliced by the layout documented above.
struct WaveNetWeights {
    std::vector<float> data;
    // Per-layer + global offsets are derived from the config in WaveNetCpu::load.
};

// CPU reference inference. Stateful across blocks (carries the dilation history),
// so process() can be called block by block like a real-time engine.
class WaveNetCpu {
public:
    bool load(const WaveNetConfig& cfg, const std::vector<float>& weights) {
        cfg_ = cfg;
        const uint32_t C = cfg.channels, K = cfg.kernel, L = cfg.num_layers();
        if (C == 0 || K == 0 || L == 0) return false;
        per_layer_ = 2u * C * C * K + 2u * C + C * C + C + C * C + C;
        const std::size_t need = static_cast<std::size_t>(per_layer_) * L
                                 + (C + C)            // input W + bias
                                 + (C + 1);           // head W + bias
        if (weights.size() < need) return false;
        w_ = weights;

        // Per-layer ring history of the layer INPUT (C channels). Sized to the
        // layer's own reach so causal indexing never runs off the front.
        hist_.assign(L, {});
        for (uint32_t l = 0; l < L; ++l) {
            const uint32_t reach = cfg.dilations[l] * (K - 1);
            hist_[l].assign(static_cast<std::size_t>(C) * (reach + 1u), 0.0f);
            hist_reach_.push_back(reach);
        }
        skip_.assign(C, 0.0f);
        gate_.assign(2u * C, 0.0f);
        act_.assign(C, 0.0f);
        return true;
    }

    void reset() {
        for (auto& h : hist_) std::fill(h.begin(), h.end(), 0.0f);
    }

    // One mono sample in → one mono sample out (the per-sample core; process()
    // wraps it for a block). Kept simple + readable as the correctness oracle.
    float process_sample(float x) {
        const uint32_t C = cfg_.channels, K = cfg_.kernel, L = cfg_.num_layers();
        const float* w = w_.data();

        // Input projection 1 -> C.
        const float* in_w = w + static_cast<std::size_t>(per_layer_) * L;     // [C]
        const float* in_b = in_w + C;                                         // [C]
        std::vector<float> cur(C);
        for (uint32_t c = 0; c < C; ++c) cur[c] = in_w[c] * x + in_b[c];

        std::fill(skip_.begin(), skip_.end(), 0.0f);
        for (uint32_t l = 0; l < L; ++l) {
            const float* lw = w + static_cast<std::size_t>(per_layer_) * l;
            const float* conv_w = lw;                       // [2C x C x K]
            const float* conv_b = conv_w + 2u * C * C * K;  // [2C]
            const float* res_w  = conv_b + 2u * C;          // [C x C]
            const float* res_b  = res_w + C * C;            // [C]
            const float* skip_w = res_b + C;                // [C x C]
            const float* skip_b = skip_w + C * C;           // [C]

            // Push cur into this layer's history ring (newest at the back).
            auto& H = hist_[l];
            const uint32_t reach = hist_reach_[l];
            const uint32_t slots = reach + 1u;
            // shift left by one slot, append cur
            std::move(H.begin() + C, H.end(), H.begin());
            std::copy(cur.begin(), cur.end(), H.end() - C);

            // Gated dilated causal conv over the history.
            const uint32_t d = cfg_.dilations[l];
            for (uint32_t oc = 0; oc < 2u * C; ++oc) {
                float acc = conv_b[oc];
                for (uint32_t k = 0; k < K; ++k) {
                    // tap k reaches back d*(K-1-k) samples
                    const uint32_t back = d * (K - 1u - k);
                    if (back > reach) continue;  // still in warm-up (zero)
                    const float* past = H.data() + static_cast<std::size_t>(slots - 1u - back) * C;
                    const float* wk = conv_w + (static_cast<std::size_t>(oc) * C * K) + k;
                    for (uint32_t ic = 0; ic < C; ++ic)
                        acc += wk[static_cast<std::size_t>(ic) * K] * past[ic];
                }
                gate_[oc] = acc;
            }
            // tanh * sigmoid gate.
            for (uint32_t c = 0; c < C; ++c) {
                const float t = std::tanh(gate_[c]);
                const float s = 1.0f / (1.0f + std::exp(-gate_[C + c]));
                act_[c] = t * s;
            }
            // residual (cur += R*act + b) and skip (skip += S*act + b).
            for (uint32_t oc = 0; oc < C; ++oc) {
                float r = res_b[oc], s = skip_b[oc];
                const float* rw = res_w + static_cast<std::size_t>(oc) * C;
                const float* sw = skip_w + static_cast<std::size_t>(oc) * C;
                for (uint32_t ic = 0; ic < C; ++ic) { r += rw[ic] * act_[ic]; s += sw[ic] * act_[ic]; }
                cur[oc] += r;
                skip_[oc] += s;
            }
        }

        // Head C -> 1.
        const float* head_w = w + static_cast<std::size_t>(per_layer_) * L + (C + C);  // [C]
        const float* head_b = head_w + C;                                              // [1]
        float y = head_b[0];
        for (uint32_t c = 0; c < C; ++c) y += head_w[c] * skip_[c];
        return y * cfg_.head_scale;
    }

    void process(const float* in, float* out, uint32_t n) {
        for (uint32_t i = 0; i < n; ++i) out[i] = process_sample(in[i]);
    }

    const WaveNetConfig& config() const { return cfg_; }
    uint32_t per_layer_floats() const { return per_layer_; }

private:
    WaveNetConfig cfg_;
    std::vector<float> w_;
    uint32_t per_layer_ = 0;
    std::vector<std::vector<float>> hist_;
    std::vector<uint32_t> hist_reach_;
    std::vector<float> skip_, gate_, act_;
};

// Deterministic synthetic model for correctness/benchmark testing (small random
// weights so the recursion stays bounded). Real captures load a .nam instead.
inline std::vector<float> make_synthetic_wavenet(const WaveNetConfig& cfg,
                                                 std::uint32_t seed = 0x51A1u) {
    const uint32_t C = cfg.channels, K = cfg.kernel, L = cfg.num_layers();
    const std::size_t per = 2u * C * C * K + 2u * C + C * C + C + C * C + C;
    const std::size_t total = per * L + (C + C) + (C + 1);
    std::vector<float> w(total);
    std::uint32_t s = seed;
    for (auto& v : w) {
        s = s * 1664525u + 1013904223u;
        v = (static_cast<float>(s >> 9) / 4194304.0f - 1.0f) * 0.1f;  // small
    }
    return w;
}

} // namespace pulp::examples
