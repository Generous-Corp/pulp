#include "detail/realtime_gpu_audio_path.hpp"
#include "detail/shared_io_stamped_bridge.hpp"
#include "detail/wavenet_realtime_channel.hpp"
#include <algorithm>
#include <chrono>
#include <pulp/gpu_audio/gpu_wavenet_realtime_node.hpp>
#include <stdexcept>
#include <thread>
#include <vector>

namespace pulp::gpu_audio {
namespace {
using Bridge = detail::SharedIoStampedBridge;
class SessionChannel final : public detail::WaveNetRealtimeChannel {
  public:
    explicit SessionChannel(std::unique_ptr<GpuWaveNetSession> session)
        : session_(std::move(session)) {}
    bool submit(std::span<const float> input, std::uint64_t sequence) noexcept override {
        return session_->submit_block(input, sequence);
    }
    void service(std::uint64_t now) noexcept override {
        session_->service(now);
    }
    void service_until(std::uint64_t, std::uint64_t deadline) noexcept override {
        session_->service_until(deadline);
    }
    std::optional<GpuWaveNetBlockResult> receive(std::span<float> output) noexcept override {
        return session_->receive(output);
    }
    bool release() noexcept override {
        return session_->release();
    }

  private:
    std::unique_ptr<GpuWaveNetSession> session_;
};
std::uint64_t now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
} // namespace

struct GpuWaveNetRealtimeNode::Impl {
    struct Pending {
        Bridge::Stamp stamp;
        std::vector<float> output;
        detail::SharedIoTraceRecord trace_record;
        std::uint64_t completed_mask = 0;
        detail::SharedIoTraceOutcome outcome = detail::SharedIoTraceOutcome::Success;
        bool occupied = false;
        bool trace_pending = false;
        bool suppress = false;

        void reset() noexcept {
            stamp = {};
            completed_mask = 0;
            outcome = detail::SharedIoTraceOutcome::Success;
            occupied = false;
            trace_pending = false;
            suppress = false;
            trace_record = {};
        }
    };

    Config config;
    GpuWaveNetTraceConfig trace_config;
    std::uint64_t trace_engine = 0;
    std::unique_ptr<detail::SharedIoTraceRecorder> trace;
    detail::SharedIoTelemetry telemetry;
    detail::SharedIoTraceStats closed_trace_stats;
    detail::SharedIoTraceDrainObserver trace_observer;
    std::uint32_t unresolved_channel_count = 0;
    detail::SharedIoTraceRecord trace_record;

    void drain_trace() noexcept {
        if (trace)
            (void)detail::drain_shared_io_trace(
                *trace, telemetry.snapshot(),
                static_cast<std::uint32_t>(detail::SharedIoTraceRecorder::capacity),
                &trace_observer);
    }
    void trace_admit(Pending& pending, const Bridge::Lease& input) noexcept {
        if (!trace)
            return;
        pending.trace_record = {};
        pending.trace_record.generation = epoch;
        pending.trace_record.sequence = input.stamp().sequence;
        pending.trace_record.gpu_work_admitted = true;
        pending.trace_record.callback_ingress_ns = input.callback_ingress_ns();
        pending.trace_record.set(detail::SharedIoTraceStage::WorkerEntry, now_ns());
        pending.trace_pending = true;
        (void)trace->publish_admission(epoch, input.stamp().sequence);
    }
    void trace_stage(Pending& pending, detail::SharedIoTraceStage stage) noexcept {
        if (pending.trace_pending && trace)
            pending.trace_record.set(stage, now_ns());
    }
    void trace_terminal(Pending& pending, detail::SharedIoTraceOutcome outcome,
                        detail::SharedIoGpuTerminalDisposition terminal,
                        detail::SharedIoFallbackReason reason, bool observed = false) noexcept {
        if (!pending.trace_pending || !trace)
            return;
        pending.trace_pending = false;
        pending.trace_record.outcome = outcome;
        pending.trace_record.gpu_terminal = terminal;
        pending.trace_record.reason = pending.trace_record.gpu_reason = reason;
        if (observed)
            pending.trace_record.set(detail::SharedIoTraceStage::CompletionObserved, now_ns());
        // Retirement is the host-side point at which this admitted block has
        // a final GPU disposition. It is deliberately separate from
        // CompletionObserved: a provider can fail, go stale, or be cancelled
        // without ever producing a completion.
        pending.trace_record.set(detail::SharedIoTraceStage::RetirementObserved, now_ns());
        trace_record = pending.trace_record;
        (void)trace->publish_worker(pending.trace_record);
        telemetry.record_retired(outcome == detail::SharedIoTraceOutcome::Success);
    }
    detail::SharedIoFallbackReason recovery_trace_reason() const noexcept {
        using Recovery = detail::SharedIoRecoveryReason;
        using Reason = detail::SharedIoFallbackReason;
        switch (bridge->recovery_reason()) {
        case Recovery::InputSaturated:
            return Reason::InputSaturated;
        case Recovery::ProviderFailure:
            return Reason::CompletionFailed;
        case Recovery::ProviderLost:
            return Reason::DeviceLost;
        case Recovery::OfflineFence:
            return Reason::Teardown;
        case Recovery::None:
            return Reason::None;
        case Recovery::InvalidCallback:
            return Reason::InvalidCallback;
        case Recovery::SequenceGap:
            return Reason::SequenceGap;
        }
        return Reason::None;
    }
    void close_pending_trace() noexcept {
        // Physical drain closes ownership; it must not erase a failure already
        // observed before teardown. No completion timestamp is inferred here.
        for (auto& pending : pending_ring) {
            if (!pending.occupied)
                continue;
            if (pending.outcome != detail::SharedIoTraceOutcome::Success)
                trace_terminal(pending, pending.outcome,
                               detail::SharedIoGpuTerminalDisposition::ProviderFailed,
                               pending.outcome == detail::SharedIoTraceOutcome::SubmissionRejected
                                   ? detail::SharedIoFallbackReason::SubmissionRejected
                                   : detail::SharedIoFallbackReason::CompletionFailed);
            else
                trace_terminal(pending, detail::SharedIoTraceOutcome::Cancelled,
                               detail::SharedIoGpuTerminalDisposition::CancelledTeardown,
                               detail::SharedIoFallbackReason::Teardown);
        }
    }
    void close_trace() noexcept {
        if (!trace)
            return;
        close_pending_trace();
        // Producers are quiescent; a bounded number of drains covers all queues.
        for (int i = 0; i < 4; ++i)
            drain_trace();
        closed_trace_stats = trace->stats();
        if (bridge)
            bridge->set_trace(nullptr);
        trace.reset();
    }

    std::vector<GpuWaveNetLayerDescriptor> layers;
    std::vector<std::vector<std::uint32_t>> dilations;
    std::vector<float> weights;
    std::vector<std::unique_ptr<detail::WaveNetRealtimeChannel>> channels;
    std::unique_ptr<Bridge> bridge;
    std::vector<float> callback_input, callback_output, worker_output;
    std::vector<Pending> pending_ring;
    Bridge::Callback callback;
    std::size_t pending_head = 0, pending_count = 0;
    std::uint64_t next_input = 0, sequence = 0, epoch = 0;
    std::uint64_t all_channels_mask = 0;
    bool prepared = false, production_provider = false;

    explicit Impl(const Config& source)
        : config(source),
          layers(source.session.descriptor.layers.begin(), source.session.descriptor.layers.end()),
          weights(source.session.weights.begin(), source.session.weights.end()) {
        dilations.reserve(layers.size());
        for (auto& layer : layers) {
            dilations.emplace_back(layer.dilations.begin(), layer.dilations.end());
            layer.dilations = dilations.back();
        }
        config.session.descriptor.layers = layers;
        config.session.weights = weights;
    }
    bool valid_config(bool include_prewarm = true) const noexcept {
        const auto& c = config;
        return c.channels > 0 && c.channels <= 64 && c.lead_blocks > 0 &&
               c.capacity > c.lead_blocks && c.max_inflight > 0 &&
               c.max_inflight <= c.session.slots && c.max_inflight <= c.capacity - c.lead_blocks &&
               c.session.slots >= 2 && c.completion_service_wait_ns <= 1'000'000 &&
               validate_gpu_wavenet_descriptor(c.session.descriptor).accepted() &&
               weights.size() == c.session.descriptor.weight_count &&
               (c.miss_policy != MissPolicy::CpuFallback || c.supports_cpu_fallback) &&
               (c.miss_policy == MissPolicy::Silence || c.miss_policy == MissPolicy::CpuFallback) &&
               epoch != UINT64_MAX && sequence < Bridge::kSequenceLimit &&
               (!include_prewarm || c.prewarm_blocks < Bridge::kSequenceLimit - sequence);
    }
    bool initialize() {
        const auto& c = config;
        if (prepared || !valid_config(false) || channels.size() != c.channels)
            return false;
        bridge = std::make_unique<Bridge>();
        if (!bridge->prepare({.capacity = c.capacity,
                              .channels = c.channels,
                              .block_size = c.session.descriptor.block_size,
                              .lead_blocks = c.lead_blocks,
                              .capture_callback_timing =
                                  trace_config.enabled && trace_config.capture_callback_timing},
                             ++epoch, sequence))
            return false;
        const auto count = bridge->samples_per_block();
        callback_input.assign(count, 0.f);
        callback_output.assign(count, 0.f);
        worker_output.assign(count, 0.f);
        pending_ring.assign(c.max_inflight, {});
        for (auto& pending : pending_ring)
            pending.output.assign(count, 0.f);
        pending_head = pending_count = 0;
        all_channels_mask = c.channels == 64 ? UINT64_MAX : (UINT64_C(1) << c.channels) - 1;
        next_input = sequence;
        if (trace_config.enabled) {
            if (!trace_engine)
                trace_engine = detail::next_shared_io_trace_engine_id();
            detail::SharedIoExecutionContract contract;
            contract.channels = c.channels;
            contract.block_size = c.session.descriptor.block_size;
            contract.sample_rate = c.session.descriptor.sample_rate;
            contract.algorithmic_lead_blocks = c.lead_blocks;
            contract.pipeline_depth = c.capacity;
            contract.provider_slots = c.session.slots;
            contract.max_inflight = c.max_inflight;
            contract.requested_path = detail::SharedIoRequest::RequireSharedHostPointer;
            contract.active_path = detail::SharedIoPath::SharedHostPointer;
            contract.shared_host_pointer_capable = true;
            contract.miss_policy = c.miss_policy;
            contract.cpu_fallback_prepared = c.supports_cpu_fallback;
            trace = std::make_unique<detail::SharedIoTraceRecorder>(
                detail::SharedIoTraceConfig{.engine_id = trace_engine,
                                            .generation = epoch,
                                            .contract = contract,
                                            .success_stride = trace_config.success_stride,
                                            .capture_admissions = trace_config.capture_admissions,
                                            .enabled = true});
            telemetry.reset();
            closed_trace_stats = {};
            bridge->set_trace(trace.get(), &telemetry);
        }
        prepared = true;
        return true;
    }
    std::uint32_t service_and_collect(std::uint64_t service_now, std::uint64_t deadline) noexcept {
        const auto n = config.session.descriptor.block_size;
        for (std::size_t ch = 0; ch < channels.size(); ++ch) {
            if (deadline != 0)
                channels[ch]->service_until(service_now, deadline);
            else
                channels[ch]->service(service_now);
            std::size_t received = 0;
            while (received < pending_count) {
                auto result = channels[ch]->receive({worker_output.data() + ch * n, n});
                if (!result)
                    break;
                ++received;
                Pending* pending = nullptr;
                for (std::size_t offset = 0; offset < pending_count; ++offset) {
                    auto& candidate = pending_ring[(pending_head + offset) % pending_ring.size()];
                    if (candidate.occupied && candidate.stamp.sequence == result->sequence) {
                        pending = &candidate;
                        break;
                    }
                }
                if (pending == nullptr) {
                    fail(detail::SharedIoRecoveryReason::ProviderFailure, result->sequence);
                    continue;
                }
                const auto bit = UINT64_C(1) << ch;
                if ((pending->completed_mask & bit) != 0) {
                    pending->outcome = detail::SharedIoTraceOutcome::CompletionFailed;
                    fail(detail::SharedIoRecoveryReason::ProviderFailure, result->sequence);
                    continue;
                }
                pending->completed_mask |= bit;
                pending->suppress = pending->suppress || result->late;
                if (result->status != GpuWaveNetBlockStatus::GpuDelivered) {
                    if (pending->outcome == detail::SharedIoTraceOutcome::Success)
                        pending->outcome = detail::SharedIoTraceOutcome::CompletionFailed;
                    fail(detail::SharedIoRecoveryReason::ProviderFailure, result->sequence);
                } else {
                    std::copy_n(worker_output.data() + ch * n, n, pending->output.data() + ch * n);
                }
            }
        }
        std::uint32_t produced = 0;
        while (pending_count != 0) {
            auto& pending = pending_ring[pending_head];
            if (!pending.occupied || pending.completed_mask != all_channels_mask)
                break;
            if (pending.outcome != detail::SharedIoTraceOutcome::Success) {
                trace_terminal(pending, pending.outcome,
                               detail::SharedIoGpuTerminalDisposition::ProviderFailed,
                               pending.outcome == detail::SharedIoTraceOutcome::SubmissionRejected
                                   ? detail::SharedIoFallbackReason::SubmissionRejected
                                   : detail::SharedIoFallbackReason::CompletionFailed,
                               true);
            } else if (pending.suppress) {
                trace_terminal(pending, detail::SharedIoTraceOutcome::LateRejected,
                               detail::SharedIoGpuTerminalDisposition::LateRejected,
                               detail::SharedIoFallbackReason::DeadlineExceeded, true);
            } else if (bridge->delivery_epoch() != pending.stamp.epoch) {
                trace_terminal(pending, detail::SharedIoTraceOutcome::StaleRejected,
                               detail::SharedIoGpuTerminalDisposition::StaleRejected,
                               recovery_trace_reason(), true);
            }
            if (!pending.suppress && bridge->delivery_epoch() == pending.stamp.epoch &&
                pending.outcome == detail::SharedIoTraceOutcome::Success) {
                const auto publication = bridge->publish_output(pending.stamp, pending.output);
                produced += publication == Bridge::Publication::Published ? 1 : 0;
                if (publication != Bridge::Publication::Published) {
                    trace_terminal(pending, detail::SharedIoTraceOutcome::LateRejected,
                                   detail::SharedIoGpuTerminalDisposition::LateRejected,
                                   detail::SharedIoFallbackReason::InputSaturated, true);
                    fail(detail::SharedIoRecoveryReason::InputSaturated, pending.stamp.sequence);
                } else {
                    trace_terminal(pending, detail::SharedIoTraceOutcome::Success,
                                   detail::SharedIoGpuTerminalDisposition::CompletedAccepted,
                                   detail::SharedIoFallbackReason::None, true);
                }
            }
            pending.reset();
            pending_head = (pending_head + 1) % pending_ring.size();
            --pending_count;
        }
        return produced;
    }
    void fail(detail::SharedIoRecoveryReason reason,
              std::uint64_t sequence = std::numeric_limits<std::uint64_t>::max()) noexcept {
        bridge->request_recovery(reason, sequence);
    }
};

GpuWaveNetRealtimeNode::GpuWaveNetRealtimeNode(const Config& config)
    : impl_(std::make_unique<Impl>(config)) {}
GpuWaveNetRealtimeNode::~GpuWaveNetRealtimeNode() {
    if (!release() && impl_->trace) {
        // Callers are quiescent. Preserve real records, but do not manufacture
        // retirement for channels whose physical release was not established.
        for (int i = 0; i < 4; ++i)
            impl_->drain_trace();
        detail::emit_shared_io_unresolved_ownership(
            {impl_->trace_engine, impl_->epoch, false, impl_->unresolved_channel_count},
            &impl_->trace_observer);
    }
}
GpuAudioNodeDescriptor GpuWaveNetRealtimeNode::descriptor() const {
    const auto& c = impl_->config;
    return {.name = "Stamped shared WaveNet",
            .input_channels = c.channels,
            .output_channels = c.channels,
            .block_size = c.session.descriptor.block_size,
            .sample_rate = c.session.descriptor.sample_rate,
            .latency_blocks = c.lead_blocks,
            .miss_policy = c.miss_policy,
            .supports_cpu_fallback = c.supports_cpu_fallback};
}
bool GpuWaveNetRealtimeNode::configure_trace(const GpuWaveNetTraceConfig& config) noexcept {
    if (impl_->prepared || impl_->trace || config.success_stride == 0)
        return false;
    impl_->trace_config = config;
    return true;
}

bool GpuWaveNetRealtimeNode::prepare() {
    if (!release())
        return false;
    auto& s = *impl_;
    try {
        if (!s.valid_config())
            return false;
        for (std::uint32_t ch = 0; ch < s.config.channels; ++ch) {
            auto result = GpuWaveNetSession::create(s.config.session);
            if (!result) {
                // Retain a partially prepared session if its physical drain needs retry.
                if (result.session)
                    s.channels.push_back(
                        std::make_unique<SessionChannel>(std::move(result.session)));
                (void)release();
                return false;
            }
            s.channels.push_back(std::make_unique<SessionChannel>(std::move(result.session)));
        }
        // Silence warmup is explicit and stopped-host-only. Never auto-reprime a
        // failed live history with silence. All channels begin at the same block.
        std::vector<float> zero(s.config.session.descriptor.block_size, 0.f), output(zero.size());
        for (std::uint32_t block = 0; block < s.config.prewarm_blocks; ++block) {
            for (auto& channel : s.channels) {
                if (!channel->submit(zero, s.sequence + block)) {
                    (void)release();
                    return false;
                }
                const auto deadline = now_ns() + 2'000'000'000ull;
                bool received = false;
                while (now_ns() < deadline) {
                    channel->service(now_ns());
                    if (auto result = channel->receive(output)) {
                        received = result->sequence == s.sequence + block &&
                                   result->status == GpuWaveNetBlockStatus::GpuDelivered;
                        break;
                    }
                    std::this_thread::yield();
                }
                if (!received) {
                    (void)release();
                    return false;
                }
            }
        }
        s.sequence += s.config.prewarm_blocks;
        if (s.initialize()) {
            s.production_provider = true;
            return true;
        }
    } catch (...) {
    }
    (void)release();
    return false;
}
bool GpuWaveNetRealtimeNode::release() noexcept {
    auto& s = *impl_;
    s.prepared = false;
    s.production_provider = false;
    if (s.bridge) {
        s.sequence = s.bridge->next_sequence();
        s.bridge->request_recovery(detail::SharedIoRecoveryReason::OfflineFence);
    }
    s.unresolved_channel_count = 0;
    for (auto& channel : s.channels)
        if (!channel->release())
            ++s.unresolved_channel_count;
    if (s.unresolved_channel_count != 0)
        return false;
    s.close_trace();
    s.channels.clear();
    s.bridge.reset();
    s.pending_ring.clear();
    s.pending_head = s.pending_count = 0;
    return true;
}
void GpuWaveNetRealtimeNode::process_block(const audio::BufferView<const float>&,
                                           audio::BufferView<float>&, std::uint32_t) {
    throw std::logic_error("WaveNet realtime node requires stamped transport");
}
bool GpuWaveNetRealtimeNode::fenced() const noexcept {
    return !impl_->prepared || !impl_->bridge || impl_->bridge->delivery_epoch() == 0;
}
GpuAudioRecoveryReason GpuWaveNetRealtimeNode::recovery_reason() const noexcept {
    if (!impl_->bridge)
        return GpuAudioRecoveryReason::None;
    switch (impl_->bridge->recovery_reason()) {
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
std::uint64_t GpuWaveNetRealtimeNode::recovery_sequence() const noexcept {
    return impl_->bridge ? impl_->bridge->recovery_sequence()
                         : std::numeric_limits<std::uint64_t>::max();
}
bool GpuWaveNetRealtimeNode::ready() const noexcept {
    return impl_->prepared;
}
bool GpuWaveNetRealtimeNode::authenticated_provider() const noexcept {
    return impl_->prepared && impl_->production_provider;
}
std::uint64_t GpuWaveNetRealtimeNode::next_sequence(void* self) noexcept {
    return static_cast<GpuWaveNetRealtimeNode*>(self)->impl_->bridge->next_sequence();
}
std::uint8_t GpuWaveNetRealtimeNode::process(void* self,
                                             const audio::BufferView<const float>& input,
                                             audio::BufferView<float>& output, std::uint32_t n,
                                             std::uint64_t sequence, bool valid,
                                             std::uint64_t start) noexcept {
    auto& s = *static_cast<GpuWaveNetRealtimeNode*>(self)->impl_;
    if (!s.prepared)
        return detail::kRealtimeGpuMissed;
    if (!valid || n != s.config.session.descriptor.block_size ||
        input.num_channels() != s.config.channels || output.num_channels() != s.config.channels ||
        input.num_samples() < n || output.num_samples() < n) {
        s.fail(detail::SharedIoRecoveryReason::InvalidCallback);
        std::fill(s.callback_input.begin(), s.callback_input.end(), 0.f);
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
std::uint32_t GpuWaveNetRealtimeNode::service(void* self, std::uint64_t) noexcept {
    auto& s = *static_cast<GpuWaveNetRealtimeNode*>(self)->impl_;
    if (!s.prepared)
        return 0;
    const auto n = s.config.session.descriptor.block_size;
    const auto pump_now = now_ns();
    const auto wait_ns = s.config.completion_service_wait_ns;
    const auto deadline = wait_ns != 0 && pump_now <= UINT64_MAX - wait_ns ? pump_now + wait_ns : 0;
    // This serialized worker is the only live trace consumer, including early returns.
    struct Drain {
        Impl& impl;
        ~Drain() {
            impl.drain_trace();
        }
    } drain{s};
    auto produced = s.service_and_collect(pump_now, deadline);
    if (!s.bridge->begin_worker_admission())
        return produced;
    bool all_submitted = false;
    while (s.pending_count < s.pending_ring.size()) {
        auto input = s.bridge->acquire_input();
        if (!input)
            break;
        auto& pending = s.pending_ring[(s.pending_head + s.pending_count) % s.pending_ring.size()];
        pending.reset();
        pending.occupied = true;
        const auto stamp = input->stamp();
        s.trace_admit(pending, *input);
        if (stamp.epoch != s.bridge->delivery_epoch() || stamp.sequence != s.next_input) {
            s.trace_terminal(pending, detail::SharedIoTraceOutcome::StaleRejected,
                             detail::SharedIoGpuTerminalDisposition::StaleRejected,
                             detail::SharedIoFallbackReason::SequenceGap);
            pending.reset();
            s.fail(detail::SharedIoRecoveryReason::SequenceGap, stamp.sequence);
            (void)s.bridge->release_input(*input);
            break;
        }
        pending.stamp = stamp;
        s.trace_stage(pending, detail::SharedIoTraceStage::EncodeBegin);
        // WaveNet's provider path does not expose a separate command encoder.
        // The channel submit boundary is the authenticated host submission
        // region, so keep encode and submit timestamps distinct without
        // claiming either is a GPU timestamp.
        s.trace_stage(pending, detail::SharedIoTraceStage::EncodeEnd);
        s.trace_stage(pending, detail::SharedIoTraceStage::SubmitBegin);
        bool submission_ok = true;
        for (std::size_t ch = 0; ch < s.channels.size(); ++ch) {
            if (!s.channels[ch]->submit(input->samples().subspan(ch * n, n), stamp.sequence)) {
                submission_ok = false;
                pending.completed_mask |= UINT64_C(1) << ch;
                pending.outcome = detail::SharedIoTraceOutcome::SubmissionRejected;
                s.fail(detail::SharedIoRecoveryReason::ProviderFailure, stamp.sequence);
            }
        }
        s.trace_stage(pending, detail::SharedIoTraceStage::SubmitEnd);
        if (s.trace)
            s.telemetry.record_submit();
        ++s.next_input;
        ++s.pending_count;
        if (s.trace)
            s.telemetry.observe_in_flight(s.pending_count);
        all_submitted = all_submitted || submission_ok;
        (void)s.bridge->release_input(*input);
    }
    s.bridge->end_worker_admission();
    if (deadline != 0 && all_submitted)
        produced += s.service_and_collect(now_ns(), deadline);
    return produced;
}

bool GpuWaveNetRealtimeNode::fence(void* self) noexcept {
    auto& s = *static_cast<GpuWaveNetRealtimeNode*>(self)->impl_;
    if (!s.bridge)
        return true;
    s.fail(detail::SharedIoRecoveryReason::OfflineFence);
    bool drained = true;
    for (auto& channel : s.channels)
        if (!channel->release())
            drained = false;
    if (drained) {
        s.close_pending_trace();
    }
    return drained;
}
void GpuWaveNetRealtimeNode::delivered(void* self, std::uint64_t sequence, std::uint8_t disposition,
                                       std::uint64_t end, std::uint64_t visible) noexcept {
    auto& s = *static_cast<GpuWaveNetRealtimeNode*>(self)->impl_;
    if (s.callback.stamp.sequence == sequence)
        (void)s.bridge->complete_callback_delivery(
            s.callback, static_cast<detail::SharedIoDeliveryDisposition>(disposition), end,
            visible);
}
void detail::WaveNetRealtimeTestAccess::request_recovery(GpuWaveNetRealtimeNode& node,
                                                         SharedIoRecoveryReason reason,
                                                         std::uint64_t sequence) noexcept {
    node.impl_->bridge->request_recovery(reason, sequence);
}
detail::SharedIoRecoveryReason
detail::WaveNetRealtimeTestAccess::recovery_reason(const GpuWaveNetRealtimeNode& node) noexcept {
    return node.impl_->bridge ? node.impl_->bridge->recovery_reason()
                              : SharedIoRecoveryReason::None;
}
std::uint64_t
detail::WaveNetRealtimeTestAccess::recovery_sequence(const GpuWaveNetRealtimeNode& node) noexcept {
    return node.impl_->bridge ? node.impl_->bridge->recovery_sequence()
                              : std::numeric_limits<std::uint64_t>::max();
}
void detail::WaveNetRealtimeTestAccess::observe_trace(
    GpuWaveNetRealtimeNode& node, SharedIoTraceDrainObserver observer) noexcept {
    node.impl_->trace_observer = observer;
}
detail::SharedIoTraceStats
detail::WaveNetRealtimeTestAccess::trace_stats(const GpuWaveNetRealtimeNode& node) noexcept {
    return node.impl_->trace ? node.impl_->trace->stats() : node.impl_->closed_trace_stats;
}
detail::SharedIoTelemetrySnapshot
detail::WaveNetRealtimeTestAccess::telemetry(const GpuWaveNetRealtimeNode& node) noexcept {
    return node.impl_->telemetry.snapshot();
}
detail::SharedIoTraceRecord
detail::WaveNetRealtimeTestAccess::last_terminal(const GpuWaveNetRealtimeNode& node) noexcept {
    return node.impl_->trace_record;
}
std::uint64_t
detail::WaveNetRealtimeTestAccess::trace_engine(const GpuWaveNetRealtimeNode& node) noexcept {
    return node.impl_->trace_engine;
}

bool detail::WaveNetRealtimeTestAccess::prepare(
    GpuWaveNetRealtimeNode& node, std::vector<std::unique_ptr<WaveNetRealtimeChannel>> channels,
    std::uint64_t first_sequence) {
    if (!node.release())
        return false;
    node.impl_->sequence = first_sequence;
    node.impl_->channels = std::move(channels);
    return node.impl_->valid_config() && node.impl_->initialize();
}
} // namespace pulp::gpu_audio
