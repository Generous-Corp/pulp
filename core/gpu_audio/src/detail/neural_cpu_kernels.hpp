#pragma once

// Private SIMD CPU kernels for the neural families whose adapters bridge a
// consumer-owned kernel: single-channel causal TCN (TcnCpuAdapter),
// multi-channel NAM-style TCN (NamTcnStreamingAdapter), one-layer LSTM/GRU
// (RecurrentCpuAdapter) and diagonal state-space (CompactSsmCpuAdapter).
//
// Each kernel is a class template over a neural_simd Isa, so the scalar and
// NEON instantiations of one source can be nulled against each other.
// Construction (control thread) copies and repacks weights and allocates all
// state. process() is allocation-free, takes any frame count, and carries
// causal state across calls. bind_neural_cpu_kernel() fills any of the
// adapters' six-function kernel structs.
//
// Construction fails closed: if the shape or a weight span is inconsistent,
// the kernel keeps no weights, valid() is false, prepare() returns false (so
// the owning adapter refuses to prepare), and process() writes silence.
// Inputs are finite by contract (see neural_simd.hpp).

#include "neural_simd.hpp"
#include "recurrent_cpu_adapter.hpp"
#include "streaming_model.hpp"
#include "tcn_cpu_adapter.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace pulp::gpu_audio::detail {

/// Binds a kernel object with prepare/process/reset to one of the adapters'
/// kernel structs (TcnCpuKernel, NamTcnCpuKernel, RecurrentCpuKernel,
/// CompactSsmCpuKernel). The kernel object must outlive the adapter.
template <class Seam, class Kernel> Seam bind_neural_cpu_kernel(Kernel& kernel) noexcept {
    return Seam{&kernel,
                [](void* self, const StreamingPrepareContext& context) noexcept {
                    return static_cast<Kernel*>(self)->prepare(context);
                },
                [](void* self, const float* in, float* out, std::uint32_t frames) noexcept {
                    static_cast<Kernel*>(self)->process(in, out, frames);
                },
                [](void* self) noexcept { static_cast<Kernel*>(self)->reset(); },
                [](void*) noexcept { return true; },
                [](void*) noexcept { return true; }};
}

namespace neural_kernels_detail {

/// acc[v] = init(col + 4v) + sum_k w[k][col + 4v] * x[k] for NV vectors.
/// NV independent accumulators hide FMA latency in a per-sample product.
template <class Isa, int NV, class Init>
inline void gemv_tile(const float* w, const float* x, std::uint32_t inputs, std::uint32_t out_pad,
                      std::uint32_t col, Init& init, float* out) noexcept {
    using V = typename Isa::V;
    V acc[NV];
    for (int v = 0; v < NV; ++v)
        acc[v] = init(col + 4u * v);
    for (std::uint32_t k = 0; k < inputs; ++k) {
        const V xk = Isa::splat(x[k]);
        const float* wk = w + static_cast<std::size_t>(k) * out_pad + col;
        for (int v = 0; v < NV; ++v)
            acc[v] = Isa::fma(acc[v], Isa::load(wk + 4 * v), xk);
    }
    for (int v = 0; v < NV; ++v)
        Isa::store(out + col + 4u * v, acc[v]);
}

/// out = init + w^T x with w laid out [inputs][out_pad].
template <class Isa, class Init>
inline void gemv(const float* w, const float* x, std::uint32_t inputs, std::uint32_t out_pad,
                 Init&& init, float* out) noexcept {
    std::uint32_t col = 0;
    while (col < out_pad) {
        const auto vectors = (out_pad - col) / neural_simd::kLanes;
        if (vectors >= 16) {
            gemv_tile<Isa, 16>(w, x, inputs, out_pad, col, init, out);
            col += 64;
        } else if (vectors >= 8) {
            gemv_tile<Isa, 8>(w, x, inputs, out_pad, col, init, out);
            col += 32;
        } else if (vectors >= 4) {
            gemv_tile<Isa, 4>(w, x, inputs, out_pad, col, init, out);
            col += 16;
        } else if (vectors >= 2) {
            gemv_tile<Isa, 2>(w, x, inputs, out_pad, col, init, out);
            col += 8;
        } else {
            gemv_tile<Isa, 1>(w, x, inputs, out_pad, col, init, out);
            col += 4;
        }
    }
}

} // namespace neural_kernels_detail

/// Single-channel dilated causal convolution with tanh, matching TcnCpuShape:
///   y[t] = tanh(bias + sum_{k < K} taps[k] * x[t - k * dilation]).
/// The history is a mirrored ring (every sample stored twice, S apart), so a
/// run of four consecutive frames is always contiguous and nothing is copied.
template <class Isa = neural_simd::DefaultIsa> class TcnSimdKernel {
  public:
    static constexpr std::uint32_t kChunk = 64;

    TcnSimdKernel(TcnCpuShape shape, std::span<const float> taps, float bias) {
        if (!valid_tcn_cpu_shape(shape) || taps.size() != shape.kernel_size)
            return;
        taps_.assign(taps.begin(), taps.end());
        bias_ = bias;
        dilation_ = shape.dilation;
        reach_ = (shape.kernel_size - 1) * shape.dilation;
        size_ = reach_ + kChunk;
        ring_.assign(2 * static_cast<std::size_t>(size_), 0.0f);
        valid_ = true;
    }

    bool valid() const noexcept {
        return valid_;
    }
    bool prepare(const StreamingPrepareContext&) noexcept {
        reset();
        return valid_;
    }
    void reset() noexcept {
        std::fill(ring_.begin(), ring_.end(), 0.0f);
        head_ = 0;
    }
    void process(const float* in, float* out, std::uint32_t frames) noexcept {
        if (!valid_) {
            std::fill(out, out + frames, 0.0f);
            return;
        }
        neural_simd::assert_finite(in, frames);
        while (frames > 0) {
            const auto n = std::min(frames, kChunk);
            chunk(in, out, n);
            in += n;
            out += n;
            frames -= n;
        }
    }

  private:
    void chunk(const float* in, float* out, std::uint32_t n) noexcept {
        using V = typename Isa::V;
        for (std::uint32_t t = 0; t < n; ++t) {
            const auto p = wrap(head_ + t);
            ring_[p] = in[t];
            ring_[p + size_] = in[t];
        }
        const auto taps = static_cast<std::uint32_t>(taps_.size());
        std::uint32_t t = 0;
        // 16 frames per pass: four independent accumulator vectors.
        for (; t + 16 <= n; t += 16) {
            V acc[4];
            for (auto& a : acc)
                a = Isa::splat(bias_);
            for (std::uint32_t k = 0; k < taps; ++k) {
                const V w = Isa::splat(taps_[k]);
                const float* x = ring_.data() + start(t, k);
                for (int v = 0; v < 4; ++v)
                    acc[v] = Isa::fma(acc[v], w, Isa::load(x + 4 * v));
            }
            for (int v = 0; v < 4; ++v)
                Isa::store(out + t + 4 * v, Isa::tanh(acc[v]));
        }
        for (; t + 4 <= n; t += 4) {
            V acc = Isa::splat(bias_);
            for (std::uint32_t k = 0; k < taps; ++k)
                acc = Isa::fma(acc, Isa::splat(taps_[k]), Isa::load(ring_.data() + start(t, k)));
            Isa::store(out + t, Isa::tanh(acc));
        }
        if (t < n) {
            // Tail frames take the same vector path; the mirrored ring keeps
            // the over-read in bounds, and only the valid lanes are stored.
            V acc = Isa::splat(bias_);
            for (std::uint32_t k = 0; k < taps; ++k)
                acc = Isa::fma(acc, Isa::splat(taps_[k]), Isa::load(ring_.data() + start(t, k)));
            float tail[neural_simd::kLanes];
            Isa::store(tail, Isa::tanh(acc));
            std::copy(tail, tail + (n - t), out + t);
        }
        head_ = wrap(head_ + n);
    }
    std::uint32_t wrap(std::uint32_t i) const noexcept {
        return i >= size_ ? i - size_ : i;
    }
    /// Ring index of x[t - k * dilation] for frame t of this chunk.
    std::size_t start(std::uint32_t t, std::uint32_t k) const noexcept {
        const auto back = k * dilation_;
        const auto pos = head_ + t;
        return pos >= back ? wrap(pos - back) : pos + size_ - back;
    }

    bool valid_ = false;
    std::vector<float> taps_;
    float bias_ = 0.0f;
    std::uint32_t dilation_ = 1, reach_ = 0, size_ = 0, head_ = 0;
    std::vector<float> ring_;
};

/// Multi-channel NAM-style TCN: a 1 -> C rechannel, then per layer
///   z = conv_b + sum_{c, t} conv_w[o][c][t] * x_c[n - t * d],  a = tanh(z),
///   x += mix_w * a,
/// and a C -> 1 head. Weights use the layouts of the reference fixture:
/// conv_w [layer][out][in][tap] (tap 0 is the current sample), conv_b
/// [layer][out], mix_w [layer][out][in].
template <class Isa = neural_simd::DefaultIsa> class NamTcnSimdKernel {
  public:
    static constexpr std::uint32_t kChunk = 64;

    struct Weights {
        std::uint32_t channels = 0, kernel = 0;
        std::span<const std::uint32_t> dilations;
        std::span<const float> in_w, conv_w, conv_b, mix_w, head_w;
    };

    static bool valid_weights(const Weights& w) noexcept {
        if (w.channels == 0 || w.channels > 64 || w.kernel == 0 || w.kernel > 64 ||
            w.dilations.empty())
            return false;
        for (const auto d : w.dilations)
            if (d == 0 || d > 65536)
                return false;
        const std::size_t c = w.channels, k = w.kernel, l = w.dilations.size();
        return w.in_w.size() >= c && w.conv_w.size() >= l * c * c * k && w.conv_b.size() >= l * c &&
               w.mix_w.size() >= l * c * c && w.head_w.size() >= c;
    }

    explicit NamTcnSimdKernel(const Weights& w) {
        if (!valid_weights(w))
            return;
        channels_ = w.channels;
        cpad_ = neural_simd::pad_lanes(w.channels);
        kernel_ = w.kernel;
        const auto c = channels_, k = kernel_;
        in_w_.assign(static_cast<std::size_t>(neural_simd::kLanes) * cpad_, 0.0f);
        std::copy(w.in_w.begin(), w.in_w.end(), in_w_.begin());
        head_w_.assign(static_cast<std::size_t>(cpad_) * neural_simd::kLanes, 0.0f);
        for (std::uint32_t i = 0; i < c; ++i)
            head_w_[static_cast<std::size_t>(i) * neural_simd::kLanes] = w.head_w[i];
        for (std::size_t l = 0; l < w.dilations.size(); ++l) {
            Layer layer;
            layer.dilation = w.dilations[l];
            layer.rows = (k - 1) * layer.dilation + kChunk;
            layer.ring.assign(static_cast<std::size_t>(layer.rows) * cpad_, 0.0f);
            layer.conv.assign(static_cast<std::size_t>(k) * cpad_ * cpad_, 0.0f);
            layer.mix.assign(static_cast<std::size_t>(cpad_) * cpad_, 0.0f);
            layer.bias.assign(cpad_, 0.0f);
            const float* cw = w.conv_w.data() + l * c * c * k;
            for (std::uint32_t o = 0; o < c; ++o)
                for (std::uint32_t i = 0; i < c; ++i) {
                    for (std::uint32_t t = 0; t < k; ++t)
                        layer.conv[(static_cast<std::size_t>(t) * cpad_ + i) * cpad_ + o] =
                            cw[(static_cast<std::size_t>(o) * c + i) * k + t];
                    layer.mix[static_cast<std::size_t>(i) * cpad_ + o] =
                        w.mix_w[l * c * c + static_cast<std::size_t>(o) * c + i];
                }
            std::copy_n(w.conv_b.data() + l * c, c, layer.bias.begin());
            layers_.push_back(std::move(layer));
        }
        input_rows_.assign(static_cast<std::size_t>(kChunk) * neural_simd::kLanes, 0.0f);
        act_.assign(static_cast<std::size_t>(kChunk) * cpad_, 0.0f);
        last_.assign(static_cast<std::size_t>(kChunk) * cpad_, 0.0f);
        head_out_.assign(static_cast<std::size_t>(kChunk) * neural_simd::kLanes, 0.0f);
        valid_ = true;
    }

    bool valid() const noexcept {
        return valid_;
    }
    bool prepare(const StreamingPrepareContext&) noexcept {
        reset();
        return valid_;
    }
    void reset() noexcept {
        for (auto& l : layers_) {
            std::fill(l.ring.begin(), l.ring.end(), 0.0f);
            l.head = 0;
        }
    }
    void process(const float* in, float* out, std::uint32_t frames) noexcept {
        if (!valid_) {
            std::fill(out, out + frames, 0.0f);
            return;
        }
        neural_simd::assert_finite(in, frames);
        while (frames > 0) {
            const auto n = std::min(frames, kChunk);
            chunk(in, out, n);
            in += n;
            out += n;
            frames -= n;
        }
    }

  private:
    struct Layer {
        std::uint32_t dilation = 1, rows = 0, head = 0;
        std::vector<float> ring, conv, mix, bias;
    };
    float* row(Layer& l, std::int64_t index) noexcept {
        if (index < 0)
            index += l.rows;
        else if (index >= l.rows)
            index -= l.rows;
        return l.ring.data() + static_cast<std::size_t>(index) * cpad_;
    }

    void chunk(const float* in, float* out, std::uint32_t n) noexcept {
        using V = typename Isa::V;
        namespace ns = neural_simd;
        constexpr auto lanes = ns::kLanes;
        for (std::uint32_t t = 0; t < n; ++t)
            input_rows_[static_cast<std::size_t>(t) * lanes] = in[t];
        const auto zero = [](std::uint32_t, std::uint32_t) { return Isa::splat(0.0f); };
        auto& first = layers_.front();
        ns::dense<Isa>(
            in_w_.data(), 1, lanes, cpad_, n,
            [&](std::uint32_t f, std::uint32_t) -> const float* {
                return input_rows_.data() + static_cast<std::size_t>(f) * lanes;
            },
            zero,
            [&](std::uint32_t f, std::uint32_t col, V v) {
                Isa::store(row(first, std::int64_t(first.head) + f) + col, v);
            });
        for (std::size_t li = 0; li < layers_.size(); ++li) {
            auto& l = layers_[li];
            const std::int64_t base = l.head;
            ns::dense<Isa>(
                l.conv.data(), kernel_, cpad_, cpad_, n,
                [&](std::uint32_t f, std::uint32_t t) -> const float* {
                    return row(l, base + f - std::int64_t(t) * l.dilation);
                },
                [&](std::uint32_t, std::uint32_t col) { return Isa::load(l.bias.data() + col); },
                [&](std::uint32_t f, std::uint32_t col, V v) {
                    Isa::store(act_.data() + static_cast<std::size_t>(f) * cpad_ + col,
                               Isa::tanh(v));
                });
            Layer* next = li + 1 < layers_.size() ? &layers_[li + 1] : nullptr;
            ns::dense<Isa>(
                l.mix.data(), 1, cpad_, cpad_, n,
                [&](std::uint32_t f, std::uint32_t) -> const float* {
                    return act_.data() + static_cast<std::size_t>(f) * cpad_;
                },
                [&](std::uint32_t f, std::uint32_t col) {
                    return Isa::load(row(l, base + f) + col);
                },
                [&](std::uint32_t f, std::uint32_t col, V v) {
                    float* dst = next != nullptr
                                     ? row(*next, std::int64_t(next->head) + f)
                                     : last_.data() + static_cast<std::size_t>(f) * cpad_;
                    Isa::store(dst + col, v);
                });
            l.head = static_cast<std::uint32_t>((l.head + n) % l.rows);
        }
        ns::dense<Isa>(
            head_w_.data(), 1, cpad_, lanes, n,
            [&](std::uint32_t f, std::uint32_t) -> const float* {
                return last_.data() + static_cast<std::size_t>(f) * cpad_;
            },
            zero,
            [&](std::uint32_t f, std::uint32_t, V v) {
                Isa::store(head_out_.data() + static_cast<std::size_t>(f) * lanes, v);
            });
        for (std::uint32_t t = 0; t < n; ++t)
            out[t] = head_out_[static_cast<std::size_t>(t) * lanes];
    }

    bool valid_ = false;
    std::uint32_t channels_ = 0, cpad_ = 0, kernel_ = 0;
    std::vector<float> in_w_, head_w_;
    std::vector<Layer> layers_;
    std::vector<float> input_rows_, act_, last_, head_out_;
};

/// One-layer, scalar-input LSTM or GRU with a dense head and an input skip,
/// matching RecurrentCpuShape:
///   LSTM: gates = bias + w_ih x + W_hh h; c = f c + i g; h = o tanh(c)
///   GRU:  r, z = sigma(bias + w_ih x + W_hh h);
///         n = tanh(bias_n + w_ih_n x + r (W_hh_n h)); h = (1 - z) n + z h
///   y = head_bias + x + head . h
/// Gate rows are [i, f, g, o] (LSTM) or [r, z, n] (GRU); w_hh is row-major
/// [gate row][hidden].
template <class Isa = neural_simd::DefaultIsa> class RecurrentSimdKernel {
  public:
    struct Weights {
        std::span<const float> w_ih, w_hh, bias, head;
        float head_bias = 0.0f;
    };

    RecurrentSimdKernel(RecurrentCpuShape shape, const Weights& w)
        : lstm_(shape.family == RecurrentFamily::Lstm), hidden_(shape.hidden_size),
          hpad_(neural_simd::pad_lanes(shape.hidden_size)),
          gates_(static_cast<std::uint32_t>(recurrent_gate_count(shape.family))),
          rows_(gates_ * hpad_), head_bias_(w.head_bias) {
        const std::size_t g = gates_, h = hidden_;
        if (!valid_recurrent_cpu_shape(shape) || w.w_ih.size() < g * h ||
            w.w_hh.size() < g * h * h || w.bias.size() < g * h || w.head.size() < h) {
            hidden_ = hpad_ = rows_ = 0;
            return;
        }
        w_t_.assign(static_cast<std::size_t>(hpad_) * rows_, 0.0f);
        seed_bias_.assign(rows_, 0.0f);
        seed_wih_.assign(rows_, 0.0f);
        bias_.assign(rows_, 0.0f);
        wih_.assign(rows_, 0.0f);
        for (std::uint32_t g = 0; g < gates_; ++g)
            for (std::uint32_t j = 0; j < hidden_; ++j) {
                const auto source = static_cast<std::size_t>(g) * hidden_ + j;
                const auto lane = static_cast<std::size_t>(g) * hpad_ + j;
                for (std::uint32_t k = 0; k < hidden_; ++k)
                    w_t_[static_cast<std::size_t>(k) * rows_ + lane] = w.w_hh[source * hidden_ + k];
                bias_[lane] = w.bias[source];
                wih_[lane] = w.w_ih[source];
                // The GRU candidate's recurrent product is gated by r, so
                // its rows start from zero.
                if (lstm_ || g < 2) {
                    seed_bias_[lane] = w.bias[source];
                    seed_wih_[lane] = w.w_ih[source];
                }
            }
        head_.assign(hpad_, 0.0f);
        std::copy(w.head.begin(), w.head.end(), head_.begin());
        h_.assign(hpad_, 0.0f);
        cell_.assign(hpad_, 0.0f);
        gate_values_.assign(rows_, 0.0f);
        valid_ = true;
    }

    bool valid() const noexcept {
        return valid_;
    }
    bool prepare(const StreamingPrepareContext&) noexcept {
        reset();
        return valid_;
    }
    void reset() noexcept {
        std::fill(h_.begin(), h_.end(), 0.0f);
        std::fill(cell_.begin(), cell_.end(), 0.0f);
    }
    void process(const float* in, float* out, std::uint32_t frames) noexcept {
        using V = typename Isa::V;
        namespace ns = neural_simd;
        if (!valid_) {
            std::fill(out, out + frames, 0.0f);
            return;
        }
        ns::assert_finite(in, frames);
        for (std::uint32_t n = 0; n < frames; ++n) {
            const float x = in[n];
            const V xv = Isa::splat(x);
            neural_kernels_detail::gemv<Isa>(
                w_t_.data(), h_.data(), hidden_, rows_,
                [&](std::uint32_t col) {
                    return Isa::fma(Isa::load(seed_bias_.data() + col),
                                    Isa::load(seed_wih_.data() + col), xv);
                },
                gate_values_.data());
            const float* g = gate_values_.data();
            V y = Isa::splat(0.0f);
            for (std::uint32_t k = 0; k < hpad_; k += ns::kLanes) {
                V h;
                if (lstm_) {
                    const V ig = Isa::sigmoid(Isa::load(g + k));
                    const V fg = Isa::sigmoid(Isa::load(g + hpad_ + k));
                    const V cg = Isa::tanh(Isa::load(g + 2 * hpad_ + k));
                    const V og = Isa::sigmoid(Isa::load(g + 3 * hpad_ + k));
                    const V c = Isa::fma(Isa::mul(fg, Isa::load(cell_.data() + k)), ig, cg);
                    Isa::store(cell_.data() + k, c);
                    h = Isa::mul(og, Isa::tanh(c));
                } else {
                    const V rg = Isa::sigmoid(Isa::load(g + k));
                    const V zg = Isa::sigmoid(Isa::load(g + hpad_ + k));
                    const V seed = Isa::fma(Isa::load(bias_.data() + 2 * hpad_ + k),
                                            Isa::load(wih_.data() + 2 * hpad_ + k), xv);
                    const V candidate = Isa::tanh(Isa::fma(seed, rg, Isa::load(g + 2 * hpad_ + k)));
                    const V previous = Isa::load(h_.data() + k);
                    h = Isa::fma(Isa::mul(Isa::sub(Isa::splat(1.0f), zg), candidate), zg, previous);
                }
                Isa::store(h_.data() + k, h);
                y = Isa::fma(y, Isa::load(head_.data() + k), h);
            }
            out[n] = head_bias_ + x + Isa::hsum(y);
        }
    }

  private:
    bool valid_ = false;
    bool lstm_ = true;
    std::uint32_t hidden_ = 0, hpad_ = 0, gates_ = 0, rows_ = 0;
    float head_bias_ = 0.0f;
    std::vector<float> w_t_, seed_bias_, seed_wih_, bias_, wih_, head_, h_, cell_, gate_values_;
};

/// Diagonal state-space model matching CompactSsmCpuShape:
///   h[s] = a[s] h[s] + b[s] x;  y = d x + sum_s c[s] h[s].
template <class Isa = neural_simd::DefaultIsa> class SsmSimdKernel {
  public:
    SsmSimdKernel(std::span<const float> a, std::span<const float> b, std::span<const float> c,
                  float d) {
        if (a.empty() || a.size() > 256 || b.size() != a.size() || c.size() != a.size())
            return;
        spad_ = neural_simd::pad_lanes(static_cast<std::uint32_t>(a.size()));
        d_ = d;
        a_.assign(spad_, 0.0f);
        b_.assign(spad_, 0.0f);
        c_.assign(spad_, 0.0f);
        h_.assign(spad_, 0.0f);
        valid_ = true;
        std::copy(a.begin(), a.end(), a_.begin());
        std::copy(b.begin(), b.end(), b_.begin());
        std::copy(c.begin(), c.end(), c_.begin());
    }

    bool valid() const noexcept {
        return valid_;
    }
    bool prepare(const StreamingPrepareContext&) noexcept {
        reset();
        return valid_;
    }
    void reset() noexcept {
        std::fill(h_.begin(), h_.end(), 0.0f);
    }
    void process(const float* in, float* out, std::uint32_t frames) noexcept {
        using V = typename Isa::V;
        constexpr auto lanes = neural_simd::kLanes;
        if (!valid_) {
            std::fill(out, out + frames, 0.0f);
            return;
        }
        neural_simd::assert_finite(in, frames);
        for (std::uint32_t n = 0; n < frames; ++n) {
            const V x = Isa::splat(in[n]);
            // Four partial sums keep the reduction off one dependency chain.
            V y[4] = {Isa::splat(0.0f), Isa::splat(0.0f), Isa::splat(0.0f), Isa::splat(0.0f)};
            for (std::uint32_t s = 0; s < spad_; s += lanes) {
                const V h = Isa::fma(Isa::mul(Isa::load(a_.data() + s), Isa::load(h_.data() + s)),
                                     Isa::load(b_.data() + s), x);
                Isa::store(h_.data() + s, h);
                auto& acc = y[(s / lanes) & 3u];
                acc = Isa::fma(acc, Isa::load(c_.data() + s), h);
            }
            out[n] = d_ * in[n] + Isa::hsum(Isa::add(Isa::add(y[0], y[1]), Isa::add(y[2], y[3])));
        }
    }

  private:
    bool valid_ = false;
    std::uint32_t spad_ = 0;
    float d_ = 0.0f;
    std::vector<float> a_, b_, c_, h_;
};

} // namespace pulp::gpu_audio::detail
