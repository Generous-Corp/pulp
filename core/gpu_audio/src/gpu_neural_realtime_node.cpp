#include "detail/gpu_neural_model.hpp"
#include "detail/gpu_neural_realtime_test_access.hpp"
#include "detail/neural_processor.hpp"
#include "detail/realtime_gpu_audio_path.hpp"
#include "detail/shared_io_execution_contract.hpp"
#include "detail/shared_io_stamped_bridge.hpp"

#include <pulp/gpu_audio/gpu_neural_realtime_node.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <thread>
#include <vector>

namespace pulp::gpu_audio {
namespace {
using Bridge = detail::SharedIoStampedBridge;
using Terminal = detail::StreamingBackendTerminalDisposition;

std::uint64_t now_ns() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

GpuAudioRecoveryReason public_reason(detail::SharedIoRecoveryReason reason) noexcept {
    switch (reason) {
    case detail::SharedIoRecoveryReason::None:
        return GpuAudioRecoveryReason::None;
    case detail::SharedIoRecoveryReason::SequenceGap:
        return GpuAudioRecoveryReason::SequenceGap;
    case detail::SharedIoRecoveryReason::InputSaturated:
        return GpuAudioRecoveryReason::InputSaturated;
    case detail::SharedIoRecoveryReason::ProviderFailure:
        return GpuAudioRecoveryReason::ProviderFailure;
    case detail::SharedIoRecoveryReason::ProviderLost:
        return GpuAudioRecoveryReason::ProviderLost;
    case detail::SharedIoRecoveryReason::OfflineFence:
        return GpuAudioRecoveryReason::OfflineFence;
    case detail::SharedIoRecoveryReason::InvalidCallback:
        return GpuAudioRecoveryReason::InvalidCallback;
    }
    return GpuAudioRecoveryReason::None;
}
} // namespace

struct GpuNeuralRealtimeNode::Impl {
    struct Pending {
        detail::StreamingBlockStamp stamp{};
        std::vector<float> input, output;
        Terminal disposition = Terminal::Cancelled;
        bool occupied = false;
        bool done = false;

        void reset() noexcept {
            stamp = {};
            disposition = Terminal::Cancelled;
            occupied = false;
            done = false;
        }
    };

    Config config;
    GpuNeuralModelHandle model;
    bool model_owner = false;

    // Built-in CPU shadow: one published processor per channel, a ring of
    // `lead_blocks` staged results, and the block due this callback.
    std::vector<std::unique_ptr<detail::NeuralProcessor>> processors;
    std::vector<float> shadow_ring, shadow_due, shadow_input;
    std::uint32_t shadow_cursor = 0;
    std::uint64_t shadow_sequence = 0;
    bool shadow_published = false;
    bool test_skip_publish = false;

    // GPU delivery, present only while a backend is prepared.
    bool backend_active = false;
    std::unique_ptr<Bridge> bridge;
    detail::SharedIoTelemetry telemetry;
    std::vector<float> callback_input, callback_output;
    std::vector<Pending> pending_ring;
    Bridge::Callback callback;
    std::size_t pending_head = 0, pending_count = 0;
    std::uint64_t next_input = 0, sequence = 0, epoch = 0;

    // CPU-only delivery: no bridge and no epoch.
    std::uint64_t cpu_next_sequence = 0;
    std::atomic<std::uint64_t> cpu_blocks{0};
    std::atomic<detail::SharedIoRecoveryReason> cpu_recovery{detail::SharedIoRecoveryReason::None};
    std::atomic<std::uint64_t> cpu_recovery_sequence{std::numeric_limits<std::uint64_t>::max()};

    bool prepared = false;

    explicit Impl(Config source) : config(std::move(source)), model(config.model) {}

    std::uint32_t block_size() const noexcept {
        return model ? model->block_size : 0;
    }

    bool valid_config() const noexcept {
        const auto& c = config;
        return model != nullptr && c.family != NeuralFamily::Unknown && c.family == model->family &&
               c.channels > 0 && c.channels <= 64 && c.channels == model->channels() &&
               model->block_size > 0 && c.lead_blocks > 0 && c.capacity > c.lead_blocks &&
               c.max_inflight > 0 && c.max_inflight <= c.capacity - c.lead_blocks &&
               c.completion_service_wait_ns <= 1'000'000 &&
               c.miss_policy == MissPolicy::CpuFallback && epoch != UINT64_MAX &&
               sequence < Bridge::kSequenceLimit &&
               c.prewarm_blocks < Bridge::kSequenceLimit - sequence;
    }

    detail::StreamingPrepareContext context_for(const detail::StreamingModelSpec& spec,
                                                bool worker_backend) const noexcept {
        return {.spec = &spec,
                .artifact_id = model->artifact_id,
                .artifact_hash = model->artifact_sha256,
                .fallback = detail::StreamingFallbackStrategy::ContinuouslyPrimedCpuShadow,
                .max_frames = model->block_size,
                .lead_blocks = config.lead_blocks,
                .worker_backend_requested = worker_backend};
    }

    bool shadow_ready() const noexcept {
        if (!shadow_published || processors.size() != config.channels)
            return false;
        for (const auto& processor : processors)
            if (!processor->snapshot().prepared)
                return false;
        return true;
    }

    // RT-safe: the processors are published and their models allocation-free.
    void run_shadow(const float* const* inputs, std::uint32_t input_channels, float* const* outputs,
                    std::uint32_t n) noexcept {
        for (std::uint32_t ch = 0; ch < config.channels; ++ch) {
            const float* x = shadow_input.data();
            if (ch < input_channels) {
                const float* source = inputs[ch];
                bool finite = true;
                for (std::uint32_t i = 0; i < n && finite; ++i)
                    finite = std::isfinite(source[i]);
                if (finite) {
                    x = source;
                } else {
                    // One non-finite sample must not poison the causal state.
                    for (std::uint32_t i = 0; i < n; ++i)
                        shadow_input[i] = std::isfinite(source[i]) ? source[i] : 0.0f;
                }
            } else {
                std::fill_n(shadow_input.data(), n, 0.0f);
            }
            const float* in_ptr[1] = {x};
            float* out_ptr[1] = {outputs[ch]};
            audio::BufferView<const float> in(in_ptr, 1, n);
            audio::BufferView<float> out(out_ptr, 1, n);
            processors[ch]->process_cpu(in, out, n, {.epoch = epoch, .sequence = shadow_sequence});
        }
        ++shadow_sequence;
    }

    void fail(detail::SharedIoRecoveryReason reason,
              std::uint64_t at = std::numeric_limits<std::uint64_t>::max()) noexcept {
        if (bridge) {
            bridge->request_recovery(reason, at);
            return;
        }
        auto expected = detail::SharedIoRecoveryReason::None;
        if (cpu_recovery.compare_exchange_strong(expected, reason, std::memory_order_acq_rel))
            cpu_recovery_sequence.store(at, std::memory_order_release);
    }

    bool prewarm_backend(detail::StreamingBackend& backend) {
        const auto count = static_cast<std::size_t>(config.channels) * block_size();
        std::vector<float> zero(count, 0.0f), output(count, 0.0f);
        for (std::uint32_t block = 0; block < config.prewarm_blocks; ++block) {
            const detail::StreamingBlock request{
                .stamp = {.epoch = epoch, .sequence = sequence + block},
                .input = zero,
                .output = output,
                .input_channels = config.channels,
                .output_channels = config.channels,
                .frames = block_size(),
                .input_channel_stride = block_size(),
                .output_channel_stride = block_size()};
            if (backend.enqueue(request) != detail::StreamingAdmission::Accepted)
                return false;
            const auto deadline = now_ns() + 2'000'000'000ull;
            detail::StreamingTerminal terminal;
            bool received = false;
            while (now_ns() < deadline) {
                (void)backend.service_until(now_ns());
                if (backend.receive(terminal)) {
                    received = terminal.stamp == request.stamp &&
                               terminal.disposition == Terminal::Completed;
                    break;
                }
                std::this_thread::yield();
            }
            if (!received)
                return false;
        }
        return true;
    }

    bool initialize_bridge() {
        const auto& c = config;
        bridge = std::make_unique<Bridge>();
        if (!bridge->prepare({.capacity = c.capacity,
                              .channels = c.channels,
                              .block_size = block_size(),
                              .lead_blocks = c.lead_blocks},
                             epoch, sequence))
            return false;
        const auto count = bridge->samples_per_block();
        callback_input.assign(count, 0.0f);
        callback_output.assign(count, 0.0f);
        pending_ring.assign(c.max_inflight, {});
        for (auto& pending : pending_ring) {
            pending.input.assign(count, 0.0f);
            pending.output.assign(count, 0.0f);
        }
        pending_head = pending_count = 0;
        next_input = sequence;
        telemetry.reset();
        bridge->set_trace(nullptr, &telemetry);
        return true;
    }

    Pending* find_pending(detail::StreamingBlockStamp stamp) noexcept {
        for (std::size_t offset = 0; offset < pending_count; ++offset) {
            auto& candidate = pending_ring[(pending_head + offset) % pending_ring.size()];
            if (candidate.occupied && !candidate.done && candidate.stamp == stamp)
                return &candidate;
        }
        return nullptr;
    }

    std::uint32_t collect(std::uint64_t deadline) noexcept {
        auto& backend = *model->backend;
        (void)backend.service_until(deadline);
        detail::StreamingTerminal terminal;
        while (backend.receive(terminal)) {
            auto* pending = find_pending(terminal.stamp);
            if (pending == nullptr) {
                fail(detail::SharedIoRecoveryReason::ProviderFailure, terminal.stamp.sequence);
                continue;
            }
            pending->done = true;
            pending->disposition = terminal.disposition;
            if (terminal.disposition == Terminal::ProviderFailed)
                fail(detail::SharedIoRecoveryReason::ProviderFailure, terminal.stamp.sequence);
            else if (terminal.disposition == Terminal::DeviceLost)
                fail(detail::SharedIoRecoveryReason::ProviderLost, terminal.stamp.sequence);
        }
        std::uint32_t produced = 0;
        while (pending_count != 0) {
            auto& pending = pending_ring[pending_head];
            if (!pending.done)
                break;
            if (pending.disposition == Terminal::Completed &&
                bridge->delivery_epoch() == pending.stamp.epoch) {
                const auto publication = bridge->publish_output(
                    {.epoch = pending.stamp.epoch, .sequence = pending.stamp.sequence},
                    pending.output);
                if (publication == Bridge::Publication::Published)
                    ++produced;
                else
                    fail(detail::SharedIoRecoveryReason::InputSaturated, pending.stamp.sequence);
            }
            pending.reset();
            pending_head = (pending_head + 1) % pending_ring.size();
            --pending_count;
        }
        return produced;
    }
};

GpuNeuralRealtimeNode::GpuNeuralRealtimeNode(Config config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

GpuNeuralRealtimeNode::~GpuNeuralRealtimeNode() {
    (void)release();
    if (impl_->model_owner)
        impl_->model->bound.store(false, std::memory_order_release);
}

GpuAudioNodeDescriptor GpuNeuralRealtimeNode::descriptor() const {
    const auto& c = impl_->config;
    return {.name = "Neural realtime",
            .input_channels = c.channels,
            .output_channels = c.channels,
            .block_size = impl_->block_size(),
            .sample_rate = impl_->model ? impl_->model->sample_rate : 0,
            .latency_blocks = c.lead_blocks,
            .miss_policy = MissPolicy::CpuFallback,
            .supports_cpu_fallback = true};
}

bool GpuNeuralRealtimeNode::prepare() {
    if (!release())
        return false;
    auto& s = *impl_;
    if (!s.valid_config())
        return false;
    if (!s.model_owner) {
        bool expected = false;
        if (!s.model->bound.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
            return false;
        s.model_owner = true;
    }
    try {
        const auto n = s.block_size();
        const auto channels = s.config.channels;
        s.shadow_ring.assign(static_cast<std::size_t>(s.config.lead_blocks) * channels * n, 0.0f);
        s.shadow_due.assign(static_cast<std::size_t>(channels) * n, 0.0f);
        s.shadow_input.assign(n, 0.0f);
        s.shadow_cursor = 0;
        s.shadow_sequence = s.sequence;
        s.shadow_published = true;
        for (std::uint32_t ch = 0; ch < channels; ++ch) {
            auto& model = *s.model->channel_models[ch];
            auto processor = std::make_unique<detail::NeuralProcessor>(model);
            if (!processor->prepare(s.context_for(model.spec(), s.model->backend != nullptr),
                                    detail::NeuralProviderPreference::CpuOnly)) {
                (void)processor->release();
                (void)release();
                return false;
            }
            if (s.test_skip_publish || !processor->publish())
                s.shadow_published = false;
            s.processors.push_back(std::move(processor));
        }
        // The shadow is the miss policy, so a node without a published shadow
        // must not reach the transport.
        if (!cpu_fallback_prepared()) {
            (void)release();
            return false;
        }

        if (s.model->backend) {
            auto& backend = *s.model->backend;
            ++s.epoch;
            const auto context = s.context_for(s.model->backend_spec, true);
            if (backend.prepare(context) &&
                backend.begin_epoch(s.epoch, detail::StreamingResetReason::TransportRestart) &&
                s.prewarm_backend(backend)) {
                s.backend_active = true;
            } else if (!backend.release()) {
                (void)release();
                return false;
            }
        }

        // Silent prewarm runs through the shadow too, so its causal history
        // starts at the same stream position as the GPU history.
        if (s.config.prewarm_blocks != 0) {
            std::vector<float> zero(n, 0.0f), discard(n, 0.0f);
            std::vector<const float*> inputs(channels, zero.data());
            std::vector<float*> outputs(channels, discard.data());
            for (std::uint32_t block = 0; block < s.config.prewarm_blocks; ++block)
                s.run_shadow(inputs.data(), channels, outputs.data(), n);
        }
        s.sequence += s.config.prewarm_blocks;
        s.shadow_sequence = s.sequence;

        if (s.backend_active) {
            if (!s.initialize_bridge()) {
                (void)release();
                return false;
            }
        } else {
            s.cpu_next_sequence = s.sequence;
            s.cpu_blocks.store(0, std::memory_order_relaxed);
            s.cpu_recovery.store(detail::SharedIoRecoveryReason::None, std::memory_order_release);
            s.cpu_recovery_sequence.store(std::numeric_limits<std::uint64_t>::max(),
                                          std::memory_order_release);
        }
        s.prepared = true;
        return true;
    } catch (...) {
    }
    (void)release();
    return false;
}

bool GpuNeuralRealtimeNode::release() noexcept {
    auto& s = *impl_;
    if (s.prepared) {
        // Keep the absolute stream position across reprepare.
        s.sequence = s.bridge ? s.bridge->next_sequence() : s.cpu_next_sequence;
    }
    s.prepared = false;
    if (s.bridge)
        s.bridge->request_recovery(detail::SharedIoRecoveryReason::OfflineFence);
    if (s.backend_active) {
        if (!s.model->backend->release())
            return false;
        s.backend_active = false;
    }
    while (!s.processors.empty()) {
        if (!s.processors.back()->release())
            return false;
        s.processors.pop_back();
    }
    s.shadow_published = false;
    s.bridge.reset();
    s.pending_ring.clear();
    s.pending_head = s.pending_count = 0;
    return true;
}

bool GpuNeuralRealtimeNode::cpu_fallback_prepared() const noexcept {
    return impl_->shadow_ready();
}

void GpuNeuralRealtimeNode::prime_fallback(const audio::BufferView<const float>& input,
                                           std::uint32_t n) noexcept {
    auto& s = *impl_;
    if (!s.prepared || n != s.block_size())
        return;
    const auto channels = s.config.channels;
    float* slot_base =
        s.shadow_ring.data() + static_cast<std::size_t>(s.shadow_cursor) * channels * n;
    const float* inputs[64];
    float* outputs[64];
    const auto input_channels =
        std::min<std::uint32_t>(static_cast<std::uint32_t>(input.num_channels()), channels);
    for (std::uint32_t ch = 0; ch < channels; ++ch) {
        float* slot = slot_base + static_cast<std::size_t>(ch) * n;
        // The slot holds model(input(t - lead)): this callback's substitute.
        std::copy_n(slot, n, s.shadow_due.data() + static_cast<std::size_t>(ch) * n);
        inputs[ch] = ch < input_channels ? input.channel_ptr(ch) : nullptr;
        outputs[ch] = slot;
    }
    s.run_shadow(inputs, input_channels, outputs, n);
    s.shadow_cursor = (s.shadow_cursor + 1) % s.config.lead_blocks;
}

void GpuNeuralRealtimeNode::process_cpu_fallback(const audio::BufferView<const float>&,
                                                 audio::BufferView<float>& output,
                                                 std::uint32_t n) noexcept {
    auto& s = *impl_;
    for (std::size_t ch = 0; ch < output.num_channels(); ++ch)
        std::fill_n(output.channel_ptr(ch), std::min<std::size_t>(n, output.num_samples()), 0.0f);
    if (!s.prepared || n != s.block_size() || output.num_samples() < n)
        return;
    const auto channels = std::min<std::uint32_t>(
        s.config.channels, static_cast<std::uint32_t>(output.num_channels()));
    for (std::uint32_t ch = 0; ch < channels; ++ch)
        std::copy_n(s.shadow_due.data() + static_cast<std::size_t>(ch) * n, n,
                    output.channel_ptr(ch));
}

void GpuNeuralRealtimeNode::process_block(const audio::BufferView<const float>&,
                                          audio::BufferView<float>&, std::uint32_t) {
    throw std::logic_error("GpuNeuralRealtimeNode requires the stamped transport");
}

NeuralFamily GpuNeuralRealtimeNode::family() const noexcept {
    return impl_->config.family;
}

bool GpuNeuralRealtimeNode::gpu_kernel_available() const noexcept {
    return impl_->prepared && impl_->backend_active;
}

std::uint64_t GpuNeuralRealtimeNode::epoch() const noexcept {
    return impl_->bridge ? impl_->bridge->delivery_epoch() : 0;
}

bool GpuNeuralRealtimeNode::fenced() const noexcept {
    return !impl_->prepared || !impl_->bridge || impl_->bridge->delivery_epoch() == 0;
}

GpuAudioRecoveryReason GpuNeuralRealtimeNode::recovery_reason() const noexcept {
    if (impl_->bridge)
        return public_reason(impl_->bridge->recovery_reason());
    return public_reason(impl_->cpu_recovery.load(std::memory_order_acquire));
}

std::uint64_t GpuNeuralRealtimeNode::recovery_sequence() const noexcept {
    if (impl_->bridge)
        return impl_->bridge->recovery_sequence();
    return impl_->cpu_recovery_sequence.load(std::memory_order_acquire);
}

GpuAudioCapabilityReport GpuNeuralRealtimeNode::capability_report() const noexcept {
    GpuAudioCapabilityReport report;
    const auto& s = *impl_;
    if (!s.prepared)
        return report;
    report.path =
        s.backend_active ? GpuAudioExecutionPath::SharedMemory : GpuAudioExecutionPath::Cpu;
    report.provider = !s.backend_active          ? GpuAudioProvider::Cpu
                      : authenticated_provider() ? GpuAudioProvider::Dawn
                                                 : GpuAudioProvider::Unknown;
    report.eligibility = GpuAudioEligibility::Eligible;
    report.fallback_policy = MissPolicy::CpuFallback;
    report.prepared_lead_blocks = s.config.lead_blocks;
    report.prepared = true;
    report.fallback_available = cpu_fallback_prepared();
    report.diagnostics_available = true;
    report.prepared_latency_blocks = s.config.lead_blocks;
    report.gpu_kernel_available = s.backend_active;
    report.recovery_policy = GpuAudioRecoveryPolicy::Manual;
    return report;
}

GpuAudioStatus GpuNeuralRealtimeNode::status_snapshot() const noexcept {
    GpuAudioStatus status;
    const auto& s = *impl_;
    if (!s.prepared) {
        status.provider_state = GpuAudioProviderState::Uninitialized;
        return status;
    }
    const auto n = s.block_size();
    status.fallback_policy = MissPolicy::CpuFallback;
    status.sample_rate = s.model->sample_rate;
    status.block_size = n;
    status.latency_samples = s.config.lead_blocks * n;
    status.prepared_lead_blocks = s.config.lead_blocks;
    status.fallback_available = cpu_fallback_prepared();
    status.diagnostics_available = true;
    status.recovery_reason = recovery_reason();
    status.recovery_sequence = recovery_sequence();
    status.epoch = epoch();
    status.delivery_epoch_active = !fenced();
    status.gpu_kernel_available = s.backend_active;
    // A model with a GPU kernel asked for the GPU; the others asked for CPU.
    status.requested_engine = s.model->backend ? GpuAudioEngine::Gpu : GpuAudioEngine::Cpu;
    if (!s.backend_active) {
        // CPU-only execution is a prepared CPU provider. It is Ready when
        // that is what was requested and Degraded when a GPU was requested
        // but its provider could not be prepared.
        status.selected_engine = GpuAudioEngine::Cpu;
        status.provider = GpuAudioProvider::Cpu;
        status.provider_state =
            s.model->backend ? GpuAudioProviderState::Degraded : GpuAudioProviderState::Ready;
        status.fallback_blocks = s.cpu_blocks.load(std::memory_order_relaxed);
        return status;
    }
    const auto telemetry = s.telemetry.snapshot();
    status.produced_blocks = telemetry.delivered_blocks;
    status.missed_blocks = telemetry.deadline_misses;
    status.fallback_blocks = telemetry.fallback_blocks;
    status.resynced_blocks = telemetry.resync_drops;
    status.input_dropped_frames = telemetry.input_drops;
    status.last_block_us = static_cast<double>(telemetry.submit_to_completion_ns) / 1000.0;
    status.provider = authenticated_provider() ? GpuAudioProvider::Dawn : GpuAudioProvider::Unknown;
    status.selected_engine =
        authenticated_provider() ? GpuAudioEngine::Gpu : GpuAudioEngine::Unknown;
    status.provider_state = authenticated_provider() && !fenced() ? GpuAudioProviderState::Ready
                                                                  : GpuAudioProviderState::Degraded;
    return status;
}

bool GpuNeuralRealtimeNode::ready() const noexcept {
    return impl_->prepared;
}

bool GpuNeuralRealtimeNode::authenticated_provider() const noexcept {
    return impl_->prepared && impl_->backend_active && impl_->model->production_backend();
}

std::uint64_t GpuNeuralRealtimeNode::next_sequence(void* self) noexcept {
    auto& s = *static_cast<GpuNeuralRealtimeNode*>(self)->impl_;
    return s.bridge ? s.bridge->next_sequence() : s.cpu_next_sequence;
}

std::uint8_t GpuNeuralRealtimeNode::process(void* self, const audio::BufferView<const float>& input,
                                            audio::BufferView<float>& output, std::uint32_t n,
                                            std::uint64_t sequence, bool valid,
                                            std::uint64_t start) noexcept {
    auto& s = *static_cast<GpuNeuralRealtimeNode*>(self)->impl_;
    if (!s.prepared)
        return detail::kRealtimeGpuMissed;
    const bool shape_ok = valid && n == s.block_size() &&
                          input.num_channels() == s.config.channels &&
                          output.num_channels() == s.config.channels && input.num_samples() >= n &&
                          output.num_samples() >= n;
    if (!s.backend_active) {
        // CPU-only: no GPU epoch exists, so every block is the shadow's. The
        // transport selects it through the CpuFallback miss policy.
        if (!shape_ok)
            s.fail(detail::SharedIoRecoveryReason::InvalidCallback, sequence);
        else if (sequence < s.cpu_next_sequence)
            s.fail(detail::SharedIoRecoveryReason::InvalidCallback, sequence);
        else if (sequence > s.cpu_next_sequence)
            s.fail(detail::SharedIoRecoveryReason::SequenceGap, sequence);
        if (sequence >= s.cpu_next_sequence && sequence < Bridge::kSequenceLimit)
            s.cpu_next_sequence = sequence + 1;
        return detail::kRealtimeGpuMissed;
    }
    if (!shape_ok) {
        s.fail(detail::SharedIoRecoveryReason::InvalidCallback);
        std::fill(s.callback_input.begin(), s.callback_input.end(), 0.0f);
    } else {
        for (std::uint32_t ch = 0; ch < s.config.channels; ++ch)
            std::copy_n(input.channel_ptr(ch), n, s.callback_input.data() + ch * n);
    }
    s.callback = s.bridge->begin_callback(s.callback_input, sequence, start);
    const auto delivery = s.bridge->consume_output(s.callback, s.callback_output, nullptr, true);
    if (delivery == Bridge::Delivery::Ready) {
        for (std::uint32_t ch = 0; ch < s.config.channels; ++ch)
            std::copy_n(s.callback_output.data() + ch * n, n, output.channel_ptr(ch));
        return detail::kRealtimeGpuReady;
    }
    return delivery == Bridge::Delivery::Priming ? detail::kRealtimeGpuPriming
                                                 : detail::kRealtimeGpuMissed;
}

std::uint32_t GpuNeuralRealtimeNode::service(void* self, std::uint64_t) noexcept {
    auto& s = *static_cast<GpuNeuralRealtimeNode*>(self)->impl_;
    if (!s.prepared || !s.backend_active)
        return detail::kRealtimeGpuServiceInactive;
    const auto n = s.block_size();
    const auto pump_now = now_ns();
    const auto wait_ns = s.config.completion_service_wait_ns;
    const auto deadline = wait_ns != 0 && pump_now <= UINT64_MAX - wait_ns ? pump_now + wait_ns : 0;
    auto produced = s.collect(deadline);
    if (!s.bridge->begin_worker_admission())
        return produced;
    bool admitted = false;
    while (s.pending_count < s.pending_ring.size()) {
        auto input = s.bridge->acquire_input();
        if (!input)
            break;
        const auto stamp = input->stamp();
        if (stamp.epoch != s.bridge->delivery_epoch() || stamp.sequence != s.next_input) {
            s.fail(detail::SharedIoRecoveryReason::SequenceGap, stamp.sequence);
            (void)s.bridge->release_input(*input);
            break;
        }
        auto& pending = s.pending_ring[(s.pending_head + s.pending_count) % s.pending_ring.size()];
        pending.reset();
        pending.occupied = true;
        pending.stamp = {.epoch = stamp.epoch, .sequence = stamp.sequence};
        // The backend contract retains both spans until the terminal, so the
        // block owns copies rather than the bridge's ingress lease.
        std::copy(input->samples().begin(), input->samples().end(), pending.input.begin());
        (void)s.bridge->release_input(*input);
        const detail::StreamingBlock block{.stamp = pending.stamp,
                                           .input = pending.input,
                                           .output = pending.output,
                                           .input_channels = s.config.channels,
                                           .output_channels = s.config.channels,
                                           .frames = n,
                                           .input_channel_stride = n,
                                           .output_channel_stride = n};
        if (s.model->backend->enqueue(block) != detail::StreamingAdmission::Accepted) {
            // A rejected block carries no backend terminal; retire it here.
            pending.done = true;
            pending.disposition = Terminal::ProviderFailed;
            s.fail(detail::SharedIoRecoveryReason::ProviderFailure, stamp.sequence);
        } else {
            admitted = true;
        }
        ++s.next_input;
        ++s.pending_count;
    }
    s.bridge->end_worker_admission();
    if (deadline != 0 && admitted)
        produced += s.collect(deadline);
    else
        produced += s.collect(0);
    return produced;
}

bool GpuNeuralRealtimeNode::fence(void* self) noexcept {
    auto& s = *static_cast<GpuNeuralRealtimeNode*>(self)->impl_;
    if (!s.bridge || !s.backend_active)
        return true;
    s.fail(detail::SharedIoRecoveryReason::OfflineFence);
    if (!s.model->backend->quiesce())
        return false;
    for (auto& pending : s.pending_ring)
        pending.reset();
    s.pending_head = s.pending_count = 0;
    return true;
}

bool GpuNeuralRealtimeNode::health(const void* self, detail::RealtimeGpuHealth& out) noexcept {
    const auto& node = *static_cast<const GpuNeuralRealtimeNode*>(self);
    out = {};
    out.fenced = node.fenced();
    out.recovery_reason = node.recovery_reason();
    out.recovery_sequence = node.recovery_sequence();
    out.epoch = node.epoch();
    out.delivery_epoch_active = !out.fenced;
    return true;
}

void GpuNeuralRealtimeNode::delivered(void* self, std::uint64_t sequence, std::uint8_t disposition,
                                      std::uint64_t end, std::uint64_t visible) noexcept {
    auto& s = *static_cast<GpuNeuralRealtimeNode*>(self)->impl_;
    const auto actual = static_cast<detail::SharedIoDeliveryDisposition>(disposition);
    if (!s.bridge) {
        if (actual == detail::SharedIoDeliveryDisposition::CpuFallbackDelivered)
            s.cpu_blocks.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (s.callback.stamp.sequence == sequence)
        (void)s.bridge->complete_callback_delivery(s.callback, actual, end, visible);
}

void detail::NeuralRealtimeTestAccess::skip_shadow_publish(GpuNeuralRealtimeNode& node,
                                                           bool skip) noexcept {
    node.impl_->test_skip_publish = skip;
}

void detail::NeuralRealtimeTestAccess::request_recovery(GpuNeuralRealtimeNode& node,
                                                        SharedIoRecoveryReason reason) noexcept {
    node.impl_->fail(reason);
}

detail::RealtimeGpuNodePath
detail::NeuralRealtimeTestAccess::path(GpuNeuralRealtimeNode& node) noexcept {
    return realtime_gpu_node_path(&node);
}

} // namespace pulp::gpu_audio
