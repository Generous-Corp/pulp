#include <catch2/catch_test_macros.hpp>

#include <pulp/gpu_audio/gpu_audio_node.hpp>
#include <pulp/gpu_audio/gpu_audio_program.hpp>
#include <pulp/gpu_audio/gpu_audio_transport.hpp>

#include "detail/gpu_audio_transport_trial_observer.hpp"
#include "detail/realtime_gpu_audio_path.hpp"
#include "detail/shared_io_trace.hpp"
#include "harness/rt_allocation_probe.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <thread>
#include <vector>

using namespace pulp::gpu_audio;
using pulp::audio::BufferView;

namespace {

GpuAudioProgramDescriptor valid_shared_program() {
    return {.kind = GpuAudioProgramKind::Neural,
            .path = GpuAudioExecutionPath::SharedMemory,
            .provider = GpuAudioProvider::Dawn,
            .miss_policy = MissPolicy::CpuFallback,
            .channels = 2,
            .block_size = 32,
            .sample_rate = 48000,
            .algorithmic_lead_blocks = 2,
            .pipeline_depth = 4,
            .provider_slots = 4,
            .provider_owned_resources = true,
            .cpu_fallback_prepared = true};
}

} // namespace

TEST_CASE("GpuAudioProgramDescriptor validates a prepared shared program", "[gpu_audio][program]") {
    const auto program = valid_shared_program();
    const auto validation = validate_gpu_audio_program(program);
    CHECK(validation.accepted());
    CHECK(program.provider == GpuAudioProvider::Dawn);
    CHECK(program.provider_owned_resources);
}

TEST_CASE("GpuAudioProgramDescriptor rejects incomplete shared preparation",
          "[gpu_audio][program]") {
    auto program = valid_shared_program();

    program.algorithmic_lead_blocks = 0;
    CHECK(validate_gpu_audio_program(program).error ==
          GpuAudioProgramError::MissingAlgorithmicLead);

    program.algorithmic_lead_blocks = 2;
    program.pipeline_depth = 2;
    CHECK(validate_gpu_audio_program(program).error ==
          GpuAudioProgramError::InsufficientPipelineDepth);

    program.pipeline_depth = 4;
    program.provider = GpuAudioProvider::Unknown;
    CHECK(validate_gpu_audio_program(program).error ==
          GpuAudioProgramError::MissingProviderIdentity);
}

TEST_CASE("GpuAudioProgramDescriptor rejects unsafe provider and fallback claims",
          "[gpu_audio][program]") {
    auto program = valid_shared_program();

    program.provider_owned_resources = false;
    CHECK(validate_gpu_audio_program(program).error ==
          GpuAudioProgramError::ProviderResourcesNotOwned);

    program = valid_shared_program();
    program.miss_policy = MissPolicy::CpuFallback;
    program.cpu_fallback_prepared = false;
    CHECK(validate_gpu_audio_program(program).error ==
          GpuAudioProgramError::CpuFallbackNotPrepared);

    program = valid_shared_program();
    program.path = GpuAudioExecutionPath::Cpu;
    program.provider = GpuAudioProvider::Unknown;
    program.provider_owned_resources = false;
    program.algorithmic_lead_blocks = 0;
    program.pipeline_depth = 0;
    program.provider_slots = 0;
    program.miss_policy = MissPolicy::Silence;
    CHECK(validate_gpu_audio_program(program).accepted());
}

namespace pulp::gpu_audio::test_detail {

class RealtimeHookNode : public GpuAudioNode {
  public:
    RealtimeHookNode(uint32_t channels, uint32_t block, MissPolicy policy, uint32_t latency = 2)
        : channels_(channels), block_(block), policy_(policy), latency_(latency) {}

    GpuAudioNodeDescriptor descriptor() const override {
        GpuAudioNodeDescriptor d;
        d.name = "realtime-hook";
        d.input_channels = channels_;
        d.output_channels = channels_;
        d.block_size = block_;
        d.sample_rate = 48000;
        d.latency_blocks = latency_;
        d.miss_policy = policy_;
        d.supports_cpu_fallback = policy_ == MissPolicy::CpuFallback;
        return d;
    }

    bool prepare() override {
        next_sequence = 0;
        fallback_delay_index_ = 0;
        fallback_delay_.assign(static_cast<std::size_t>(latency_) * channels_ * block_, 0.0f);
        fallback_due_.assign(static_cast<std::size_t>(channels_) * block_, 0.0f);
        return true;
    }

    void process_block(const BufferView<const float>& input, BufferView<float>& output,
                       uint32_t n) override {
        ++process_block_calls;
        output.clear();
        for (uint32_t c = 0; c < channels_ && c < output.num_channels(); ++c) {
            const float* src = input.channel_ptr(c);
            float* dst = output.channel_ptr(c);
            for (uint32_t i = 0; i < n; ++i)
                dst[i] = src[i] * 10.0f;
        }
    }

    void process_cpu_fallback(const BufferView<const float>& input, BufferView<float>& output,
                              uint32_t n) noexcept override {
        ++cpu_fallback_calls;
        output.clear();
        for (uint32_t c = 0; c < channels_ && c < output.num_channels(); ++c) {
            const float* src = stateful_fallback
                                   ? fallback_due_.data() + static_cast<std::size_t>(c) * block_
                                   : input.channel_ptr(c);
            float* dst = output.channel_ptr(c);
            for (uint32_t i = 0; i < n; ++i)
                dst[i] = stateful_fallback ? src[i] : src[i] * -1.0f;
        }
    }

    void prime_fallback(const BufferView<const float>& input, uint32_t n) noexcept override {
        ++prime_fallback_calls;
        if (!stateful_fallback || n != block_)
            return;
        auto* slot = fallback_delay_.data() +
                     static_cast<std::size_t>(fallback_delay_index_) * channels_ * block_;
        std::copy_n(slot, fallback_due_.size(), fallback_due_.data());
        for (uint32_t c = 0; c < channels_; ++c)
            std::copy_n(input.channel_ptr(c), block_, slot + static_cast<std::size_t>(c) * block_);
        fallback_delay_index_ = (fallback_delay_index_ + 1) % latency_;
    }

    detail::RealtimeGpuNodePath path() noexcept {
        return {.context = this,
                .process = &RealtimeHookNode::process_realtime,
                .service = &RealtimeHookNode::service_realtime,
                .fence = &RealtimeHookNode::fence_realtime,
                .delivered = &RealtimeHookNode::delivered_realtime,
                .next_sequence = &RealtimeHookNode::get_next_sequence};
    }

    std::uint64_t last_sequence = 0;
    std::uint64_t delivered_sequence = 0;
    std::uint32_t delivery_calls = 0;
    detail::SharedIoDeliveryDisposition delivery = detail::SharedIoDeliveryDisposition::None;
    std::uint8_t realtime_status = detail::kRealtimeGpuReady;
    std::uint32_t service_return_blocks = 3;
    bool fence_result = true;
    std::uint32_t realtime_calls = 0;
    std::uint32_t service_calls = 0;
    std::uint32_t fence_calls = 0;
    std::uint32_t process_block_calls = 0;
    std::uint32_t prime_fallback_calls = 0;
    std::uint32_t cpu_fallback_calls = 0;
    bool stateful_fallback = false;
    bool enforce_sequence = false;
    bool sequence_fenced = false;
    std::uint64_t next_sequence = 0;
    std::uint32_t stale_callback_calls = 0;
    std::uint32_t sequence_gap_calls = 0;
    std::uint32_t next_sequence_queries = 0;
    std::uint32_t invalid_callback_calls = 0;
    bool invalid_input_zero_filled = true;
    bool callback_shape_valid = true;
    const float* external_output_sample = nullptr;
    bool external_output_silent_on_rejection = true;

  private:
    static std::uint8_t process_realtime(void* self, const BufferView<const float>& input,
                                         BufferView<float>& output, std::uint32_t n,
                                         std::uint64_t sequence, bool input_valid,
                                         std::uint64_t /*callback_start_ns*/) noexcept {
        auto* node = static_cast<RealtimeHookNode*>(self);
        node->last_sequence = sequence;
        ++node->realtime_calls;
        if (node->enforce_sequence && sequence != node->next_sequence) {
            if (sequence < node->next_sequence)
                ++node->stale_callback_calls;
            else
                ++node->sequence_gap_calls;
            node->sequence_fenced = true;
        }
        node->next_sequence = std::max(node->next_sequence, sequence + 1);
        const bool shape_valid = n == node->block_ && input.num_channels() == node->channels_ &&
                                 output.num_channels() == node->channels_ &&
                                 input.num_samples() >= n && output.num_samples() >= n;
        node->callback_shape_valid &= shape_valid;
        if (!input_valid) {
            ++node->invalid_callback_calls;
            node->sequence_fenced = true;
            node->invalid_input_zero_filled &= shape_valid;
            if (shape_valid) {
                for (uint32_t c = 0; c < node->channels_; ++c)
                    for (uint32_t i = 0; i < n; ++i)
                        node->invalid_input_zero_filled &= input.channel_ptr(c)[i] == 0.0f;
            }
        }
        if (node->sequence_fenced)
            return detail::kRealtimeGpuMissed;
        if (node->realtime_status == detail::kRealtimeGpuReady) {
            output.clear();
            for (uint32_t c = 0; c < node->channels_ && c < output.num_channels(); ++c) {
                const float* src = input.channel_ptr(c);
                float* dst = output.channel_ptr(c);
                for (uint32_t i = 0; i < n; ++i)
                    dst[i] = src[i] + 1000.0f;
            }
        }
        return node->realtime_status;
    }

    static std::uint64_t get_next_sequence(void* self) noexcept {
        auto* node = static_cast<RealtimeHookNode*>(self);
        ++node->next_sequence_queries;
        return node->next_sequence;
    }

    static void delivered_realtime(void* self, std::uint64_t sequence, std::uint8_t disposition,
                                   std::uint64_t /*callback_end_ns*/,
                                   std::uint64_t /*result_visible_ns*/) noexcept {
        auto* node = static_cast<RealtimeHookNode*>(self);
        ++node->delivery_calls;
        node->delivered_sequence = sequence;
        node->delivery = static_cast<detail::SharedIoDeliveryDisposition>(disposition);
        if (node->delivery == detail::SharedIoDeliveryDisposition::InvalidRejected &&
            node->external_output_sample)
            node->external_output_silent_on_rejection &= *node->external_output_sample == 0.0f;
    }

    static std::uint32_t service_realtime(void* self, std::uint64_t) noexcept {
        auto* node = static_cast<RealtimeHookNode*>(self);
        ++node->service_calls;
        return node->service_return_blocks;
    }

    static bool fence_realtime(void* self) noexcept {
        auto* node = static_cast<RealtimeHookNode*>(self);
        ++node->fence_calls;
        node->realtime_status = detail::kRealtimeGpuMissed;
        return node->fence_result;
    }

    uint32_t channels_;
    uint32_t block_;
    MissPolicy policy_;
    uint32_t latency_;
    uint32_t fallback_delay_index_ = 0;
    std::vector<float> fallback_delay_;
    std::vector<float> fallback_due_;
};

} // namespace pulp::gpu_audio::test_detail

namespace pulp::gpu_audio::detail {

RealtimeGpuNodePath realtime_gpu_node_path(GpuAudioNode* node) noexcept {
    if (auto* hook = dynamic_cast<test_detail::RealtimeHookNode*>(node))
        return hook->path();
    return {};
}

GpuAudioProvider realtime_gpu_provider(GpuAudioNode*) noexcept {
    // The test hook deliberately has no authenticated provider identity.
    return GpuAudioProvider::Unknown;
}

TEST_CASE("GpuAudioTransport capability report is an honest staged snapshot",
          "[gpu_audio][transport][capability]") {
    constexpr uint32_t BS = 32;
    GpuAudioTransport transport;

    const auto inactive = transport.capability_report();
    CHECK(inactive.path == GpuAudioExecutionPath::Unavailable);
    CHECK(inactive.provider == GpuAudioProvider::Unknown);
    CHECK(inactive.eligibility == GpuAudioEligibility::Unavailable);
    CHECK_FALSE(inactive.prepared);
    CHECK(inactive.prepared_lead_blocks == 0);
    CHECK_FALSE(inactive.fallback_available);
    CHECK_FALSE(inactive.diagnostics_available);

    test_detail::RealtimeHookNode node(1, BS, MissPolicy::CpuFallback, 3);
    REQUIRE(node.prepare());
    REQUIRE(transport.prepare(&node, {.ring_blocks = 8}));

    const auto report = transport.capability_report();
    // The test hook exercises the private seam but has no authenticated
    // provider identity, so the report must classify it as staged.
    CHECK(report.path == GpuAudioExecutionPath::Staged);
    CHECK(report.provider == GpuAudioProvider::Unknown);
    CHECK(report.eligibility == GpuAudioEligibility::Eligible);
    CHECK(report.prepared);
    CHECK(report.prepared_lead_blocks == 3);
    CHECK(report.fallback_policy == MissPolicy::CpuFallback);
    CHECK(report.fallback_available);
    CHECK(report.diagnostics_available);

    transport.release();
    const auto released = transport.capability_report();
    CHECK(released.path == GpuAudioExecutionPath::Unavailable);
    CHECK_FALSE(released.prepared);
}

} // namespace pulp::gpu_audio::detail

namespace {

using pulp::gpu_audio::test_detail::RealtimeHookNode;

// Test-only node: out = gain * in. Deterministic, no GPU — exercises the
// transport's scheduling, latency, and miss handling.
class GainNode : public GpuAudioNode {
  public:
    GainNode(uint32_t channels, uint32_t block, float gain, MissPolicy mp, uint32_t latency = 2,
             bool supports_fallback = true)
        : channels_(channels), block_(block), gain_(gain), mp_(mp), latency_(latency),
          supports_fallback_(supports_fallback) {}

    GpuAudioNodeDescriptor descriptor() const override {
        GpuAudioNodeDescriptor d;
        d.name = "gain";
        d.input_channels = channels_;
        d.output_channels = channels_;
        d.block_size = block_;
        d.sample_rate = 48000;
        d.latency_blocks = latency_;
        d.miss_policy = mp_;
        d.supports_cpu_fallback = supports_fallback_;
        return d;
    }
    bool prepare() override {
        return true;
    }
    void process_block(const BufferView<const float>& in, BufferView<float>& out,
                       uint32_t n) override {
        for (uint32_t c = 0; c < channels_; ++c) {
            const float* s = in.channel_ptr(c);
            float* d = out.channel_ptr(c);
            for (uint32_t i = 0; i < n; ++i)
                d[i] = s[i] * gain_;
        }
    }

  private:
    uint32_t channels_, block_;
    float gain_;
    MissPolicy mp_;
    uint32_t latency_;
    bool supports_fallback_;
};

// Per-channel storage with stable float / const-float pointer arrays.
struct Block {
    Block(uint32_t ch, uint32_t n)
        : storage(ch, std::vector<float>(n, 0.0f)), ptrs(ch), cptrs(ch), n(n) {
        for (uint32_t c = 0; c < ch; ++c) {
            ptrs[c] = storage[c].data();
            cptrs[c] = storage[c].data();
        }
    }
    void fill(float v) {
        for (auto& ch : storage)
            std::fill(ch.begin(), ch.end(), v);
    }
    BufferView<float> view() {
        return BufferView<float>(ptrs.data(), ptrs.size(), n);
    }
    BufferView<const float> cview() {
        return BufferView<const float>(cptrs.data(), cptrs.size(), n);
    }
    std::vector<std::vector<float>> storage;
    std::vector<float*> ptrs;
    std::vector<const float*> cptrs;
    uint32_t n;
};

struct TrialDeliveryCapture {
    struct Entry {
        std::uint64_t sequence = 0;
        std::uint8_t disposition = 0;
        std::uint64_t callback_start_ns = 0;
        std::uint64_t callback_end_ns = 0;
        std::uint64_t result_visible_ns = 0;
    };
    std::array<Entry, 16> entries{};
    std::uint32_t count = 0;

    static void observe(void* context, std::uint64_t sequence, std::uint8_t disposition,
                        std::uint64_t callback_start_ns, std::uint64_t callback_end_ns,
                        std::uint64_t result_visible_ns) noexcept {
        auto& capture = *static_cast<TrialDeliveryCapture*>(context);
        if (capture.count >= capture.entries.size())
            return;
        capture.entries[capture.count++] =
            Entry{sequence, disposition, callback_start_ns, callback_end_ns, result_visible_ns};
    }
};

} // namespace

TEST_CASE("GpuAudioTransport trial observer records staged callback delivery",
          "[gpu_audio][transport][trace]") {
    constexpr uint32_t CH = 1, BS = 32, L = 2, RING = 8;
    GainNode node(CH, BS, 2.0f, MissPolicy::PassthroughDry, L);
    REQUIRE(node.prepare());
    TrialDeliveryCapture capture;
    GpuAudioTransport t;
    REQUIRE(t.prepare(&node, {.ring_blocks = RING}));
    REQUIRE(
        configure_gpu_audio_transport_trial_observer(t, &capture, &TrialDeliveryCapture::observe));

    Block in(CH, BS), out(CH, BS);
    for (int k = 0; k < 4; ++k) {
        in.fill(static_cast<float>(k + 1));
        auto output = out.view();
        t.process(in.cview(), output, BS);
        t.pump();
    }

    REQUIRE(capture.count == 4);
    for (std::uint32_t i = 0; i < capture.count; ++i) {
        CHECK(capture.entries[i].sequence == i);
        const auto expected = i < L ? detail::SharedIoDeliveryDisposition::Priming
                                    : detail::SharedIoDeliveryDisposition::GpuDelivered;
        CHECK(capture.entries[i].disposition == static_cast<std::uint8_t>(expected));
        CHECK(capture.entries[i].callback_start_ns != 0);
        CHECK(capture.entries[i].callback_end_ns >= capture.entries[i].callback_start_ns);
        CHECK(capture.entries[i].callback_end_ns != 0);
        CHECK(capture.entries[i].result_visible_ns >= capture.entries[i].callback_end_ns);
    }
}

TEST_CASE("GpuAudioTransport trial observer records fallback dispositions",
          "[gpu_audio][transport][trace]") {
    constexpr uint32_t CH = 1, BS = 32, L = 2, RING = 8;
    for (const auto policy : {MissPolicy::CpuFallback, MissPolicy::Silence}) {
        GainNode node(CH, BS, 2.0f, policy, L);
        REQUIRE(node.prepare());
        TrialDeliveryCapture capture;
        GpuAudioTransport t;
        REQUIRE(t.prepare(&node, {.ring_blocks = RING}));
        REQUIRE(configure_gpu_audio_transport_trial_observer(t, &capture,
                                                             &TrialDeliveryCapture::observe));

        Block in(CH, BS), out(CH, BS);
        for (int k = 0; k < 3; ++k) {
            in.fill(static_cast<float>(k + 1));
            auto output = out.view();
            t.process(in.cview(), output, BS);
        }

        REQUIRE(capture.count == 3);
        const auto fallback = policy == MissPolicy::CpuFallback
                                  ? detail::SharedIoDeliveryDisposition::CpuFallbackDelivered
                                  : detail::SharedIoDeliveryDisposition::SilenceDelivered;
        for (std::uint32_t i = 0; i < capture.count; ++i) {
            const auto expected = i < L ? detail::SharedIoDeliveryDisposition::Priming : fallback;
            CHECK(capture.entries[i].disposition == static_cast<std::uint8_t>(expected));
        }
    }
}

TEST_CASE("GpuAudioTransport applies fixed latency + node processing", "[gpu_audio][transport]") {
    constexpr uint32_t CH = 2, BS = 64, L = 2, RING = 8;
    GainNode node(CH, BS, 2.0f, MissPolicy::PassthroughDry, L);
    REQUIRE(node.prepare());

    GpuAudioTransport t;
    REQUIRE(t.prepare(&node, {RING}));
    REQUIRE(t.latency_samples() == L * BS);

    Block in(CH, BS), out(CH, BS);
    std::vector<float> first_sample;
    constexpr int NBLK = 10;
    for (int k = 0; k < NBLK; ++k) {
        in.fill(static_cast<float>(k + 1));
        auto iv = in.cview();
        auto ov = out.view();
        t.process(iv, ov, BS); // RT: write input, read delayed output
        t.pump();              // worker keeps pace (one block per call)
        first_sample.push_back(out.storage[0][0]);
    }

    for (int k = 0; k < NBLK; ++k) {
        const float expected =
            (k >= static_cast<int>(L)) ? 2.0f * static_cast<float>(k - L + 1) : 0.0f;
        REQUIRE(first_sample[k] == expected);
    }
    REQUIRE(t.stats().miss_blocks == 0);
    REQUIRE(t.stats().produced_blocks == NBLK);
}

TEST_CASE("GpuAudioTransport miss policy fills dry on starvation", "[gpu_audio][transport]") {
    constexpr uint32_t CH = 1, BS = 32, L = 2, RING = 8;
    GainNode node(CH, BS, 2.0f, MissPolicy::PassthroughDry, L);
    REQUIRE(node.prepare());
    GpuAudioTransport t;
    REQUIRE(t.prepare(&node, {RING}));

    Block in(CH, BS), out(CH, BS);
    std::vector<float> first_sample;
    for (int k = 0; k < 4; ++k) {
        in.fill(static_cast<float>(k + 1));
        auto iv = in.cview();
        auto ov = out.view();
        t.process(iv, ov, BS); // NEVER pump → worker starved
        first_sample.push_back(out.storage[0][0]);
    }

    REQUIRE(first_sample[0] == 0.0f);
    REQUIRE(first_sample[1] == 0.0f);
    REQUIRE(first_sample[2] == 3.0f); // dry input value k+1 = 3
    REQUIRE(first_sample[3] == 4.0f);
    REQUIRE(t.stats().miss_blocks == 2);
}

TEST_CASE("GpuAudioTransport process_offline drives the node synchronously",
          "[gpu_audio][transport]") {
    // Offline render (e.g. a faster-than-real-time bounce): NO worker thread and
    // we NEVER call pump() manually — process_offline() must pump the node inline
    // so every block is captured. This is exactly the case where the realtime
    // process() path starves and misses (see the miss-policy test above); here the
    // miss policy is Silence so any drop would show up as a zero, yet none occur.
    // Latency stays identical to process() (the primed output ring).
    constexpr uint32_t CH = 2, BS = 64, L = 2, RING = 8;
    GainNode node(CH, BS, 2.0f, MissPolicy::Silence, L);
    REQUIRE(node.prepare());
    GpuAudioTransport t;
    REQUIRE(t.prepare(&node, {RING})); // run_worker_thread defaults to false
    REQUIRE(t.latency_samples() == L * BS);

    Block in(CH, BS), out(CH, BS);
    std::vector<float> first_sample;
    constexpr int NBLK = 10;
    for (int k = 0; k < NBLK; ++k) {
        in.fill(static_cast<float>(k + 1));
        auto iv = in.cview();
        auto ov = out.view();
        t.process_offline(iv, ov, BS); // synchronous: no worker, no manual pump
        first_sample.push_back(out.storage[0][0]);
    }

    // Same latency-delayed gain output as the worker-paced realtime case, but with
    // ZERO misses — proving the node ran for every block under offline drive.
    for (int k = 0; k < NBLK; ++k) {
        const float expected =
            (k >= static_cast<int>(L)) ? 2.0f * static_cast<float>(k - L + 1) : 0.0f;
        REQUIRE(first_sample[k] == expected);
    }
    REQUIRE(t.stats().miss_blocks == 0);
    REQUIRE(t.stats().produced_blocks == NBLK);
}

TEST_CASE("GpuAudioTransport drops input on a full ring (no block)", "[gpu_audio][transport]") {
    constexpr uint32_t CH = 1, BS = 32, RING = 4;
    GainNode node(CH, BS, 1.0f, MissPolicy::Silence, 2);
    REQUIRE(node.prepare());
    GpuAudioTransport t;
    REQUIRE(t.prepare(&node, {RING}));

    Block in(CH, BS), out(CH, BS);
    in.fill(1.0f);
    for (int k = 0; k < 20; ++k) {
        auto iv = in.cview();
        auto ov = out.view();
        t.process(iv, ov, BS);
    }
    REQUIRE(t.stats().input_dropped_frames > 0);
}

TEST_CASE("GpuAudioTransport rejects invalid descriptor / config", "[gpu_audio][transport]") {
    // Zero channels.
    {
        GainNode bad(0, 64, 1.0f, MissPolicy::Silence, 2);
        GpuAudioTransport t;
        REQUIRE_FALSE(t.prepare(&bad, {8}));
        REQUIRE_FALSE(t.is_prepared());
    }
    // ring_blocks too small for latency (needs latency + 2).
    {
        GainNode node(1, 64, 1.0f, MissPolicy::Silence, 4);
        GpuAudioTransport t;
        REQUIRE_FALSE(t.prepare(&node, {5})); // 5 < 4 + 2
        REQUIRE(t.prepare(&node, {6}));       // ok
    }
    // CpuFallback miss policy without a real fallback.
    {
        GainNode node(1, 64, 1.0f, MissPolicy::CpuFallback, 2, /*supports_fallback=*/false);
        GpuAudioTransport t;
        REQUIRE_FALSE(t.prepare(&node, {8}));
    }
    // null node.
    {
        GpuAudioTransport t;
        REQUIRE_FALSE(t.prepare(nullptr, {8}));
    }
}

TEST_CASE("GpuAudioTransport background worker drains the pipeline", "[gpu_audio][transport]") {
    constexpr uint32_t CH = 2, BS = 64, L = 2, RING = 32;
    GainNode node(CH, BS, 2.0f, MissPolicy::PassthroughDry, L);
    REQUIRE(node.prepare());

    GpuAudioTransport t;
    REQUIRE(t.prepare(&node, {RING, /*run_worker_thread=*/true}));

    Block in(CH, BS), out(CH, BS);
    constexpr int N = 200;
    for (int k = 0; k < N; ++k) {
        in.fill(static_cast<float>(k + 1));
        auto iv = in.cview();
        auto ov = out.view();
        t.process(iv, ov, BS);
        std::this_thread::sleep_for(std::chrono::microseconds(300)); // ~ worker poll pace
    }

    // Liveness: give the worker a bounded grace period to drain the backlog.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto s = t.stats();
        if (s.produced_blocks + s.input_dropped_frames / BS >= static_cast<std::uint64_t>(N))
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    const auto s = t.stats();
    REQUIRE(s.produced_blocks > 0); // the worker actually ran
    // Exact, timing-independent invariant: every fed block was produced or
    // dropped whole (no partial/misaligned blocks).
    REQUIRE(s.produced_blocks + s.input_dropped_frames / BS == static_cast<std::uint64_t>(N));
    // A resync can only ever cancel a prior miss's late wet block, so the count
    // of resynced blocks can never exceed the misses. A depth-based resync would
    // violate this under worker timing (it drains a block the worker raced ahead
    // to produce even with zero misses); the miss-counting resync cannot.
    REQUIRE(s.resynced_blocks <= s.miss_blocks);

    // release() must cleanly stop + join the worker (no hang / no crash).
    t.release();
    REQUIRE_FALSE(t.is_prepared());
}

TEST_CASE("GpuAudioTransport resyncs the wet timeline after a miss", "[gpu_audio][transport]") {
    // A miss emits a substitute (dry) block for its timeline slot; when the worker
    // later catches up it back-fills that slot's wet block, pushing the output ring
    // above the primed steady-state depth. Without a resync the RT read would pick
    // up that stale wet block, permanently delaying the wet stream one block per
    // miss (a comb filter of dry against a one-block-late wet). This drives a
    // deterministic miss-then-catch-up sequence with manual pump control and proves
    // the resync drops the redundant block so effective latency stays pinned.
    constexpr uint32_t CH = 1, BS = 32, L = 2, RING = 8;
    GainNode node(CH, BS, 2.0f, MissPolicy::PassthroughDry, L);
    REQUIRE(node.prepare());
    GpuAudioTransport t;
    REQUIRE(t.prepare(&node, {RING})); // no worker thread — we drive pump()

    Block in(CH, BS), out(CH, BS);
    std::vector<float> got;
    constexpr int NBLK = 8;
    for (int k = 0; k < NBLK; ++k) {
        // Worker catches up BEFORE the callback from k>=6 on: drain the backlog
        // (including the late wet block for the missed slot) so the RT read sees
        // the over-filled ring the resync must correct.
        if (k >= 6)
            t.pump();
        in.fill(static_cast<float>(k + 1));
        auto iv = in.cview();
        auto ov = out.view();
        t.process(iv, ov, BS);
        // Keep pace through call 2; starve calls 3-5 (drains the 2-block cushion,
        // forcing exactly one miss at call 5); catch-up handled above from k>=6.
        if (k <= 2)
            t.pump();
        got.push_back(out.storage[0][0]);
    }

    // out[5] is the dry substitute (input 6) — the one block the miss cost us.
    // out[6] is realigned to the no-miss value (10), NOT the stale late wet (8).
    const std::vector<float> expected = {0, 0, 2, 4, 6, /*miss→dry*/ 6, /*resynced*/ 10, 12};
    REQUIRE(got == expected);
    REQUIRE(t.stats().miss_blocks == 1);
    REQUIRE(t.stats().resynced_blocks == 1); // exactly the one late wet block dropped
}

TEST_CASE("GpuAudioTransport wake-on-write worker drains the pipeline", "[gpu_audio][transport]") {
    // Same whole-block conservation invariant as the polling worker, but with the
    // opt-in wake-on-write path: process() posts a semaphore the worker waits on.
    // Proves the semaphore-driven worker still drains every fed block and that
    // release() cleanly joins it.
    constexpr uint32_t CH = 2, BS = 64, L = 2, RING = 32;
    GainNode node(CH, BS, 2.0f, MissPolicy::PassthroughDry, L);
    REQUIRE(node.prepare());

    GpuAudioTransport t;
    REQUIRE(t.prepare(&node, {RING, /*run_worker_thread=*/true, /*wake_on_write=*/true}));

    Block in(CH, BS), out(CH, BS);
    constexpr int N = 200;
    for (int k = 0; k < N; ++k) {
        in.fill(static_cast<float>(k + 1));
        auto iv = in.cview();
        auto ov = out.view();
        t.process(iv, ov, BS);
        std::this_thread::sleep_for(std::chrono::microseconds(300));
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto s = t.stats();
        if (s.produced_blocks + s.input_dropped_frames / BS >= static_cast<std::uint64_t>(N))
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    const auto s = t.stats();
    REQUIRE(s.produced_blocks > 0);
    REQUIRE(s.produced_blocks + s.input_dropped_frames / BS == static_cast<std::uint64_t>(N));

    t.release();
    REQUIRE_FALSE(t.is_prepared());
}

TEST_CASE("GpuAudioTransport silences mismatched / too-small views", "[gpu_audio][transport]") {
    constexpr uint32_t CH = 2, BS = 64, RING = 8;
    GainNode node(CH, BS, 2.0f, MissPolicy::PassthroughDry, 2);
    REQUIRE(node.prepare());
    GpuAudioTransport t;
    REQUIRE(t.prepare(&node, {RING}));

    Block in(CH, BS), out(CH, BS);
    in.fill(1.0f);
    out.fill(7.0f); // sentinel — must be overwritten with silence

    // Wrong block size.
    auto iv = in.cview();
    auto ov = out.view();
    t.process(iv, ov, BS / 2);
    REQUIRE(out.storage[0][0] == 0.0f);

    // Too few output channels.
    Block mono_out(1, BS);
    mono_out.fill(7.0f);
    auto mov = mono_out.view();
    t.process(iv, mov, BS);
    REQUIRE(mono_out.storage[0][0] == 0.0f);
}

// A node that declares nothing about its miss behavior must not pass audio
// through on a miss. Passing through is the WORST default here: the transport's
// output is delayed by latency_blocks, so a dry sample for the CURRENT time jumps
// the stream forward by the whole latency for one block and back again -- a
// timeline break rather than an honest dropout. And a node that never thought
// about misses is exactly the node whose CPU fallback is not correct.
TEST_CASE("GpuAudioNodeDescriptor defaults fail closed", "[gpu_audio][transport]") {
    GpuAudioNodeDescriptor d;
    REQUIRE(d.miss_policy == MissPolicy::Silence);
    REQUIRE_FALSE(d.supports_cpu_fallback);
}

TEST_CASE("GpuAudioTransport starves to silence when the node declares no policy",
          "[gpu_audio][transport]") {
    constexpr uint32_t CH = 1, BS = 32, L = 2, RING = 8;

    // A node whose descriptor leaves miss_policy and supports_cpu_fallback at
    // their defaults -- the shape a first-time GPU node author writes.
    struct DefaultPolicyNode : GpuAudioNode {
        GpuAudioNodeDescriptor descriptor() const override {
            GpuAudioNodeDescriptor d; // defaults, deliberately untouched
            d.name = "defaults";
            d.input_channels = CH;
            d.output_channels = CH;
            d.block_size = BS;
            d.sample_rate = 48000;
            d.latency_blocks = L;
            return d;
        }
        bool prepare() override {
            return true;
        }
        void process_block(const BufferView<const float>&, BufferView<float>& out,
                           uint32_t n) override {
            for (uint32_t i = 0; i < n; ++i)
                out.channel_ptr(0)[i] = 1.0f;
        }
    };

    DefaultPolicyNode node;
    REQUIRE(node.prepare());
    GpuAudioTransport t;
    REQUIRE(t.prepare(&node, {RING}));

    // Never pump the worker, so every read is a miss. The dry input is 0.5 --
    // any nonzero output here is the dry signal leaking through on a miss.
    Block in(CH, BS), out(CH, BS);
    for (int k = 0; k < 4; ++k) {
        in.fill(0.5f);
        auto iv = in.cview();
        auto ov = out.view();
        t.process(iv, ov, BS);
        for (uint32_t i = 0; i < BS; ++i) {
            INFO("block " << k << " sample " << i);
            REQUIRE(out.storage[0][i] == 0.0f);
        }
    }
    // Misses were recorded (the exact count depends on the transport's priming
    // and resync bookkeeping; what this test pins is that they came out SILENT).
    REQUIRE(t.stats().miss_blocks >= 1);
}

TEST_CASE("GpuAudioTransport rejected callbacks preserve the fallback impulse position",
          "[gpu_audio][transport][realtime-path][callback-timeline]") {
    constexpr uint32_t BS = 32;
    RealtimeHookNode node(1, BS, MissPolicy::CpuFallback);
    node.stateful_fallback = true;
    node.enforce_sequence = true;
    node.realtime_status = detail::kRealtimeGpuMissed;
    REQUIRE(node.prepare());
    GpuAudioTransport transport;
    REQUIRE(transport.prepare(&node, {8}));
    Block input(1, BS), output(1, BS);
    auto in = input.cview();
    auto out = output.view();

    bool invalid = false;
    bool offline = false;
    SECTION("valid silent callback") {}
    SECTION("invalid realtime callback") {
        invalid = true;
    }
    SECTION("invalid offline callback") {
        invalid = true;
        offline = true;
    }

    input.storage[0][0] = 1.0f;
    transport.process(in, out, BS);
    for (float sample : output.storage[0])
        CHECK(sample == 0.0f);
    input.fill(invalid ? 9.0f : 0.0f);
    output.fill(7.0f);
    node.external_output_sample = output.storage[0].data();
    if (offline)
        transport.process_offline(in, out, invalid ? BS / 2 : BS);
    else
        transport.process(in, out, invalid ? BS / 2 : BS);
    for (float sample : output.storage[0])
        CHECK(sample == 0.0f);
    CHECK(node.prime_fallback_calls == 2);
    CHECK(node.next_sequence == 2);
    CHECK(node.invalid_callback_calls == (invalid ? 1u : 0u));
    CHECK(node.invalid_input_zero_filled);
    CHECK(node.callback_shape_valid);
    CHECK(node.external_output_silent_on_rejection);
    if (invalid)
        CHECK(node.delivery == detail::SharedIoDeliveryDisposition::InvalidRejected);

    input.fill(0.0f);
    transport.process(in, out, BS);
    for (uint32_t sample = 0; sample < BS; ++sample)
        CHECK(output.storage[0][sample] == (sample == 0 ? 1.0f : 0.0f));
    transport.process(in, out, BS);
    for (float sample : output.storage[0])
        CHECK(sample == 0.0f);
    CHECK(node.prime_fallback_calls == 4);
    CHECK(node.next_sequence == 4);
    CHECK(node.stale_callback_calls == 0);
    CHECK(node.sequence_gap_calls == 0);
}

TEST_CASE("GpuAudioTransport rejected realtime views use prepared allocation-free storage",
          "[gpu_audio][transport][realtime-path][rt-safety]") {
    constexpr uint32_t BS = 32;
    RealtimeHookNode node(1, BS, MissPolicy::CpuFallback);
    node.stateful_fallback = true;
    node.enforce_sequence = true;
    REQUIRE(node.prepare());
    GpuAudioTransport transport;
    REQUIRE(transport.prepare(&node, {8}));
    Block input(1, BS), output(1, BS), short_input(1, BS / 2), short_output(1, BS / 2);
    input.fill(9.0f);
    short_input.fill(9.0f);
    output.fill(9.0f);
    short_output.fill(9.0f);
    auto in = input.cview();
    auto out = output.view();
    auto short_in = short_input.cview();
    auto short_out = short_output.view();
    BufferView<const float> no_input_channels(input.cptrs.data(), 0, BS);
    BufferView<float> no_output_channels(output.ptrs.data(), 0, BS);
    std::size_t allocations = 1;
    {
        pulp::test::RtAllocationProbe probe;
        transport.process(in, out, BS / 2);
        transport.process(short_in, out, BS);
        transport.process(in, short_out, BS);
        transport.process(no_input_channels, out, BS);
        transport.process(in, no_output_channels, BS);
        allocations = probe.allocation_count();
    }
    CHECK(allocations == 0);
    CHECK(node.invalid_callback_calls == 5);
    CHECK(node.prime_fallback_calls == 5);
    CHECK(node.next_sequence == 5);
    CHECK(node.invalid_input_zero_filled);
    CHECK(node.callback_shape_valid);
    CHECK(node.delivery == detail::SharedIoDeliveryDisposition::InvalidRejected);
    CHECK(node.stale_callback_calls == 0);
    CHECK(node.sequence_gap_calls == 0);
    for (float sample : output.storage[0])
        CHECK(sample == 0.0f);
    for (float sample : short_output.storage[0])
        CHECK(sample == 0.0f);
}

TEST_CASE("GpuAudioTransport retains the prepared node timeline across transport lifetimes",
          "[gpu_audio][transport][realtime-path][callback-timeline]") {
    constexpr uint32_t BS = 32;
    RealtimeHookNode node(1, BS, MissPolicy::CpuFallback);
    node.enforce_sequence = true;
    REQUIRE(node.prepare());
    GpuAudioTransport transport;
    REQUIRE(transport.prepare(&node, {8}));
    Block input(1, BS), output(1, BS);
    auto in = input.cview();
    auto out = output.view();
    transport.process(in, out, BS);
    transport.process(in, out, BS);
    REQUIRE(node.next_sequence == 2);

    SECTION("reprepare the same transport") {
        REQUIRE(transport.prepare(&node, {8}));
        transport.process(in, out, BS);
    }
    SECTION("attach a replacement transport after release") {
        transport.release();
        GpuAudioTransport replacement;
        REQUIRE(replacement.prepare(&node, {8}));
        replacement.process(in, out, BS);
    }

    CHECK(node.last_sequence == 2);
    CHECK(node.next_sequence == 3);
    CHECK(node.next_sequence_queries == 2);
    CHECK(node.delivered_sequence == 2);
    CHECK(node.stale_callback_calls == 0);
    CHECK(node.sequence_gap_calls == 0);
    CHECK_FALSE(node.sequence_fenced);
    CHECK(node.delivery == detail::SharedIoDeliveryDisposition::GpuDelivered);
    CHECK(output.storage[0][0] == 1000.0f);
}

TEST_CASE("GpuAudioTransport offline fence retains fallback and the monotonic timeline",
          "[gpu_audio][transport][realtime-path]") {
    constexpr uint32_t CH = 1, BS = 32;
    RealtimeHookNode node(CH, BS, MissPolicy::CpuFallback);
    REQUIRE(node.prepare());
    GpuAudioTransport transport;
    REQUIRE(transport.prepare(&node, {8}));
    Block input(CH, BS), output(CH, BS);
    input.fill(5.0f);
    auto in = input.cview();
    auto out = output.view();
    transport.process(in, out, BS);
    CHECK(node.last_sequence == 0);
    CHECK(output.storage[0][0] == 1005.0f);
    CHECK(node.delivery == detail::SharedIoDeliveryDisposition::GpuDelivered);
    transport.process(in, out, BS / 2); // invalid calls still consume absolute sequence
    CHECK(node.realtime_calls == 2);
    CHECK(output.storage[0][0] == 0.0f);
    CHECK(node.delivery == detail::SharedIoDeliveryDisposition::InvalidRejected);
    transport.process_offline(in, out, BS);
    CHECK(node.last_sequence == 2);
    CHECK(node.delivered_sequence == 2);
    CHECK(node.fence_calls == 1);
    CHECK(node.delivery == detail::SharedIoDeliveryDisposition::CpuFallbackDelivered);
    CHECK(output.storage[0][0] == -5.0f);
    transport.process(in, out, BS);
    CHECK(node.last_sequence == 3);
    CHECK(output.storage[0][0] == -5.0f);
    transport.process_offline(in, out, BS);
    CHECK(node.last_sequence == 4);
    CHECK(node.fence_calls == 1);
    CHECK(node.prime_fallback_calls == 5);
    CHECK(node.delivery_calls == 5);
    CHECK(node.process_block_calls == 0);
}

TEST_CASE("GpuAudioTransport failed offline drain retains hooks and never starts staged GPU work",
          "[gpu_audio][transport][realtime-path]") {
    constexpr uint32_t BS = 32;
    RealtimeHookNode node(1, BS, MissPolicy::CpuFallback);
    node.fence_result = false;
    REQUIRE(node.prepare());
    GpuAudioTransport transport;
    REQUIRE(transport.prepare(&node, {8}));
    Block input(1, BS), output(1, BS);
    input.fill(7.0f);
    auto in = input.cview();
    auto out = output.view();
    transport.process_offline(in, out, BS);
    CHECK(node.fence_calls == 1);
    CHECK(output.storage[0][0] == -7.0f);
    transport.process(in, out, BS);
    transport.pump();
    CHECK(node.service_calls == 1);
    CHECK(node.process_block_calls == 0);
    CHECK(output.storage[0][0] == -7.0f);
    node.fence_result = true;
    transport.process_offline(in, out, BS);
    CHECK(node.fence_calls == 2);
    CHECK(node.last_sequence == 2);
    CHECK(node.prime_fallback_calls == 3);
    CHECK(node.cpu_fallback_calls == 3);
    CHECK(node.delivery_calls == 3);
    CHECK(node.process_block_calls == 0);
}

TEST_CASE("GpuAudioTransport inactive private provider retains CPU-only fallback",
          "[gpu_audio][transport][realtime-path]") {
    constexpr uint32_t BS = 32;
    RealtimeHookNode node(1, BS, MissPolicy::CpuFallback);
    node.realtime_status = detail::kRealtimeGpuInactive;
    node.service_return_blocks = detail::kRealtimeGpuServiceInactive;
    REQUIRE(node.prepare());
    GpuAudioTransport transport;
    REQUIRE(transport.prepare(&node, {8}));
    Block input(1, BS), output(1, BS);
    input.fill(2.0f);
    auto in = input.cview();
    auto out = output.view();
    transport.process(in, out, BS);
    CHECK(output.storage[0][0] == -2.0f);
    transport.pump();
    CHECK(node.service_calls == 1);
    CHECK(node.process_block_calls == 0);
    CHECK(node.prime_fallback_calls == 1);
    CHECK(node.cpu_fallback_calls == 1);
    CHECK(node.delivery == detail::SharedIoDeliveryDisposition::CpuFallbackDelivered);
}

TEST_CASE("GpuAudioTransport private realtime miss uses the declared miss policy",
          "[gpu_audio][transport][realtime-path]") {
    constexpr uint32_t CH = 1, BS = 32, L = 2, RING = 8;
    RealtimeHookNode node(CH, BS, MissPolicy::PassthroughDry, L);
    node.realtime_status = detail::kRealtimeGpuMissed;
    REQUIRE(node.prepare());

    GpuAudioTransport t;
    REQUIRE(t.prepare(&node, {RING}));

    Block in(CH, BS), out(CH, BS);
    in.fill(42.0f);
    auto iv = in.cview();
    auto ov = out.view();
    t.process(iv, ov, BS);

    REQUIRE(out.storage[0][0] == 42.0f);
    REQUIRE(node.realtime_calls == 1);
    REQUIRE(node.process_block_calls == 0);
    REQUIRE(t.stats().miss_blocks == 1);
}
