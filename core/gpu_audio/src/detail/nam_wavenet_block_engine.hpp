#pragma once

// Private layer-major block engine for the serialized NAM WaveNet (A1)
// topology that NamTcnArtifact parses.
//
// The per-sample reference walks every layer once per sample with
// matrix-vector products. This engine processes a chunk of frames one layer
// at a time instead, so each layer's weights are reused across the chunk and
// the products become small matrix-matrix tiles (neural_simd::dense):
//
//   - activations are frame rows of `pad_lanes(channels)` floats;
//   - each layer keeps its input history as a ring of rows sized for its
//     dilation reach plus one chunk, so a dilated tap is a row offset and no
//     history is ever copied;
//   - bias and the input mix-in seed the convolution accumulators, the
//     activation and the skip-sum run in one pass over the convolution
//     output, and the 1x1 residual writes straight into the next layer's ring.
//
// Weights are repacked once, off the callback, into [tap][in][out] with zero
// padding, so every padded lane stays exactly zero through the network.
// process() never allocates, locks, or calls into the platform.

#include "neural_simd.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pulp::gpu_audio::detail {

enum class NamActivation : std::uint8_t { Tanh, ReLU, Hardtanh, Sigmoid, Identity };

/// Weights for one dilated layer, in the reference layouts NamTcnArtifact
/// holds after loading: convolution [tap][out][in], residual [out][in].
struct NamWaveNetEngineLayer {
    const float* conv = nullptr;
    const float* conv_bias = nullptr;
    const float* mixin = nullptr; // [out]
    const float* residual = nullptr;
    const float* residual_bias = nullptr;
    std::uint32_t dilation = 1;
};

struct NamWaveNetEngineArray {
    std::uint32_t input_size = 1;
    std::uint32_t channels = 1;
    std::uint32_t kernel = 1;
    std::uint32_t head_size = 1;
    bool gated = false;
    NamActivation activation = NamActivation::Tanh;
    const float* rechannel = nullptr; // [channels][input_size]
    const float* head = nullptr;      // [head_size][channels]
    const float* head_bias = nullptr; // [head_size] or null
    std::vector<NamWaveNetEngineLayer> layers;
};

class NamWaveNetBlockEngine {
  public:
    static constexpr std::uint32_t kChunk = 64;

    /// The engine reproduces the reference only when each later array's
    /// accumulator can be seeded entirely from the previous array's head.
    static bool supports(const std::vector<NamWaveNetEngineArray>& arrays) noexcept {
        if (arrays.empty() || arrays.front().input_size != 1)
            return false;
        for (std::size_t i = 0; i < arrays.size(); ++i) {
            const auto& a = arrays[i];
            if (a.channels == 0 || a.kernel == 0 || a.head_size == 0 || a.layers.empty() ||
                a.rechannel == nullptr || a.head == nullptr)
                return false;
            if (i > 0 && (a.input_size != arrays[i - 1].channels ||
                          (i > 1 && arrays[i - 1].head_size < a.channels)))
                return false;
        }
        return true;
    }

    /// Control thread only: allocates every buffer process() will touch.
    bool configure(const std::vector<NamWaveNetEngineArray>& arrays, float head_scale) {
        arrays_.clear();
        configured_ = false;
        if (!supports(arrays))
            return false;
        head_scale_ = head_scale;
        std::uint32_t widest = 4;
        for (std::size_t index = 0; index < arrays.size(); ++index) {
            const auto& source = arrays[index];
            Array a;
            a.channels = source.channels;
            a.cpad = neural_simd::pad_lanes(source.channels);
            a.in_pad = index == 0 ? neural_simd::kLanes : arrays_.back().cpad;
            a.head_size = source.head_size;
            a.hpad = neural_simd::pad_lanes(source.head_size);
            a.kernel = source.kernel;
            a.gated = source.gated;
            a.activation = source.activation;
            a.zpad = a.gated ? 2 * a.cpad : a.cpad;
            a.rechannel =
                pack_1x1(source.rechannel, source.channels, source.input_size, a.cpad, a.in_pad);
            a.head = pack_1x1(source.head, source.head_size, source.channels, a.hpad, a.cpad);
            a.head_bias.assign(a.hpad, 0.0f);
            if (source.head_bias != nullptr)
                std::copy(source.head_bias, source.head_bias + source.head_size,
                          a.head_bias.begin());
            for (const auto& sl : source.layers) {
                Layer l;
                l.dilation = sl.dilation;
                l.reach = (a.kernel - 1) * sl.dilation;
                l.rows = l.reach + kChunk;
                l.ring.assign(static_cast<std::size_t>(l.rows) * a.cpad, 0.0f);
                // Gated layers place the gate half at lane offset cpad so both
                // halves stay vector-aligned.
                const auto out = a.gated ? 2 * a.channels : a.channels;
                const auto out_lane = [&](std::uint32_t o) {
                    return o < a.channels ? o : a.cpad + (o - a.channels);
                };
                l.conv.assign(static_cast<std::size_t>(a.kernel) * a.cpad * a.zpad, 0.0f);
                for (std::uint32_t k = 0; k < a.kernel; ++k)
                    for (std::uint32_t o = 0; o < out; ++o)
                        for (std::uint32_t i = 0; i < a.channels; ++i)
                            l.conv[(static_cast<std::size_t>(k) * a.cpad + i) * a.zpad +
                                   out_lane(o)] =
                                sl.conv[(static_cast<std::size_t>(k) * out + o) * a.channels + i];
                l.conv_bias.assign(a.zpad, 0.0f);
                l.mixin.assign(a.zpad, 0.0f);
                for (std::uint32_t o = 0; o < out; ++o) {
                    l.conv_bias[out_lane(o)] = sl.conv_bias[o];
                    l.mixin[out_lane(o)] = sl.mixin[o];
                }
                l.residual = pack_1x1(sl.residual, a.channels, a.channels, a.cpad, a.cpad);
                l.residual_bias.assign(a.cpad, 0.0f);
                std::copy(sl.residual_bias, sl.residual_bias + a.channels, l.residual_bias.begin());
                a.layers.push_back(std::move(l));
            }
            a.z.assign(static_cast<std::size_t>(kChunk) * a.zpad, 0.0f);
            a.act.assign(static_cast<std::size_t>(kChunk) * a.cpad, 0.0f);
            a.skip.assign(static_cast<std::size_t>(kChunk) * a.cpad, 0.0f);
            a.out.assign(static_cast<std::size_t>(kChunk) * a.cpad, 0.0f);
            a.head_out.assign(static_cast<std::size_t>(kChunk) * a.hpad, 0.0f);
            widest = std::max(widest, a.cpad);
            arrays_.push_back(std::move(a));
        }
        input_rows_.assign(static_cast<std::size_t>(kChunk) * neural_simd::kLanes, 0.0f);
        condition_.assign(kChunk, 0.0f);
        configured_ = true;
        reset();
        return true;
    }

    bool configured() const noexcept {
        return configured_;
    }

    void reset() noexcept {
        for (auto& a : arrays_)
            for (auto& l : a.layers) {
                std::fill(l.ring.begin(), l.ring.end(), 0.0f);
                l.head = 0;
            }
    }

    /// Any frame count; long blocks are processed in chunks. The output for a
    /// given input stream does not depend on how it is split into calls.
    template <class Isa = neural_simd::DefaultIsa>
    void process(const float* input, float* output, std::uint32_t frames) noexcept {
        while (frames > 0) {
            const auto n = std::min(frames, kChunk);
            process_chunk<Isa>(input, output, n);
            input += n;
            output += n;
            frames -= n;
        }
    }

  private:
    struct Layer {
        std::uint32_t dilation = 1, reach = 0, rows = 0, head = 0;
        std::vector<float> ring, conv, conv_bias, mixin, residual, residual_bias;
    };
    struct Array {
        std::uint32_t channels = 0, cpad = 0, in_pad = 0, head_size = 0, hpad = 0, kernel = 1,
                      zpad = 0;
        bool gated = false;
        NamActivation activation = NamActivation::Tanh;
        std::vector<float> rechannel, head, head_bias;
        std::vector<Layer> layers;
        std::vector<float> z, act, skip, out, head_out;
    };

    /// Repacks a reference [out][in] matrix into a zero-padded
    /// [in_pad][out_pad] single-tap weight block.
    static std::vector<float> pack_1x1(const float* w, std::uint32_t out, std::uint32_t in,
                                       std::uint32_t out_pad, std::uint32_t in_pad) {
        std::vector<float> packed(static_cast<std::size_t>(in_pad) * out_pad, 0.0f);
        for (std::uint32_t o = 0; o < out; ++o)
            for (std::uint32_t i = 0; i < in; ++i)
                packed[static_cast<std::size_t>(i) * out_pad + o] =
                    w[static_cast<std::size_t>(o) * in + i];
        return packed;
    }

    static float* ring_row(Layer& l, std::uint32_t cpad, std::int64_t index) noexcept {
        if (index < 0)
            index += l.rows;
        else if (index >= l.rows)
            index -= l.rows;
        return l.ring.data() + static_cast<std::size_t>(index) * cpad;
    }

    template <class Isa> static typename Isa::V activate(NamActivation a, typename Isa::V v) {
        switch (a) {
        case NamActivation::ReLU:
            return Isa::max(v, Isa::splat(0.0f));
        case NamActivation::Hardtanh:
            return Isa::min(Isa::max(v, Isa::splat(-1.0f)), Isa::splat(1.0f));
        case NamActivation::Sigmoid:
            return Isa::sigmoid(v);
        case NamActivation::Identity:
            return v;
        case NamActivation::Tanh:
            break;
        }
        return Isa::tanh(v);
    }

    template <class Isa>
    void process_chunk(const float* input, float* output, std::uint32_t n) noexcept {
        using V = typename Isa::V;
        namespace ns = neural_simd;
        for (std::uint32_t t = 0; t < n; ++t) {
            input_rows_[static_cast<std::size_t>(t) * ns::kLanes] = input[t];
            condition_[t] = input[t];
        }
        const float* in_rows = input_rows_.data();
        const float* prior_head = nullptr;
        std::uint32_t prior_hpad = 0;
        for (std::size_t ai = 0; ai < arrays_.size(); ++ai) {
            auto& a = arrays_[ai];
            const bool last_array = ai + 1 == arrays_.size();
            const auto in_pad = a.in_pad;
            const auto cpad = a.cpad;

            // Rechannel into the first layer's ring.
            {
                auto& first = a.layers.front();
                ns::dense<Isa>(
                    a.rechannel.data(), 1, in_pad, cpad, n,
                    [&](std::uint32_t f, std::uint32_t) {
                        return in_rows + static_cast<std::size_t>(f) * in_pad;
                    },
                    [](std::uint32_t, std::uint32_t) { return Isa::splat(0.0f); },
                    [&](std::uint32_t f, std::uint32_t col, V v) {
                        Isa::store(ring_row(first, cpad, std::int64_t(first.head) + f) + col, v);
                    });
            }

            // Skip-sum seed: the previous array's head, or zero.
            for (std::uint32_t t = 0; t < n; ++t) {
                float* s = a.skip.data() + static_cast<std::size_t>(t) * cpad;
                std::fill(s, s + cpad, 0.0f);
                if (prior_head != nullptr)
                    std::copy(prior_head + static_cast<std::size_t>(t) * prior_hpad,
                              prior_head + static_cast<std::size_t>(t) * prior_hpad +
                                  std::min(a.channels, prior_hpad),
                              s);
            }

            for (std::size_t li = 0; li < a.layers.size(); ++li) {
                auto& l = a.layers[li];
                const auto zpad = a.zpad;
                const auto kernel = a.kernel;
                const auto dilation = l.dilation;
                const std::int64_t base = l.head;

                // Dilated convolution, seeded with bias + mix-in * condition.
                ns::dense<Isa>(
                    l.conv.data(), kernel, cpad, zpad, n,
                    [&](std::uint32_t f, std::uint32_t k) -> const float* {
                        return ring_row(l, cpad,
                                        base + f - std::int64_t(kernel - 1 - k) * dilation);
                    },
                    [&](std::uint32_t f, std::uint32_t col) {
                        return Isa::fma(Isa::load(l.conv_bias.data() + col),
                                        Isa::load(l.mixin.data() + col), Isa::splat(condition_[f]));
                    },
                    [&](std::uint32_t f, std::uint32_t col, V v) {
                        Isa::store(a.z.data() + static_cast<std::size_t>(f) * zpad + col, v);
                    });

                // Activation (and gate) plus the skip sum, one pass.
                for (std::uint32_t t = 0; t < n; ++t) {
                    const float* z = a.z.data() + static_cast<std::size_t>(t) * zpad;
                    float* act = a.act.data() + static_cast<std::size_t>(t) * cpad;
                    float* skip = a.skip.data() + static_cast<std::size_t>(t) * cpad;
                    for (std::uint32_t c = 0; c < cpad; c += ns::kLanes) {
                        V h = activate<Isa>(a.activation, Isa::load(z + c));
                        if (a.gated)
                            h = Isa::mul(h, Isa::sigmoid(Isa::load(z + cpad + c)));
                        Isa::store(act + c, h);
                        Isa::store(skip + c, Isa::add(Isa::load(skip + c), h));
                    }
                }

                // Residual 1x1 into the next layer's ring (or the array
                // output). The final layer of the final array feeds nothing.
                const bool last_layer = li + 1 == a.layers.size();
                if (!(last_layer && last_array)) {
                    Layer* next = last_layer ? nullptr : &a.layers[li + 1];
                    ns::dense<Isa>(
                        l.residual.data(), 1, cpad, cpad, n,
                        [&](std::uint32_t f, std::uint32_t) -> const float* {
                            return a.act.data() + static_cast<std::size_t>(f) * cpad;
                        },
                        [&](std::uint32_t f, std::uint32_t col) {
                            return Isa::add(Isa::load(ring_row(l, cpad, base + f) + col),
                                            Isa::load(l.residual_bias.data() + col));
                        },
                        [&](std::uint32_t f, std::uint32_t col, V v) {
                            float* dst = next != nullptr
                                             ? ring_row(*next, cpad, std::int64_t(next->head) + f)
                                             : a.out.data() + static_cast<std::size_t>(f) * cpad;
                            Isa::store(dst + col, v);
                        });
                }
                l.head = static_cast<std::uint32_t>((l.head + n) % l.rows);
            }

            // Head rechannel of the skip sum.
            ns::dense<Isa>(
                a.head.data(), 1, cpad, a.hpad, n,
                [&](std::uint32_t f, std::uint32_t) -> const float* {
                    return a.skip.data() + static_cast<std::size_t>(f) * cpad;
                },
                [&](std::uint32_t, std::uint32_t col) {
                    return Isa::load(a.head_bias.data() + col);
                },
                [&](std::uint32_t f, std::uint32_t col, V v) {
                    Isa::store(a.head_out.data() + static_cast<std::size_t>(f) * a.hpad + col, v);
                });
            in_rows = a.out.data();
            prior_head = a.head_out.data();
            prior_hpad = a.hpad;
        }
        for (std::uint32_t t = 0; t < n; ++t)
            output[t] = head_scale_ * prior_head[static_cast<std::size_t>(t) * prior_hpad];
    }

    std::vector<Array> arrays_;
    std::vector<float> input_rows_, condition_;
    float head_scale_ = 1.0f;
    bool configured_ = false;
};

} // namespace pulp::gpu_audio::detail
