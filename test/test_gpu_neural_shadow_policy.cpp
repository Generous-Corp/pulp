// Shadow placement for GpuNeuralRealtimeNode: Inline, Worker, None and Auto.
//
// The fake WaveNet provider and the shadow compute the same FIR. A provider
// gain marks GPU blocks where a test must tell them apart from shadow blocks.
// Worker tests step the shadow explicitly unless they say otherwise, so slot
// timing is deterministic. None of this observes a real GPU.

#include "gpu_neural_test_kernels.hpp"

#include "detail/gpu_neural_realtime_test_access.hpp"
#include "harness/rt_allocation_probe.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <pulp/gpu_audio/gpu_audio_transport.hpp>
#include <pulp/gpu_audio/gpu_neural_realtime_node.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

using namespace pulp;
using namespace pulp::gpu_audio;
using namespace pulp::gpu_audio::neural_test;
using Policy = GpuNeuralRealtimeNode::ShadowPolicy;
using Mode = GpuAudioShadowMode;

namespace {

constexpr std::uint32_t kChannels = 2;
constexpr std::uint32_t kBlock = 64;
constexpr std::uint32_t kLead = 2;

// Set while the test thread is inside the audio callback.
thread_local bool t_in_callback = false;

/// FirKernel that counts how often the model ran inside the callback.
struct CountingFir {
    FirKernel fir;
    std::uint64_t calls = 0, calls_in_callback = 0;
    void reset() noexcept {
        fir.reset();
    }
    void process(const float* in, float* out, std::uint32_t n) noexcept {
        ++calls;
        if (t_in_callback)
            ++calls_in_callback;
        fir.process(in, out, n);
    }
};

struct Options {
    Policy policy = Policy::Auto;
    float injected_cost = -1.0f;
    bool manual = true;
    bool gpu_backend = true;
    NeuralFamily family = NeuralFamily::WaveNet;
    float gpu_gain = 1.0f;
    std::uint32_t queue_blocks = 16;
    bool fail_backend = false;
};

/// The node's public status, checked against the shared status contract.
GpuAudioStatus validated(const GpuAudioStatus& status) {
    CHECK(validate_gpu_audio_status(status).error == GpuAudioStatusError::None);
    return status;
}

struct Rig {
    Options options;
    FamilyFixture fixture;
    ChannelPlan plan;
    WaveNetShape shape;
    std::array<CountingFir, kChannels> kernels;
    std::vector<NeuralCpuKernel> seams;
    GpuNeuralModelHandle model;
    std::unique_ptr<GpuNeuralRealtimeNode> node;
    GpuAudioTransport transport;
    std::uint64_t frame = 0;
    std::array<std::vector<float>, kChannels> output;
    std::size_t callback_allocations = 0;

    explicit Rig(Options o, GpuNeuralModelHandle shadow_model = {})
        : options(o), fixture(o.family, kChannels, kBlock) {
        plan.gain = o.gpu_gain;
        auto request = fixture.request();
        if (o.family == NeuralFamily::WaveNet) {
            // Same FIR, instrumented to show which thread runs the model.
            for (auto& kernel : kernels)
                seams.push_back(kernel_for(kernel));
            request.channel_kernels = seams;
        }
        if (o.gpu_backend)
            request.backend =
                make_fake_wavenet_backend(plan, shape, kChannels, kBlock, 1, o.fail_backend);
        auto build = detail::make_neural_model(std::move(request));
        REQUIRE(build);
        model = build.model;
        node = std::make_unique<GpuNeuralRealtimeNode>(
            GpuNeuralRealtimeNode::Config{.family = o.family,
                                          .model = model,
                                          .channels = kChannels,
                                          .lead_blocks = kLead,
                                          .capacity = kLead + 3,
                                          .shadow_policy = o.policy,
                                          .shadow_model = shadow_model,
                                          .shadow_queue_blocks = o.queue_blocks});
        detail::NeuralRealtimeTestAccess::inject_shadow_cost(*node, o.injected_cost);
        detail::NeuralRealtimeTestAccess::manual_shadow(*node, o.manual);
    }

    bool prepare() {
        return node->prepare() && transport.prepare(node.get(), {});
    }

    // The node's status, and the transport's mirror of it after the last
    // pump: the shadow fields must arrive unchanged through the transport.
    // After an offline render the transport's mirror holds the sample taken
    // at the fence (no worker pumps offline), so only validity is checked.
    GpuAudioStatus offline_status() const {
        validated(transport.status_snapshot());
        return validated(node->status_snapshot());
    }
    GpuAudioStatus status() const {
        const auto hosted = validated(transport.status_snapshot());
        const auto own = validated(node->status_snapshot());
        CHECK(hosted.shadow_mode == own.shadow_mode);
        CHECK(hosted.shadow_inline_cost_pct == own.shadow_inline_cost_pct);
        CHECK(hosted.shadow_delivered_blocks == own.shadow_delivered_blocks);
        CHECK(hosted.shadow_late_blocks == own.shadow_late_blocks);
        CHECK(hosted.shadow_double_miss_blocks == own.shadow_double_miss_blocks);
        CHECK(hosted.shadow_model_differs == own.shadow_model_differs);
        return own;
    }
    Mode mode() const {
        return node->status_snapshot().shadow_mode;
    }

    std::uint64_t calls_in_callback() const {
        std::uint64_t total = 0;
        for (const auto& kernel : kernels)
            total += kernel.calls_in_callback;
        return total;
    }

    /// One callback, then the GPU worker pump and (when asked) the shadow.
    void block(bool pump_gpu = true, bool pump_shadow = true) {
        std::array<std::vector<float>, kChannels> in, out;
        std::array<const float*, kChannels> in_ptrs{};
        std::array<float*, kChannels> out_ptrs{};
        for (std::uint32_t ch = 0; ch < kChannels; ++ch) {
            in[ch].resize(kBlock);
            out[ch].assign(kBlock, 0.0f);
            for (std::uint32_t i = 0; i < kBlock; ++i)
                in[ch][i] = stimulus(ch, frame + i);
            in_ptrs[ch] = in[ch].data();
            out_ptrs[ch] = out[ch].data();
        }
        audio::BufferView<const float> input(in_ptrs.data(), kChannels, kBlock);
        audio::BufferView<float> result(out_ptrs.data(), kChannels, kBlock);
        std::size_t allocations = 0;
        {
            pulp::test::RtAllocationProbe probe;
            t_in_callback = true;
            transport.process(input, result, kBlock);
            t_in_callback = false;
            allocations = probe.allocation_count();
        }
        callback_allocations += allocations;
        for (std::uint32_t ch = 0; ch < kChannels; ++ch)
            output[ch].insert(output[ch].end(), out[ch].begin(), out[ch].end());
        frame += kBlock;
        if (pump_gpu)
            for (int i = 0; i < 2; ++i)
                transport.pump();
        if (pump_shadow && options.manual)
            (void)detail::NeuralRealtimeTestAccess::pump_shadow(*node);
        if (!options.manual)
            wait_for_shadow();
    }
    void render(std::uint32_t blocks, bool pump_gpu = true, bool pump_shadow = true) {
        for (std::uint32_t b = 0; b < blocks; ++b)
            block(pump_gpu, pump_shadow);
    }
    // Real-thread Worker: wait (bounded) until the shadow caught up, so slot
    // timing matches the explicit-pump tests.
    void wait_for_shadow() {
        const auto mode = node->status_snapshot().shadow_mode;
        if (mode != Mode::Worker && mode != Mode::None)
            return;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (detail::NeuralRealtimeTestAccess::shadow_processed(*node) < frame / kBlock &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
    }
};

/// The family kernel driven directly, delayed by the lead (same seeds).
std::array<std::vector<float>, kChannels> reference(NeuralFamily family, std::uint64_t frames) {
    FamilyFixture fixture(family, kChannels, kBlock);
    fixture.build_kernels();
    std::array<std::vector<float>, kChannels> out;
    const auto delay = std::uint64_t{kLead} * kBlock;
    for (std::uint32_t ch = 0; ch < kChannels; ++ch) {
        out[ch].assign(frames, 0.0f);
        const auto& kernel = fixture.kernels[ch];
        for (std::uint64_t i = 0; i + delay < frames; ++i) {
            const float x = stimulus(ch, i);
            float y = 0.0f;
            kernel.process(kernel.state, &x, &y, 1);
            out[ch][i + delay] = y;
        }
    }
    return out;
}

/// Largest |actual - gain * reference| over blocks [first, last).
float block_error(const Rig& rig, const std::array<std::vector<float>, kChannels>& ref,
                  std::uint32_t first, std::uint32_t last, float gain) {
    float worst = 0.0f;
    for (std::uint32_t ch = 0; ch < kChannels; ++ch)
        for (std::uint64_t i = std::uint64_t{first} * kBlock; i < std::uint64_t{last} * kBlock; ++i)
            worst = std::max(worst, std::abs(rig.output[ch][i] - gain * ref[ch][i]));
    return worst;
}

} // namespace

TEST_CASE("Auto picks Inline when the total inline cost is within the threshold",
          "[gpu_audio][neural][shadow-policy]") {
    // Two channels: Auto compares twice the per-channel cost.
    const auto threshold = GpuNeuralRealtimeNode::kAutoInlineMaxCostPct;
    for (const auto [cost, expected] :
         {std::pair{threshold / 2, Mode::Inline}, std::pair{threshold / 2 + 0.01f, Mode::Worker},
          std::pair{1.0f, Mode::Inline}, std::pair{30.0f, Mode::Worker}}) {
        CAPTURE(cost);
        Rig rig({.policy = Policy::Auto, .injected_cost = cost});
        REQUIRE(rig.prepare());
        const auto diagnostics = rig.node->shadow_status();
        CHECK(diagnostics.requested == Policy::Auto);
        CHECK(diagnostics.inline_cost_per_channel_pct == cost);
        CHECK(diagnostics.inline_cost_total_pct == cost * kChannels);
        const auto status = rig.status();
        CHECK(status.shadow_mode == expected);
        CHECK(status.shadow_inline_cost_pct == cost * kChannels);
    }
    SECTION("an 8-channel node at 14% per channel costs 112% inline, so Worker") {
        constexpr std::uint32_t wide = 8;
        for (const auto [cost, expected] :
             {std::pair{14.0f, Mode::Worker}, std::pair{threshold / wide, Mode::Inline}}) {
            CAPTURE(cost);
            FamilyFixture fixture(NeuralFamily::WaveNet, wide, kBlock);
            ChannelPlan plan;
            WaveNetShape shape;
            auto request = fixture.request();
            request.backend = make_fake_wavenet_backend(plan, shape, wide, kBlock);
            auto build = detail::make_neural_model(std::move(request));
            REQUIRE(build);
            GpuNeuralRealtimeNode node({.family = NeuralFamily::WaveNet,
                                        .model = build.model,
                                        .channels = wide,
                                        .lead_blocks = kLead,
                                        .capacity = kLead + 3});
            detail::NeuralRealtimeTestAccess::inject_shadow_cost(node, cost);
            detail::NeuralRealtimeTestAccess::manual_shadow(node, true);
            REQUIRE(node.prepare());
            const auto status = validated(node.status_snapshot());
            CHECK(status.shadow_mode == expected);
            CHECK(status.shadow_inline_cost_pct == cost * wide);
        }
    }
    SECTION("families without a GPU backend stay inline at any cost") {
        Rig rig({.policy = Policy::Auto,
                 .injected_cost = 90.0f,
                 .gpu_backend = false,
                 .family = NeuralFamily::Lstm});
        REQUIRE(rig.prepare());
        CHECK(rig.mode() == Mode::Inline);
        Rig worker({.policy = Policy::Worker, .gpu_backend = false, .family = NeuralFamily::Lstm});
        REQUIRE(worker.prepare());
        CHECK(worker.mode() == Mode::Inline);
        rig.render(4);
        CHECK(rig.status().shadow_delivered_blocks == 4);
    }
    SECTION("without injection the cost is measured") {
        Rig rig({.policy = Policy::Auto});
        REQUIRE(rig.prepare());
        CHECK(rig.node->shadow_status().inline_cost_per_channel_pct > 0.0f);
    }
}

TEST_CASE("each shadow policy nulls against the reference across a provider fault",
          "[gpu_audio][neural][shadow-policy]") {
    const auto policy = GENERATE(Policy::Inline, Policy::Worker);
    const auto manual = GENERATE(true, false);
    CAPTURE(static_cast<int>(policy), manual);
    Rig rig({.policy = policy, .manual = manual});
    REQUIRE(rig.prepare());
    REQUIRE(rig.mode() == (policy == Policy::Inline ? Mode::Inline : Mode::Worker));
    rig.render(12);
    rig.plan.fail_completions = true;
    rig.render(20);
    const auto ref = reference(NeuralFamily::WaveNet, 32ull * kBlock);
    CHECK(block_error(rig, ref, 0, 32, 1.0f) == 0.0f);
    const auto status = rig.status();
    CHECK(status.shadow_delivered_blocks > 0);
    CHECK(status.shadow_double_miss_blocks == 0);
    CHECK(status.fallback_blocks == status.shadow_delivered_blocks);
    // Positive control for the None case: a substituting shadow is a fallback.
    CHECK(status.fallback_policy == MissPolicy::CpuFallback);
    CHECK(status.fallback_available);
    CHECK(rig.transport.delivery_snapshot().gpu_blocks > 0);
    CHECK(rig.transport.status_snapshot().fallback_blocks == status.shadow_delivered_blocks);
    CHECK(rig.callback_allocations == 0);
    if (policy == Policy::Worker) {
        // Copy only: the model never ran inside the callback.
        CHECK(rig.calls_in_callback() == 0);
        // Channel 1 is never timed by Auto, so it ran exactly once per block.
        CHECK(rig.kernels[1].calls == 32);
    } else {
        // Positive control for the instrument: Inline runs it every callback.
        CHECK(rig.calls_in_callback() == 32ull * kChannels);
    }
}

TEST_CASE("None policy plays the GPU and is silent on a realtime miss",
          "[gpu_audio][neural][shadow-policy]") {
    Rig rig({.policy = Policy::None, .gpu_gain = 2.0f});
    REQUIRE(rig.prepare());
    CHECK(rig.mode() == Mode::None);
    rig.render(10);
    rig.plan.fail_completions = true;
    rig.render(10);
    const auto ref = reference(NeuralFamily::WaveNet, 20ull * kBlock);
    CHECK(block_error(rig, ref, kLead, 10, 2.0f) == 0.0f);
    // After the fault every block is a miss, and None substitutes nothing.
    CHECK(block_error(rig, ref, 13, 20, 0.0f) == 0.0f);
    // The model stays primed off the audio thread for an offline render.
    CHECK(rig.calls_in_callback() == 0);
    CHECK(rig.kernels[1].calls == 20);
    const auto status = rig.status();
    CHECK(status.shadow_delivered_blocks == 0);
    CHECK(status.shadow_double_miss_blocks == 0);
    CHECK(status.shadow_inline_cost_pct == 0.0f);
    // A realtime miss is silent under None, and the node says so.
    CHECK(status.fallback_policy == MissPolicy::Silence);
    CHECK_FALSE(status.fallback_available);
    CHECK(rig.node->capability_report().fallback_policy == MissPolicy::Silence);
    CHECK_FALSE(rig.node->capability_report().fallback_available);

    SECTION("None needs a family with a GPU kernel") {
        Rig cpu({.policy = Policy::None, .gpu_backend = false, .family = NeuralFamily::Lstm});
        CHECK_FALSE(cpu.node->prepare());
        CHECK(cpu.node->last_prepare_error() ==
              GpuNeuralRealtimeNode::PrepareError::ShadowRequired);
    }
    SECTION("a GPU family whose provider is unavailable runs inline anyway") {
        Rig degraded({.policy = Policy::None, .fail_backend = true});
        REQUIRE(degraded.prepare());
        CHECK(degraded.mode() == Mode::Inline);
        degraded.render(8);
        const auto ref8 = reference(NeuralFamily::WaveNet, 8ull * kBlock);
        CHECK(block_error(degraded, ref8, 0, 8, 1.0f) == 0.0f);
        CHECK(degraded.status().provider_state == GpuAudioProviderState::Degraded);
    }
}

TEST_CASE("a starved Worker shadow drops late blocks and stays silent on a double miss",
          "[gpu_audio][neural][shadow-policy]") {
    Rig rig({.policy = Policy::Worker});
    REQUIRE(rig.prepare());
    rig.plan.fail_completions = true; // every block comes from the shadow
    rig.render(8);
    // Starve the shadow for callbacks 8..12. The results for slots 6 and 7
    // were published before the stall; slots 8..11 (callbacks 10..13) miss,
    // because the shadow first runs again after callback 13.
    rig.render(5, true, false);
    // The stalled backlog is then late for its slots and must not shift later.
    rig.render(10);
    const auto ref = reference(NeuralFamily::WaveNet, 23ull * kBlock);
    const auto status = rig.status();
    CHECK(status.shadow_double_miss_blocks == 4);
    CHECK(status.shadow_late_blocks == 4);
    CHECK(block_error(rig, ref, 0, 10, 1.0f) == 0.0f);
    CHECK(block_error(rig, ref, 10, 14, 0.0f) == 0.0f); // the declared silence
    CHECK(block_error(rig, ref, 14, 23, 1.0f) == 0.0f); // aligned again
    CHECK(rig.callback_allocations == 0);
}

TEST_CASE("an offline render moves a Worker shadow inline on the same timeline",
          "[gpu_audio][neural][shadow-policy]") {
    const auto manual = GENERATE(true, false);
    CAPTURE(manual);
    Rig rig({.policy = Policy::Worker, .manual = manual, .gpu_gain = 2.0f});
    REQUIRE(rig.prepare());
    rig.render(4);
    // With an explicit shadow, leave the last two inputs unprocessed so the
    // handover itself must run them before seeding the inline ring.
    rig.render(2, true, !manual);
    // The Worker still holds results for the next two slots; the handover to
    // the inline ring must carry them.
    std::array<std::vector<float>, kChannels> in, out;
    std::array<const float*, kChannels> in_ptrs{};
    std::array<float*, kChannels> out_ptrs{};
    for (std::uint32_t b = 0; b < 10; ++b) {
        for (std::uint32_t ch = 0; ch < kChannels; ++ch) {
            in[ch].resize(kBlock);
            out[ch].assign(kBlock, 0.0f);
            for (std::uint32_t i = 0; i < kBlock; ++i)
                in[ch][i] = stimulus(ch, rig.frame + i);
            in_ptrs[ch] = in[ch].data();
            out_ptrs[ch] = out[ch].data();
        }
        audio::BufferView<const float> input(in_ptrs.data(), kChannels, kBlock);
        audio::BufferView<float> result(out_ptrs.data(), kChannels, kBlock);
        rig.transport.process_offline(input, result, kBlock);
        for (std::uint32_t ch = 0; ch < kChannels; ++ch)
            rig.output[ch].insert(rig.output[ch].end(), out[ch].begin(), out[ch].end());
        rig.frame += kBlock;
    }
    const auto ref = reference(NeuralFamily::WaveNet, 16ull * kBlock);
    CHECK(block_error(rig, ref, kLead, 6, 2.0f) == 0.0f); // GPU, marked
    CHECK(block_error(rig, ref, 6, 16, 1.0f) == 0.0f);    // shadow, continuous
    CHECK(rig.mode() == Mode::Inline);
    rig.offline_status();
}

TEST_CASE("a different shadow model is reported and must match the stream shape",
          "[gpu_audio][neural][shadow-policy]") {
    FamilyFixture lite(NeuralFamily::Lstm, kChannels, kBlock);
    auto build = detail::make_neural_model(lite.request());
    REQUIRE(build);
    Rig rig({.policy = Policy::Worker}, build.model);
    REQUIRE(rig.prepare());
    CHECK(rig.status().shadow_model_differs);
    rig.plan.fail_completions = true;
    rig.render(12);
    // The substitutes are the lite model's, not the GPU model's.
    const auto ref = reference(NeuralFamily::Lstm, 12ull * kBlock);
    CHECK(block_error(rig, ref, 4, 12, 1.0f) == 0.0f);

    FamilyFixture narrow(NeuralFamily::Lstm, 1, kBlock);
    auto mono = detail::make_neural_model(narrow.request());
    REQUIRE(mono);
    Rig mismatched({.policy = Policy::Worker}, mono.model);
    CHECK_FALSE(mismatched.node->prepare());
    CHECK(mismatched.node->last_prepare_error() ==
          GpuNeuralRealtimeNode::PrepareError::ShadowModelMismatch);
    Rig same({.policy = Policy::Worker});
    REQUIRE(same.prepare());
    CHECK_FALSE(same.status().shadow_model_differs);
}

// Measurement, not a gate. Callback CPU per channel for a NAM-standard-sized
// WaveNet shadow (scalar test kernel) with the shadow Inline vs on the Worker.
TEST_CASE("neural shadow callback cost by policy", "[.][neural-shadow-cost]") {
    const std::array<std::array<std::uint32_t, 3>, 2> nam_standard{{{1, 16, 8}, {8, 8, 1}}};
    for (const std::uint32_t block : {64u, 128u}) {
        for (const auto [policy, gpu_failed] :
             {std::pair{Policy::Inline, false}, std::pair{Policy::Worker, false},
              std::pair{Policy::Worker, true}}) {
            WaveNetKernel kernel(nam_standard, 10, 7);
            const auto seam = kernel_for(kernel);
            FamilyFixture fixture(NeuralFamily::WaveNet, 1, block);
            ChannelPlan plan;
            WaveNetShape shape;
            auto request = fixture.request();
            request.channel_kernels = {&seam, 1};
            request.backend = make_fake_wavenet_backend(plan, shape, 1, block);
            auto build = detail::make_neural_model(std::move(request));
            REQUIRE(build);
            GpuNeuralRealtimeNode node({.family = NeuralFamily::WaveNet,
                                        .model = build.model,
                                        .channels = 1,
                                        .lead_blocks = kLead,
                                        .capacity = kLead + 3,
                                        .shadow_policy = policy});
            GpuAudioTransport transport;
            REQUIRE(node.prepare());
            REQUIRE(transport.prepare(&node, {}));
            // A failed GPU makes every block a shadow substitution.
            plan.fail_completions = gpu_failed;
            std::vector<float> x(block), y(block);
            const float* in_ptr[1] = {x.data()};
            float* out_ptr[1] = {y.data()};
            audio::BufferView<const float> in(in_ptr, 1, block);
            audio::BufferView<float> out(out_ptr, 1, block);
            std::vector<double> samples;
            std::uint64_t frame = 0;
            const auto period = std::chrono::microseconds(1'000'000ull * block / 48'000);
            for (int b = 0; b < 1200; ++b) {
                for (std::uint32_t i = 0; i < block; ++i)
                    x[i] = stimulus(0, frame + i);
                frame += block;
                const auto t0 = std::chrono::steady_clock::now();
                transport.process(in, out, block);
                const auto t1 = std::chrono::steady_clock::now();
                transport.pump();
                if (b >= 200)
                    samples.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
                // Pace like a device so the Worker shadow sees real time.
                std::this_thread::sleep_until(t0 + period);
            }
            std::sort(samples.begin(), samples.end());
            const double period_us = 1e6 * block / 48000.0;
            const auto status = node.status_snapshot();
            std::printf(
                "  block %3u  %-6s gpu %-6s callback median %8.2f us  p99 %8.2f us  %6.2f%%  "
                "delivered %llu late %llu double-miss %llu\n",
                block, policy == Policy::Inline ? "inline" : "worker", gpu_failed ? "failed" : "ok",
                samples[samples.size() / 2], samples[samples.size() * 99 / 100],
                100.0 * samples[samples.size() / 2] / period_us,
                static_cast<unsigned long long>(status.shadow_delivered_blocks),
                static_cast<unsigned long long>(status.shadow_late_blocks),
                static_cast<unsigned long long>(status.shadow_double_miss_blocks));
        }
    }
}

TEST_CASE("an offline bounce under None continues the stream through the CPU model",
          "[gpu_audio][neural][shadow-policy]") {
    const auto manual = GENERATE(true, false);
    CAPTURE(manual);
    Rig rig({.policy = Policy::None, .manual = manual, .gpu_gain = 2.0f});
    REQUIRE(rig.prepare());
    rig.render(6);
    std::array<std::vector<float>, kChannels> in, out;
    std::array<const float*, kChannels> in_ptrs{};
    std::array<float*, kChannels> out_ptrs{};
    for (std::uint32_t b = 0; b < 10; ++b) {
        for (std::uint32_t ch = 0; ch < kChannels; ++ch) {
            in[ch].resize(kBlock);
            out[ch].assign(kBlock, 0.0f);
            for (std::uint32_t i = 0; i < kBlock; ++i)
                in[ch][i] = stimulus(ch, rig.frame + i);
            in_ptrs[ch] = in[ch].data();
            out_ptrs[ch] = out[ch].data();
        }
        audio::BufferView<const float> input(in_ptrs.data(), kChannels, kBlock);
        audio::BufferView<float> result(out_ptrs.data(), kChannels, kBlock);
        rig.transport.process_offline(input, result, kBlock);
        for (std::uint32_t ch = 0; ch < kChannels; ++ch)
            rig.output[ch].insert(rig.output[ch].end(), out[ch].begin(), out[ch].end());
        rig.frame += kBlock;
    }
    const auto ref = reference(NeuralFamily::WaveNet, 16ull * kBlock);
    CHECK(block_error(rig, ref, kLead, 6, 2.0f) == 0.0f); // realtime: the GPU, marked
    CHECK(block_error(rig, ref, 6, 16, 1.0f) == 0.0f);    // offline: the CPU model, continuous
    CHECK(rig.mode() == Mode::Inline);
    // Offline moved the model inline, so misses are covered again.
    CHECK(rig.node->status_snapshot().fallback_policy == MissPolicy::CpuFallback);
    rig.offline_status();
}

TEST_CASE("the Worker shadow keeps delivering after a fence with no GPU ingress",
          "[gpu_audio][neural][shadow-policy]") {
    const auto manual = GENERATE(true, false);
    CAPTURE(manual);
    Rig rig({.policy = Policy::Worker, .manual = manual});
    REQUIRE(rig.prepare());
    rig.render(4);
    rig.plan.fail_completions = true;
    rig.render(4); // the first failed completion fences the GPU
    REQUIRE(rig.node->fenced());
    const auto submissions = rig.plan.submissions;
    const auto delivered = rig.status().shadow_delivered_blocks;
    rig.render(16);
    // No block reached the GPU after the fence, yet every slot was covered.
    CHECK(rig.plan.submissions == submissions);
    CHECK(rig.node->epoch() == 0);
    const auto status = rig.status();
    CHECK(status.shadow_delivered_blocks == delivered + 16);
    CHECK(status.shadow_double_miss_blocks == 0);
    const auto ref = reference(NeuralFamily::WaveNet, 24ull * kBlock);
    CHECK(block_error(rig, ref, 0, 24, 1.0f) == 0.0f);
}

TEST_CASE("a full Worker input queue drops blocks and restarts the shadow state",
          "[gpu_audio][neural][shadow-policy]") {
    Rig rig({.policy = Policy::Worker, .queue_blocks = kLead + 2});
    REQUIRE(rig.prepare());
    rig.plan.fail_completions = true;
    rig.render(8);
    // Callbacks 8..13 run with the shadow stalled. Its four-block input queue
    // takes inputs 8..11 and drops 12..14 (14 arrives before the first pump).
    rig.render(6, true, false);
    rig.render(10);
    const auto diagnostics = rig.node->shadow_status();
    CHECK(diagnostics.shadow_input_dropped == 3);
    CHECK(diagnostics.shadow_resyncs == 1);
    const auto status = rig.status();
    CHECK(status.shadow_late_blocks == 4);
    CHECK(status.shadow_double_miss_blocks == 7);
    const auto ref = reference(NeuralFamily::WaveNet, 24ull * kBlock);
    CHECK(block_error(rig, ref, 0, 10, 1.0f) == 0.0f);
    CHECK(block_error(rig, ref, 10, 17, 0.0f) == 0.0f);
    // Block 17 carries the restarted state; the FIR's short memory has
    // refilled by block 18, so the stream is exact again.
    CHECK(block_error(rig, ref, 18, 24, 1.0f) == 0.0f);
    CHECK(rig.callback_allocations == 0);
}

TEST_CASE("a lagging shadow under a healthy GPU is not counted late",
          "[gpu_audio][neural][shadow-policy]") {
    Rig rig({.policy = Policy::Worker});
    REQUIRE(rig.prepare());
    rig.render(6);
    rig.render(4, true, false); // the shadow lags; the GPU covers every slot
    rig.render(6);
    const auto status = rig.status();
    CHECK(status.shadow_late_blocks == 0);
    CHECK(status.shadow_double_miss_blocks == 0);
    CHECK(status.shadow_delivered_blocks == 0);
    const auto ref = reference(NeuralFamily::WaveNet, 16ull * kBlock);
    CHECK(block_error(rig, ref, 0, 16, 1.0f) == 0.0f);
}
