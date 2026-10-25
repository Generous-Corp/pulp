// GpuNeuralRealtimeNode: one realtime contract for every neural family.
//
// Each family's node output, driven through GpuAudioTransport, must null
// sample-exactly against the family's private StreamingModel adapter driven
// directly and delayed by the node's lead. The fake WaveNet provider and the
// WaveNet shadow compute the same FIR, so GPU deliveries and CPU substitutes
// are interchangeable sample for sample. None of this observes a real GPU.

#include "gpu_neural_test_kernels.hpp"

#include "detail/gpu_neural_realtime_test_access.hpp"
#include "detail/realtime_gpu_audio_path.hpp"
#include "detail/realtime_gpu_health_mirror.hpp"
#include "harness/rt_allocation_probe.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <pulp/gpu_audio/gpu_audio_transport.hpp>
#include <pulp/gpu_audio/gpu_neural_realtime_node.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <vector>

using namespace pulp;
using namespace pulp::gpu_audio;
using namespace pulp::gpu_audio::neural_test;

namespace {

constexpr std::uint32_t kChannels = 2;
constexpr std::uint32_t kBlock = 64;
constexpr std::uint32_t kLead = 2;

struct FamilyCase {
    const char* name;
    NeuralFamily family;
    bool nam_tcn = false;
    bool gpu_backend = false;
};

const FamilyCase kWaveNetGpu{"wavenet-gpu", NeuralFamily::WaveNet, false, true};
const FamilyCase kWaveNetCpu{"wavenet-cpu", NeuralFamily::WaveNet};
const FamilyCase kTcn{"tcn", NeuralFamily::Tcn};
const FamilyCase kNamTcn{"nam-tcn", NeuralFamily::Tcn, true};
const FamilyCase kSsm{"ssm", NeuralFamily::Ssm};
const FamilyCase kLstm{"lstm", NeuralFamily::Lstm};
const FamilyCase kGru{"gru", NeuralFamily::Gru};
const FamilyCase kCustom{"custom", NeuralFamily::Custom};

/// A prepared node behind a transport, with the fake WaveNet provider when
/// the case asks for one.
struct Rig {
    FamilyCase which;
    FamilyFixture fixture;
    ChannelPlan plan;
    WaveNetShape shape;
    GpuNeuralModelHandle model;
    std::unique_ptr<FamilyFixture> shadow_fixture;
    GpuNeuralModelHandle shadow_model;
    std::unique_ptr<GpuNeuralRealtimeNode> node;
    GpuAudioTransport transport;
    std::uint64_t frame = 0;
    std::array<std::vector<float>, kChannels> output;
    std::size_t callback_allocations = 0;
    // The mechanics tests pin the window to the ring (capacity - 1); zero
    // selects the node's time-based default.
    std::uint32_t watchdog_blocks = kLead + 2;

    explicit Rig(FamilyCase c, std::uint32_t block = kBlock, bool fail_backend = false,
                 bool skip_publish = false, std::uint32_t prewarm = 0)
        : which(c), fixture(c.family, kChannels, block, c.nam_tcn) {
        init(block, fail_backend, skip_publish, prewarm, nullptr, 1);
    }

    /// A WaveNet GPU node whose CPU shadow runs `shadow`'s model, so a GPU
    /// fault hands the stream to that family's CPU path.
    struct GpuShadow {
        FamilyCase shadow;
        std::uint32_t max_inflight = 1;
        std::uint32_t watchdog_blocks = kLead + 2;
    };
    explicit Rig(GpuShadow gpu)
        : which(kWaveNetGpu), fixture(NeuralFamily::WaveNet, kChannels, kBlock) {
        const auto shadow = gpu.shadow;
        const auto max_inflight = gpu.max_inflight;
        watchdog_blocks = gpu.watchdog_blocks;
        if (shadow.family != NeuralFamily::WaveNet) {
            shadow_fixture =
                std::make_unique<FamilyFixture>(shadow.family, kChannels, kBlock, shadow.nam_tcn);
            auto build = detail::make_neural_model(shadow_fixture->request());
            REQUIRE(build);
            shadow_model = build.model;
        }
        init(kBlock, false, false, 0, shadow_model, max_inflight);
    }

    void init(std::uint32_t block, bool fail_backend, bool skip_publish, std::uint32_t prewarm,
              GpuNeuralModelHandle shadow, std::uint32_t max_inflight) {
        auto request = fixture.request();
        if (which.gpu_backend)
            request.backend = make_fake_wavenet_backend(plan, shape, kChannels, block, max_inflight,
                                                        fail_backend);
        auto build = detail::make_neural_model(std::move(request));
        REQUIRE(build);
        model = build.model;
        node = std::make_unique<GpuNeuralRealtimeNode>(GpuNeuralRealtimeNode::Config{
            .family = which.family,
            .model = model,
            .channels = kChannels,
            .lead_blocks = kLead,
            .capacity = kLead + 3,
            .max_inflight = max_inflight,
            .prewarm_blocks = prewarm,
            .shadow_policy = GpuNeuralRealtimeNode::ShadowPolicy::Inline,
            .shadow_model = shadow,
            .watchdog_blocks = watchdog_blocks});
        detail::NeuralRealtimeTestAccess::skip_shadow_publish(*node, skip_publish);
    }

    bool prepare() {
        return node->prepare() && transport.prepare(node.get(), {});
    }

    /// One callback of stimulus, then (unless starved) the worker pump.
    void block(bool pump = true) {
        const auto n = fixture.block;
        std::array<std::vector<float>, kChannels> in, out;
        std::array<const float*, kChannels> in_ptrs{};
        std::array<float*, kChannels> out_ptrs{};
        for (std::uint32_t ch = 0; ch < kChannels; ++ch) {
            in[ch].resize(n);
            out[ch].assign(n, 0.0f);
            for (std::uint32_t i = 0; i < n; ++i)
                in[ch][i] = stimulus(ch, frame + i);
            in_ptrs[ch] = in[ch].data();
            out_ptrs[ch] = out[ch].data();
        }
        audio::BufferView<const float> input(in_ptrs.data(), kChannels, n);
        audio::BufferView<float> result(out_ptrs.data(), kChannels, n);
        std::size_t allocations = 0;
        {
            // No Catch assertion inside the probe: a reporter may allocate.
            pulp::test::RtAllocationProbe probe;
            transport.process(input, result, n);
            allocations = probe.allocation_count();
        }
        callback_allocations += allocations;
        for (std::uint32_t ch = 0; ch < kChannels; ++ch)
            output[ch].insert(output[ch].end(), out[ch].begin(), out[ch].end());
        frame += n;
        if (pump)
            for (int i = 0; i < 2; ++i)
                transport.pump();
    }
    void render(std::uint32_t blocks, bool pump = true) {
        for (std::uint32_t b = 0; b < blocks; ++b)
            block(pump);
    }
};

/// The family's adapter driven directly (no node, transport, or delay ring),
/// then delayed by the node's lead. Same seeds, independent states.
std::array<std::vector<float>, kChannels> direct_reference(FamilyCase c, std::uint64_t frames,
                                                           std::uint32_t block = kBlock,
                                                           std::uint32_t prewarm = 0) {
    FamilyFixture fixture(c.family, kChannels, block, c.nam_tcn);
    auto build = detail::make_neural_model(fixture.request());
    REQUIRE(build);
    auto& model = *build.model;
    std::array<std::vector<float>, kChannels> reference;
    std::vector<float> zero(block, 0.0f), y(block, 0.0f), x(block, 0.0f);
    for (std::uint32_t ch = 0; ch < kChannels; ++ch) {
        auto& adapter = *model.channel_models[ch];
        const detail::StreamingPrepareContext context{
            .spec = &adapter.spec(),
            .artifact_id = model.artifact_id,
            .artifact_hash = model.artifact_sha256,
            .fallback = detail::StreamingFallbackStrategy::ContinuouslyPrimedCpuShadow,
            .max_frames = block,
            .lead_blocks = kLead};
        REQUIRE(adapter.prepare(context));
        const float* in_ptr[1] = {x.data()};
        float* out_ptr[1] = {y.data()};
        audio::BufferView<const float> in(in_ptr, 1, block);
        audio::BufferView<float> out(out_ptr, 1, block);
        for (std::uint32_t b = 0; b < prewarm; ++b) {
            std::fill(x.begin(), x.end(), 0.0f);
            adapter.process_cpu(in, out, block, {});
        }
        reference[ch].assign(frames, 0.0f);
        const auto delay = static_cast<std::uint64_t>(kLead) * block;
        for (std::uint64_t start = 0; start < frames; start += block) {
            for (std::uint32_t i = 0; i < block; ++i)
                x[i] = stimulus(ch, start + i);
            adapter.process_cpu(in, out, block, {});
            for (std::uint32_t i = 0; i < block && start + delay + i < frames; ++i)
                reference[ch][start + delay + i] = y[i];
        }
    }
    return reference;
}

/// Every status the tests read must satisfy the shared status contract.
GpuAudioStatus validated(const GpuAudioStatus& status) {
    CHECK(validate_gpu_audio_status(status).error == GpuAudioStatusError::None);
    return status;
}

float max_abs_difference(const std::array<std::vector<float>, kChannels>& a,
                         const std::array<std::vector<float>, kChannels>& b) {
    float worst = 0.0f;
    for (std::uint32_t ch = 0; ch < kChannels; ++ch) {
        REQUIRE(a[ch].size() == b[ch].size());
        for (std::size_t i = 0; i < a[ch].size(); ++i)
            worst = std::max(worst, std::abs(a[ch][i] - b[ch][i]));
    }
    return worst;
}

bool any_nonzero(const std::array<std::vector<float>, kChannels>& a) {
    for (const auto& channel : a)
        for (const auto value : channel)
            if (value != 0.0f)
                return true;
    return false;
}

} // namespace

TEST_CASE("every neural family nulls against its direct adapter with the lead delay",
          "[gpu_audio][neural][realtime]") {
    const auto c = GENERATE(kWaveNetGpu, kWaveNetCpu, kTcn, kNamTcn, kSsm, kLstm, kGru, kCustom);
    const auto prewarm = GENERATE(0u, 3u);
    CAPTURE(c.name, prewarm);
    Rig rig(c, kBlock, false, false, prewarm);
    REQUIRE(rig.prepare());
    constexpr std::uint32_t blocks = 40;
    rig.render(blocks);
    const auto reference = direct_reference(c, std::uint64_t{blocks} * kBlock, kBlock, prewarm);
    REQUIRE(any_nonzero(reference));
    CHECK(max_abs_difference(rig.output, reference) == 0.0f);

    const auto delivery = rig.transport.delivery_snapshot();
    if (c.gpu_backend) {
        // The GPU produced the audio; the shadow nulls with it anyway.
        CHECK(delivery.gpu_blocks == blocks - kLead);
        CHECK(delivery.priming_blocks == kLead);
        CHECK(delivery.cpu_fallback_blocks == 0);
        CHECK_FALSE(rig.plan.sequence_gap_seen);
    } else {
        CHECK(delivery.gpu_blocks == 0);
        CHECK(delivery.cpu_fallback_blocks == blocks);
    }
    CHECK(delivery.silence_blocks == 0);
    CHECK(rig.callback_allocations == 0);
}

TEST_CASE("every neural family callback performs no allocations", "[gpu_audio][neural][realtime]") {
    const auto c = GENERATE(kWaveNetGpu, kWaveNetCpu, kTcn, kNamTcn, kSsm, kLstm, kGru, kCustom);
    CAPTURE(c.name);
    Rig rig(c);
    REQUIRE(rig.prepare());
    rig.render(8);
    CHECK(rig.callback_allocations == 0);
    if (c.gpu_backend) {
        // The fallback path after a provider fault is a callback path too.
        rig.plan.fail_completions = true;
        rig.render(8);
        CHECK(rig.transport.delivery_snapshot().cpu_fallback_blocks > 0);
        CHECK(rig.callback_allocations == 0);
    }

    // Positive control: the probe sees an allocation made inside its scope.
    std::size_t control = 0;
    std::vector<float>* escaped = nullptr;
    {
        pulp::test::RtAllocationProbe probe;
        escaped = new std::vector<float>(16);
        control = probe.allocation_count();
    }
    // The pointer escapes the scope, so the allocation cannot be elided.
    CHECK(escaped->size() == 16);
    delete escaped;
    CHECK(control > 0);
}

TEST_CASE("neural node registers its realtime path and CPU provider through the registry",
          "[gpu_audio][neural][registry]") {
    Rig rig(kTcn);
    // Negative: an unprepared node still requires the realtime path (it has no
    // staged fallback) but has no active path and no provider yet.
    CHECK(detail::requires_realtime_gpu_path(rig.node.get()));
    CHECK_FALSE(detail::realtime_gpu_node_path(rig.node.get()).active());
    CHECK(detail::realtime_gpu_provider(rig.node.get()) == GpuAudioProvider::Unknown);
    // Positive: once prepared it binds an active path and its CPU identity.
    REQUIRE(rig.prepare());
    CHECK(detail::realtime_gpu_node_path(rig.node.get()).active());
    CHECK(detail::realtime_gpu_provider(rig.node.get()) == GpuAudioProvider::Cpu);
    CHECK(detail::realtime_gpu_provider_identity(rig.node.get()).provider == GpuAudioProvider::Cpu);
}

TEST_CASE("CPU-only families report CPU execution and never open a GPU epoch",
          "[gpu_audio][neural][status]") {
    const auto c = GENERATE(kWaveNetCpu, kTcn, kNamTcn, kSsm, kLstm, kGru, kCustom);
    CAPTURE(c.name);
    Rig rig(c);
    CHECK(validated(rig.node->status_snapshot()).provider_state ==
          GpuAudioProviderState::Uninitialized);
    CHECK(rig.node->epoch() == 0);
    REQUIRE(rig.prepare());
    for (int cycle = 0; cycle < 2; ++cycle) {
        CHECK(rig.node->epoch() == 0);
        rig.render(12);
        CHECK(rig.node->epoch() == 0);
        CHECK(rig.node->family() == c.family);
        CHECK_FALSE(rig.node->gpu_kernel_available());
        // Nothing to fence: there is no GPU delivery.
        CHECK_FALSE(rig.node->fenced());
        CHECK(rig.node->cpu_fallback_prepared());

        const auto capability = rig.node->capability_report();
        CHECK(capability.path == GpuAudioExecutionPath::Cpu);
        CHECK(capability.provider == GpuAudioProvider::Cpu);
        CHECK_FALSE(capability.gpu_kernel_available);
        CHECK(capability.fallback_policy == MissPolicy::CpuFallback);
        CHECK(capability.fallback_available);
        CHECK(capability.prepared_latency_blocks == kLead);

        const auto status = validated(rig.node->status_snapshot());
        CHECK(status.requested_engine == GpuAudioEngine::Cpu);
        CHECK(status.selected_engine == GpuAudioEngine::Cpu);
        CHECK(status.provider_state == GpuAudioProviderState::Ready);
        CHECK(status.provider == GpuAudioProvider::Cpu);
        CHECK_FALSE(status.gpu_kernel_available);
        CHECK(status.epoch == 0);
        CHECK_FALSE(status.delivery_epoch_active);
        CHECK(status.fallback_available);
        CHECK(status.produced_blocks == 0);
        CHECK(status.missed_blocks == 0);
        CHECK(status.fallback_blocks == 12);
        CHECK(status.latency_samples == kLead * kBlock);
        CHECK(status.recovery_reason == GpuAudioRecoveryReason::None);

        auto path = detail::NeuralRealtimeTestAccess::path(*rig.node);
        REQUIRE(path.active());
        CHECK(path.service(path.context, 0) == detail::kRealtimeGpuServiceInactive);
        detail::RealtimeGpuHealth health;
        REQUIRE(path.health != nullptr);
        REQUIRE(path.health(path.context, health));
        CHECK_FALSE(health.fenced);
        CHECK(health.epoch == 0);
        CHECK_FALSE(health.delivery_epoch_active);
        // Reached through the base class, as a host holding GpuAudioNode* would.
        const GpuAudioNode& base = *rig.node;
        CHECK(validated(base.status_snapshot()).selected_engine == GpuAudioEngine::Cpu);
        CHECK(detail::realtime_gpu_provider(rig.node.get()) == GpuAudioProvider::Cpu);

        // The transport hosting the node agrees: CPU path, CPU provider, Ready.
        const auto hosted = rig.transport.capability_report();
        CHECK(hosted.path == GpuAudioExecutionPath::Cpu);
        CHECK(hosted.provider == GpuAudioProvider::Cpu);
        CHECK(hosted.prepared_lead_blocks == kLead);
        const auto hosted_status = validated(rig.transport.status_snapshot());
        CHECK(hosted_status.selected_engine == GpuAudioEngine::Cpu);
        CHECK(hosted_status.provider == GpuAudioProvider::Cpu);
        CHECK(hosted_status.provider_state == GpuAudioProviderState::Ready);
        CHECK(hosted_status.epoch == 0);
        // The CPU model is this node's primary path, not a fallback for a miss.
        CHECK(hosted_status.missed_blocks == 0);
        CHECK(hosted_status.fallback_blocks == 12);
        CHECK(hosted_status.recovery_reason == GpuAudioRecoveryReason::None);

        // Reprepare (host reactivation) keeps the stream position.
        const auto position = path.next_sequence(path.context);
        rig.transport.release();
        REQUIRE(rig.node->prepare());
        CHECK(detail::NeuralRealtimeTestAccess::path(*rig.node).next_sequence(rig.node.get()) ==
              position);
        REQUIRE(rig.transport.prepare(rig.node.get(), {}));
    }
}

TEST_CASE("WaveNet family reports its GPU backend truthfully", "[gpu_audio][neural][status]") {
    SECTION("fake provider prepared") {
        Rig rig(kWaveNetGpu);
        REQUIRE(rig.prepare());
        rig.render(6);
        CHECK(rig.node->gpu_kernel_available());
        CHECK(rig.node->epoch() > 0);
        CHECK_FALSE(rig.node->fenced());
        const auto capability = rig.node->capability_report();
        CHECK(capability.path == GpuAudioExecutionPath::SharedMemory);
        CHECK(capability.gpu_kernel_available);
        // An injected channel is not an authenticated provider.
        CHECK(capability.provider == GpuAudioProvider::Unknown);
        const auto status = validated(rig.node->status_snapshot());
        CHECK(status.requested_engine == GpuAudioEngine::Gpu);
        CHECK(status.selected_engine == GpuAudioEngine::Unknown);
        CHECK(status.provider_state == GpuAudioProviderState::Degraded);
        CHECK(status.produced_blocks > 0);
        CHECK(status.gpu_kernel_available);
        CHECK(status.epoch == rig.node->epoch());
        CHECK(status.delivery_epoch_active);
        CHECK(status.fallback_available);
        CHECK(status.recovery_reason == GpuAudioRecoveryReason::None);
        // The hosting transport's capability and status agree that a GPU
        // kernel is prepared.
        const auto hosted = rig.transport.capability_report();
        CHECK(hosted.path != GpuAudioExecutionPath::Cpu);
        CHECK(hosted.gpu_kernel_available);
        CHECK(validated(rig.transport.status_snapshot()).gpu_kernel_available);
    }
    SECTION("provider unavailable degrades to the CPU-only contract") {
        Rig rig(kWaveNetGpu, kBlock, /*fail_backend=*/true);
        REQUIRE(rig.prepare());
        rig.render(6);
        CHECK_FALSE(rig.node->gpu_kernel_available());
        CHECK(rig.node->epoch() == 0);
        CHECK(rig.node->capability_report().path == GpuAudioExecutionPath::Cpu);
        CHECK(rig.node->capability_report().provider == GpuAudioProvider::Cpu);
        const auto status = validated(rig.node->status_snapshot());
        CHECK_FALSE(status.gpu_kernel_available);
        CHECK(status.requested_engine == GpuAudioEngine::Gpu);
        CHECK(status.selected_engine == GpuAudioEngine::Cpu);
        CHECK(status.provider_state == GpuAudioProviderState::Degraded);
        CHECK(rig.transport.delivery_snapshot().cpu_fallback_blocks == 6);
        CHECK(rig.plan.submissions == 0);
        // The hosting transport reports the real path, degraded, with a reason.
        CHECK(status.recovery_reason == GpuAudioRecoveryReason::ProviderFailure);
        const auto hosted = rig.transport.capability_report();
        CHECK(hosted.path == GpuAudioExecutionPath::Cpu);
        CHECK(hosted.provider == GpuAudioProvider::Cpu);
        CHECK_FALSE(hosted.gpu_kernel_available);
        const auto hosted_status = validated(rig.transport.status_snapshot());
        CHECK(hosted_status.provider_state == GpuAudioProviderState::Degraded);
        CHECK(hosted_status.selected_engine == GpuAudioEngine::Cpu);
        CHECK(hosted_status.recovery_reason == GpuAudioRecoveryReason::ProviderFailure);
        CHECK(hosted_status.missed_blocks == 0);
    }
}

TEST_CASE("WaveNet family stays continuous across provider faults",
          "[gpu_audio][neural][realtime]") {
    enum class Fault { FailCompletions, RejectSubmissions, ProviderLost, Unresponsive, Starve };
    const auto fault = GENERATE(Fault::FailCompletions, Fault::RejectSubmissions,
                                Fault::ProviderLost, Fault::Unresponsive, Fault::Starve);
    CAPTURE(static_cast<int>(fault));
    Rig rig(kWaveNetGpu);
    REQUIRE(rig.prepare());
    rig.render(10);
    REQUIRE(rig.transport.delivery_snapshot().gpu_blocks > 0);
    switch (fault) {
    case Fault::FailCompletions:
        rig.plan.fail_completions = true;
        break;
    case Fault::RejectSubmissions:
        rig.plan.reject_submissions = true;
        break;
    case Fault::ProviderLost:
        detail::NeuralRealtimeTestAccess::request_recovery(
            *rig.node, detail::SharedIoRecoveryReason::ProviderLost);
        break;
    case Fault::Unresponsive:
        detail::NeuralRealtimeTestAccess::request_recovery(
            *rig.node, detail::SharedIoRecoveryReason::ProviderUnresponsive);
        break;
    case Fault::Starve:
        rig.plan.starve = true;
        break;
    }
    rig.render(20);
    const auto reference = direct_reference(kWaveNetGpu, 30ull * kBlock);
    CHECK(max_abs_difference(rig.output, reference) == 0.0f);
    const auto delivery = rig.transport.delivery_snapshot();
    CHECK(delivery.cpu_fallback_blocks > 0);
    CHECK(delivery.silence_blocks == 0);
    CHECK(rig.callback_allocations == 0);
    // Positive control for the CPU-only rule: a GPU family's misses count.
    CHECK(validated(rig.transport.status_snapshot()).missed_blocks == delivery.cpu_fallback_blocks);
    if (fault == Fault::Starve) {
        // The completion watchdog names a hung provider before the ingress
        // ring saturates.
        CHECK(rig.node->recovery_reason() == GpuAudioRecoveryReason::ProviderUnresponsive);
        CHECK(validated(rig.node->status_snapshot()).provider_state ==
              GpuAudioProviderState::Unavailable);
    } else {
        CHECK(rig.node->fenced());
        const bool lost = fault == Fault::ProviderLost || fault == Fault::Unresponsive;
        const auto expected_state =
            lost ? GpuAudioProviderState::Unavailable : GpuAudioProviderState::Degraded;
        CHECK(validated(rig.node->status_snapshot()).provider_state == expected_state);
        CHECK(validated(rig.transport.status_snapshot()).provider_state == expected_state);
        const auto expected_reason =
            fault == Fault::ProviderLost   ? GpuAudioRecoveryReason::ProviderLost
            : fault == Fault::Unresponsive ? GpuAudioRecoveryReason::ProviderUnresponsive
                                           : GpuAudioRecoveryReason::ProviderFailure;
        CHECK(rig.node->recovery_reason() == expected_reason);
        CHECK(validated(rig.transport.status_snapshot()).recovery_reason == expected_reason);
    }
}

TEST_CASE("neural node whose GPU results all arrive late reads as not GPU-delivering",
          "[gpu_audio][neural][realtime][status]") {
    Rig rig(kWaveNetGpu);
    REQUIRE(rig.prepare());
    detail::NeuralRealtimeTestAccess::assume_authenticated(*rig.node);
    // Priming blocks are no evidence: the window stays empty until delivery.
    rig.render(kLead);
    CHECK_FALSE(detail::NeuralRealtimeTestAccess::gpu_lagging(*rig.node));
    // Positive control: a provider that keeps up delivers every block.
    rig.render(40);
    CHECK(rig.transport.delivery_snapshot().gpu_blocks > 0);
    CHECK_FALSE(detail::NeuralRealtimeTestAccess::gpu_lagging(*rig.node));
    auto healthy = validated(rig.node->status_snapshot());
    CHECK(healthy.provider_state == GpuAudioProviderState::Ready);
    CHECK(healthy.selected_engine == GpuAudioEngine::Gpu);
    // Stuck late: it keeps completing, too late for every consumer, so the
    // shadow carries the stream although no recovery cause latches.
    rig.plan.late = true;
    const auto gpu_before = rig.transport.delivery_snapshot().gpu_blocks;
    rig.render(40);
    // Only the blocks already completed in time when the delay began.
    CHECK(rig.transport.delivery_snapshot().gpu_blocks <= gpu_before + kLead);
    CHECK(rig.node->recovery_reason() == GpuAudioRecoveryReason::None);
    CHECK(detail::NeuralRealtimeTestAccess::gpu_lagging(*rig.node));
    auto status = validated(rig.node->status_snapshot());
    CHECK(status.provider_state == GpuAudioProviderState::Degraded);
    CHECK(status.selected_engine == GpuAudioEngine::Cpu);
    CHECK(validated(rig.transport.status_snapshot()).selected_engine != GpuAudioEngine::Gpu);
    // Recovery: the GPU carries the window again.
    rig.plan.late = false;
    rig.render(40);
    CHECK_FALSE(detail::NeuralRealtimeTestAccess::gpu_lagging(*rig.node));
    CHECK(rig.node->recovery_reason() == GpuAudioRecoveryReason::None);
    auto recovered = validated(rig.node->status_snapshot());
    CHECK(recovered.provider_state == GpuAudioProviderState::Ready);
    CHECK(recovered.selected_engine ==
          GpuAudioEngine::Gpu); // The callback records each block without allocating.
    CHECK(rig.callback_allocations == 0);
}

TEST_CASE("offline rendering fences the GPU and keeps the shadow timeline",
          "[gpu_audio][neural][realtime]") {
    const auto c = GENERATE(kWaveNetGpu, kLstm);
    CAPTURE(c.name);
    Rig rig(c);
    // GPU blocks come back doubled, so they cannot pass for shadow output.
    rig.plan.gain = 2.0f;
    REQUIRE(rig.prepare());
    constexpr std::uint32_t realtime_blocks = 6;
    rig.render(realtime_blocks);
    const auto submissions = rig.plan.submissions;
    const auto gpu_blocks = rig.transport.delivery_snapshot().gpu_blocks;
    const auto n = kBlock;
    std::array<std::vector<float>, kChannels> in, out;
    std::array<const float*, kChannels> in_ptrs{};
    std::array<float*, kChannels> out_ptrs{};
    for (std::uint32_t b = 0; b < 10; ++b) {
        for (std::uint32_t ch = 0; ch < kChannels; ++ch) {
            in[ch].resize(n);
            out[ch].assign(n, 0.0f);
            for (std::uint32_t i = 0; i < n; ++i)
                in[ch][i] = stimulus(ch, rig.frame + i);
            in_ptrs[ch] = in[ch].data();
            out_ptrs[ch] = out[ch].data();
        }
        audio::BufferView<const float> input(in_ptrs.data(), kChannels, n);
        audio::BufferView<float> result(out_ptrs.data(), kChannels, n);
        rig.transport.process_offline(input, result, n);
        for (std::uint32_t ch = 0; ch < kChannels; ++ch)
            rig.output[ch].insert(rig.output[ch].end(), out[ch].begin(), out[ch].end());
        rig.frame += n;
    }
    const auto reference = direct_reference(c, 16ull * kBlock);
    const auto split = std::uint64_t{realtime_blocks} * kBlock;
    float gpu_marked = 0.0f, offline_error = 0.0f, realtime_error = 0.0f;
    for (std::uint32_t ch = 0; ch < kChannels; ++ch)
        for (std::uint64_t i = 0; i < reference[ch].size(); ++i) {
            const float y = rig.output[ch][i], r = reference[ch][i];
            if (i >= split) {
                offline_error = std::max(offline_error, std::abs(y - r));
            } else if (c.gpu_backend && i >= std::uint64_t{kLead} * kBlock) {
                realtime_error = std::max(realtime_error, std::abs(y - 2.0f * r));
                gpu_marked = std::max(gpu_marked, std::abs(r));
            } else {
                realtime_error = std::max(realtime_error, std::abs(y - r));
            }
        }
    // Positive control: before the fence the GPU's doubled output was heard.
    CHECK(realtime_error == 0.0f);
    if (c.gpu_backend)
        CHECK(gpu_marked > 0.0f);
    // Every offline block is the unscaled shadow.
    CHECK(offline_error == 0.0f);
    CHECK(rig.node->fenced() == c.gpu_backend);
    CHECK(rig.plan.submissions == submissions);
    CHECK(rig.transport.delivery_snapshot().gpu_blocks == gpu_blocks);
    CHECK_FALSE(rig.node->gpu_kernel_available());
    CHECK_FALSE(rig.node->capability_report().gpu_kernel_available);
}

TEST_CASE("an unpublished CPU shadow makes prepare fail", "[gpu_audio][neural][negative]") {
    const auto c = GENERATE(kWaveNetGpu, kWaveNetCpu, kTcn, kNamTcn, kSsm, kLstm, kGru, kCustom);
    CAPTURE(c.name);
    {
        // Positive control: the same model and config prepare when published.
        Rig control(c);
        CHECK(control.prepare());
        CHECK(control.node->cpu_fallback_prepared());
    }
    Rig rig(c, kBlock, false, /*skip_publish=*/true);
    CHECK_FALSE(rig.node->prepare());
    CHECK(rig.node->last_prepare_error() == GpuNeuralRealtimeNode::PrepareError::ShadowUnpublished);
    CHECK_FALSE(rig.node->cpu_fallback_prepared());
    CHECK_FALSE(detail::realtime_gpu_node_path(rig.node.get()).active());
    CHECK_FALSE(rig.transport.prepare(rig.node.get(), {}));
    // Nothing reached the provider.
    CHECK(rig.plan.submissions == 0);
    CHECK(rig.node->epoch() == 0);
}

TEST_CASE("node configuration fails closed", "[gpu_audio][neural][negative]") {
    FamilyFixture fixture(NeuralFamily::Lstm, kChannels, kBlock);
    auto build = detail::make_neural_model(fixture.request());
    REQUIRE(build);
    const GpuNeuralRealtimeNode::Config good{.family = NeuralFamily::Lstm,
                                             .model = build.model,
                                             .channels = kChannels,
                                             .lead_blocks = kLead,
                                             .capacity = kLead + 3};
    using Error = GpuNeuralRealtimeNode::PrepareError;
    const auto mode = GENERATE(0, 1, 2, 3, 4, 5, 6);
    CAPTURE(mode);
    auto config = good;
    auto expected = Error::InvalidConfig;
    switch (mode) {
    case 0: // positive control
        expected = Error::None;
        break;
    case 1:
        config.family = NeuralFamily::Gru;
        expected = Error::FamilyMismatch;
        break;
    case 2:
        config.channels = 1;
        expected = Error::ChannelMismatch;
        break;
    case 3:
        config.miss_policy = MissPolicy::Silence;
        break;
    case 4:
        config.capacity = config.lead_blocks;
        break;
    case 5:
        config.lead_blocks = 0;
        break;
    case 6:
        config.model = nullptr;
        break;
    }
    GpuNeuralRealtimeNode node(config);
    CHECK(node.prepare() == (mode == 0));
    CHECK(node.last_prepare_error() == expected);
}

TEST_CASE("a model binds to exactly one node", "[gpu_audio][neural][negative]") {
    FamilyFixture fixture(NeuralFamily::Gru, kChannels, kBlock);
    auto build = detail::make_neural_model(fixture.request());
    REQUIRE(build);
    const GpuNeuralRealtimeNode::Config config{
        .family = NeuralFamily::Gru, .model = build.model, .channels = kChannels};
    auto first = std::make_unique<GpuNeuralRealtimeNode>(config);
    GpuNeuralRealtimeNode second(config);
    REQUIRE(first->prepare());
    CHECK_FALSE(second.prepare());
    CHECK(second.last_prepare_error() == GpuNeuralRealtimeNode::PrepareError::ModelBound);
    // Reprepare by the owner is allowed; destruction unbinds the model.
    CHECK(first->prepare());
    first.reset();
    CHECK(second.prepare());
}

TEST_CASE("model factory selects adapters per family and fails closed",
          "[gpu_audio][neural][factory]") {
    SECTION("only families with a GPU kernel accept a backend") {
        ChannelPlan plan;
        WaveNetShape shape;
        for (const auto& c : {kWaveNetCpu, kTcn, kNamTcn, kSsm, kLstm, kGru, kCustom}) {
            CAPTURE(c.name);
            FamilyFixture fixture(c.family, kChannels, kBlock, c.nam_tcn);
            auto request = fixture.request();
            request.backend = make_fake_wavenet_backend(plan, shape, kChannels, kBlock);
            const auto build = detail::make_neural_model(std::move(request));
            CHECK(build.error == (c.family == NeuralFamily::WaveNet
                                      ? detail::NeuralModelFactoryError::None
                                      : detail::NeuralModelFactoryError::UnsupportedBackend));
        }
    }
    SECTION("adapter type follows the family") {
        const auto adapter_is = []<class Adapter>(FamilyCase c, Adapter*) {
            FamilyFixture fixture(c.family, 1, kBlock, c.nam_tcn);
            auto build = detail::make_neural_model(fixture.request());
            REQUIRE(build);
            return dynamic_cast<Adapter*>(build.model->channel_models[0].get()) != nullptr;
        };
        CHECK(adapter_is(kTcn, static_cast<detail::TcnCpuAdapter*>(nullptr)));
        CHECK(adapter_is(kNamTcn, static_cast<detail::NamTcnStreamingAdapter*>(nullptr)));
        CHECK(adapter_is(kSsm, static_cast<detail::CompactSsmCpuAdapter*>(nullptr)));
        CHECK(adapter_is(kLstm, static_cast<detail::RecurrentCpuAdapter*>(nullptr)));
        CHECK(adapter_is(kGru, static_cast<detail::RecurrentCpuAdapter*>(nullptr)));
        CHECK(adapter_is(kWaveNetCpu, static_cast<detail::NamTcnStreamingAdapter*>(nullptr)));
        CHECK_FALSE(adapter_is(kTcn, static_cast<detail::RecurrentCpuAdapter*>(nullptr)));
    }
    SECTION("invalid requests") {
        const auto mode = GENERATE(0, 1, 2, 3, 4, 5, 6);
        CAPTURE(mode);
        FamilyFixture fixture(NeuralFamily::Lstm, kChannels, kBlock);
        auto request = fixture.request();
        auto expected = detail::NeuralModelFactoryError::None;
        switch (mode) {
        case 0: // positive control
            break;
        case 1:
            request.manifest.artifact_sha256 = "not-a-hash";
            expected = detail::NeuralModelFactoryError::InvalidManifest;
            break;
        case 2:
            request.manifest.state_bytes += 4;
            expected = detail::NeuralModelFactoryError::ShapeMismatch;
            break;
        case 3:
            request.recurrent.family = detail::RecurrentFamily::Gru;
            expected = detail::NeuralModelFactoryError::ShapeMismatch;
            break;
        case 4:
            request.channel_kernels = {};
            expected = detail::NeuralModelFactoryError::MissingKernels;
            break;
        case 5:
            request.family = NeuralFamily::Unknown;
            expected = detail::NeuralModelFactoryError::UnsupportedFamily;
            break;
        case 6:
            request.block_size = 0;
            expected = detail::NeuralModelFactoryError::InvalidBlockSize;
            break;
        }
        const auto build = detail::make_neural_model(std::move(request));
        CHECK(build.error == expected);
        CHECK(static_cast<bool>(build) == (mode == 0));
    }
}

// Measurement, not a gate: the per-block cost of the built-in shadow on the
// audio thread at 48 kHz. Run explicitly with "[neural-shadow-cost]".
TEST_CASE("neural shadow per-block CPU cost", "[.][neural-shadow-cost]") {
    struct CostCase {
        std::string name;
        NeuralFamily family;
        std::function<NeuralCpuKernel(std::vector<std::shared_ptr<void>>&)> make;
        std::uint64_t state_bytes = 0;
        std::string schema;
        detail::TcnCpuShape tcn{};
        detail::RecurrentCpuShape recurrent{};
        detail::CompactSsmCpuShape ssm{};
        std::string_view architecture = "custom";
    };
    const auto owned = [](auto kernel, std::vector<std::shared_ptr<void>>& keep) {
        auto shared = std::make_shared<decltype(kernel)>(std::move(kernel));
        keep.push_back(shared);
        return kernel_for(*shared);
    };
    std::vector<CostCase> cases;
    const std::array<std::array<std::uint32_t, 3>, 2> nam_standard{{{1, 16, 8}, {8, 8, 1}}};
    const std::array<std::array<std::uint32_t, 3>, 2> nam_lite{{{1, 12, 6}, {6, 6, 1}}};
    const std::array<std::array<std::uint32_t, 3>, 2> nam_feather{{{1, 8, 4}, {4, 4, 1}}};
    const std::array<std::array<std::uint32_t, 3>, 2> nam_nano{{{1, 4, 2}, {2, 2, 1}}};
    for (const auto& [name, shape] : {std::pair{"wavenet.nam-standard(16/8ch,2x10)", nam_standard},
                                      std::pair{"wavenet.nam-lite(12/6ch,2x10)", nam_lite},
                                      std::pair{"wavenet.nam-feather(8/4ch,2x10)", nam_feather},
                                      std::pair{"wavenet.nam-nano(4/2ch,2x10)", nam_nano}}) {
        cases.push_back(
            {name,
             NeuralFamily::WaveNet,
             [shape, &owned](auto& keep) { return owned(WaveNetKernel(shape, 10, 7), keep); },
             0,
             "",
             {},
             {},
             {},
             "nam.wavenet"});
    }
    const detail::TcnCpuShape tcn_max{
        .channels = 1, .kernel_size = 64, .dilation = 64, .layers = 1};
    cases.push_back({"tcn.scalar(k64,d64)",
                     NeuralFamily::Tcn,
                     [&](auto& keep) { return owned(TcnKernel(tcn_max, 7), keep); },
                     detail::tcn_cpu_state_bytes(tcn_max),
                     "causal-tcn-v1",
                     tcn_max,
                     {},
                     {},
                     "tcn.causal"});
    for (const auto& [name, c, l] :
         {std::tuple{"nam-tcn(8ch,k3,10L)", 8u, 10u}, std::tuple{"nam-tcn(16ch,k3,10L)", 16u, 10u}})
        cases.push_back(
            {name,
             NeuralFamily::Tcn,
             [c = c, l = l, &owned](auto& keep) { return owned(NamTcnKernel(c, 3, l, 7), keep); },
             0,
             "",
             {},
             {},
             {},
             detail::kNamTcnArchitecture});
    for (const std::uint32_t states : {64u, 256u}) {
        const detail::CompactSsmCpuShape ssm{
            .state_size = states, .input_size = 1, .output_size = 1, .layers = 1};
        cases.push_back({states == 64 ? "ssm.diagonal(64)" : "ssm.diagonal(256)",
                         NeuralFamily::Ssm,
                         [states, &owned](auto& keep) { return owned(SsmKernel(states, 7), keep); },
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
            cases.push_back(
                {std::string(family == detail::RecurrentFamily::Lstm ? "lstm" : "gru") + "(h" +
                     std::to_string(hidden) + ")",
                 family == detail::RecurrentFamily::Lstm ? NeuralFamily::Lstm : NeuralFamily::Gru,
                 [family, hidden, &owned](auto& keep) {
                     return owned(RecurrentKernel(family, hidden, 7), keep);
                 },
                 detail::recurrent_state_bytes(shape),
                 std::string(detail::recurrent_state_schema(family)),
                 {},
                 shape,
                 {},
                 family == detail::RecurrentFamily::Lstm ? "rnn.lstm" : "rnn.gru"});
        }

    std::printf("\nneural shadow cost, 1 channel, 48 kHz (median / p99 us per block, %% of "
                "block period)\n");
    for (const std::uint32_t block : {64u, 128u}) {
        const double period_us = 1e6 * block / 48000.0;
        for (const auto& cost : cases) {
            std::vector<std::shared_ptr<void>> keep;
            const auto kernel = cost.make(keep);
            detail::NeuralModelRequest request;
            request.family = cost.family;
            request.manifest = {.model_id = "pulp.cost",
                                .architecture = cost.architecture,
                                .model_version = "1",
                                .artifact_id = "pulp.cost.weights",
                                .artifact_sha256 = kSha,
                                .license = "MIT",
                                .runtime = "pulp.neural.test.v1",
                                .state_schema = cost.schema,
                                .artifact_size_bytes = 1,
                                .sample_rate_hz = 48000,
                                .state_bytes = cost.state_bytes,
                                .state_schema_version = cost.state_bytes ? 1u : 0u,
                                .redistributable = true};
            request.block_size = block;
            request.tcn = cost.tcn;
            request.recurrent = cost.recurrent;
            request.ssm = cost.ssm;
            request.channel_kernels = {&kernel, 1};
            auto build = detail::make_neural_model(std::move(request));
            REQUIRE(build);
            GpuNeuralRealtimeNode node({.family = cost.family,
                                        .model = build.model,
                                        .channels = 1,
                                        .lead_blocks = kLead,
                                        .capacity = kLead + 3});
            REQUIRE(node.prepare());
            std::vector<float> x(block), y(block);
            const float* in_ptr[1] = {x.data()};
            float* out_ptr[1] = {y.data()};
            audio::BufferView<const float> in(in_ptr, 1, block);
            audio::BufferView<float> out(out_ptr, 1, block);
            constexpr int warmup = 200, measured = 3000;
            std::vector<double> samples;
            samples.reserve(measured);
            std::uint64_t frame = 0;
            for (int b = 0; b < warmup + measured; ++b) {
                for (std::uint32_t i = 0; i < block; ++i)
                    x[i] = stimulus(0, frame + i);
                frame += block;
                const auto t0 = std::chrono::steady_clock::now();
                node.prime_fallback(in, block);
                node.process_cpu_fallback(in, out, block);
                const auto t1 = std::chrono::steady_clock::now();
                if (b >= warmup)
                    samples.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
            }
            std::sort(samples.begin(), samples.end());
            const double median = samples[samples.size() / 2];
            const double p99 = samples[samples.size() * 99 / 100];
            std::printf("  block %3u  %-36s median %8.2f us  p99 %8.2f us  %6.2f%%\n", block,
                        cost.name.c_str(), median, p99, 100.0 * median / period_us);
        }
    }
}

TEST_CASE("node in-flight depth may not exceed the backend's", "[gpu_audio][neural][negative]") {
    for (const std::uint32_t backend_inflight : {2u, 1u}) {
        CAPTURE(backend_inflight);
        FamilyFixture fixture(NeuralFamily::WaveNet, kChannels, kBlock);
        ChannelPlan plan;
        WaveNetShape shape;
        auto request = fixture.request();
        request.backend =
            make_fake_wavenet_backend(plan, shape, kChannels, kBlock, backend_inflight);
        auto build = detail::make_neural_model(std::move(request));
        REQUIRE(build);
        GpuNeuralRealtimeNode node({.family = NeuralFamily::WaveNet,
                                    .model = build.model,
                                    .channels = kChannels,
                                    .lead_blocks = kLead,
                                    .capacity = kLead + 3,
                                    .max_inflight = 2});
        // Positive control: matching depths prepare.
        CHECK(node.prepare() == (backend_inflight == 2));
        CHECK(node.last_prepare_error() ==
              (backend_inflight == 2
                   ? GpuNeuralRealtimeNode::PrepareError::None
                   : GpuNeuralRealtimeNode::PrepareError::BackendInflightExceeded));
    }
}

TEST_CASE("a failed GPU prewarm degrades to the CPU-only contract",
          "[gpu_audio][neural][negative]") {
    Rig rig(kWaveNetGpu, kBlock, false, false, /*prewarm=*/1);
    rig.plan.starve = true; // the prewarm block never completes
    REQUIRE(rig.prepare());
    CHECK_FALSE(rig.node->gpu_kernel_available());
    CHECK(rig.node->epoch() == 0);
    CHECK(rig.plan.releases >= kChannels);
    CHECK(validated(rig.node->status_snapshot()).provider_state == GpuAudioProviderState::Degraded);
    rig.plan.starve = false;
    rig.render(8);
    CHECK(max_abs_difference(rig.output, direct_reference(kWaveNetGpu, 8ull * kBlock, kBlock, 1)) ==
          0.0f);
}

TEST_CASE("a node whose release fails keeps its model bound", "[gpu_audio][neural][negative]") {
    for (const bool release_ok : {true, false}) {
        CAPTURE(release_ok);
        Rig rig(kWaveNetGpu);
        REQUIRE(rig.prepare());
        rig.render(4);
        rig.transport.release();
        rig.plan.release_ok = release_ok;
        rig.node.reset();
        CHECK(rig.model->bound.load() == !release_ok);
        GpuNeuralRealtimeNode adopter({.family = NeuralFamily::WaveNet,
                                       .model = rig.model,
                                       .channels = kChannels,
                                       .lead_blocks = kLead,
                                       .capacity = kLead + 3});
        // Positive control: a clean release hands the model on.
        CHECK(adopter.prepare() == release_ok);
        rig.plan.release_ok = true;
    }
}

TEST_CASE("the node's status projection keeps sustained deadline misses Degraded",
          "[gpu_audio][neural][status]") {
    // The node and the hosting transport both project health through this
    // mapping; only provider loss or an unresponsive provider is Unavailable.
    detail::RealtimeGpuHealth health;
    health.recovery_reason = GpuAudioRecoveryReason::SustainedDeadlineMisses;
    CHECK(detail::provider_state_for_health(health) == GpuAudioProviderState::Degraded);
    health.recovery_reason = GpuAudioRecoveryReason::ProviderUnresponsive;
    CHECK(detail::provider_state_for_health(health) == GpuAudioProviderState::Unavailable);
    health.recovery_reason = GpuAudioRecoveryReason::None;
    CHECK(detail::provider_state_for_health(health) == GpuAudioProviderState::Ready);
}

TEST_CASE("a degraded node delivers CPU only from a prepared fallback",
          "[gpu_audio][neural][status]") {
    using detail::project_node_state;
    const auto none = GpuAudioRecoveryReason::None;
    // Positive control: a Ready provider delivers GPU whatever the fallback.
    auto ready = project_node_state(false, none, true, MissPolicy::CpuFallback, false);
    CHECK(ready.provider_state == GpuAudioProviderState::Ready);
    CHECK(ready.selected_engine == GpuAudioEngine::Gpu);
    // Degraded with a prepared CPU fallback: the callback runs it.
    auto prepared = project_node_state(false, none, false, MissPolicy::CpuFallback, true);
    CHECK(prepared.provider_state == GpuAudioProviderState::Degraded);
    CHECK(prepared.selected_engine == GpuAudioEngine::Cpu);
    // Declared but not prepared, or a silent policy: no engine delivers.
    CHECK(project_node_state(false, none, false, MissPolicy::CpuFallback, false).selected_engine ==
          GpuAudioEngine::Unknown);
    CHECK(project_node_state(false, none, false, MissPolicy::Silence, true).selected_engine ==
          GpuAudioEngine::Unknown);
    // Loss outranks authentication.
    auto lost = project_node_state(true, none, true, MissPolicy::CpuFallback, true);
    CHECK(lost.provider_state == GpuAudioProviderState::Unavailable);
    CHECK(lost.selected_engine == GpuAudioEngine::Cpu);
    CHECK(detail::public_recovery_reason(detail::SharedIoRecoveryReason::ProviderLost) ==
          GpuAudioRecoveryReason::ProviderLost);
}

TEST_CASE("a GPU the miss substitute carries reads Degraded on the substitute's engine",
          "[gpu_audio][neural][status]") {
    using detail::project_node_state;
    using D = detail::SharedIoDeliveryDisposition;
    const auto none = GpuAudioRecoveryReason::None;
    detail::RecentGpuDelivery recent;
    // Priming is never counted, so an empty window is no evidence.
    for (int i = 0; i < 64; ++i)
        recent.record(D::Priming);
    CHECK_FALSE(recent.lagging());
    // A counted block the GPU did not deliver, with none it did (a re-acquired
    // epoch's warmup), is the substitute carrying the stream.
    recent.record(D::CpuFallbackDelivered);
    CHECK(recent.lagging());
    // Too few counted blocks are no evidence of falling behind once the GPU
    // delivered one of them.
    recent.reset();
    recent.record(D::GpuDelivered);
    for (int i = 0; i < detail::RecentGpuDelivery::kMinBlocks - 2; ++i)
        recent.record(D::CpuFallbackDelivered);
    CHECK_FALSE(recent.lagging());
    recent.record(D::CpuFallbackDelivered);
    CHECK(recent.lagging());
    // Half the window delivered by the GPU is not lagging; one block fewer is.
    recent.reset();
    for (int i = 0; i < 16; ++i)
        recent.record(D::GpuDelivered);
    for (int i = 0; i < 16; ++i)
        recent.record(D::SilenceDelivered);
    CHECK_FALSE(recent.lagging());
    recent.record(D::PassthroughDelivered);
    CHECK(recent.lagging());
    // Recovery: the GPU carrying the window again.
    for (int i = 0; i < 16; ++i)
        recent.record(D::GpuDelivered);
    CHECK_FALSE(recent.lagging());

    // Positive control: an authenticated, healthy provider whose GPU delivers.
    auto delivering = project_node_state(false, none, true, MissPolicy::CpuFallback, true, false);
    CHECK(delivering.provider_state == GpuAudioProviderState::Ready);
    CHECK(delivering.selected_engine == GpuAudioEngine::Gpu);
    auto lagging = project_node_state(false, none, true, MissPolicy::CpuFallback, true, true);
    CHECK(lagging.provider_state == GpuAudioProviderState::Degraded);
    CHECK(lagging.selected_engine == GpuAudioEngine::Cpu);
    auto silent = project_node_state(false, none, true, MissPolicy::Silence, false, true);
    CHECK(silent.provider_state == GpuAudioProviderState::Degraded);
    CHECK(silent.selected_engine == GpuAudioEngine::Unknown);
    // Loss stays Unavailable.
    CHECK(
        project_node_state(true, none, true, MissPolicy::CpuFallback, true, true).provider_state ==
        GpuAudioProviderState::Unavailable);
}

TEST_CASE("non-finite input reaches neither the GPU nor the shadow",
          "[gpu_audio][neural][realtime]") {
    const auto c = GENERATE(kWaveNetGpu, kLstm);
    const bool poison = GENERATE(true, false); // false is the positive control
    CAPTURE(c.name, poison);
    Rig rig(c);
    REQUIRE(rig.prepare());
    // A few NaN/Inf samples on both channels, early enough to be heard.
    const auto bad = [&](std::uint32_t ch, std::uint64_t frame) {
        return poison && (frame == 70 + ch || frame == 300 || frame == 515 - ch);
    };
    const auto value = [&](std::uint32_t ch, std::uint64_t frame) {
        if (!bad(ch, frame))
            return stimulus(ch, frame);
        return frame == 300 ? std::numeric_limits<float>::infinity()
               : ch == 0    ? std::numeric_limits<float>::quiet_NaN()
                            : -std::numeric_limits<float>::infinity();
    };
    constexpr std::uint32_t blocks = 16;
    std::array<std::vector<float>, kChannels> output;
    for (std::uint32_t b = 0; b < blocks; ++b) {
        std::array<std::vector<float>, kChannels> in, out;
        std::array<const float*, kChannels> in_ptrs{};
        std::array<float*, kChannels> out_ptrs{};
        for (std::uint32_t ch = 0; ch < kChannels; ++ch) {
            in[ch].resize(kBlock);
            out[ch].assign(kBlock, 0.0f);
            for (std::uint32_t i = 0; i < kBlock; ++i)
                in[ch][i] = value(ch, std::uint64_t{b} * kBlock + i);
            in_ptrs[ch] = in[ch].data();
            out_ptrs[ch] = out[ch].data();
        }
        audio::BufferView<const float> input(in_ptrs.data(), kChannels, kBlock);
        audio::BufferView<float> result(out_ptrs.data(), kChannels, kBlock);
        rig.transport.process(input, result, kBlock);
        for (std::uint32_t ch = 0; ch < kChannels; ++ch)
            output[ch].insert(output[ch].end(), out[ch].begin(), out[ch].end());
        for (int i = 0; i < 2; ++i)
            rig.transport.pump();
    }
    // Reference: the same adapter fed zeros where the input was non-finite.
    FamilyFixture fixture(c.family, kChannels, kBlock, c.nam_tcn);
    fixture.build_kernels();
    float worst = 0.0f;
    bool finite = true;
    const auto delay = std::uint64_t{kLead} * kBlock;
    for (std::uint32_t ch = 0; ch < kChannels; ++ch) {
        const auto& kernel = fixture.kernels[ch];
        for (std::uint64_t i = 0; i + delay < std::uint64_t{blocks} * kBlock; ++i) {
            const float x = bad(ch, i) ? 0.0f : stimulus(ch, i);
            float y = 0.0f;
            kernel.process(kernel.state, &x, &y, 1);
            finite = finite && std::isfinite(output[ch][i + delay]);
            worst = std::max(worst, std::abs(output[ch][i + delay] - y));
        }
    }
    CHECK(finite);
    CHECK(worst == 0.0f);
    CHECK(rig.node->sanitized_input_samples() == (poison ? 6u : 0u));
    if (c.gpu_backend) {
        // The GPU path delivered (so the provider saw the sanitized block).
        CHECK(rig.transport.delivery_snapshot().gpu_blocks == blocks - kLead);
        CHECK(rig.node->recovery_reason() == GpuAudioRecoveryReason::None);
    }
    validated(rig.node->status_snapshot());
    validated(rig.transport.status_snapshot());
}

namespace {

enum class GpuFault { None, LoseDevice, HangProvider };

/// Largest |output - reference| over output blocks [first, last).
float block_error(const Rig& rig, const std::array<std::vector<float>, kChannels>& reference,
                  std::uint32_t first, std::uint32_t last) {
    float worst = 0.0f;
    for (std::uint32_t ch = 0; ch < kChannels; ++ch)
        for (std::uint64_t i = std::uint64_t{first} * kBlock; i < std::uint64_t{last} * kBlock; ++i)
            worst = std::max(worst, std::abs(rig.output[ch][i] - reference[ch][i]));
    return worst;
}

} // namespace

TEST_CASE("GPU device loss and a hung provider are detected whatever the shadow family",
          "[gpu_audio][neural][realtime][loss]") {
    // The GPU path is WaveNet's; the CPU path that takes over is each
    // family's own model, so every family's fallback is exercised.
    const auto shadow = GENERATE(kWaveNetGpu, kTcn, kNamTcn, kSsm, kLstm, kGru);
    const auto fault = GENERATE(GpuFault::None, GpuFault::LoseDevice, GpuFault::HangProvider);
    CAPTURE(shadow.name, static_cast<int>(fault));
    Rig rig(Rig::GpuShadow{shadow, 1});
    REQUIRE(rig.prepare());
    rig.render(8);
    if (fault == GpuFault::LoseDevice)
        rig.plan.lost = true;
    if (fault == GpuFault::HangProvider)
        rig.plan.starve = true;
    rig.render(16);

    // Before the fault the GPU's FIR plays; from the first slot it cannot
    // deliver, the shadow family's model continues the stream exactly.
    const auto gpu_ref = direct_reference(kWaveNetGpu, 24ull * kBlock);
    const auto cpu_ref = direct_reference(shadow, 24ull * kBlock);
    // Loss fences delivery in the pump after callback 8, so callback 9 is
    // already the shadow's; a hang only misses from callback 10, block 8's
    // slot.
    const std::uint32_t handover = fault == GpuFault::None         ? 24
                                   : fault == GpuFault::LoseDevice ? 9
                                                                   : 10;
    CHECK(block_error(rig, gpu_ref, kLead, handover) == 0.0f);
    CHECK(block_error(rig, cpu_ref, handover, 24) == 0.0f);
    CHECK(rig.callback_allocations == 0);
    CHECK(rig.transport.delivery_snapshot().silence_blocks == 0);

    const auto own = validated(rig.node->status_snapshot());
    const auto hosted = validated(rig.transport.status_snapshot());
    detail::RealtimeGpuHealth health;
    const auto path = detail::NeuralRealtimeTestAccess::path(*rig.node);
    REQUIRE(path.health(path.context, health));
    switch (fault) {
    case GpuFault::None:
        // Positive control: a healthy provider latches nothing.
        CHECK(rig.node->recovery_reason() == GpuAudioRecoveryReason::None);
        CHECK(own.provider_state != GpuAudioProviderState::Unavailable);
        CHECK_FALSE(health.device_lost);
        CHECK(rig.transport.delivery_snapshot().gpu_blocks == 24 - kLead);
        break;
    case GpuFault::LoseDevice:
        // Loss outranks the failed submissions and completions it causes.
        CHECK(rig.node->recovery_reason() == GpuAudioRecoveryReason::ProviderLost);
        CHECK(hosted.recovery_reason == GpuAudioRecoveryReason::ProviderLost);
        CHECK(health.device_lost);
        break;
    case GpuFault::HangProvider:
        // Named by the watchdog, before the ingress ring saturates.
        CHECK(rig.node->recovery_reason() == GpuAudioRecoveryReason::ProviderUnresponsive);
        CHECK(hosted.recovery_reason == GpuAudioRecoveryReason::ProviderUnresponsive);
        CHECK_FALSE(health.device_lost);
        break;
    }
    if (fault != GpuFault::None) {
        CHECK(own.provider_state == GpuAudioProviderState::Unavailable);
        CHECK(hosted.provider_state == GpuAudioProviderState::Unavailable);
        CHECK(rig.node->fenced());
        CHECK(rig.node->epoch() == 0);
    }
}

TEST_CASE("the watchdog counts a pump's completions before judging silence",
          "[gpu_audio][neural][realtime][loss]") {
    // The default watchdog is capacity - 1 = 4 callback blocks. A provider
    // that stalls for four callbacks and completes in the fifth callback's
    // pump is slow, not unresponsive; one more stalled callback is a hang.
    const auto stalled = GENERATE(4u, 5u);
    CAPTURE(stalled);
    Rig rig(Rig::GpuShadow{kWaveNetGpu, 1});
    REQUIRE(rig.prepare());
    rig.render(8);
    rig.plan.starve = true;
    rig.render(stalled);
    rig.plan.starve = false;
    rig.render(8);
    CHECK(rig.node->recovery_reason() == (stalled == 4
                                              ? GpuAudioRecoveryReason::None
                                              : GpuAudioRecoveryReason::ProviderUnresponsive));
    validated(rig.node->status_snapshot());
    validated(rig.transport.status_snapshot());
    CHECK(rig.callback_allocations == 0);
}

TEST_CASE("the default watchdog is 200 ms of callback blocks and outlasts the ring",
          "[gpu_audio][neural][realtime][loss]") {
    // 200 ms at 48 kHz in 64-frame blocks is 150 callback blocks, and the
    // ring grows to hold them: a provider that stalls for 150 callbacks and
    // then completes is slow; one more stalled callback is a hang, reported
    // as ProviderUnresponsive rather than as a saturated ring.
    // Positive control: the same rig with an explicit 4-block window latches
    // after 5 stalled callbacks, so only the default's size is under test.
    const auto stalled = GENERATE(150u, 151u, 5u);
    CAPTURE(stalled);
    Rig rig(Rig::GpuShadow{.shadow = kWaveNetGpu, .watchdog_blocks = stalled == 5 ? 4u : 0u});
    REQUIRE(rig.prepare());
    rig.render(8);
    rig.plan.starve = true;
    rig.render(stalled);
    rig.plan.starve = false;
    rig.render(8);
    CHECK(rig.node->recovery_reason() == (stalled == 150
                                              ? GpuAudioRecoveryReason::None
                                              : GpuAudioRecoveryReason::ProviderUnresponsive));
    validated(rig.node->status_snapshot());
    validated(rig.transport.status_snapshot());
    CHECK(rig.callback_allocations == 0);
}

TEST_CASE("a ring saturated behind a hung provider is reported as the hang",
          "[gpu_audio][neural][realtime][loss]") {
    // A window wider than the ring: the input ring fills while the oldest
    // in-flight block has seen no completion since its admission.
    const auto window = GENERATE(64u, 3u);
    CAPTURE(window);
    // 64 is wider than the ring; 3, inside it, is the positive control: the
    // watchdog itself reports the same hang.
    Rig rig(Rig::GpuShadow{.shadow = kWaveNetGpu, .watchdog_blocks = window});
    REQUIRE(rig.prepare());
    rig.render(8);
    rig.plan.starve = true;
    rig.render(16);
    CHECK(rig.node->recovery_reason() == GpuAudioRecoveryReason::ProviderUnresponsive);
    CHECK(rig.node->status_snapshot().provider_state == GpuAudioProviderState::Unavailable);
    validated(rig.node->status_snapshot());
    CHECK(rig.callback_allocations == 0);
}

TEST_CASE("a completion seen only in the worker's tail pass refreshes the watchdog",
          "[gpu_audio][neural][realtime][loss]") {
    // Three blocks in flight; block 8 is slow. Newer blocks complete in the
    // tail pass of the pump that admitted them, which is progress. With the
    // refresh, silence starts after the last newer completion (callback 10),
    // so the watchdog fires after callback 14; holding block 8 through
    // callback 12 must not latch, holding it through callback 14 must.
    const auto held = GENERATE(5u, 7u);
    CAPTURE(held);
    Rig rig(Rig::GpuShadow{kWaveNetGpu, 3});
    REQUIRE(rig.prepare());
    rig.render(8);
    rig.plan.hold = 8;
    rig.render(held);
    INFO("after hold: reason " << static_cast<int>(rig.node->recovery_reason()) << " at "
                               << rig.node->recovery_sequence());
    rig.plan.hold = std::numeric_limits<std::uint64_t>::max();
    rig.render(6);
    INFO("final latch at " << rig.node->recovery_sequence());
    CHECK(
        rig.node->recovery_reason() ==
        (held == 5 ? GpuAudioRecoveryReason::None : GpuAudioRecoveryReason::ProviderUnresponsive));
    validated(rig.node->status_snapshot());
    validated(rig.transport.status_snapshot());
}

TEST_CASE("an invalid callback is counted, not latched, on the CPU-only path",
          "[gpu_audio][neural][status]") {
    const auto c = GENERATE(kLstm, kWaveNetGpu);
    CAPTURE(c.name);
    Rig rig(c);
    REQUIRE(rig.prepare());
    rig.render(4);
    // One callback with the wrong frame count: the transport rejects it.
    std::array<std::vector<float>, kChannels> in, out;
    std::array<const float*, kChannels> in_ptrs{};
    std::array<float*, kChannels> out_ptrs{};
    for (std::uint32_t ch = 0; ch < kChannels; ++ch) {
        in[ch].assign(kBlock, 0.25f);
        out[ch].assign(kBlock, 0.0f);
        in_ptrs[ch] = in[ch].data();
        out_ptrs[ch] = out[ch].data();
    }
    audio::BufferView<const float> input(in_ptrs.data(), kChannels, kBlock);
    audio::BufferView<float> result(out_ptrs.data(), kChannels, kBlock);
    rig.transport.process(input, result, kBlock / 2);
    rig.render(4);
    const auto status = validated(rig.node->status_snapshot());
    if (c.gpu_backend) {
        // Positive control: on the GPU path the violation fences delivery.
        CHECK(rig.node->recovery_reason() == GpuAudioRecoveryReason::InvalidCallback);
        CHECK(rig.node->fenced());
    } else {
        CHECK(rig.node->callback_contract_violations() == 1);
        CHECK(rig.node->recovery_reason() == GpuAudioRecoveryReason::None);
        CHECK(status.provider_state == GpuAudioProviderState::Ready);
        CHECK(validated(rig.transport.status_snapshot()).provider_state ==
              GpuAudioProviderState::Ready);
    }
}

namespace {
// A session with GpuWaveNetSession's surface and no GPU: each block returns
// its input. `Outcome` picks what create() reports.
template <GpuWaveNetSessionError Outcome, bool WithSession> struct FakeSession {
    struct CreateResult {
        std::unique_ptr<FakeSession> session;
        GpuWaveNetSessionError error = GpuWaveNetSessionError::None;
        explicit operator bool() const noexcept {
            return session != nullptr && error == GpuWaveNetSessionError::None;
        }
    };
    static inline int created = 0, released = 0;
    static CreateResult create(const GpuWaveNetSession::Config&) {
        ++created;
        return {WithSession ? std::make_unique<FakeSession>() : nullptr, Outcome};
    }
    std::array<float, 4096> held{};
    std::optional<std::uint64_t> pending;
    bool submit_block(std::span<const float> input, std::uint64_t sequence) noexcept {
        if (pending)
            return false;
        std::copy(input.begin(), input.end(), held.begin());
        pending = sequence;
        return true;
    }
    std::size_t service(std::uint64_t) noexcept {
        return 0;
    }
    std::size_t service_until(std::uint64_t) noexcept {
        return 0;
    }
    std::optional<GpuWaveNetBlockResult> receive(std::span<float> output) noexcept {
        if (!pending)
            return std::nullopt;
        std::copy_n(held.begin(), output.size(), output.begin());
        const GpuWaveNetBlockResult result{.sequence = *pending,
                                           .status = GpuWaveNetBlockStatus::GpuDelivered};
        pending.reset();
        return result;
    }
    bool release() noexcept {
        ++released;
        return true;
    }
    bool device_lost() const noexcept {
        return false;
    }
};
using CreatedSession = FakeSession<GpuWaveNetSessionError::None, true>;
using PartialSession = FakeSession<GpuWaveNetSessionError::PreparationFailed, true>;
using MissingSession = FakeSession<GpuWaveNetSessionError::ProviderUnavailable, false>;

template <class Session> std::unique_ptr<detail::WaveNetStreamingBackend> session_backend() {
    static const WaveNetShape shape;
    return std::make_unique<detail::WaveNetStreamingBackend>(
        detail::WaveNetStreamingBackend::Config{.session = shape.session(kBlock, 48000),
                                                .channels = kChannels},
        detail::WaveNetStreamingBackend::ChannelFactory{},
        &detail::create_session_channel<Session>);
}
} // namespace

/// The backend's own prepare context, as the node builds it.
struct BackendContext {
    detail::StreamingModelSpec spec{.model_id = "pulp.test.wavenet",
                                    .architecture = "wavenet",
                                    .model_version = "1",
                                    .weights_hash = kSha,
                                    .runtime_hash = "pulp.neural.test.v1",
                                    .input_channels = kChannels,
                                    .output_channels = kChannels,
                                    .sample_rate = 48000,
                                    .block_size = kBlock,
                                    .feature_rate = 48000};
    detail::StreamingPrepareContext context() const {
        return {.spec = &spec,
                .artifact_id = "pulp.test.wavenet.weights",
                .artifact_hash = kSha,
                .fallback = detail::StreamingFallbackStrategy::ContinuouslyPrimedCpuShadow,
                .max_frames = kBlock,
                .lead_blocks = kLead,
                .worker_backend_requested = true};
    }
};

TEST_CASE("the WaveNet backend prepares on the session path only when create succeeds",
          "[gpu_audio][neural][wavenet-session]") {
    const BackendContext ctx;
    REQUIRE(detail::valid_streaming_prepare_context(ctx.context()));
    SECTION("a created session: one session channel per audio channel") {
        CreatedSession::created = CreatedSession::released = 0;
        auto backend = session_backend<CreatedSession>();
        REQUIRE(backend->prepare(ctx.context()));
        CHECK(backend->prepared());
        CHECK(backend->channel_count() == kChannels);
        CHECK(CreatedSession::created == static_cast<int>(kChannels));
        CHECK_FALSE(backend->production_provider());
        REQUIRE(backend->release());
        CHECK(CreatedSession::released == static_cast<int>(kChannels));
    }
    SECTION("a session created with an error: refused, released, nothing retained") {
        PartialSession::created = PartialSession::released = 0;
        auto backend = session_backend<PartialSession>();
        CHECK_FALSE(backend->prepare(ctx.context()));
        CHECK_FALSE(backend->prepared());
        CHECK(PartialSession::created == 1);
        CHECK(PartialSession::released == 1);
        CHECK(backend->channel_count() == 0);
    }
    SECTION("no session: refused, nothing retained") {
        MissingSession::created = 0;
        auto backend = session_backend<MissingSession>();
        CHECK_FALSE(backend->prepare(ctx.context()));
        CHECK(MissingSession::created == 1);
        CHECK(backend->channel_count() == 0);
    }
    SECTION("the production creator agrees with GpuWaveNetSession::create on this host") {
        // No factory and no creator: the authenticated session path. Without
        // a provider (GPU-less CI) both refuse; with one both succeed.
        static const WaveNetShape shape;
        const auto config = shape.session(kBlock, 48000);
        bool available = false;
        {
            auto probe = GpuWaveNetSession::create(config);
            available = static_cast<bool>(probe);
            if (probe.session)
                REQUIRE(probe.session->release());
        }
        detail::WaveNetStreamingBackend backend({.session = config, .channels = kChannels});
        CAPTURE(available);
        CHECK(backend.prepare(ctx.context()) == available);
        CHECK(backend.production_provider() == available);
        CHECK(backend.channel_count() == (available ? kChannels : 0u));
        REQUIRE(backend.release());
        CHECK(backend.channel_count() == 0);
    }
}

/// A WaveNet node over a substituted session creator, prepared.
template <class Session> struct SessionNode {
    FamilyFixture fixture{NeuralFamily::WaveNet, kChannels, kBlock};
    std::unique_ptr<GpuNeuralRealtimeNode> node;
    bool prepared = false;
    SessionNode() {
        Session::created = Session::released = 0;
        auto request = fixture.request();
        request.backend = session_backend<Session>();
        auto build = detail::make_neural_model(std::move(request));
        REQUIRE(build);
        node = std::make_unique<GpuNeuralRealtimeNode>(
            GpuNeuralRealtimeNode::Config{.family = NeuralFamily::WaveNet,
                                          .model = build.model,
                                          .channels = kChannels,
                                          .lead_blocks = kLead,
                                          .capacity = kLead + 3});
        prepared = node->prepare();
    }
};

TEST_CASE("a WaveNet node prepares its GPU backend when the session is created",
          "[gpu_audio][neural][wavenet-session]") {
    SECTION("a created session is adopted and the node runs the GPU path") {
        SessionNode<CreatedSession> rig;
        REQUIRE(rig.prepared);
        CHECK(CreatedSession::created == static_cast<int>(kChannels));
        CHECK(rig.node->gpu_kernel_available());
        const auto status = rig.node->status_snapshot();
        CHECK(validate_gpu_audio_status(status).accepted());
        CHECK(status.gpu_kernel_available);
        CHECK(status.recovery_reason == GpuAudioRecoveryReason::None);
        CHECK(status.provider_state != GpuAudioProviderState::Unavailable);
        REQUIRE(rig.node->release());
        CHECK(CreatedSession::released == static_cast<int>(kChannels));
    }
    SECTION("a session created with an error is refused, then released") {
        SessionNode<PartialSession> rig;
        // The node still prepares, on its CPU model, and says why.
        REQUIRE(rig.prepared);
        CHECK_FALSE(rig.node->gpu_kernel_available());
        CHECK(rig.node->status_snapshot().recovery_reason ==
              GpuAudioRecoveryReason::ProviderFailure);
        CHECK(PartialSession::created == 1);
        CHECK(PartialSession::released == 1);
    }
    SECTION("no session at all is refused") {
        SessionNode<MissingSession> rig;
        REQUIRE(rig.prepared);
        CHECK_FALSE(rig.node->gpu_kernel_available());
        CHECK(MissingSession::created == 1);
    }
}

TEST_CASE("the neural WaveNet node keeps two blocks in flight by default when it can",
          "[gpu_audio][neural][depth]") {
    // max_inflight = 0 resolves to two when the backend admits two and the
    // ring holds two blocks past the lead, else one; an explicit value is
    // kept. A backend left at zero takes two from the session's slots. The
    // lead, and so the reported latency, does not change.
    struct Case {
        std::uint32_t node_inflight, backend_inflight, capacity;
        std::uint64_t expected_depth;
    };
    for (const auto c :
         {Case{0, 2, kLead + 3, 2}, Case{0, 0, kLead + 3, 2}, Case{0, 2, kLead + 1, 1},
          Case{0, 1, kLead + 3, 1}, Case{1, 2, kLead + 3, 1}}) {
        CAPTURE(c.node_inflight, c.backend_inflight, c.capacity);
        FamilyFixture fixture(NeuralFamily::WaveNet, kChannels, kBlock);
        ChannelPlan plan;
        WaveNetShape shape;
        auto request = fixture.request();
        request.backend =
            make_fake_wavenet_backend(plan, shape, kChannels, kBlock, c.backend_inflight);
        auto build = detail::make_neural_model(std::move(request));
        REQUIRE(build);
        GpuNeuralRealtimeNode node({.family = NeuralFamily::WaveNet,
                                    .model = build.model,
                                    .channels = kChannels,
                                    .lead_blocks = kLead,
                                    .capacity = c.capacity,
                                    .max_inflight = c.node_inflight,
                                    .shadow_policy = GpuNeuralRealtimeNode::ShadowPolicy::Inline,
                                    .watchdog_blocks = 64});
        REQUIRE(node.prepare());
        GpuAudioTransport transport;
        REQUIRE(transport.prepare(&node, {}));
        // Nothing completes, so the worker admits up to its depth and stops.
        plan.starve = true;
        std::array<std::vector<float>, kChannels> in, out;
        std::array<const float*, kChannels> in_ptrs{};
        std::array<float*, kChannels> out_ptrs{};
        for (std::uint32_t ch = 0; ch < kChannels; ++ch) {
            in[ch].assign(kBlock, 0.25f);
            out[ch].assign(kBlock, 0.0f);
            in_ptrs[ch] = in[ch].data();
            out_ptrs[ch] = out[ch].data();
        }
        audio::BufferView<const float> input(in_ptrs.data(), kChannels, kBlock);
        audio::BufferView<float> result(out_ptrs.data(), kChannels, kBlock);
        for (std::uint32_t b = 0; b + 1 < c.capacity; ++b) {
            transport.process(input, result, kBlock);
            transport.pump();
            transport.pump();
        }
        CHECK(plan.submissions == c.expected_depth * kChannels);
        CHECK(detail::NeuralRealtimeTestAccess::in_flight_high_water(node) == c.expected_depth);
        const auto status = node.status_snapshot();
        CHECK(validate_gpu_audio_status(status).accepted());
        CHECK(validate_gpu_audio_status(transport.status_snapshot()).accepted());
        CHECK(status.prepared_lead_blocks == kLead);
        CHECK(status.latency_samples == kLead * kBlock);
        transport.release();
        REQUIRE(node.release());
    }
}
