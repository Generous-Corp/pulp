#pragma once

// Reference per-family CPU kernels for GpuNeuralRealtimeNode tests. Each
// kernel is a consumer-owned stateful model behind the fn-pointer seam the
// private adapters accept: storage is allocated at construction, and
// process() is allocation-free. Sizes are configurable so the same kernels
// serve the contract tests (small) and the shadow-cost measurement (shipping
// sizes).

#include "detail/gpu_neural_model.hpp"
#include "detail/wavenet_realtime_channel.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#if defined(_MSC_VER)
#define PULP_NEURAL_TEST_NOINLINE __declspec(noinline)
#else
#define PULP_NEURAL_TEST_NOINLINE __attribute__((noinline))
#endif

namespace pulp::gpu_audio::neural_test {

using detail::NeuralCpuKernel;

/// Deterministic weights in [-scale, scale].
class WeightSource {
  public:
    explicit WeightSource(std::uint32_t seed) : state_(seed ? seed : 1u) {}
    float next(float scale) noexcept {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;
        return scale * (static_cast<float>(state_ & 0xffffffu) / 8388608.0f - 1.0f);
    }
    void fill(std::vector<float>& values, float scale) {
        for (auto& value : values)
            value = next(scale);
    }

  private:
    std::uint32_t state_;
};

inline float sigmoid(float x) noexcept {
    return 1.0f / (1.0f + std::exp(-x));
}

/// Adapts a kernel object with reset()/process() to the fn-pointer seam.
template <class State> NeuralCpuKernel kernel_for(State& state) {
    return {&state,
            [](void* self, const detail::StreamingPrepareContext&) noexcept {
                static_cast<State*>(self)->reset();
                return true;
            },
            [](void* self, const float* in, float* out, std::uint32_t n) noexcept {
                static_cast<State*>(self)->process(in, out, n);
            },
            [](void* self) noexcept { static_cast<State*>(self)->reset(); },
            [](void*) noexcept { return true; },
            [](void*) noexcept { return true; }};
}

/// Stateful FIR. Serves as the WaveNet-family shadow in parity tests so the
/// fake GPU channel can compute exactly the same function.
struct FirKernel {
    static constexpr std::array<float, 5> taps{0.5f, -0.3f, 0.2f, -0.1f, 0.05f};
    std::array<float, taps.size() - 1> z{};
    void reset() noexcept {
        z.fill(0.0f);
    }
    PULP_NEURAL_TEST_NOINLINE float step(float x) noexcept {
        float y = taps[0] * x;
        for (std::size_t i = 0; i < z.size(); ++i)
            y += taps[i + 1] * z[i];
        for (std::size_t i = z.size() - 1; i > 0; --i)
            z[i] = z[i - 1];
        z[0] = x;
        return y;
    }
    void process(const float* in, float* out, std::uint32_t n) noexcept {
        for (std::uint32_t i = 0; i < n; ++i)
            out[i] = step(in[i]);
    }
};

/// Single-layer scalar dilated causal TCN matching TcnCpuShape.
struct TcnKernel {
    std::vector<float> taps, ring;
    float bias = 0.0f;
    std::uint32_t dilation = 1, cursor = 0;
    TcnKernel(detail::TcnCpuShape shape, std::uint32_t seed)
        : taps(shape.kernel_size), ring(detail::tcn_cpu_receptive_field(shape), 0.0f),
          dilation(shape.dilation) {
        WeightSource w(seed);
        w.fill(taps, 0.6f);
        bias = w.next(0.1f);
    }
    void reset() noexcept {
        std::fill(ring.begin(), ring.end(), 0.0f);
        cursor = 0;
    }
    void process(const float* in, float* out, std::uint32_t n) noexcept {
        const auto size = static_cast<std::uint32_t>(ring.size());
        for (std::uint32_t i = 0; i < n; ++i) {
            ring[cursor] = in[i];
            float y = bias;
            for (std::uint32_t k = 0; k < taps.size(); ++k)
                y += taps[k] * ring[(cursor + size - k * dilation) % size];
            out[i] = std::tanh(y);
            cursor = (cursor + 1) % size;
        }
    }
};

/// Multi-layer, multi-channel dilated TCN (NAM-TCN-like): 1 -> C rechannel,
/// L residual layers of C x C x K dilated convolutions with tanh, C -> 1 head.
struct NamTcnKernel {
    std::uint32_t channels, kernel, layers, cursor = 0, ring_size = 0;
    std::vector<std::uint32_t> dilations;
    std::vector<float> in_w, conv_w, conv_b, mix_w, head_w, rings, act, x;
    NamTcnKernel(std::uint32_t c, std::uint32_t k, std::uint32_t l, std::uint32_t seed)
        : channels(c), kernel(k), layers(l) {
        std::uint32_t reach = 0;
        for (std::uint32_t i = 0; i < l; ++i) {
            dilations.push_back(1u << (i % 10));
            reach = std::max(reach, (k - 1) * dilations.back());
        }
        ring_size = reach + 1;
        WeightSource w(seed);
        const float conv_scale = 1.0f / std::sqrt(static_cast<float>(c * k));
        in_w.resize(c);
        conv_w.resize(static_cast<std::size_t>(l) * c * c * k);
        conv_b.resize(static_cast<std::size_t>(l) * c);
        mix_w.resize(static_cast<std::size_t>(l) * c * c);
        head_w.resize(c);
        w.fill(in_w, 0.5f);
        w.fill(conv_w, conv_scale);
        w.fill(conv_b, 0.05f);
        w.fill(mix_w, 1.0f / std::sqrt(static_cast<float>(c)));
        w.fill(head_w, 0.5f);
        rings.assign(static_cast<std::size_t>(l) * c * ring_size, 0.0f);
        act.assign(c, 0.0f);
        x.assign(c, 0.0f);
    }
    void reset() noexcept {
        std::fill(rings.begin(), rings.end(), 0.0f);
        cursor = 0;
    }
    void process(const float* in, float* out, std::uint32_t n) noexcept {
        for (std::uint32_t i = 0; i < n; ++i) {
            for (std::uint32_t c = 0; c < channels; ++c)
                x[c] = in_w[c] * in[i];
            for (std::uint32_t layer = 0; layer < layers; ++layer) {
                float* ring = rings.data() + static_cast<std::size_t>(layer) * channels * ring_size;
                for (std::uint32_t c = 0; c < channels; ++c)
                    ring[c * ring_size + cursor] = x[c];
                const auto d = dilations[layer];
                const float* wl =
                    conv_w.data() + static_cast<std::size_t>(layer) * channels * channels * kernel;
                for (std::uint32_t o = 0; o < channels; ++o) {
                    float z = conv_b[layer * channels + o];
                    for (std::uint32_t c = 0; c < channels; ++c) {
                        const float* r = ring + c * ring_size;
                        const float* wk =
                            wl + (static_cast<std::size_t>(o) * channels + c) * kernel;
                        for (std::uint32_t t = 0; t < kernel; ++t)
                            z += wk[t] * r[(cursor + ring_size - t * d) % ring_size];
                    }
                    act[o] = std::tanh(z);
                }
                const float* ml =
                    mix_w.data() + static_cast<std::size_t>(layer) * channels * channels;
                for (std::uint32_t o = 0; o < channels; ++o) {
                    float r = 0.0f;
                    for (std::uint32_t c = 0; c < channels; ++c)
                        r += ml[o * channels + c] * act[c];
                    x[o] += r;
                }
            }
            float y = 0.0f;
            for (std::uint32_t c = 0; c < channels; ++c)
                y += head_w[c] * x[c];
            out[i] = y;
            cursor = (cursor + 1) % ring_size;
        }
    }
};

/// Diagonal compact SSM matching CompactSsmCpuShape.
struct SsmKernel {
    std::vector<float> a, b, c, h;
    float d = 0.0f;
    SsmKernel(std::uint32_t states, std::uint32_t seed)
        : a(states), b(states), c(states), h(states) {
        WeightSource w(seed);
        for (auto& value : a)
            value = 0.5f + 0.49f * w.next(1.0f);
        w.fill(b, 0.5f);
        w.fill(c, 1.0f / std::sqrt(static_cast<float>(states)));
        d = w.next(0.3f);
    }
    void reset() noexcept {
        std::fill(h.begin(), h.end(), 0.0f);
    }
    void process(const float* in, float* out, std::uint32_t n) noexcept {
        const auto states = h.size();
        for (std::uint32_t i = 0; i < n; ++i) {
            float y = d * in[i];
            for (std::size_t s = 0; s < states; ++s) {
                h[s] = a[s] * h[s] + b[s] * in[i];
                y += c[s] * h[s];
            }
            out[i] = y;
        }
    }
};

/// One-layer scalar-input LSTM or GRU with a dense head and skip, matching
/// RecurrentCpuShape (NAM/GuitarML-style amp models).
struct RecurrentKernel {
    bool lstm;
    std::uint32_t hidden;
    std::vector<float> w_ih, w_hh, bias, head, h, cell, gates, h_next;
    float head_bias = 0.0f;
    RecurrentKernel(detail::RecurrentFamily family, std::uint32_t hidden_size, std::uint32_t seed)
        : lstm(family == detail::RecurrentFamily::Lstm), hidden(hidden_size) {
        const auto gate_count = detail::recurrent_gate_count(family);
        WeightSource w(seed);
        w_ih.resize(gate_count * hidden);
        w_hh.resize(gate_count * hidden * hidden);
        bias.resize(gate_count * hidden);
        head.resize(hidden);
        w.fill(w_ih, 0.8f);
        w.fill(w_hh, 1.0f / std::sqrt(static_cast<float>(hidden)));
        w.fill(bias, 0.1f);
        w.fill(head, 1.0f / std::sqrt(static_cast<float>(hidden)));
        head_bias = w.next(0.05f);
        h.assign(hidden, 0.0f);
        cell.assign(hidden, 0.0f);
        gates.assign(gate_count * hidden, 0.0f);
        h_next.assign(hidden, 0.0f);
    }
    void reset() noexcept {
        std::fill(h.begin(), h.end(), 0.0f);
        std::fill(cell.begin(), cell.end(), 0.0f);
    }
    void process(const float* in, float* out, std::uint32_t n) noexcept {
        const auto rows = gates.size();
        for (std::uint32_t i = 0; i < n; ++i) {
            const float x = in[i];
            for (std::size_t r = 0; r < rows; ++r) {
                float g = bias[r] + w_ih[r] * x;
                // GRU applies its reset gate to the candidate's recurrent term.
                if (lstm || r < 2u * hidden) {
                    const float* wr = w_hh.data() + r * hidden;
                    for (std::uint32_t k = 0; k < hidden; ++k)
                        g += wr[k] * h[k];
                }
                gates[r] = g;
            }
            if (lstm) {
                for (std::uint32_t k = 0; k < hidden; ++k) {
                    const float ig = sigmoid(gates[k]);
                    const float fg = sigmoid(gates[hidden + k]);
                    const float cg = std::tanh(gates[2 * hidden + k]);
                    const float og = sigmoid(gates[3 * hidden + k]);
                    cell[k] = fg * cell[k] + ig * cg;
                    h_next[k] = og * std::tanh(cell[k]);
                }
            } else {
                for (std::uint32_t k = 0; k < hidden; ++k) {
                    const float rg = sigmoid(gates[k]);
                    const float zg = sigmoid(gates[hidden + k]);
                    float candidate = gates[2 * hidden + k];
                    const float* wr = w_hh.data() + (2u * hidden + k) * hidden;
                    float recurrent = 0.0f;
                    for (std::uint32_t j = 0; j < hidden; ++j)
                        recurrent += wr[j] * h[j];
                    candidate = std::tanh(candidate + rg * recurrent);
                    h_next[k] = (1.0f - zg) * candidate + zg * h[k];
                }
            }
            std::copy(h_next.begin(), h_next.end(), h.begin());
            float y = head_bias + x;
            for (std::uint32_t k = 0; k < hidden; ++k)
                y += head[k] * h[k];
            out[i] = y;
        }
    }
};

/// NAM-style WaveNet on the CPU: layer arrays of C channels with K-tap
/// dilated convolutions, input mix-in, tanh, 1x1 residual and a summed head.
/// Used only for the shadow-cost measurement.
struct WaveNetKernel {
    struct Array {
        std::uint32_t in_channels, channels, head_size;
        std::vector<std::uint32_t> dilations;
        std::vector<float> rechannel, conv, conv_b, mixin, one_by_one, one_b, head_rechannel;
        std::vector<float> rings;
        std::uint32_t ring_size = 0;
    };
    std::uint32_t kernel = 3, cursor = 0;
    std::vector<Array> arrays;
    std::vector<float> x, z, head_accum, next_x;
    float head_scale = 0.02f;

    WaveNetKernel(std::span<const std::array<std::uint32_t, 3>> shape, std::uint32_t layers,
                  std::uint32_t seed) {
        WeightSource w(seed);
        std::uint32_t widest = 1;
        for (const auto& [in_channels, channels, head_size] : shape) {
            Array a;
            a.in_channels = in_channels;
            a.channels = channels;
            a.head_size = head_size;
            std::uint32_t reach = 0;
            for (std::uint32_t i = 0; i < layers; ++i) {
                a.dilations.push_back(1u << i);
                reach = std::max(reach, (kernel - 1) * a.dilations.back());
            }
            a.ring_size = reach + 1;
            a.rechannel.resize(static_cast<std::size_t>(channels) * in_channels);
            a.conv.resize(static_cast<std::size_t>(layers) * channels * channels * kernel);
            a.conv_b.resize(static_cast<std::size_t>(layers) * channels);
            a.mixin.resize(static_cast<std::size_t>(layers) * channels);
            a.one_by_one.resize(static_cast<std::size_t>(layers) * channels * channels);
            a.one_b.resize(static_cast<std::size_t>(layers) * channels);
            a.head_rechannel.resize(static_cast<std::size_t>(head_size) * channels);
            w.fill(a.rechannel, 0.5f);
            w.fill(a.conv, 1.0f / std::sqrt(static_cast<float>(channels * kernel)));
            w.fill(a.conv_b, 0.05f);
            w.fill(a.mixin, 0.3f);
            w.fill(a.one_by_one, 1.0f / std::sqrt(static_cast<float>(channels)));
            w.fill(a.one_b, 0.05f);
            w.fill(a.head_rechannel, 1.0f / std::sqrt(static_cast<float>(channels)));
            a.rings.assign(static_cast<std::size_t>(layers) * channels * a.ring_size, 0.0f);
            widest = std::max({widest, channels, in_channels, head_size});
            arrays.push_back(std::move(a));
        }
        x.assign(widest, 0.0f);
        z.assign(widest, 0.0f);
        head_accum.assign(widest, 0.0f);
        next_x.assign(widest, 0.0f);
    }
    void reset() noexcept {
        for (auto& a : arrays)
            std::fill(a.rings.begin(), a.rings.end(), 0.0f);
        cursor = 0;
    }
    void process(const float* in, float* out, std::uint32_t n) noexcept {
        for (std::uint32_t i = 0; i < n; ++i) {
            const float condition = in[i];
            x[0] = condition;
            for (auto& a : arrays) {
                const auto C = a.channels;
                for (std::uint32_t o = 0; o < C; ++o) {
                    float v = 0.0f;
                    for (std::uint32_t c = 0; c < a.in_channels; ++c)
                        v += a.rechannel[o * a.in_channels + c] * x[c];
                    next_x[o] = v;
                }
                std::copy_n(next_x.begin(), C, x.begin());
                std::fill_n(head_accum.begin(), C, 0.0f);
                const auto ring_cursor = cursor % a.ring_size;
                for (std::size_t layer = 0; layer < a.dilations.size(); ++layer) {
                    float* ring = a.rings.data() + layer * C * a.ring_size;
                    for (std::uint32_t c = 0; c < C; ++c)
                        ring[c * a.ring_size + ring_cursor] = x[c];
                    const auto d = a.dilations[layer];
                    const float* wl = a.conv.data() + layer * C * C * kernel;
                    for (std::uint32_t o = 0; o < C; ++o) {
                        float v = a.conv_b[layer * C + o] + a.mixin[layer * C + o] * condition;
                        for (std::uint32_t c = 0; c < C; ++c) {
                            const float* r = ring + c * a.ring_size;
                            const float* wk = wl + (static_cast<std::size_t>(o) * C + c) * kernel;
                            for (std::uint32_t t = 0; t < kernel; ++t)
                                v += wk[t] * r[(ring_cursor + a.ring_size - t * d) % a.ring_size];
                        }
                        z[o] = std::tanh(v);
                        head_accum[o] += z[o];
                    }
                    const float* m = a.one_by_one.data() + layer * C * C;
                    for (std::uint32_t o = 0; o < C; ++o) {
                        float v = a.one_b[layer * C + o];
                        for (std::uint32_t c = 0; c < C; ++c)
                            v += m[o * C + c] * z[c];
                        x[o] += v;
                    }
                }
                for (std::uint32_t o = 0; o < a.head_size; ++o) {
                    float v = 0.0f;
                    for (std::uint32_t c = 0; c < C; ++c)
                        v += a.head_rechannel[o * C + c] * head_accum[c];
                    next_x[o] = v;
                }
                std::copy_n(next_x.begin(), a.head_size, x.begin());
            }
            out[i] = head_scale * x[0];
            ++cursor;
        }
    }
};

/// Fake WaveNet provider channel: computes the same FIR as FirKernel at
/// submission, in stream order, and returns it on the next receive. Faults
/// are controlled through a shared plan.
struct ChannelPlan {
    bool fail_completions = false;
    bool reject_submissions = false;
    bool starve = false; // completions withheld (a hung provider)
    bool late = false;   // completions arrive, each too late for its consumer
    bool lost = false;   // the device is gone: lost() is true, work fails
    // One slow block: this sequence completes only once cleared.
    std::uint64_t hold = std::numeric_limits<std::uint64_t>::max();
    bool release_ok = true;
    bool sequence_gap_seen = false;
    // Scales the provider's output so a test can tell GPU blocks from
    // shadow blocks; 1 makes the two interchangeable.
    float gain = 1.0f;
    std::uint64_t submissions = 0;
    std::uint32_t releases = 0;
};

class FirChannel final : public detail::WaveNetRealtimeChannel {
  public:
    FirChannel(ChannelPlan& plan, std::uint32_t block) : plan_(plan) {
        for (auto& slot : slots_)
            slot.data.assign(block, 0.0f);
    }
    bool submit(std::span<const float> input,
                const detail::SharedIoSubmissionTicket& ticket) noexcept override {
        const auto sequence = ticket.sequence;
        if (plan_.reject_submissions || plan_.lost)
            return false;
        auto slot = std::find_if(slots_.begin(), slots_.end(), [](auto& s) { return !s.active; });
        if (slot == slots_.end())
            return false;
        if (has_expected_ && sequence != expected_)
            plan_.sequence_gap_seen = true;
        has_expected_ = true;
        expected_ = sequence + 1;
        for (std::size_t i = 0; i < input.size(); ++i)
            slot->data[i] = fir_.step(input[i]);
        slot->sequence = sequence;
        slot->ticket = ticket;
        slot->order = next_order_++;
        slot->active = true;
        ++plan_.submissions;
        return true;
    }
    void service(std::uint64_t) noexcept override {}
    void service_until(std::uint64_t, std::uint64_t) noexcept override {}
    std::optional<detail::WaveNetChannelCompletion>
    receive(std::span<float> output) noexcept override {
        if (plan_.starve)
            return {};
        Slot* oldest = nullptr;
        for (auto& slot : slots_)
            if (slot.active && slot.sequence != plan_.hold &&
                (oldest == nullptr || slot.order < oldest->order))
                oldest = &slot;
        if (oldest == nullptr)
            return {};
        oldest->active = false;
        if (plan_.fail_completions || plan_.lost)
            return detail::WaveNetChannelCompletion{
                GpuWaveNetBlockResult{oldest->sequence, GpuWaveNetBlockStatus::ProviderFailed,
                                      false},
                oldest->ticket};
        for (std::size_t i = 0; i < output.size(); ++i)
            output[i] = plan_.gain * oldest->data[i];
        return detail::WaveNetChannelCompletion{
            GpuWaveNetBlockResult{oldest->sequence, GpuWaveNetBlockStatus::GpuDelivered,
                                  plan_.late},
            oldest->ticket};
    }
    bool lost() const noexcept override {
        return plan_.lost;
    }
    bool release() noexcept override {
        ++plan_.releases;
        if (!plan_.release_ok)
            return false;
        for (auto& slot : slots_)
            slot.active = false;
        return true;
    }

  private:
    struct Slot {
        std::vector<float> data;
        std::uint64_t sequence = 0, order = 0;
        detail::SharedIoSubmissionTicket ticket{};
        bool active = false;
    };
    ChannelPlan& plan_;
    FirKernel fir_;
    std::array<Slot, 8> slots_{};
    std::uint64_t expected_ = 0, next_order_ = 0;
    bool has_expected_ = false;
};

/// Smallest WaveNet descriptor the backend accepts; the fake channel ignores
/// its weights and computes the FIR instead.
struct WaveNetShape {
    GpuWaveNetLayerDescriptor layer{.input_size = 1,
                                    .condition_size = 1,
                                    .channels = 1,
                                    .kernel = 2,
                                    .head_size = 1,
                                    .dilation = 1,
                                    .tanh_activation = true};
    std::array<float, 9> weights{};
    GpuWaveNetSession::Config session(std::uint32_t block, std::uint32_t sample_rate) const {
        return {.descriptor = {.block_size = block,
                               .sample_rate = sample_rate,
                               .stream_instances = 1,
                               .layers = {&layer, 1},
                               .weight_count = weights.size()},
                .weights = weights,
                .slots = 4};
    }
};

inline constexpr std::string_view kSha =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

/// Builds per-family requests with valid manifests. Owns the kernel states:
/// one per channel, so channels never share activations.
struct FamilyFixture {
    NeuralFamily family;
    std::uint32_t channels;
    std::uint32_t block;
    std::uint32_t sample_rate = 48000;
    bool nam_tcn = false;
    detail::TcnCpuShape tcn{.channels = 1, .kernel_size = 4, .dilation = 3, .layers = 1};
    detail::RecurrentCpuShape recurrent{};
    detail::CompactSsmCpuShape ssm{
        .state_size = 16, .input_size = 1, .output_size = 1, .layers = 1};
    std::uint32_t hidden = 8;
    std::vector<std::unique_ptr<TcnKernel>> tcn_states;
    std::vector<std::unique_ptr<NamTcnKernel>> nam_tcn_states;
    std::vector<std::unique_ptr<SsmKernel>> ssm_states;
    std::vector<std::unique_ptr<RecurrentKernel>> recurrent_states;
    std::vector<std::unique_ptr<FirKernel>> fir_states;
    std::vector<std::unique_ptr<WaveNetKernel>> wavenet_states;
    std::vector<NeuralCpuKernel> kernels;
    std::string state_schema;
    std::uint64_t state_bytes = 0;

    FamilyFixture(NeuralFamily f, std::uint32_t ch, std::uint32_t n, bool use_nam_tcn = false)
        : family(f), channels(ch), block(n), nam_tcn(use_nam_tcn) {
        recurrent = {.family = f == NeuralFamily::Gru ? detail::RecurrentFamily::Gru
                                                      : detail::RecurrentFamily::Lstm,
                     .input_size = 1,
                     .hidden_size = hidden,
                     .layers = 1,
                     .directions = 1,
                     .output_size = 1};
    }

    std::string_view architecture() const {
        switch (family) {
        case NeuralFamily::WaveNet:
            return "nam.wavenet";
        case NeuralFamily::Tcn:
            return nam_tcn ? detail::kNamTcnArchitecture : "tcn.causal";
        case NeuralFamily::Ssm:
            return "ssm.diagonal";
        case NeuralFamily::Lstm:
            return "rnn.lstm";
        case NeuralFamily::Gru:
            return "rnn.gru";
        default:
            return "custom";
        }
    }

    /// Creates fresh kernel states (seeded identically per channel index so a
    /// second fixture reproduces the same function).
    void build_kernels() {
        kernels.clear();
        state_schema.clear();
        state_bytes = 0;
        for (std::uint32_t ch = 0; ch < channels; ++ch) {
            const auto seed = 1000u + ch;
            switch (family) {
            case NeuralFamily::Tcn:
                if (nam_tcn) {
                    nam_tcn_states.push_back(std::make_unique<NamTcnKernel>(4, 3, 4, seed));
                    kernels.push_back(kernel_for(*nam_tcn_states.back()));
                } else {
                    tcn_states.push_back(std::make_unique<TcnKernel>(tcn, seed));
                    kernels.push_back(kernel_for(*tcn_states.back()));
                    state_schema = "causal-tcn-v1";
                    state_bytes = detail::tcn_cpu_state_bytes(tcn);
                }
                break;
            case NeuralFamily::Ssm:
                ssm_states.push_back(std::make_unique<SsmKernel>(ssm.state_size, seed));
                kernels.push_back(kernel_for(*ssm_states.back()));
                state_schema = "compact-ssm-diagonal-v1";
                state_bytes = detail::compact_ssm_cpu_state_bytes(ssm);
                break;
            case NeuralFamily::Lstm:
            case NeuralFamily::Gru:
                recurrent_states.push_back(std::make_unique<RecurrentKernel>(
                    recurrent.family, recurrent.hidden_size, seed));
                kernels.push_back(kernel_for(*recurrent_states.back()));
                state_schema = std::string(detail::recurrent_state_schema(recurrent.family));
                state_bytes = detail::recurrent_state_bytes(recurrent);
                break;
            default:
                fir_states.push_back(std::make_unique<FirKernel>());
                kernels.push_back(kernel_for(*fir_states.back()));
                break;
            }
        }
    }

    detail::NeuralModelManifest manifest() const {
        return {.model_id = "pulp.test.neural",
                .architecture = architecture(),
                .model_version = "1",
                .artifact_id = "pulp.test.neural.weights",
                .artifact_sha256 = kSha,
                .license = "MIT",
                .runtime = "pulp.neural.test.v1",
                .state_schema = state_schema,
                .artifact_size_bytes = 4096,
                .sample_rate_hz = sample_rate,
                .state_bytes = state_bytes,
                .state_schema_version = state_bytes != 0 ? 1u : 0u,
                .redistributable = true};
    }

    detail::NeuralModelRequest request() {
        if (kernels.empty())
            build_kernels();
        detail::NeuralModelRequest r;
        r.family = family;
        r.manifest = manifest();
        r.block_size = block;
        r.tcn = tcn;
        r.recurrent = recurrent;
        r.ssm = ssm;
        r.channel_kernels = kernels;
        return r;
    }
};

/// WaveNet backend over fake FIR channels sharing one plan.
inline std::unique_ptr<detail::StreamingBackend>
make_fake_wavenet_backend(ChannelPlan& plan, const WaveNetShape& shape, std::uint32_t channels,
                          std::uint32_t block, std::uint32_t max_inflight = 1,
                          bool fail_factory = false, std::uint32_t sample_rate = 48000) {
    return std::make_unique<detail::WaveNetStreamingBackend>(
        detail::WaveNetStreamingBackend::Config{.session = shape.session(block, sample_rate),
                                                .channels = channels,
                                                .max_inflight = max_inflight},
        [&plan, block,
         fail_factory](std::uint32_t) -> std::unique_ptr<detail::WaveNetRealtimeChannel> {
            if (fail_factory)
                return nullptr;
            return std::make_unique<FirChannel>(plan, block);
        });
}

/// Continuous broadband stimulus: seeded noise plus a tone.
inline float stimulus(std::uint32_t ch, std::uint64_t frame) {
    std::uint32_t seed =
        0x9e3779b9u ^ static_cast<std::uint32_t>(frame * 2654435761u) ^ (ch * 0x85ebca6bu);
    seed ^= seed >> 15;
    seed *= 0x2c1b3c6du;
    seed ^= seed >> 12;
    const float noise = static_cast<float>(static_cast<double>(seed >> 8) / 8388608.0 - 1.0);
    const float tone =
        static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * (220.0 + 110.0 * ch) *
                                    static_cast<double>(frame) / 48000.0));
    return 0.25f * noise + 0.5f * tone;
}

} // namespace pulp::gpu_audio::neural_test
