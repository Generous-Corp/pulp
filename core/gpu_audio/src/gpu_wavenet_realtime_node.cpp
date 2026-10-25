#include "detail/realtime_gpu_audio_path.hpp"
#include "detail/realtime_gpu_health_mirror.hpp"
#include "detail/shared_io_stamped_bridge.hpp"
#include "detail/shared_io_watchdog.hpp"
#include "detail/wavenet_realtime_channel.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <pulp/gpu_audio/gpu_audio_program.hpp>
#include <pulp/gpu_audio/gpu_audio_provider_pool.hpp>
#include <pulp/gpu_audio/gpu_wavenet_realtime_node.hpp>
#include <stdexcept>
#include <thread>
#include <vector>

namespace pulp::gpu_audio {
namespace {
using Bridge = detail::SharedIoStampedBridge;
std::uint64_t now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
} // namespace

struct GpuWaveNetRealtimeNode::Impl {
    struct Pending {
        // Captured when the worker admits the block; the completion must carry
        // the same ticket to retire it.
        detail::SharedIoSubmissionTicket ticket;
        std::vector<float> output;
        detail::SharedIoTraceRecord trace_record;
        std::uint64_t completed_mask = 0;
        detail::SharedIoTraceOutcome outcome = detail::SharedIoTraceOutcome::Success;
        bool occupied = false;
        bool trace_pending = false;
        bool suppress = false;
        bool counted = false;
        // The worker's completion count when this block was admitted.
        std::uint64_t completions_at_admit = 0;

        Bridge::Stamp stamp() const noexcept {
            return {ticket.generation, ticket.sequence};
        }
        void reset() noexcept {
            ticket = {};
            completed_mask = 0;
            outcome = detail::SharedIoTraceOutcome::Success;
            occupied = false;
            trace_pending = false;
            suppress = false;
            counted = false;
            completions_at_admit = 0;
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
    detail::SharedIoTraceDrainResult last_trace_drain;
    std::uint32_t unresolved_channel_count = 0;
    detail::SharedIoTraceRecord trace_record;

    // Pass telemetry_quiescent only once the callback and the transport worker
    // are stopped; the live worker drain cannot exclude concurrent callbacks.
    void drain_trace(bool telemetry_quiescent = false) noexcept {
        if (trace) {
            const auto* observer =
                (trace_observer.record || trace_observer.admission || trace_observer.ownership)
                    ? &trace_observer
                    : nullptr;
            last_trace_drain = detail::drain_shared_io_trace(
                *trace, telemetry.snapshot(),
                static_cast<std::uint32_t>(detail::SharedIoTraceRecorder::capacity), observer,
                telemetry_quiescent);
        }
    }
    void trace_admit(Pending& pending, const Bridge::Lease& input) noexcept {
        if (!trace)
            return;
        telemetry.record_admission();
        pending.trace_record = {};
        pending.trace_record.generation = epoch;
        pending.trace_record.sequence = input.stamp().sequence;
        pending.trace_record.gpu_work_admitted = true;
        pending.trace_record.callback_ingress_ns = input.callback_ingress_ns();
        pending.trace_record.set(detail::SharedIoTraceStage::WorkerEntry, now_ns());
        pending.trace_pending = true;
        pending.trace_record.admission_identity_matched =
            trace->publish_admission(epoch, input.stamp().sequence);
    }
    void trace_stage(Pending& pending, detail::SharedIoTraceStage stage) noexcept {
        if (pending.trace_pending && trace)
            pending.trace_record.set(stage, now_ns());
    }
    void trace_terminal(Pending& pending, detail::SharedIoTraceOutcome outcome,
                        detail::SharedIoGpuTerminalDisposition terminal,
                        detail::SharedIoFallbackReason reason, bool observed = false) noexcept {
        if (!pending.counted) {
            pending.counted = true;
            tally(terminal, pending.ticket.sequence);
        }
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
    // One host-visible disposition per admitted block, counted only once the
    // audio it names is certain, so a quiescent snapshot never reports more
    // GPU or CPU-fallback blocks than the callback has delivered. A delivered
    // GPU block is counted by the callback that consumed it (delivered()). A
    // provider failure has no GPU-side disposition of its own; the block's
    // audio comes from the declared miss policy, and under CpuFallback it is
    // counted once its consumer has delivered that fallback.
    void tally(detail::SharedIoGpuTerminalDisposition terminal, std::uint64_t block) noexcept {
        using Gpu = detail::SharedIoGpuTerminalDisposition;
        using Public = GpuAudioTerminalDisposition;
        switch (terminal) {
        case Gpu::CompletedAccepted:
        case Gpu::None:
            return;
        case Gpu::StaleRejected:
            return count(Public::StaleRejected);
        case Gpu::LateRejected:
            return count(Public::LateRejected);
        case Gpu::DeviceLost:
            return count(Public::DeviceLost);
        case Gpu::CancelledTeardown:
            return count(Public::Cancelled);
        case Gpu::ProviderFailed:
            if (config.miss_policy != MissPolicy::CpuFallback)
                return count(Public::Silence);
            ++deferred_fallbacks;
            deferred_until = std::max(deferred_until, block + config.lead_blocks);
            return;
        }
    }
    void count(GpuAudioTerminalDisposition disposition) noexcept {
        auto& value = terminal_counts[static_cast<std::size_t>(disposition)];
        value.store(value.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }
    // Worker only: settle failed blocks whose consumers have delivered.
    void settle_fallbacks() noexcept {
        if (deferred_fallbacks == 0 ||
            delivered_next.load(std::memory_order_acquire) <= deferred_until)
            return;
        auto& value =
            terminal_counts[static_cast<std::size_t>(GpuAudioTerminalDisposition::CpuFallback)];
        value.store(value.load(std::memory_order_relaxed) + deferred_fallbacks,
                    std::memory_order_relaxed);
        deferred_fallbacks = 0;
    }
    // Device loss is the root cause of the completion failures and silence it
    // produces, so once loss is observed it replaces those latched symptoms.
    // Other causes stay first: they are independent of the device. A ring
    // saturated behind a hung provider is reported as the hang.
    detail::SharedIoRecoveryReason recovery() const noexcept {
        using Recovery = detail::SharedIoRecoveryReason;
        const auto latched = detail::saturated_projection(
            bridge->recovery_reason(), saturated_unresponsive.load(std::memory_order_acquire));
        if (device_lost.load(std::memory_order_acquire) &&
            (latched == Recovery::ProviderFailure || latched == Recovery::ProviderUnresponsive))
            return Recovery::ProviderLost;
        return latched;
    }
    std::uint64_t latched_sequence() const noexcept {
        if (bridge->recovery_reason() == detail::SharedIoRecoveryReason::InputSaturated &&
            saturated_unresponsive.load(std::memory_order_acquire))
            return unresponsive_sequence.load(std::memory_order_acquire);
        return bridge->recovery_sequence();
    }
    void latch_loss(std::uint64_t sequence) noexcept {
        device_lost.store(true, std::memory_order_release);
        fail(detail::SharedIoRecoveryReason::ProviderLost, sequence);
    }
    static detail::SharedIoGpuTerminalDisposition
    failed_terminal(detail::SharedIoTraceOutcome outcome) noexcept {
        return outcome == detail::SharedIoTraceOutcome::DeviceLost
                   ? detail::SharedIoGpuTerminalDisposition::DeviceLost
                   : detail::SharedIoGpuTerminalDisposition::ProviderFailed;
    }
    static detail::SharedIoFallbackReason
    failed_reason(detail::SharedIoTraceOutcome outcome) noexcept {
        switch (outcome) {
        case detail::SharedIoTraceOutcome::SubmissionRejected:
            return detail::SharedIoFallbackReason::SubmissionRejected;
        case detail::SharedIoTraceOutcome::DeviceLost:
            return detail::SharedIoFallbackReason::DeviceLost;
        default:
            return detail::SharedIoFallbackReason::CompletionFailed;
        }
    }
    detail::SharedIoFallbackReason recovery_trace_reason() const noexcept {
        using Recovery = detail::SharedIoRecoveryReason;
        using Reason = detail::SharedIoFallbackReason;
        switch (recovery()) {
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
        case Recovery::ProviderUnresponsive:
            // Same mapping as the bridge's trace record for this reason.
            return Reason::DeadlineExceeded;
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
                trace_terminal(pending, pending.outcome, failed_terminal(pending.outcome),
                               failed_reason(pending.outcome));
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
        // release() requires the callback and worker to be stopped and joined,
        // and this point is reached only after every channel released, so the
        // telemetry has no remaining producer and its final receipt can be
        // validated. A bounded number of drains covers all queues.
        for (int i = 0; i < 4; ++i)
            drain_trace(true);
        closed_trace_stats = trace->stats();
        if (bridge)
            bridge->set_trace(nullptr);
        trace.reset();
    }

    std::vector<GpuWaveNetLayerDescriptor> layers;
    std::vector<std::vector<std::uint32_t>> dilations;
    std::vector<float> weights;
    std::vector<std::unique_ptr<detail::WaveNetRealtimeChannel>> channels;
    // Channels whose provider could not drain within its budget during a
    // re-acquire. They stay owned, and release() retries them.
    std::vector<std::unique_ptr<detail::WaveNetRealtimeChannel>> quarantine;
    detail::WaveNetRealtimeTestAccess::ChannelFactory channel_factory = nullptr;
    void* channel_factory_context = nullptr;
    std::unique_ptr<Bridge> bridge;
    std::vector<float> callback_input, callback_output, worker_output;
    std::vector<Pending> pending_ring;
    Bridge::Callback callback;
    std::size_t pending_head = 0, pending_count = 0;
    // Completions dropped because an earlier generation admitted them.
    std::uint64_t stale_completions = 0;
    std::uint64_t next_input = 0, sequence = 0, epoch = 0;
    std::uint64_t all_channels_mask = 0;
    // The sequence after the last callback that has finished consuming its
    // output. Written by the callback once consumption is over, so a block
    // whose consumer is behind this position can no longer be delivered.
    std::atomic<std::uint64_t> callback_next{0};
    // Worker-only watchdog state, in callback blocks: the worker's pump rate is
    // a poll interval, not an audio clock.
    std::uint64_t watchdog_limit = 0, completion_mark = 0;
    // Input ring capacity in effect: the configured capacity, grown to hold
    // the default watchdog window (see size_provider_watchdog()).
    std::uint32_t ring_capacity = 0;
    // Worker only: completions collected since prepare(), and whether a
    // latched InputSaturated has been classified.
    std::uint64_t completions = 0;
    bool saturation_classified = false;
    // Written by the worker once: the input ring saturated during the silence
    // of an in-flight block that had seen no completion since its admission.
    // That is a hung provider, so the node reports ProviderUnresponsive at
    // that block.
    std::atomic<bool> saturated_unresponsive{false};
    std::atomic<std::uint64_t> unresponsive_sequence{std::numeric_limits<std::uint64_t>::max()};
    // The sequence after the last callback whose delivery was reported.
    std::atomic<std::uint64_t> delivered_next{0};
    // Worker only: provider failures awaiting their consumer's fallback.
    std::uint64_t deferred_fallbacks = 0, deferred_until = 0;
    // Written by the worker, read by status_snapshot() on a host thread. The
    // GpuDelivered count is written only by the callback.
    std::atomic<bool> device_lost{false};
    std::array<std::atomic<std::uint64_t>, GpuAudioStatus::kTerminalDispositionCount>
        terminal_counts{};
    // Re-acquire. A fresh provider's causal history is empty (or, for a
    // recycled one, stale), so the first `warmup_blocks` of an epoch opened by
    // a re-acquire are submitted only to fill it and are never delivered. The
    // worker learns the epoch's first sequence from its first ingress record.
    std::uint32_t warmup_blocks = 0;
    std::uint64_t warmup_until = 0;
    bool awaiting_epoch = false;
    // A provider borrowed warm from the pool carries another stream's history.
    bool warm_start = false;
    // First sequence the re-acquired epoch can deliver, or UINT64_MAX before
    // any re-acquire. Written by the worker, read by status_snapshot().
    std::atomic<std::uint64_t> reacquired_sequence{UINT64_MAX};
    // `epoch` for status readers on another thread; the worker may advance it.
    std::atomic<std::uint64_t> published_epoch{0};
    // Whether a full set of provider channels holds a prepared kernel, for
    // status readers. An offline fence or a failed re-acquire leaves none.
    std::atomic<bool> kernel_prepared{false};
    std::atomic<std::uint64_t> reacquire_attempts{0}, reacquire_successes{0};
    // Callback-written per delivered block, read by status_snapshot().
    detail::RecentGpuDelivery recent;
    // The transport's completion notifier, host or worker only. Every channel
    // in `channels` routes to it; a channel leaving them is cleared first, so a
    // parked or quarantined provider never wakes a transport it outlives.
    detail::SharedIoCompletionNotifier notifier{};
    bool prepared = false, production_provider = false;
    // Reported by the prepared provider itself; empty for injected channels.
    detail::SharedIoProviderIdentity provider_identity;
    bool fallback_prepared = false;

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
        if (config.max_inflight == 0)
            config.max_inflight =
                config.session.slots >= 2 && config.capacity >= config.lead_blocks + 2 ? 2 : 1;
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
        if (prepared || !valid_config(false) || channels.size() != c.channels ||
            (c.miss_policy == MissPolicy::CpuFallback && !fallback_prepared))
            return false;
        recent.reset();
        bridge = std::make_unique<Bridge>();
        const auto sizing = detail::size_provider_watchdog(
            c.capacity, c.lead_blocks, c.watchdog_blocks, c.session.descriptor.block_size,
            c.session.descriptor.sample_rate);
        ring_capacity = sizing.ring_blocks;
        if (!bridge->prepare({.capacity = ring_capacity,
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
        stale_completions = 0;
        all_channels_mask = c.channels == 64 ? UINT64_MAX : (UINT64_C(1) << c.channels) - 1;
        next_input = sequence;
        published_epoch.store(epoch, std::memory_order_release);
        callback_next.store(sequence, std::memory_order_relaxed);
        delivered_next.store(sequence, std::memory_order_relaxed);
        deferred_fallbacks = deferred_until = 0;
        watchdog_limit = sizing.window_blocks;
        completion_mark = sequence;
        completions = 0;
        saturation_classified = false;
        saturated_unresponsive.store(false, std::memory_order_relaxed);
        unresponsive_sequence.store(std::numeric_limits<std::uint64_t>::max(),
                                    std::memory_order_relaxed);
        warmup_blocks = history_warmup_blocks();
        warmup_until = warm_start ? sequence + warmup_blocks : sequence;
        warm_start = false;
        awaiting_epoch = false;
        reacquired_sequence.store(UINT64_MAX, std::memory_order_relaxed);
        reacquire_attempts.store(0, std::memory_order_relaxed);
        reacquire_successes.store(0, std::memory_order_relaxed);
        device_lost.store(false, std::memory_order_relaxed);
        for (auto& count : terminal_counts)
            count.store(0, std::memory_order_relaxed);
        telemetry.reset();
        if (trace_config.enabled) {
            if (!trace_engine)
                trace_engine = detail::next_shared_io_trace_engine_id();
            detail::SharedIoExecutionContract contract;
            contract.channels = c.channels;
            contract.block_size = c.session.descriptor.block_size;
            contract.sample_rate = c.session.descriptor.sample_rate;
            contract.algorithmic_lead_blocks = c.lead_blocks;
            contract.pipeline_depth = ring_capacity;
            contract.provider_slots = c.session.slots;
            contract.max_inflight = c.max_inflight;
            contract.requested_path = detail::SharedIoRequest::RequireSharedHostPointer;
            contract.active_path = detail::SharedIoPath::SharedHostPointer;
            contract.shared_host_pointer_capable = true;
            contract.miss_policy = c.miss_policy;
            contract.cpu_fallback_prepared = fallback_prepared;
            trace = std::make_unique<detail::SharedIoTraceRecorder>(
                detail::SharedIoTraceConfig{.engine_id = trace_engine,
                                            .generation = epoch,
                                            .contract = contract,
                                            .success_stride = trace_config.success_stride,
                                            .capture_admissions = trace_config.capture_admissions,
                                            .enabled = true});
            closed_trace_stats = {};
        }
        // Telemetry feeds the status projection even when detailed trace
        // recording is disabled. The recorder is optional; the counters are
        // part of the provider-neutral lifecycle contract.
        bridge->set_trace(trace.get(), &telemetry);
        prepared = true;
        kernel_prepared.store(true, std::memory_order_release);
        return true;
    }
    bool any_channel_lost() const noexcept {
        return std::any_of(channels.begin(), channels.end(),
                           [](const auto& channel) { return channel->lost(); });
    }
    // The block the provider will not deliver: the oldest one in flight, or the
    // next to be admitted when nothing is in flight.
    std::uint64_t first_undelivered() const noexcept {
        return pending_count != 0 ? pending_ring[pending_head].ticket.sequence : next_input;
    }
    std::uint32_t service_and_collect(std::uint64_t service_now, std::uint64_t deadline,
                                      bool* progressed = nullptr) noexcept {
        const auto n = config.session.descriptor.block_size;
        for (auto& channel : channels) {
            if (deadline != 0)
                channel->service_until(service_now, deadline);
            else
                channel->service(service_now);
        }
        // Device loss outranks the failed completions it causes, so it is
        // latched before any of this pump's results can latch ProviderFailure.
        const bool lost = any_channel_lost();
        if (lost)
            latch_loss(first_undelivered());
        const auto failure = lost ? detail::SharedIoRecoveryReason::ProviderLost
                                  : detail::SharedIoRecoveryReason::ProviderFailure;
        const auto failed_outcome = lost ? detail::SharedIoTraceOutcome::DeviceLost
                                         : detail::SharedIoTraceOutcome::CompletionFailed;
        for (std::size_t ch = 0; ch < channels.size(); ++ch) {
            std::size_t received = 0;
            while (received < pending_count) {
                auto completion = channels[ch]->receive({worker_output.data() + ch * n, n});
                if (!completion)
                    break;
                ++received;
                ++completions;
                if (progressed)
                    *progressed = true;
                const auto* result = &completion->result;
                // Only the ticket a block was admitted with can complete it.
                Pending* pending = nullptr;
                bool stale = false;
                for (std::size_t offset = 0; offset < pending_count; ++offset) {
                    auto& candidate = pending_ring[(pending_head + offset) % pending_ring.size()];
                    if (!candidate.occupied || candidate.ticket.sequence != result->sequence)
                        continue;
                    if (completion->ticket.admits(candidate.ticket.generation,
                                                  candidate.ticket.sequence))
                        pending = &candidate;
                    else
                        stale = completion->ticket.accepted() &&
                                completion->ticket.generation != candidate.ticket.generation;
                    break;
                }
                if (pending == nullptr) {
                    // A ticket from another generation names a block this
                    // admission never issued: drop it, it is not a provider
                    // fault. An unattributable completion still fails.
                    if (stale) {
                        ++stale_completions;
                        continue;
                    }
                    fail(failure, result->sequence);
                    continue;
                }
                const auto bit = UINT64_C(1) << ch;
                if ((pending->completed_mask & bit) != 0) {
                    pending->outcome = detail::SharedIoTraceOutcome::CompletionFailed;
                    fail(failure, result->sequence);
                    continue;
                }
                pending->completed_mask |= bit;
                pending->suppress = pending->suppress || result->late;
                if (result->status != GpuWaveNetBlockStatus::GpuDelivered) {
                    if (pending->outcome == detail::SharedIoTraceOutcome::Success)
                        pending->outcome = failed_outcome;
                    fail(failure, result->sequence);
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
            // Late means the callback that would consume this block has
            // already finished; wall-clock time plays no part. A stale block
            // keeps its recovery cause instead.
            const bool consumer_passed = callback_next.load(std::memory_order_acquire) >
                                         pending.ticket.sequence + config.lead_blocks;
            const bool current = bridge->delivery_epoch() == pending.ticket.generation;
            if (pending.outcome != detail::SharedIoTraceOutcome::Success) {
                trace_terminal(pending, pending.outcome, failed_terminal(pending.outcome),
                               failed_reason(pending.outcome), true);
            } else if (current && pending.ticket.sequence < warmup_until) {
                // History warmup after a re-acquire: computed, never delivered.
                trace_terminal(pending, detail::SharedIoTraceOutcome::Cancelled,
                               detail::SharedIoGpuTerminalDisposition::CancelledTeardown,
                               detail::SharedIoFallbackReason::None, true);
            } else if (pending.suppress || (current && consumer_passed)) {
                trace_terminal(pending, detail::SharedIoTraceOutcome::LateRejected,
                               detail::SharedIoGpuTerminalDisposition::LateRejected,
                               detail::SharedIoFallbackReason::DeadlineExceeded, true);
            } else if (!current) {
                trace_terminal(pending, detail::SharedIoTraceOutcome::StaleRejected,
                               detail::SharedIoGpuTerminalDisposition::StaleRejected,
                               recovery_trace_reason(), true);
            } else {
                const auto publication = bridge->publish_output(pending.stamp(), pending.output);
                produced += publication == Bridge::Publication::Published ? 1 : 0;
                if (publication != Bridge::Publication::Published) {
                    trace_terminal(pending, detail::SharedIoTraceOutcome::LateRejected,
                                   detail::SharedIoGpuTerminalDisposition::LateRejected,
                                   detail::SharedIoFallbackReason::InputSaturated, true);
                    fail(detail::SharedIoRecoveryReason::InputSaturated, pending.ticket.sequence);
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
    // A provider is unresponsive when it completes nothing for `watchdog_limit`
    // callback blocks while work is in flight. Silence is counted from the
    // later of the last completion and the oldest in-flight admission, so an
    // idle stretch with nothing in flight never counts against it. A provider
    // that keeps completing, however late, is slow, not unresponsive.
    void watch_provider(bool progressed) noexcept {
        classify_saturation();
        const auto position = callback_next.load(std::memory_order_acquire);
        if (progressed) {
            completion_mark = position;
            return;
        }
        if (pending_count == 0 || bridge->delivery_epoch() == 0)
            return;
        // The oldest in-flight block was admitted after its own callback.
        const auto since = std::max(completion_mark, first_undelivered() + 1);
        // The callback publishes its record before advancing the position, so
        // a preempted callback can leave the position behind; never wrap.
        const auto silent = position > since ? position - since : 0;
        if (silent >= watchdog_limit)
            fail(detail::SharedIoRecoveryReason::ProviderUnresponsive, first_undelivered());
    }
    // The input ring fills when the worker stops admitting. If it filled during
    // the silence of an in-flight block that has seen no completion since its
    // admission, the provider, not the worker, stopped the stream: report the
    // hang. Runs after this pump's completions are collected, so a block that
    // completed late keeps the saturation as reported.
    void classify_saturation() noexcept {
        if (saturation_classified ||
            bridge->recovery_reason() != detail::SharedIoRecoveryReason::InputSaturated)
            return;
        saturation_classified = true;
        if (pending_count == 0 || completions != pending_ring[pending_head].completions_at_admit)
            return;
        // The ring must have filled during this block's silence. A provider
        // that completes every block, only slower than the callback, fills it
        // from a backlog while each block's own silence stays short: that is
        // saturation, not a hang.
        const auto position = callback_next.load(std::memory_order_acquire);
        const auto since = std::max(completion_mark, first_undelivered() + 1);
        const auto silent = position > since ? position - since : 0;
        if (silent + config.lead_blocks + 1 < ring_capacity)
            return;
        unresponsive_sequence.store(first_undelivered(), std::memory_order_release);
        saturated_unresponsive.store(true, std::memory_order_release);
    }
    // Blocks of input that fully refill every layer's causal history.
    std::uint32_t history_warmup_blocks() const noexcept {
        std::uint64_t reach = 0;
        for (const auto& layer : layers) {
            const auto kernel_reach = static_cast<std::uint64_t>(layer.kernel - 1u);
            if (layer.dilations.empty())
                reach += kernel_reach * layer.dilation;
            for (const auto dilation : layer.dilations)
                reach += kernel_reach * dilation;
        }
        const auto block = config.session.descriptor.block_size;
        const auto blocks = (reach + block - 1) / block;
        return blocks > UINT32_MAX ? UINT32_MAX : static_cast<std::uint32_t>(blocks);
    }
    std::unique_ptr<detail::WaveNetRealtimeChannel> make_channel(std::uint64_t budget_ns) noexcept;
    // Keeps a healthy channel warm in the configured pool instead of
    // releasing it; on success `channel` is left empty. `budget_ns` bounds the
    // recycle drain (zero: the provider's default).
    bool park(std::unique_ptr<detail::WaveNetRealtimeChannel>& channel,
              std::uint64_t budget_ns) noexcept {
        if (config.provider_pool == nullptr || !channel || channel->lost())
            return false;
        (void)channel->set_completion_notifier({});
        if (!channel->recycle(budget_ns))
            return false;
        try {
            return config.provider_pool->give(config.session, channel->provider_key(), channel);
        } catch (...) {
            return false;
        }
    }
    // A warm channel for this program, or null. Providers the pool finds lost
    // on the way release within `budget_ns`.
    std::unique_ptr<detail::WaveNetRealtimeChannel> borrow(std::uint64_t budget_ns) noexcept {
        if (config.provider_pool == nullptr)
            return nullptr;
        try {
            return config.provider_pool->acquire(
                config.session, detail::GpuWaveNetSessionPoolAccess::requested_provider_key(),
                budget_ns);
        } catch (...) {
            return nullptr;
        }
    }
    // Worker only, fenced, callback running. Replaces the provider and opens a
    // new epoch the callback adopts at its next block. Old channels release
    // within `budget_ns`; one that cannot is quarantined, never destroyed.
    bool reacquire(std::uint64_t budget_ns) noexcept {
        // A fault can fence the previous re-acquire's provider before the
        // callback adopts its epoch; withdraw that request and replace it.
        if (!prepared || !bridge || bridge->delivery_epoch() != 0 ||
            bridge->recovery_reason() == detail::SharedIoRecoveryReason::OfflineFence ||
            epoch == UINT64_MAX || !bridge->cancel_requested_epoch())
            return false;
        reacquire_attempts.fetch_add(1, std::memory_order_relaxed);
        // The new epoch clears the latched cause; keep it in the trace.
        const auto cause = recovery_trace_reason();
        // Nothing in flight can retire into the new epoch: the old provider is
        // being released, so close every admitted block now.
        close_pending_trace();
        for (auto& pending : pending_ring)
            pending.reset();
        pending_head = pending_count = 0;
        settle_fallbacks();
        // Reserve before releasing anything: nothing below may throw.
        std::vector<std::unique_ptr<detail::WaveNetRealtimeChannel>> fresh;
        try {
            fresh.reserve(config.channels);
            quarantine.reserve(quarantine.size() + channels.size() + 2u * config.channels);
        } catch (...) {
            return false;
        }
        const auto deadline = budget_ns > UINT64_MAX - now_ns() ? UINT64_MAX : now_ns() + budget_ns;
        // Never zero: a provider reads a zero budget as "use the default".
        const auto remaining = [&] {
            const auto now = now_ns();
            return now >= deadline ? std::uint64_t{1} : deadline - now;
        };
        kernel_prepared.store(false, std::memory_order_release);
        for (auto& channel : channels) {
            (void)channel->set_completion_notifier({});
            if (!channel->release_bounded(remaining()))
                quarantine.push_back(std::move(channel));
        }
        channels.clear();
        for (std::uint32_t ch = 0; ch < config.channels; ++ch) {
            auto channel = make_channel(remaining());
            if (!channel) {
                for (auto& created : fresh)
                    if (!created->release_bounded(remaining()))
                        quarantine.push_back(std::move(created));
                return false;
            }
            fresh.push_back(std::move(channel));
        }
        channels = std::move(fresh);
        kernel_prepared.store(true, std::memory_order_release);
        // The replacement routes its completions where the old provider did. A
        // replacement without an event source leaves the transport on its
        // one-block fallback poll.
        if (notifier.notify != nullptr && !install_notifier(notifier))
            notifier = {};
        // An input stall leaves the ring full of old-epoch records that
        // nothing else retires; the new epoch's first block needs the room.
        while (const auto stale = bridge->acquire_input())
            (void)bridge->release_input(*stale);
        if (!bridge->request_epoch(epoch + 1))
            return false;
        // request_epoch() cleared the latched cause, so the old epoch's hang
        // classification must not project a saturation the new epoch latches,
        // even one that lands before the worker admits its first block.
        saturation_classified = false;
        saturated_unresponsive.store(false, std::memory_order_release);
        unresponsive_sequence.store(std::numeric_limits<std::uint64_t>::max(),
                                    std::memory_order_release);
        if (trace) {
            detail::SharedIoTraceRecord record;
            record.kind = detail::SharedIoTraceKind::Recovery;
            record.generation = epoch;
            record.next_generation = epoch + 1;
            record.sequence = callback_next.load(std::memory_order_acquire);
            record.reason = record.gpu_reason = cause;
            (void)trace->publish_worker(record);
        }
        // The previous re-acquire's first deliverable block belongs to the
        // epoch being replaced; the new one is known only once it is adopted.
        reacquired_sequence.store(UINT64_MAX, std::memory_order_release);
        ++epoch;
        published_epoch.store(epoch, std::memory_order_release);
        reacquire_successes.fetch_add(1, std::memory_order_relaxed);
        awaiting_epoch = true;
        // The window described the provider just replaced.
        recent.reset();
        device_lost.store(false, std::memory_order_release);
        completion_mark = callback_next.load(std::memory_order_acquire);
        return true;
    }
    void fail(detail::SharedIoRecoveryReason reason,
              std::uint64_t sequence = std::numeric_limits<std::uint64_t>::max()) noexcept {
        bridge->request_recovery(reason, sequence);
    }
    // All or nothing: one channel without an event source would otherwise
    // lose its completions to the transport's longer event-driven fallback.
    bool install_notifier(detail::SharedIoCompletionNotifier installed) noexcept {
        for (std::size_t i = 0; i < channels.size(); ++i) {
            if (!channels[i]->set_completion_notifier(installed)) {
                for (std::size_t j = 0; j < i; ++j)
                    (void)channels[j]->set_completion_notifier({});
                return false;
            }
        }
        return !channels.empty();
    }
};

std::unique_ptr<detail::WaveNetRealtimeChannel>
GpuWaveNetRealtimeNode::Impl::make_channel(std::uint64_t budget_ns) noexcept {
    if (auto warm = borrow(budget_ns))
        return warm;
    if (channel_factory != nullptr)
        return channel_factory(channel_factory_context);
    try {
        auto result = GpuWaveNetSession::create(config.session);
        if (!result) {
            // A partially prepared session may still hold provider memory.
            if (result.session && !result.session->release())
                quarantine.push_back(std::make_unique<detail::GpuWaveNetSessionChannel<>>(
                    std::move(result.session), config.session.slots));
            return nullptr;
        }
        return std::make_unique<detail::GpuWaveNetSessionChannel<>>(std::move(result.session),
                                                                    config.session.slots);
    } catch (...) {
        return nullptr;
    }
}

GpuWaveNetRealtimeNode::GpuWaveNetRealtimeNode(const Config& config)
    : impl_(std::make_unique<Impl>(config)) {}
GpuWaveNetRealtimeNode::~GpuWaveNetRealtimeNode() {
    if (!release() && impl_->trace) {
        // Callers are quiescent. Preserve real records, but do not manufacture
        // retirement for channels whose physical release was not established.
        // Unresolved channels cannot update telemetry: only the stopped worker
        // collects their completions.
        for (int i = 0; i < 4; ++i)
            impl_->drain_trace(true);
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
            // Host thread: the pool's own release budget applies.
            if (auto warm = s.borrow(0)) {
                s.warm_start = true;
                s.channels.push_back(std::move(warm));
                continue;
            }
            auto result = GpuWaveNetSession::create(s.config.session);
            if (!result) {
                // Retain a partially prepared session if its physical drain needs retry.
                if (result.session)
                    s.channels.push_back(std::make_unique<detail::GpuWaveNetSessionChannel<>>(
                        std::move(result.session), s.config.session.slots));
                (void)release();
                return false;
            }
            s.channels.push_back(std::make_unique<detail::GpuWaveNetSessionChannel<>>(
                std::move(result.session), s.config.session.slots));
        }
        // Silence warmup is explicit and stopped-host-only. Never auto-reprime a
        // failed live history with silence. All channels begin at the same block.
        std::vector<float> zero(s.config.session.descriptor.block_size, 0.f), output(zero.size());
        for (std::uint32_t block = 0; block < s.config.prewarm_blocks; ++block) {
            for (auto& channel : s.channels) {
                // Warm-up runs before any delivery epoch opens, so it is admitted
                // under generation 0, which no delivery can ever carry.
                if (!channel->submit(zero, detail::admit_submission(0, s.sequence + block))) {
                    (void)release();
                    return false;
                }
                const auto deadline = now_ns() + 2'000'000'000ull;
                bool received = false;
                while (now_ns() < deadline) {
                    channel->service(now_ns());
                    if (auto completion = channel->receive(output)) {
                        received = completion->ticket.admits(0, s.sequence + block) &&
                                   completion->result.status == GpuWaveNetBlockStatus::GpuDelivered;
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
        s.fallback_prepared = cpu_fallback_prepared();
        if (s.initialize()) {
            s.production_provider = true;
            s.provider_identity = s.channels.front()->provider_identity();
            return true;
        }
    } catch (...) {
    }
    (void)release();
    return false;
}
bool GpuWaveNetRealtimeNode::release() noexcept {
    return release_bounded(UINT64_MAX);
}
bool GpuWaveNetRealtimeNode::release_bounded(std::uint64_t budget_ns) noexcept {
    auto& s = *impl_;
    s.prepared = false;
    s.kernel_prepared.store(false, std::memory_order_release);
    s.production_provider = false;
    s.provider_identity = {};
    s.fallback_prepared = false;
    if (s.bridge) {
        s.sequence = s.bridge->next_sequence();
        s.bridge->request_recovery(detail::SharedIoRecoveryReason::OfflineFence);
    }
    // UINT64_MAX keeps each channel's own unbounded release.
    const bool bounded = budget_ns != UINT64_MAX;
    const auto deadline =
        !bounded || budget_ns > UINT64_MAX - now_ns() ? UINT64_MAX : now_ns() + budget_ns;
    // Never zero once bounded: a provider reads zero as "use the default".
    const auto remaining = [&] {
        const auto now = now_ns();
        return now >= deadline ? std::uint64_t{1} : deadline - now;
    };
    const auto release_one = [&](detail::WaveNetRealtimeChannel& channel) {
        return bounded ? channel.release_bounded(remaining()) : channel.release();
    };
    s.unresolved_channel_count = 0;
    // A parked channel's session now belongs to the pool; drop the channel.
    std::erase_if(s.channels, [&](auto& channel) {
        if (s.park(channel, bounded ? remaining() : 0))
            return true;
        if (!release_one(*channel))
            ++s.unresolved_channel_count;
        return false;
    });
    std::erase_if(s.quarantine, [&](auto& channel) { return release_one(*channel); });
    s.unresolved_channel_count += static_cast<std::uint32_t>(s.quarantine.size());
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
    return detail::public_recovery_reason(impl_->recovery());
}
std::uint64_t GpuWaveNetRealtimeNode::recovery_sequence() const noexcept {
    return impl_->bridge ? impl_->latched_sequence() : std::numeric_limits<std::uint64_t>::max();
}
GpuAudioStatus GpuWaveNetRealtimeNode::status_snapshot() const noexcept {
    GpuAudioStatus status;
    if (!impl_->prepared) {
        status.provider_state = GpuAudioProviderState::Uninitialized;
        return status;
    }
    const auto telemetry = impl_->telemetry.snapshot();
    const auto& descriptor = impl_->config.session.descriptor;
    status.requested_engine = GpuAudioEngine::Gpu;
    status.fallback_policy = impl_->config.miss_policy;
    status.sample_rate = descriptor.sample_rate;
    status.block_size = descriptor.block_size;
    status.latency_samples = impl_->config.lead_blocks * descriptor.block_size;
    status.prepared_lead_blocks = impl_->config.lead_blocks;
    status.fallback_available = impl_->fallback_prepared;
    status.produced_blocks = telemetry.delivered_blocks;
    status.missed_blocks = telemetry.deadline_misses;
    status.fallback_blocks = telemetry.fallback_blocks;
    status.resynced_blocks = telemetry.resync_drops;
    status.input_dropped_frames = telemetry.input_drops;
    status.last_block_us = static_cast<double>(telemetry.submit_to_completion_ns) / 1000.0;
    // The private telemetry snapshot exposes a latest sample, not an average;
    // leave the aggregate average unavailable rather than relabeling a GPU
    // timestamp as a worker-time mean.
    status.avg_block_us = 0.0;
    status.provider = authenticated_provider() ? GpuAudioProvider::Dawn : GpuAudioProvider::Unknown;
    status.recovery_reason = recovery_reason();
    status.recovery_sequence = recovery_sequence();
    // A re-acquire clears the latched cause; report it, at the first block
    // the new provider can deliver, until something else latches.
    if (const auto reacquired = impl_->reacquired_sequence.load(std::memory_order_acquire);
        status.recovery_reason == GpuAudioRecoveryReason::None && reacquired != UINT64_MAX) {
        status.recovery_reason = GpuAudioRecoveryReason::ProviderReacquired;
        status.recovery_sequence = reacquired;
    }
    status.diagnostics_available = true;
    status.epoch = impl_->published_epoch.load(std::memory_order_acquire);
    status.reacquire_attempts = impl_->reacquire_attempts.load(std::memory_order_relaxed);
    status.reacquire_successes = impl_->reacquire_successes.load(std::memory_order_relaxed);
    // One read serves both the field and the fence term below: a callback
    // adopting a re-acquired epoch between two reads would report Ready with
    // no delivery epoch, which the validator rejects.
    status.delivery_epoch_active = impl_->bridge && impl_->bridge->delivery_epoch() != 0;
    for (std::size_t i = 0; i < impl_->terminal_counts.size(); ++i)
        status.terminal_counts[i] = impl_->terminal_counts[i].load(std::memory_order_relaxed);
    switch (impl_->config.session.completion_policy) {
    case GpuWaveNetCompletionPolicy::ProcessEvents:
        status.completion_mode = GpuAudioCompletionMode::ProcessEvents;
        break;
    case GpuWaveNetCompletionPolicy::WaitAny:
        status.completion_mode = GpuAudioCompletionMode::WaitAny;
        break;
    case GpuWaveNetCompletionPolicy::TimedWaitAny:
        status.completion_mode = GpuAudioCompletionMode::TimedWaitAny;
        break;
    }
    // The WaveNet provider path records no GPU timestamps.
    status.timestamps_available = false;
    status.gpu_kernel_available = impl_->kernel_prepared.load(std::memory_order_acquire);
    // A lost device or an unresponsive provider needs a new provider before
    // the GPU can deliver again, whatever cause latched first; every other
    // recovery reason leaves it recoverable. The engine is the GPU only while
    // it delivers; otherwise the prepared CPU shadow, or none when silent.
    const auto projection = detail::project_node_state(
        impl_->device_lost.load(std::memory_order_acquire), status.recovery_reason,
        authenticated_provider() && status.delivery_epoch_active, status.fallback_policy,
        status.fallback_available, impl_->recent.lagging());
    status.provider_state = projection.provider_state;
    status.selected_engine = projection.selected_engine;
    return status;
}
bool GpuWaveNetRealtimeNode::ready() const noexcept {
    return impl_->prepared;
}
bool GpuWaveNetRealtimeNode::authenticated_provider() const noexcept {
    return impl_->prepared && impl_->production_provider;
}
bool GpuWaveNetRealtimeNode::bind_realtime_path(detail::RealtimeGpuNodeBinding& binding) noexcept {
    binding.requires_realtime_path = true;
    binding.identity_context = this;
    binding.identity = [](const void* self) noexcept {
        const auto* node = static_cast<const GpuWaveNetRealtimeNode*>(self);
        return node->authenticated_provider() ? node->impl_->provider_identity
                                              : detail::SharedIoProviderIdentity{};
    };
    if (ready()) {
        binding.path = {.context = this,
                        .process = &GpuWaveNetRealtimeNode::process,
                        .service = &GpuWaveNetRealtimeNode::service,
                        .fence = &GpuWaveNetRealtimeNode::fence,
                        .delivered = &GpuWaveNetRealtimeNode::delivered,
                        .next_sequence = &GpuWaveNetRealtimeNode::next_sequence,
                        // Serialized non-RT read; the status projection is
                        // allocation-free and carries the first latched cause.
                        .health =
                            [](const void* self, detail::RealtimeGpuHealth& out) noexcept {
                                const auto* node = static_cast<const GpuWaveNetRealtimeNode*>(self);
                                // Fence from the snapshot's own epoch read, not a
                                // second one that could see a later adoption.
                                const auto status = node->status_snapshot();
                                out = detail::health_from_status(status);
                                out.fenced = out.fenced || !status.delivery_epoch_active;
                                return true;
                            },
                        .reprepare = &GpuWaveNetRealtimeNode::reprepare,
                        .set_completion_notifier = &GpuWaveNetRealtimeNode::set_notifier};
    }
    return true;
}
bool GpuWaveNetRealtimeNode::reprepare(void* self, std::uint64_t budget_ns) noexcept {
    return static_cast<GpuWaveNetRealtimeNode*>(self)->impl_->reacquire(budget_ns);
}
bool GpuWaveNetRealtimeNode::set_notifier(void* self,
                                          detail::SharedIoCompletionNotifier notifier) noexcept {
    auto& s = *static_cast<GpuWaveNetRealtimeNode*>(self)->impl_;
    if (notifier.notify == nullptr) {
        for (auto& channel : s.channels)
            (void)channel->set_completion_notifier({});
        s.notifier = {};
        return true;
    }
    if (!s.install_notifier(notifier))
        return false;
    s.notifier = notifier;
    return true;
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
        s.fail(detail::SharedIoRecoveryReason::InvalidCallback, sequence);
        std::fill(s.callback_input.begin(), s.callback_input.end(), 0.f);
    } else {
        for (std::uint32_t ch = 0; ch < s.config.channels; ++ch)
            std::copy_n(input.channel_ptr(ch), n, s.callback_input.data() + ch * n);
    }
    s.callback = s.bridge->begin_callback(s.callback_input, sequence, start);
    const auto delivery = s.bridge->consume_output(s.callback, s.callback_output, nullptr, true);
    s.callback_next.store(s.bridge->next_sequence(), std::memory_order_release);
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
    bool progressed = false;
    s.settle_fallbacks();
    auto produced = s.service_and_collect(pump_now, deadline, &progressed);
    s.watch_provider(progressed);
    if (!s.bridge->begin_worker_admission())
        return produced;
    bool all_submitted = false;
    while (s.pending_count < s.pending_ring.size()) {
        auto input = s.bridge->acquire_input();
        if (!input)
            break;
        // Records the callback queued before the epoch was replaced belong to
        // no admission; discard them.
        if (input->stamp().epoch < s.epoch) {
            (void)s.bridge->release_input(*input);
            continue;
        }
        if (s.awaiting_epoch && input->stamp().epoch == s.epoch) {
            s.awaiting_epoch = false;
            s.next_input = input->stamp().sequence;
            s.warmup_until = s.next_input + s.warmup_blocks;
            s.reacquired_sequence.store(s.warmup_until, std::memory_order_release);
        }
        auto& pending = s.pending_ring[(s.pending_head + s.pending_count) % s.pending_ring.size()];
        pending.reset();
        pending.occupied = true;
        pending.completions_at_admit = s.completions;
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
        pending.ticket = detail::admit_submission(stamp.epoch, stamp.sequence);
        s.trace_stage(pending, detail::SharedIoTraceStage::EncodeBegin);
        // WaveNet's provider path does not expose a separate command encoder.
        // The channel submit boundary is the authenticated host submission
        // region, so keep encode and submit timestamps distinct without
        // claiming either is a GPU timestamp.
        s.trace_stage(pending, detail::SharedIoTraceStage::EncodeEnd);
        s.trace_stage(pending, detail::SharedIoTraceStage::SubmitBegin);
        bool submission_ok = true;
        for (std::size_t ch = 0; ch < s.channels.size(); ++ch) {
            if (!s.channels[ch]->submit(input->samples().subspan(ch * n, n), pending.ticket)) {
                submission_ok = false;
                pending.completed_mask |= UINT64_C(1) << ch;
                const bool lost = s.channels[ch]->lost();
                pending.outcome = lost ? detail::SharedIoTraceOutcome::DeviceLost
                                       : detail::SharedIoTraceOutcome::SubmissionRejected;
                if (lost)
                    s.latch_loss(stamp.sequence);
                else
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
    if (deadline != 0 && all_submitted) {
        // A completion seen only in this tail pass is progress too: the next
        // pump's watchdog check must count silence from it.
        progressed = false;
        produced += s.service_and_collect(now_ns(), deadline, &progressed);
        if (progressed)
            s.watch_provider(true);
    }
    return produced;
}

bool GpuWaveNetRealtimeNode::fence(void* self) noexcept {
    auto& s = *static_cast<GpuWaveNetRealtimeNode*>(self)->impl_;
    if (!s.bridge)
        return true;
    s.fail(detail::SharedIoRecoveryReason::OfflineFence, s.first_undelivered());
    // Every channel is released below; even one that cannot drain no longer
    // holds a usable kernel.
    s.kernel_prepared.store(false, std::memory_order_release);
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
    if (s.callback.stamp.sequence != sequence)
        return;
    const auto actual = static_cast<detail::SharedIoDeliveryDisposition>(disposition);
    s.recent.record(actual);
    // The bridge records this delivery in telemetry before it is counted here.
    if (s.bridge->complete_callback_delivery(s.callback, actual, end, visible) &&
        actual == detail::SharedIoDeliveryDisposition::GpuDelivered)
        s.count(GpuAudioTerminalDisposition::GpuDelivered);
    s.delivered_next.store(sequence + 1, std::memory_order_release);
}
void detail::WaveNetRealtimeTestAccess::request_recovery(GpuWaveNetRealtimeNode& node,
                                                         SharedIoRecoveryReason reason,
                                                         std::uint64_t sequence) noexcept {
    // A latched cause always names a block; default to the next one due.
    if (sequence == std::numeric_limits<std::uint64_t>::max())
        sequence = node.impl_->bridge->next_sequence();
    node.impl_->bridge->request_recovery(reason, sequence);
}
detail::SharedIoRecoveryReason
detail::WaveNetRealtimeTestAccess::recovery_reason(const GpuWaveNetRealtimeNode& node) noexcept {
    return node.impl_->bridge ? node.impl_->recovery() : SharedIoRecoveryReason::None;
}
std::uint64_t
detail::WaveNetRealtimeTestAccess::recovery_sequence(const GpuWaveNetRealtimeNode& node) noexcept {
    return node.impl_->bridge ? node.impl_->latched_sequence()
                              : std::numeric_limits<std::uint64_t>::max();
}
void detail::WaveNetRealtimeTestAccess::hold_callback_position(GpuWaveNetRealtimeNode& node,
                                                               std::uint64_t position) noexcept {
    node.impl_->callback_next.store(position, std::memory_order_release);
}
std::uint64_t
detail::WaveNetRealtimeTestAccess::stale_completions(const GpuWaveNetRealtimeNode& node) noexcept {
    return node.impl_->stale_completions;
}
void detail::WaveNetRealtimeTestAccess::observe_trace(
    GpuWaveNetRealtimeNode& node, SharedIoTraceDrainObserver observer) noexcept {
    node.impl_->trace_observer =
        valid_shared_io_trace_drain_observer(&observer) ? observer : SharedIoTraceDrainObserver{};
}
detail::SharedIoTraceStats
detail::WaveNetRealtimeTestAccess::trace_stats(const GpuWaveNetRealtimeNode& node) noexcept {
    return node.impl_->trace ? node.impl_->trace->stats() : node.impl_->closed_trace_stats;
}
detail::SharedIoTelemetrySnapshot
detail::WaveNetRealtimeTestAccess::telemetry(const GpuWaveNetRealtimeNode& node) noexcept {
    return node.impl_->telemetry.snapshot();
}
bool detail::WaveNetRealtimeTestAccess::gpu_lagging(const GpuWaveNetRealtimeNode& node) noexcept {
    return node.impl_->recent.lagging();
}
void detail::WaveNetRealtimeTestAccess::assume_authenticated(
    GpuWaveNetRealtimeNode& node) noexcept {
    node.impl_->production_provider = true;
}
detail::SharedIoTraceDrainResult
detail::WaveNetRealtimeTestAccess::last_trace_drain(const GpuWaveNetRealtimeNode& node) noexcept {
    return node.impl_->last_trace_drain;
}
detail::SharedIoTraceRecord
detail::WaveNetRealtimeTestAccess::last_terminal(const GpuWaveNetRealtimeNode& node) noexcept {
    return node.impl_->trace_record;
}
std::uint64_t
detail::WaveNetRealtimeTestAccess::trace_engine(const GpuWaveNetRealtimeNode& node) noexcept {
    return node.impl_->trace_engine;
}

void detail::WaveNetRealtimeTestAccess::set_channel_factory(GpuWaveNetRealtimeNode& node,
                                                            ChannelFactory factory,
                                                            void* context) noexcept {
    node.impl_->channel_factory = factory;
    node.impl_->channel_factory_context = context;
}
std::size_t detail::WaveNetRealtimeTestAccess::quarantined_channels(
    const GpuWaveNetRealtimeNode& node) noexcept {
    return node.impl_->quarantine.size();
}
std::uint32_t
detail::WaveNetRealtimeTestAccess::warmup_blocks(const GpuWaveNetRealtimeNode& node) noexcept {
    return node.impl_->history_warmup_blocks();
}
void detail::WaveNetRealtimeTestAccess::quarantine_for_testing(
    GpuWaveNetRealtimeNode& node, std::unique_ptr<WaveNetRealtimeChannel> channel) {
    node.impl_->quarantine.push_back(std::move(channel));
}
bool detail::WaveNetRealtimeTestAccess::prepare(
    GpuWaveNetRealtimeNode& node, std::vector<std::unique_ptr<WaveNetRealtimeChannel>> channels,
    std::uint64_t first_sequence) {
    if (!node.release())
        return false;
    node.impl_->sequence = first_sequence;
    if (!node.impl_->valid_config())
        return false;
    node.impl_->channels = std::move(channels);
    node.impl_->fallback_prepared = node.cpu_fallback_prepared();
    return node.impl_->initialize();
}
} // namespace pulp::gpu_audio
