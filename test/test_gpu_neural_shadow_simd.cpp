// GpuNeuralRealtimeNode with WP-S's SIMD CPU kernels as the shadow model.
//
// The SIMD kernels are built from the same weights as the scalar reference
// kernels in gpu_neural_test_kernels.hpp, so a node running them must agree
// with one running the scalar-ISA build of the same kernel. Non-finite input
// goes through the node's sanitizer before either kernel sees it.

#include "gpu_neural_test_kernels.hpp"

#include "detail/gpu_neural_realtime_test_access.hpp"
#include "detail/nam_tcn_artifact.hpp"
#include "detail/neural_cpu_kernels.hpp"
#include "harness/rt_allocation_probe.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <pulp/gpu_audio/gpu_audio_transport.hpp>
#include <pulp/gpu_audio/gpu_neural_realtime_node.hpp>
#include <pulp/runtime/crypto.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace pulp;
using namespace pulp::gpu_audio;
using namespace pulp::gpu_audio::neural_test;
namespace ns = pulp::gpu_audio::detail::neural_simd;
using Policy = GpuNeuralRealtimeNode::ShadowPolicy;

namespace {

constexpr std::uint32_t kChannels = 2;
constexpr std::uint32_t kBlock = 64;
constexpr std::uint32_t kLead = 2;

/// SIMD kernels for one CPU-only family, built from a FamilyFixture's
/// scalar reference weights, with a chosen instruction set.
template <class Isa> struct SimdFamily {
    std::vector<std::unique_ptr<detail::RecurrentSimdKernel<Isa>>> recurrent;
    std::vector<std::unique_ptr<detail::NamTcnSimdKernel<Isa>>> nam_tcn;
    std::vector<NeuralCpuKernel> seams;

    explicit SimdFamily(FamilyFixture& fixture) {
        fixture.build_kernels();
        for (std::uint32_t ch = 0; ch < fixture.channels; ++ch) {
            if (fixture.family == NeuralFamily::Gru || fixture.family == NeuralFamily::Lstm) {
                const auto& r = *fixture.recurrent_states[ch];
                recurrent.push_back(std::make_unique<detail::RecurrentSimdKernel<Isa>>(
                    fixture.recurrent, typename detail::RecurrentSimdKernel<Isa>::Weights{
                                           r.w_ih, r.w_hh, r.bias, r.head, r.head_bias}));
                REQUIRE(recurrent.back()->valid());
                seams.push_back(detail::bind_neural_cpu_kernel<NeuralCpuKernel>(*recurrent.back()));
            } else {
                const auto& t = *fixture.nam_tcn_states[ch];
                nam_tcn.push_back(std::make_unique<detail::NamTcnSimdKernel<Isa>>(
                    typename detail::NamTcnSimdKernel<Isa>::Weights{t.channels, t.kernel,
                                                                    t.dilations, t.in_w, t.conv_w,
                                                                    t.conv_b, t.mix_w, t.head_w}));
                REQUIRE(nam_tcn.back()->valid());
                seams.push_back(detail::bind_neural_cpu_kernel<NeuralCpuKernel>(*nam_tcn.back()));
            }
        }
    }
};

/// A CPU-only node whose shadow (its output) is the given kernels, rendered
/// through the transport.
struct SimdNode {
    GpuNeuralModelHandle model;
    std::unique_ptr<GpuNeuralRealtimeNode> node;
    GpuAudioTransport transport;

    SimdNode(FamilyFixture& fixture, std::span<const NeuralCpuKernel> kernels) {
        auto request = fixture.request();
        request.channel_kernels = kernels;
        auto build = detail::make_neural_model(std::move(request));
        REQUIRE(build);
        model = build.model;
        node = std::make_unique<GpuNeuralRealtimeNode>(
            GpuNeuralRealtimeNode::Config{.family = fixture.family,
                                          .model = model,
                                          .channels = kChannels,
                                          .lead_blocks = kLead,
                                          .capacity = kLead + 3});
        REQUIRE(node->prepare());
        REQUIRE(transport.prepare(node.get(), {}));
    }

    std::array<std::vector<float>, kChannels>
    render(const std::array<std::vector<float>, kChannels>& x) {
        std::array<std::vector<float>, kChannels> y;
        for (auto& channel : y)
            channel.assign(x[0].size(), 0.0f);
        for (std::size_t at = 0; at + kBlock <= x[0].size(); at += kBlock) {
            std::array<const float*, kChannels> in{};
            std::array<float*, kChannels> out{};
            for (std::uint32_t ch = 0; ch < kChannels; ++ch) {
                in[ch] = x[ch].data() + at;
                out[ch] = y[ch].data() + at;
            }
            audio::BufferView<const float> input(in.data(), kChannels, kBlock);
            audio::BufferView<float> output(out.data(), kChannels, kBlock);
            transport.process(input, output, kBlock);
        }
        return y;
    }
};

// A NAM A1 "standard" WaveNet, written deterministically: 1 -> 16 channels
// with head 8, then 16 -> 8 channels with head 1 and head bias, 10 dilated
// layers each, kernel 3, tanh, head_scale 0.02. Weights come from the shared
// xorshift WeightSource in the serialized order NamTcnArtifact reads (the
// same generator WP-S's SIMD tests use), so every run measures one model.
// Seed 21 writes the file with SHA-256 74dfc74186a694b9d125e9190b9a67f44a0fb71dffc1bc3982d8923d58f09d64.
std::filesystem::path write_nam_standard(std::uint32_t seed) {
    struct Array {
        int input_size, channels, head_size;
        bool head_bias;
    };
    constexpr Array arrays[] = {{1, 16, 8, false}, {16, 8, 1, true}};
    constexpr int kernel = 3, layers = 10;
    WeightSource rng(seed);
    std::vector<float> w;
    const auto push = [&](std::size_t count, float scale) {
        for (std::size_t i = 0; i < count; ++i)
            w.push_back(rng.next(scale));
    };
    std::ostringstream config;
    for (std::size_t a = 0; a < std::size(arrays); ++a) {
        const auto& s = arrays[a];
        const float conv = 1.2f / std::sqrt(static_cast<float>(s.channels * kernel));
        const float one = 1.0f / std::sqrt(static_cast<float>(s.channels));
        push(static_cast<std::size_t>(s.channels) * s.input_size, 0.8f);
        for (int l = 0; l < layers; ++l) {
            push(static_cast<std::size_t>(s.channels) * s.channels * kernel, conv);
            push(s.channels, 0.05f);
            push(s.channels, 0.3f);
            push(static_cast<std::size_t>(s.channels) * s.channels, one);
            push(s.channels, 0.05f);
        }
        push(static_cast<std::size_t>(s.head_size) * s.channels, one);
        if (s.head_bias)
            push(s.head_size, 0.05f);
        config << (a ? "," : "") << "{\"input_size\":" << s.input_size
               << ",\"condition_size\":1,\"head_size\":" << s.head_size
               << ",\"channels\":" << s.channels << ",\"kernel_size\":" << kernel
               << ",\"dilations\":[";
        for (int d = 0; d < layers; ++d)
            config << (d ? "," : "") << (1 << d);
        config << "],\"activation\":\"Tanh\",\"gated\":false,\"head_bias\":"
               << (s.head_bias ? "true" : "false") << "}";
    }
    w.push_back(0.02f);
    std::ostringstream json;
    json << "{\"version\":\"0.5.4\",\"architecture\":\"WaveNet\",\"sample_rate\":48000,"
            "\"config\":{\"layers\":["
         << config.str() << "],\"head\":null,\"head_scale\":0.02},\"weights\":[";
    char buffer[32];
    for (std::size_t i = 0; i < w.size(); ++i) {
        std::snprintf(buffer, sizeof buffer, "%.9g", w[i]);
        json << (i ? "," : "") << buffer;
    }
    json << "]}";
    const auto path = std::filesystem::temp_directory_path() /
                      ("pulp-neural-shadow-nam-standard-" + std::to_string(seed) + ".nam");
    std::ofstream(path) << json.str();
    return path;
}

double max_abs_diff(const std::array<std::vector<float>, kChannels>& a,
                    const std::array<std::vector<float>, kChannels>& b) {
    double worst = 0.0;
    for (std::uint32_t ch = 0; ch < kChannels; ++ch)
        for (std::size_t i = 0; i < a[ch].size(); ++i)
            worst = std::max(worst, static_cast<double>(std::abs(a[ch][i] - b[ch][i])));
    return worst;
}

} // namespace

TEST_CASE("NaN input through the node: SIMD shadow equals scalar and the zeroed run",
          "[gpu_audio][neural][simd][nan]") {
    const auto family = GENERATE(NeuralFamily::Gru, NeuralFamily::Tcn);
    CAPTURE(static_cast<int>(family));
    const bool nam_tcn = family == NeuralFamily::Tcn;
    constexpr std::size_t frames = 64ull * kBlock;
    std::array<std::vector<float>, kChannels> poisoned, zeroed;
    std::uint64_t injected = 0;
    for (std::uint32_t ch = 0; ch < kChannels; ++ch) {
        poisoned[ch].resize(frames);
        for (std::size_t i = 0; i < frames; ++i)
            poisoned[ch][i] = stimulus(ch, i);
        zeroed[ch] = poisoned[ch];
        for (std::size_t i = 13 + ch; i < frames; i += 97) {
            poisoned[ch][i] = i % 2 ? std::numeric_limits<float>::quiet_NaN()
                                    : -std::numeric_limits<float>::infinity();
            zeroed[ch][i] = 0.0f;
            ++injected;
        }
    }
    auto make_fixture = [&] {
        FamilyFixture fixture(family, kChannels, kBlock, nam_tcn);
        if (!nam_tcn)
            fixture.recurrent.hidden_size = 32;
        return fixture;
    };
    auto f_simd = make_fixture(), f_scalar = make_fixture(), f_zeroed = make_fixture();
    SimdFamily<ns::DefaultIsa> k_simd(f_simd), k_zeroed(f_zeroed);
    SimdFamily<ns::ScalarIsa> k_scalar(f_scalar);
    SimdNode simd(f_simd, k_simd.seams), scalar(f_scalar, k_scalar.seams),
        clean(f_zeroed, k_zeroed.seams);

    const auto a = simd.render(poisoned);
    const auto b = scalar.render(poisoned);
    const auto c = clean.render(zeroed);
    bool finite = true;
    for (const auto& channel : a)
        finite = finite && std::all_of(channel.begin(), channel.end(),
                                       [](float v) { return std::isfinite(v); });
    CHECK(finite);
    CHECK(simd.node->sanitized_input_samples() == injected);
    CHECK(clean.node->sanitized_input_samples() == 0); // positive control
    const auto simd_vs_scalar = max_abs_diff(a, b);
    INFO("max |SIMD - scalar| = " << simd_vs_scalar);
    // Bit-for-bit: the SIMD and scalar-ISA builds share every rounding step.
    CHECK(simd_vs_scalar == 0.0);
    // The sanitizer's contract: a replaced sample is exactly a zero sample.
    CHECK(max_abs_diff(a, c) == 0.0);
    CHECK(validate_gpu_audio_status(simd.node->status_snapshot()).accepted());
}

// Measurement, not a gate: the shadow with WP-S's SIMD kernels. The WaveNet
// row runs a generated NAM-standard model; its SHA-256 is printed.
TEST_CASE("neural shadow cost with SIMD kernels", "[.][neural-shadow-cost-simd]") {
    struct Row {
        const char* name;
        NeuralFamily family;
        std::function<NeuralCpuKernel(std::vector<std::shared_ptr<void>>&, std::uint32_t)> make;
        std::uint64_t state_bytes = 0;
        std::string schema;
        detail::TcnCpuShape tcn{};
        detail::RecurrentCpuShape recurrent{};
        detail::CompactSsmCpuShape ssm{};
        std::string_view architecture = "custom";
    };
    const auto keep_kernel = [](auto kernel, std::vector<std::shared_ptr<void>>& keep) {
        auto shared = std::make_shared<decltype(kernel)>(std::move(kernel));
        keep.push_back(shared);
        return detail::bind_neural_cpu_kernel<NeuralCpuKernel>(*shared);
    };
    std::vector<Row> rows;
    {
        struct ArtifactKernel {
            std::shared_ptr<detail::NamTcnArtifact> artifact;
            bool prepare(const detail::StreamingPrepareContext&) noexcept {
                artifact->reset();
                return true;
            }
            void reset() noexcept {
                artifact->reset();
            }
            void process(const float* in, float* out, std::uint32_t n) noexcept {
                artifact->process(in, out, n);
            }
        };
        const std::string path = write_nam_standard(21).string();
        const auto hash = pulp::runtime::sha256_file_hex(path, 64ull << 20);
        std::printf("\nNAM-standard model %s sha256 %s\n", path.c_str(),
                    hash ? hash->c_str() : "unavailable");
        rows.push_back({"wavenet.nam-standard (BlockSimd)",
                        NeuralFamily::WaveNet,
                        [path, keep_kernel](auto& keep, std::uint32_t) {
                            auto artifact = std::make_shared<detail::NamTcnArtifact>();
                            REQUIRE(artifact->load(path));
                            artifact->set_cpu_kernel(detail::NamCpuKernel::BlockSimd);
                            return keep_kernel(ArtifactKernel{artifact}, keep);
                        },
                        0,
                        "",
                        {},
                        {},
                        {},
                        "nam.wavenet"});
    }
    for (const auto& [name, c] :
         {std::pair{"nam-tcn(8ch,k3,10L)", 8u}, std::pair{"nam-tcn(16ch,k3,10L)", 16u}})
        rows.push_back(
            {name,
             NeuralFamily::Tcn,
             [c = c, keep_kernel](auto& keep, std::uint32_t) {
                 NamTcnKernel ref(c, 3, 10, 7);
                 return keep_kernel(
                     detail::NamTcnSimdKernel<>({ref.channels, ref.kernel, ref.dilations, ref.in_w,
                                                 ref.conv_w, ref.conv_b, ref.mix_w, ref.head_w}),
                     keep);
             },
             0,
             "",
             {},
             {},
             {},
             detail::kNamTcnArchitecture});
    for (const std::uint32_t states : {64u, 256u}) {
        const detail::CompactSsmCpuShape ssm{
            .state_size = states, .input_size = 1, .output_size = 1, .layers = 1};
        rows.push_back({states == 64 ? "ssm.diagonal(64)" : "ssm.diagonal(256)",
                        NeuralFamily::Ssm,
                        [states, keep_kernel](auto& keep, std::uint32_t) {
                            SsmKernel ref(states, 7);
                            return keep_kernel(detail::SsmSimdKernel<>(ref.a, ref.b, ref.c, ref.d),
                                               keep);
                        },
                        detail::compact_ssm_cpu_state_bytes(ssm),
                        "compact-ssm-diagonal-v1",
                        {},
                        {},
                        ssm,
                        "ssm.diagonal"});
    }
    for (const auto family : {detail::RecurrentFamily::Lstm, detail::RecurrentFamily::Gru})
        for (const std::uint32_t hidden : {16u, 32u, 64u}) {
            const detail::RecurrentCpuShape shape{.family = family,
                                                  .input_size = 1,
                                                  .hidden_size = hidden,
                                                  .layers = 1,
                                                  .directions = 1,
                                                  .output_size = 1};
            static std::vector<std::string> names;
            names.reserve(16);
            names.push_back(std::string(family == detail::RecurrentFamily::Lstm ? "lstm" : "gru") +
                            "(h" + std::to_string(hidden) + ")");
            rows.push_back(
                {names.back().c_str(),
                 family == detail::RecurrentFamily::Lstm ? NeuralFamily::Lstm : NeuralFamily::Gru,
                 [shape, keep_kernel](auto& keep, std::uint32_t) {
                     RecurrentKernel ref(shape.family, shape.hidden_size, 7);
                     return keep_kernel(
                         detail::RecurrentSimdKernel<>(
                             shape, {ref.w_ih, ref.w_hh, ref.bias, ref.head, ref.head_bias}),
                         keep);
                 },
                 detail::recurrent_state_bytes(shape),
                 std::string(detail::recurrent_state_schema(family)),
                 {},
                 shape,
                 {},
                 family == detail::RecurrentFamily::Lstm ? "rnn.lstm" : "rnn.gru"});
        }

    std::printf("\nSIMD shadow, 48 kHz: inline cost per channel (median) and Auto's choice\n");
    for (const std::uint32_t block : {64u, 128u}) {
        const double period_us = 1e6 * block / 48000.0;
        for (const auto& row : rows) {
            // A GPU backend is attached so Auto decides (CPU-only families
            // always run Inline); the fake provider's output is irrelevant.
            const auto build_node = [&](std::uint32_t channels,
                                        std::vector<std::shared_ptr<void>>& keep, ChannelPlan& plan,
                                        WaveNetShape& shape) {
                std::vector<NeuralCpuKernel> seams;
                for (std::uint32_t ch = 0; ch < channels; ++ch)
                    seams.push_back(row.make(keep, ch));
                detail::NeuralModelRequest request;
                request.family = NeuralFamily::WaveNet;
                request.manifest = {.model_id = "pulp.cost",
                                    .architecture = "nam.wavenet",
                                    .model_version = "1",
                                    .artifact_id = "pulp.cost.weights",
                                    .artifact_sha256 = kSha,
                                    .license = "MIT",
                                    .runtime = "pulp.neural.test.v1",
                                    .artifact_size_bytes = 1,
                                    .sample_rate_hz = 48000,
                                    .redistributable = true};
                request.block_size = block;
                auto owned = std::make_shared<std::vector<NeuralCpuKernel>>(seams);
                keep.push_back(owned);
                request.channel_kernels = *owned;
                request.backend = make_fake_wavenet_backend(plan, shape, channels, block);
                auto build = detail::make_neural_model(std::move(request));
                REQUIRE(build);
                return build.model;
            };
            std::vector<std::shared_ptr<void>> keep;
            ChannelPlan plan;
            WaveNetShape shape;
            GpuNeuralRealtimeNode mono({.family = NeuralFamily::WaveNet,
                                        .model = build_node(1, keep, plan, shape),
                                        .channels = 1,
                                        .lead_blocks = kLead,
                                        .capacity = kLead + 3,
                                        .shadow_policy = Policy::Inline});
            REQUIRE(mono.prepare());
            std::vector<float> x(block), y(block);
            const float* in_ptr[1] = {x.data()};
            float* out_ptr[1] = {y.data()};
            audio::BufferView<const float> in(in_ptr, 1, block);
            audio::BufferView<float> out(out_ptr, 1, block);
            std::vector<double> samples;
            std::uint64_t frame = 0;
            for (int b = 0; b < 2200; ++b) {
                for (std::uint32_t i = 0; i < block; ++i)
                    x[i] = stimulus(0, frame + i);
                frame += block;
                const auto t0 = std::chrono::steady_clock::now();
                mono.prime_fallback(in, block);
                mono.process_cpu_fallback(in, out, block);
                const auto t1 = std::chrono::steady_clock::now();
                if (b >= 200)
                    samples.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
            }
            std::sort(samples.begin(), samples.end());
            const double median = samples[samples.size() / 2];
            // What Auto picks for mono and stereo on its own measurement.
            ChannelPlan plan2;
            WaveNetShape shape2;
            GpuNeuralRealtimeNode stereo({.family = NeuralFamily::WaveNet,
                                          .model = build_node(2, keep, plan2, shape2),
                                          .channels = 2,
                                          .lead_blocks = kLead,
                                          .capacity = kLead + 3});
            detail::NeuralRealtimeTestAccess::manual_shadow(stereo, true);
            REQUIRE(stereo.prepare());
            const auto auto_stereo = stereo.status_snapshot();
            std::printf("  block %3u  %-34s median %7.2f us  p99 %7.2f us  %6.2f%%/ch  "
                        "Auto stereo: %s (total %.2f%%)\n",
                        block, row.name, median, samples[samples.size() * 99 / 100],
                        100.0 * median / period_us,
                        auto_stereo.shadow_mode == GpuAudioShadowMode::Inline ? "Inline" : "Worker",
                        auto_stereo.shadow_inline_cost_pct);
        }
    }

    // Worker double misses for the heaviest row (NAM-standard when given).
    const auto& heavy = rows.front();
    std::printf("\nWorker with the GPU failed, %s, paced at 48 kHz:\n", heavy.name);
    for (const std::uint32_t block : {64u, 128u}) {
        for (const std::uint32_t lead : {2u, 3u}) {
            std::vector<std::shared_ptr<void>> keep;
            ChannelPlan plan;
            WaveNetShape shape;
            std::vector<NeuralCpuKernel> seams{heavy.make(keep, 0)};
            detail::NeuralModelRequest request;
            request.family = NeuralFamily::WaveNet;
            request.manifest = {.model_id = "pulp.cost",
                                .architecture = "nam.wavenet",
                                .model_version = "1",
                                .artifact_id = "pulp.cost.weights",
                                .artifact_sha256 = kSha,
                                .license = "MIT",
                                .runtime = "pulp.neural.test.v1",
                                .artifact_size_bytes = 1,
                                .sample_rate_hz = 48000,
                                .redistributable = true};
            request.block_size = block;
            request.channel_kernels = seams;
            request.backend = make_fake_wavenet_backend(plan, shape, 1, block);
            auto build = detail::make_neural_model(std::move(request));
            REQUIRE(build);
            GpuNeuralRealtimeNode node({.family = NeuralFamily::WaveNet,
                                        .model = build.model,
                                        .channels = 1,
                                        .lead_blocks = lead,
                                        .capacity = lead + 3,
                                        .shadow_policy = Policy::Worker});
            GpuAudioTransport transport;
            REQUIRE(node.prepare());
            REQUIRE(transport.prepare(&node, {.ring_blocks = lead + 2}));
            plan.fail_completions = true;
            std::vector<float> x(block), y(block);
            const float* in_ptr[1] = {x.data()};
            float* out_ptr[1] = {y.data()};
            audio::BufferView<const float> in(in_ptr, 1, block);
            audio::BufferView<float> out(out_ptr, 1, block);
            std::vector<double> samples;
            std::uint64_t frame = 0;
            const auto period = std::chrono::microseconds(1'000'000ull * block / 48'000);
            constexpr int blocks = 4000;
            for (int b = 0; b < blocks; ++b) {
                for (std::uint32_t i = 0; i < block; ++i)
                    x[i] = stimulus(0, frame + i);
                frame += block;
                const auto t0 = std::chrono::steady_clock::now();
                transport.process(in, out, block);
                const auto t1 = std::chrono::steady_clock::now();
                transport.pump();
                samples.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
                std::this_thread::sleep_until(t0 + period);
            }
            std::sort(samples.begin(), samples.end());
            const auto status = node.status_snapshot();
            std::printf("  block %3u lead %u  callback median %6.2f us  delivered %llu late %llu "
                        "double-miss %llu (%.3f%%)\n",
                        block, lead, samples[samples.size() / 2],
                        static_cast<unsigned long long>(status.shadow_delivered_blocks),
                        static_cast<unsigned long long>(status.shadow_late_blocks),
                        static_cast<unsigned long long>(status.shadow_double_miss_blocks),
                        100.0 * static_cast<double>(status.shadow_double_miss_blocks) / blocks);
        }
    }
}
