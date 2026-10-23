#include <pulp/gpu_audio/gpu_audio_transport.hpp>

#include "detail/gpu_audio_transport_trial_observer.hpp"
#include "detail/realtime_gpu_audio_path.hpp"
#include "detail/worker_pipeline_hosting.hpp"
#include "detail/realtime_gpu_health_mirror.hpp"
#include "detail/shared_io_trace.hpp"
#include "detail/worker_wake.hpp"

#include <algorithm>
#include <chrono>

#if defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#endif

namespace pulp::gpu_audio {
namespace {
// Each failed re-acquire attempt waits this many times longer than the last.
constexpr std::uint64_t kReacquireBackoffGrowth = 5;

std::uint64_t monotonic_now_ns() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}
} // namespace

void GpuAudioTransport::publish_trial_delivery(std::uint64_t sequence, std::uint8_t disposition,
                                               std::uint64_t callback_start_ns,
                                               std::uint64_t callback_end_ns,
                                               std::uint64_t result_visible_ns) noexcept {
    if (trial_observer_ != nullptr)
        trial_observer_(trial_observer_context_, sequence, disposition, callback_start_ns,
                        callback_end_ns, result_visible_ns);
}

bool configure_gpu_audio_transport_trial_observer(
    GpuAudioTransport& transport, void* context,
    GpuAudioTransportTrialDeliveryFn observer) noexcept {
    if (transport.trial_observer_ != nullptr || context == nullptr || observer == nullptr)
        return false;
    transport.trial_observer_context_ = context;
    transport.trial_observer_ = observer;
    return true;
}

namespace detail {

// Everything capability_report() and status_snapshot() need from a prepared
// node, copied at prepare() so neither read touches the node again.
struct GpuAudioTransportConfiguration {
    bool prepared = false;
    bool realtime_health = false;
    bool staged_health = false;
    GpuAudioProvider provider = GpuAudioProvider::Unknown;
    MissPolicy miss_policy = MissPolicy::Silence;
    std::uint32_t sample_rate = 0;
    std::uint32_t block_size = 0;
    std::uint32_t latency_blocks = 0;
    GpuAudioRecoveryPolicy recovery_policy = GpuAudioRecoveryPolicy::Manual;
};

struct GpuAudioTransportDiagnostics {
    SeqlockCell<GpuAudioTransportConfiguration> configuration;
    RealtimeGpuHealthMirror health;
    // Host-written at prepare/release, callback-read: the prepared node's
    // primary path is its CPU model, so a CPU-delivered block is not a miss.
    bool cpu_primary = false;
    // Host-only, prepare/release: the worker-pipelining link a hosted node
    // bound at prepare(). release() undoes exactly that opt-in through the
    // link, which the node detaches if it is destroyed first. Null when the
    // node does not pipeline.
    std::shared_ptr<WorkerPipelineLink> pipeline;
};

} // namespace detail

GpuAudioTransport::GpuAudioTransport()
    : wake_(std::make_unique<detail::WorkerWake>()),
      diagnostics_(std::make_unique<detail::GpuAudioTransportDiagnostics>()) {
    (void)diagnostics_->configuration.publish({});
}

namespace {
// Wake posts one worker iteration consumes at most before it pumps again.
constexpr int kMaxCoalescedWakes = 1024;
} // namespace

void GpuAudioTransport::notify_worker(void* self) noexcept {
    // Provider callback thread: a counting-semaphore post only.
    static_cast<GpuAudioTransport*>(self)->wake_->post();
}

GpuAudioTransport::~GpuAudioTransport() {
    release();
}

void GpuAudioTransport::refresh_node_health() noexcept {
    detail::RealtimeGpuHealth health;
    if (realtime_gpu_health_ != nullptr) {
        if (realtime_gpu_health_(realtime_gpu_context_, health))
            (void)diagnostics_->health.publish(health);
        return;
    }
    if (!sample_staged_health_ || node_ == nullptr)
        return;
    // The base default (Uninitialized) carries no health to publish.
    const auto status = node_->status_snapshot();
    if (status.provider_state != GpuAudioProviderState::Uninitialized)
        (void)diagnostics_->health.publish(detail::health_from_status(status));
}

void GpuAudioTransport::steward_reacquire() noexcept {
    if (recovery_policy_ != GpuAudioRecoveryPolicy::AutoReacquire ||
        realtime_gpu_reprepare_ == nullptr || realtime_gpu_health_ == nullptr ||
        realtime_gpu_fenced_for_offline_)
        return;
    detail::RealtimeGpuHealth health;
    if (!realtime_gpu_health_(realtime_gpu_context_, health))
        return;
    const auto reason = health.recovery_reason;
    const bool outage = !health.delivery_epoch_active && reason != GpuAudioRecoveryReason::None &&
                        reason != GpuAudioRecoveryReason::OfflineFence &&
                        reason != GpuAudioRecoveryReason::ProviderReacquired;
    const auto now = monotonic_now_ns();
    // Only GPU audio from a re-acquired epoch ends an outage: the callback
    // clears the pending resume on its first GPU block. Until then attempts
    // keep counting toward the cap and backing off. A delivery that happened
    // between pumps still ends it, even if a new fault has latched since.
    const bool resumed =
        resume_armed_ && resume_pending_from_ns_.load(std::memory_order_acquire) == 0;
    if (resumed || (!outage && health.delivery_epoch_active &&
                    resume_pending_from_ns_.load(std::memory_order_acquire) == 0)) {
        resume_armed_ = false;
        reacquire_round_ = 0;
        next_reacquire_ns_ = 0;
        outage_started_ns_ = 0;
    }
    if (!outage)
        return;
    if (outage_started_ns_ == 0)
        outage_started_ns_ = now;
    if (reacquire_round_ >= reacquire_max_attempts_ || now < next_reacquire_ns_)
        return;
    reacquire_attempts_.fetch_add(1, std::memory_order_relaxed);
    // Every attempt counts and backs off, successful or not: a new epoch that
    // fails before delivering must not be replaced on every pump.
    std::uint64_t wait = reacquire_backoff_ns_;
    for (std::uint32_t i = 0; i < reacquire_round_ && wait <= UINT64_MAX / kReacquireBackoffGrowth;
         ++i)
        wait *= kReacquireBackoffGrowth;
    ++reacquire_round_;
    next_reacquire_ns_ = wait > UINT64_MAX - now ? UINT64_MAX : now + wait;
    if (realtime_gpu_reprepare_(realtime_gpu_context_, reacquire_budget_ns_)) {
        reacquire_successes_.fetch_add(1, std::memory_order_relaxed);
        // Keep the earliest outage start: an epoch that failed before
        // delivering does not restart the time to GPU.
        auto expected = std::uint64_t{0};
        (void)resume_pending_from_ns_.compare_exchange_strong(expected, outage_started_ns_,
                                                              std::memory_order_acq_rel);
        resume_armed_ = true;
        refresh_node_health();
    }
}

void GpuAudioTransport::publish_prepared_configuration() noexcept {
    detail::GpuAudioTransportConfiguration configuration;
    if (prepared_ && node_ != nullptr) {
        configuration.prepared = true;
        configuration.realtime_health = realtime_gpu_health_ != nullptr;
        configuration.staged_health = sample_staged_health_;
        // Generic private/test hooks cannot establish provider identity. The
        // concrete shared path helper returns Dawn only after identifying the
        // exact provider-backed node; future providers must add equivalent
        // proof.
        configuration.provider = detail::realtime_gpu_provider(node_);
        configuration.miss_policy = miss_policy_;
        configuration.sample_rate = sample_rate_;
        configuration.block_size = block_size_;
        configuration.latency_blocks = latency_blocks_;
        // Only a node that can re-acquire honours AutoReacquire.
        configuration.recovery_policy =
            realtime_gpu_reprepare_ != nullptr ? recovery_policy_ : GpuAudioRecoveryPolicy::Manual;
    }
    diagnostics_->cpu_primary = configuration.provider == GpuAudioProvider::Cpu;
    // Host-only writer, while neither the worker nor a pump caller runs.
    while (!diagnostics_->configuration.publish(configuration)) {
    }
}

void GpuAudioTransport::release_pipeline_hosting() noexcept {
    auto& d = *diagnostics_;
    if (d.pipeline != nullptr)
        (void)d.pipeline->set_hosting(false);
    d.pipeline.reset();
}

bool GpuAudioTransport::prepare(GpuAudioNode* node, const Config& config) {
    release();
    if (node == nullptr)
        return false;

    // Opt the node into worker pipelining before reading its descriptor; a
    // node that pipelines declares worker_pipeline_blocks only while hosted.
    // Undone here, while the node is known alive, if preparation fails or the
    // node does not pipeline; release() undoes it only for a pipelining node.
    {
        const auto binding = detail::realtime_gpu_node_binding(node);
        if (binding.pipeline != nullptr && binding.pipeline->set_hosting(true))
            diagnostics_->pipeline = binding.pipeline;
    }
    struct HostingGuard {
        GpuAudioTransport& transport;
        bool keep = false;
        ~HostingGuard() {
            if (!keep)
                transport.release_pipeline_hosting();
        }
    } hosting{*this};

    // The descriptor is the single source of truth for channels/block/latency.
    const GpuAudioNodeDescriptor desc = node->descriptor(); // host thread — may allocate
    if (desc.input_channels == 0 || desc.output_channels == 0 ||
        desc.input_channels != desc.output_channels) {
        return false; // 3a: symmetric channel count only
    }
    // A zero rate would prepare a shape that the status validator then
    // rejects as PreparedShapeMissing, so refuse it here instead.
    if (desc.block_size == 0 || desc.sample_rate == 0 || desc.latency_blocks == 0)
        return false;
    // CpuFallback requires both a declaration and a host-thread witness that
    // prepare() built the allocation-free, latency-aligned fallback state.
    if (desc.miss_policy == MissPolicy::CpuFallback &&
        (!desc.supports_cpu_fallback || !node->cpu_fallback_prepared())) {
        return false;
    }
    // Need the primed latency plus one in-flight block and one free slot.
    if (config.ring_blocks < desc.latency_blocks + 2)
        return false;
    // A pipelined node emits wet(N - D); the primed ring makes up the rest.
    if (desc.worker_pipeline_blocks >= desc.latency_blocks)
        return false;

    node_ = node;
    channels_ = desc.output_channels;
    block_size_ = desc.block_size;
    sample_rate_ = desc.sample_rate;
    latency_blocks_ = desc.latency_blocks;
    ring_blocks_ = config.ring_blocks;
    miss_policy_ = desc.miss_policy;
    const auto realtime_path = detail::realtime_gpu_node_path(node);
    if (!realtime_path.active() && detail::requires_realtime_gpu_path(node)) {
        release();
        return false;
    }
    if (realtime_path.active()) {
        realtime_gpu_context_ = realtime_path.context;
        realtime_gpu_process_ = realtime_path.process;
        realtime_gpu_service_ = realtime_path.service;
        realtime_gpu_fence_ = realtime_path.fence;
        realtime_gpu_delivered_ = realtime_path.delivered;
        realtime_gpu_health_ = realtime_path.health;
        realtime_gpu_offline_render_ = realtime_path.offline_render;
        realtime_gpu_reprepare_ = realtime_path.reprepare;
        realtime_gpu_set_notifier_ = realtime_path.set_completion_notifier;
        callback_sequence_ = realtime_path.next_sequence(realtime_path.context);
    }
    sample_staged_health_ = !realtime_path.active();

    const std::uint64_t cap = static_cast<std::uint64_t>(ring_blocks_) * block_size_;
    if (!input_ring_.prepare(channels_, cap) || !output_ring_.prepare(channels_, cap)) {
        release();
        return false;
    }

    worker_in_.resize(channels_, block_size_);
    worker_out_.resize(channels_, block_size_);
    worker_in_.clear();
    worker_out_.clear();
    rejected_input_.resize(channels_, block_size_);
    rejected_output_.resize(channels_, block_size_);
    rejected_input_.clear();
    rejected_output_.clear();
    rejected_input_ptrs_.resize(channels_);
    rejected_output_ptrs_.resize(channels_);
    auto rejected_in = rejected_input_.view();
    auto rejected_out = rejected_output_.view();
    for (uint32_t ch = 0; ch < channels_; ++ch) {
        rejected_input_ptrs_[ch] = rejected_in.channel_ptr(ch);
        rejected_output_ptrs_[ch] = rejected_out.channel_ptr(ch);
    }

    in_fptrs_.resize(channels_);
    in_cptrs_.resize(channels_);
    out_fptrs_.resize(channels_);
    auto iv = worker_in_.view();
    auto ov = worker_out_.view();
    for (uint32_t ch = 0; ch < channels_; ++ch) {
        in_fptrs_[ch] = iv.channel_ptr(ch);
        in_cptrs_[ch] = iv.channel_ptr(ch);
        out_fptrs_[ch] = ov.channel_ptr(ch);
    }

    reset_staged_transport_state();

    produced_blocks_.store(0, std::memory_order_relaxed);
    miss_blocks_.store(0, std::memory_order_relaxed);
    input_dropped_blocks_.store(0, std::memory_order_relaxed);
    resynced_blocks_.store(0, std::memory_order_relaxed);
    blocks_owed_ = 0;
    last_block_ns_.store(0, std::memory_order_relaxed);
    avg_block_ns_.store(0, std::memory_order_relaxed);
    delivery_gpu_.store(0, std::memory_order_relaxed);
    delivery_worker_.store(0, std::memory_order_relaxed);
    delivery_fallback_.store(0, std::memory_order_relaxed);
    delivery_silence_.store(0, std::memory_order_relaxed);
    delivery_passthrough_.store(0, std::memory_order_relaxed);
    delivery_priming_.store(0, std::memory_order_relaxed);
    delivery_invalid_.store(0, std::memory_order_relaxed);
    // Wake-on-write only matters when we own the worker thread; when the caller
    // drives pump() there is nothing waiting on the semaphore to wake.
    wake_on_write_ = config.wake_on_write && config.run_worker_thread;
    audio_workgroup_ = config.audio_workgroup;
    join_audio_workgroup_ = config.join_audio_workgroup && config.run_worker_thread;
    worker_workgroup_joined_.store(false, std::memory_order_relaxed);
    worker_workgroup_join_failures_.store(0, std::memory_order_relaxed);
    recovery_policy_ = config.recovery_policy;
    reacquire_budget_ns_ = config.reacquire_release_budget_ns;
    reacquire_max_attempts_ = config.reacquire_max_attempts;
    reacquire_backoff_ns_ = config.reacquire_backoff_ns;
    reacquire_round_ = 0;
    outage_started_ns_ = next_reacquire_ns_ = 0;
    resume_armed_ = false;
    reacquire_attempts_.store(0, std::memory_order_relaxed);
    reacquire_successes_.store(0, std::memory_order_relaxed);
    last_reacquire_to_gpu_ns_.store(0, std::memory_order_relaxed);
    resume_pending_from_ns_.store(0, std::memory_order_relaxed);
    worker_interactive_qos_.store(false, std::memory_order_relaxed);
    prepared_ = true;
    // Quiescent host reads: neither the callback nor the worker has started.
    refresh_node_health();
    publish_prepared_configuration();

    if (config.run_worker_thread) {
        // The poll interval is a fallback only. Without wake-on-write it is
        // the worker's sole clock, so it runs a few times per block. With
        // wake-on-write every callback posts the semaphore, so the interval
        // only bounds a missed or coalesced post and a completion that has no
        // event source: a quarter block, capped, with no lower floor.
        const double block_us = (desc.sample_rate > 0)
                                    ? (1.0e6 * static_cast<double>(block_size_) / desc.sample_rate)
                                    : 800.0;
        // A provider with a completion event source wakes the worker itself,
        // so the fallback then only covers a missed callback post: one block.
        completion_notifier_installed_ =
            wake_on_write_ && realtime_gpu_set_notifier_ != nullptr &&
            realtime_gpu_set_notifier_(realtime_gpu_context_,
                                       {&GpuAudioTransport::notify_worker, this});
        long iv = static_cast<long>(completion_notifier_installed_ ? block_us : block_us / 4.0);
        if (iv < 1)
            iv = 1;
        if (iv > 2000)
            iv = 2000;
        poll_interval_ = std::chrono::microseconds(iv);
        worker_running_.store(true, std::memory_order_release);
        worker_ = std::thread([this] { worker_loop(); });
    }
    // Keep the opt-in only for a node that actually pipelines; release() must
    // touch the node for that case alone (the guard undoes it otherwise).
    if (desc.worker_pipeline_blocks > 0) {
        hosting.keep = true;
        pipeline_hosted_ = true;
    }
    return true;
}

void GpuAudioTransport::worker_loop() noexcept {
    audio::AudioWorkgroup workgroup;
    bool joined_workgroup = false;
#if defined(__APPLE__)
    if (join_audio_workgroup_ && audio_workgroup_ != nullptr) {
        workgroup.set_workgroup(reinterpret_cast<os_workgroup_t>(audio_workgroup_));
        joined_workgroup = workgroup.join_from_audio_thread();
        if (joined_workgroup) {
            worker_workgroup_joined_.store(true, std::memory_order_release);
        } else {
            worker_workgroup_join_failures_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Outside a host workgroup, run the worker at the interactive QoS class so
    // a saturated host does not park it behind default-priority work.
    if (!joined_workgroup)
        worker_interactive_qos_.store(
            pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0) == 0,
            std::memory_order_release);
#else
    (void)joined_workgroup;
#endif
    while (worker_running_.load(std::memory_order_acquire)) {
        // Consume the posts this pump will cover. A post that lands during the
        // pump stays pending and wakes the next wait at once, so coalesced
        // posts never make the worker spin and a late one is never lost. The
        // drain is bounded: a provider posting faster than the drain cannot
        // keep the worker from seeing a stop request.
        if (wake_on_write_) {
            for (int drained = 0; drained < kMaxCoalescedWakes && wake_->try_consume(); ++drained) {
            }
        }
        // Yield while an offline (synchronous) render owns the node — process_offline
        // pumps inline. The mutex serializes node access for the brief window where
        // the flag flips after this check.
        if (!synchronous_.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lock(pump_mutex_);
            pump();
        }
        // Wait for the next RT input write, bounded by the poll interval so a
        // missed/coalesced post (or offline mode, which posts nothing) still
        // makes progress. Falls back to a plain sleep when wake-on-write is off.
        if (wake_on_write_) {
            (void)wake_->wait_for(poll_interval_);
        } else {
            std::this_thread::sleep_for(poll_interval_);
        }
    }
    std::lock_guard<std::mutex> lock(pump_mutex_);
    pump(); // final drain of any input left at stop
    if (joined_workgroup)
        worker_workgroup_joined_.store(false, std::memory_order_release);
    worker_interactive_qos_.store(false, std::memory_order_release);
}

void GpuAudioTransport::reset_staged_transport_state() noexcept {
    input_ring_.reset();
    output_ring_.reset();
    worker_in_.clear();
    worker_out_.clear();
    blocks_owed_ = 0;

    if (channels_ == 0 || block_size_ == 0 || latency_blocks_ == 0 || out_fptrs_.empty())
        return;

    audio::BufferView<float> silence(out_fptrs_.data(), channels_, block_size_);
    // A node whose worker emits D blocks behind its submit supplies D of the
    // latency itself, so prime only the remainder.
    const uint32_t primed = latency_blocks_ - node_->descriptor().worker_pipeline_blocks;
    for (uint32_t b = 0; b < primed; ++b) {
        (void)output_ring_.write(silence, block_size_);
    }
}

void GpuAudioTransport::release() noexcept {
    // Stop the worker before tearing down the rings/buffers it touches.
    worker_running_.store(false, std::memory_order_release);
    if (worker_.joinable())
        worker_.join();
    // No service step runs now. After the clearing call returns the provider
    // never invokes notify_worker() for this transport again.
    if (completion_notifier_installed_ && realtime_gpu_set_notifier_ != nullptr)
        (void)realtime_gpu_set_notifier_(realtime_gpu_context_, {});
    completion_notifier_installed_ = false;
    realtime_gpu_set_notifier_ = nullptr;

    input_ring_.release();
    output_ring_.release();
    in_fptrs_.clear();
    in_cptrs_.clear();
    out_fptrs_.clear();
    rejected_input_ptrs_.clear();
    rejected_output_ptrs_.clear();
    // The worker has stopped: return a pipelining node to block-in,
    // block-out use. Only such a node is touched here; any other node may
    // already be gone.
    if (pipeline_hosted_)
        release_pipeline_hosting();
    pipeline_hosted_ = false;
    node_ = nullptr;
    realtime_gpu_context_ = nullptr;
    realtime_gpu_process_ = nullptr;
    realtime_gpu_service_ = nullptr;
    realtime_gpu_fence_ = nullptr;
    realtime_gpu_delivered_ = nullptr;
    realtime_gpu_health_ = nullptr;
    realtime_gpu_offline_render_ = nullptr;
    realtime_gpu_reprepare_ = nullptr;
    recovery_policy_ = GpuAudioRecoveryPolicy::Manual;
    sample_staged_health_ = false;
    diagnostics_->health.reset();
    trial_observer_context_ = nullptr;
    trial_observer_ = nullptr;
    callback_sequence_ = 0;
    realtime_gpu_fenced_for_offline_ = false;
    realtime_gpu_offline_ = false;
    audio_workgroup_ = nullptr;
    join_audio_workgroup_ = false;
    worker_workgroup_joined_.store(false, std::memory_order_release);
    channels_ = block_size_ = sample_rate_ = latency_blocks_ = ring_blocks_ = 0;
    prepared_ = false;
    publish_prepared_configuration();
}

void GpuAudioTransport::process(const audio::BufferView<const float>& input,
                                audio::BufferView<float>& output, uint32_t n) noexcept {
    if (!prepared_) {
        output.clear();
        return;
    }
    const auto sequence = callback_sequence_++;
    const auto callback_start_ns = monotonic_now_ns();
    synchronous_.store(false, std::memory_order_release);
    if (n != block_size_ || input.num_channels() < channels_ || output.num_channels() < channels_ ||
        input.num_samples() < n || output.num_samples() < n) {
        process_invalid_position(output, sequence, false);
        return;
    }
    process_realtime_position(input, output, n, sequence, true, callback_start_ns);
}

void GpuAudioTransport::process_realtime_position(const audio::BufferView<const float>& input,
                                                  audio::BufferView<float>& output, uint32_t n,
                                                  std::uint64_t sequence, bool input_valid,
                                                  std::uint64_t callback_start_ns) noexcept {
    if (realtime_gpu_process_ != nullptr) {
        if (realtime_gpu_offline_) {
            realtime_gpu_offline_ = false;
            realtime_gpu_offline_render_(realtime_gpu_context_, false);
        }
        process_shared(input, output, n, sequence, input_valid, callback_start_ns, true);
        return;
    }

    const auto publish_delivery = [&](std::uint8_t disposition) noexcept {
        record_delivery(input_valid ? disposition
                                    : static_cast<std::uint8_t>(
                                          detail::SharedIoDeliveryDisposition::InvalidRejected),
                        true);
        if (input_valid && trial_observer_ != nullptr) {
            const auto callback_end_ns = monotonic_now_ns();
            publish_trial_delivery(sequence, disposition, callback_start_ns, callback_end_ns,
                                   monotonic_now_ns());
        }
    };

    // Hand the input block to the worker as a WHOLE block (all-or-nothing) so
    // the ring stays block-aligned — a partial write would split a block and
    // misalign every later block-sized read. Single-producer, so a free-space
    // check that passes here still holds at write time (the worker only frees
    // more). A full ring drops the whole block (telemetry) rather than blocking.
    if (input_ring_.free_frames() >= n) {
        input_ring_.write(input, n);
        // Nudge the worker awake (opt-in) so it reacts within the block instead
        // of at the next poll tick. RT-safe: a counting-semaphore post neither
        // allocates nor blocks. No-op when wake-on-write is off.
        if (wake_on_write_)
            wake_->post();
    } else {
        input_dropped_blocks_.fetch_add(1, std::memory_order_relaxed);
    }

    // Resync after prior misses. Each miss substituted a dry/fallback block for
    // its timeline slot; when the worker later catches up it back-fills that
    // slot's wet block, one block behind the timeline. Reading that stale block
    // would comb-filter dry against a one-block-late wet and creep effective
    // latency one block per miss. Drop the redundant late block(s) so the read
    // below returns the block that is exactly `latency_blocks` old.
    //
    // We drain exactly `blocks_owed_` — the count of past misses, mutated only
    // here on the audio thread — never "excess ring depth". Inferring the debt
    // from depth is racy under a real worker thread: the worker can commit a
    // fresh block into the output ring between this call's input write and this
    // read, transiently lifting depth above steady-state with NO miss, which a
    // depth-based drain would misread as owed and discard the very block being
    // read. Counting misses is immune to that. Discard stale results even when
    // that empties the ring: another fallback is preferable to delivering audio
    // for an expired timeline slot. drain() only advances the read cursor.
    if (blocks_owed_ > 0) {
        const std::uint64_t avail_blocks = output_ring_.available_frames() / n;
        if (avail_blocks > 0) {
            const std::uint64_t drop = std::min<std::uint64_t>(blocks_owed_, avail_blocks);
            output_ring_.drain(drop * n);
            blocks_owed_ -= drop;
            resynced_blocks_.fetch_add(drop, std::memory_order_relaxed);
        }
    }

    // Keep the CPU fallback's convolution history current: feed it THIS block
    // (hit or miss) and let it stage a latency-aligned substitute, so if the read
    // below misses, the substitute is a correct continuation of the stream rather
    // than a stale block with gaps in its overlap-add tail. Cheap (one partitioned
    // block); only for CpuFallback nodes.
    if (miss_policy_ == MissPolicy::CpuFallback)
        node_->prime_fallback(input, n);

    // Read the latency-delayed output produced earlier by the worker.
    // If debt remains, a worker write racing the snapshot above is still stale.
    // Leave it for the next bounded drain rather than accepting it as current.
    if (blocks_owed_ == 0 && output_ring_.read(output, n)) {
        const auto disposition = sequence < latency_blocks_
                                     ? detail::SharedIoDeliveryDisposition::Priming
                                     : detail::SharedIoDeliveryDisposition::GpuDelivered;
        publish_delivery(static_cast<std::uint8_t>(disposition));
        return;
    }

    // Miss: the worker has not produced this block in time. Substitute per the
    // policy and record the debt so the late wet block is dropped once it lands.
    miss_blocks_.fetch_add(1, std::memory_order_relaxed);
    ++blocks_owed_;
    std::uint8_t delivery =
        static_cast<std::uint8_t>(detail::SharedIoDeliveryDisposition::SilenceDelivered);
    switch (miss_policy_) {
    case MissPolicy::Silence:
        output.clear();
        delivery = static_cast<std::uint8_t>(detail::SharedIoDeliveryDisposition::SilenceDelivered);
        break;
    case MissPolicy::PassthroughDry:
        for (uint32_t c = 0; c < channels_; ++c) {
            const float* src = input.channel_ptr(c);
            float* dst = output.channel_ptr(c);
            for (uint32_t i = 0; i < n; ++i)
                dst[i] = src[i];
        }
        delivery =
            static_cast<std::uint8_t>(detail::SharedIoDeliveryDisposition::PassthroughDelivered);
        break;
    case MissPolicy::CpuFallback:
        node_->process_cpu_fallback(input, output, n);
        delivery =
            static_cast<std::uint8_t>(detail::SharedIoDeliveryDisposition::CpuFallbackDelivered);
        break;
    }
    publish_delivery(delivery);
}

void GpuAudioTransport::process_shared(const audio::BufferView<const float>& input,
                                       audio::BufferView<float>& output, std::uint32_t n,
                                       std::uint64_t sequence, bool input_valid,
                                       std::uint64_t callback_start_ns,
                                       bool count_delivery) noexcept {
    using detail::SharedIoDeliveryDisposition;
    if (miss_policy_ == MissPolicy::CpuFallback)
        node_->prime_fallback(input, n);
    const auto status = realtime_gpu_process_(realtime_gpu_context_, input, output, n, sequence,
                                              input_valid, callback_start_ns);
    const auto callback_end_ns = monotonic_now_ns();
    auto delivered = SharedIoDeliveryDisposition::GpuDelivered;
    if (status == detail::kRealtimeGpuPriming) {
        output.clear();
        delivered = SharedIoDeliveryDisposition::Priming;
    } else if (status != detail::kRealtimeGpuReady) {
        if (!diagnostics_->cpu_primary)
            miss_blocks_.fetch_add(1, std::memory_order_relaxed);
        switch (miss_policy_) {
        case MissPolicy::Silence:
            output.clear();
            delivered = SharedIoDeliveryDisposition::SilenceDelivered;
            break;
        case MissPolicy::PassthroughDry:
            for (std::uint32_t c = 0; c < channels_; ++c)
                std::copy_n(input.channel_ptr(c), n, output.channel_ptr(c));
            delivered = SharedIoDeliveryDisposition::PassthroughDelivered;
            break;
        case MissPolicy::CpuFallback:
            node_->process_cpu_fallback(input, output, n);
            delivered = SharedIoDeliveryDisposition::CpuFallbackDelivered;
            break;
        }
    }
    if (!input_valid) {
        output.clear();
        delivered = SharedIoDeliveryDisposition::InvalidRejected;
    }
    const auto result_visible_ns = monotonic_now_ns();
    // The first GPU block after a re-acquire ends the outage it measures.
    if (delivered == SharedIoDeliveryDisposition::GpuDelivered) {
        auto from = resume_pending_from_ns_.load(std::memory_order_relaxed);
        if (from != 0 &&
            resume_pending_from_ns_.compare_exchange_strong(from, 0, std::memory_order_acq_rel))
            last_reacquire_to_gpu_ns_.store(result_visible_ns > from ? result_visible_ns - from : 0,
                                            std::memory_order_relaxed);
    }
    realtime_gpu_delivered_(realtime_gpu_context_, sequence, static_cast<std::uint8_t>(delivered),
                            callback_end_ns, result_visible_ns);
    if (count_delivery)
        record_delivery(static_cast<std::uint8_t>(delivered), false);
    if (wake_on_write_)
        wake_->post();
}

void GpuAudioTransport::process_offline(const audio::BufferView<const float>& input,
                                        audio::BufferView<float>& output, uint32_t n) noexcept {
    if (!prepared_) {
        output.clear();
        return;
    }
    const auto sequence = callback_sequence_++;
    if (n != block_size_ || input.num_channels() < channels_ || output.num_channels() < channels_ ||
        input.num_samples() < n || output.num_samples() < n) {
        process_invalid_position(output, sequence, true);
        return;
    }
    process_offline_position(input, output, n, sequence, true);
}

void GpuAudioTransport::process_invalid_position(audio::BufferView<float>& output,
                                                 std::uint64_t sequence, bool offline) noexcept {
    // The shared final-disposition hook runs inside the sanitized position.
    // Commit the externally visible silence before that hook can report it.
    output.clear();
    audio::BufferView<const float> zero_input(rejected_input_ptrs_.data(), channels_, block_size_);
    audio::BufferView<float> discarded_output(rejected_output_ptrs_.data(), channels_, block_size_);
    if (offline)
        process_offline_position(zero_input, discarded_output, block_size_, sequence, false);
    else
        process_realtime_position(zero_input, discarded_output, block_size_, sequence, false,
                                  monotonic_now_ns());
}

void GpuAudioTransport::process_offline_position(const audio::BufferView<const float>& input,
                                                 audio::BufferView<float>& output, uint32_t n,
                                                 std::uint64_t sequence,
                                                 bool input_valid) noexcept {
    // Take ownership of pumping: tell the worker to yield, then serialize node
    // access against any pump it is already inside. No real-time deadline here.
    synchronous_.store(true, std::memory_order_release);
    std::lock_guard<std::mutex> lock(pump_mutex_);

    if (realtime_gpu_fence_ != nullptr) {
        if (!realtime_gpu_offline_ && realtime_gpu_offline_render_ != nullptr) {
            realtime_gpu_offline_ = true;
            realtime_gpu_offline_render_(realtime_gpu_context_, true);
        }
        if (!realtime_gpu_fenced_for_offline_) {
            realtime_gpu_fenced_for_offline_ = realtime_gpu_fence_(realtime_gpu_context_);
            refresh_node_health();
        }
        // A failed barrier retains its owner and stays CPU-only. The same
        // continuously primed fallback carries the RT/offline timeline.
        process_shared(input, output, n, sequence, input_valid, monotonic_now_ns(), false);
        return;
    }

    // Write this block, then pump SYNCHRONOUSLY so the node actually produces it
    // now (instead of the async worker maybe-producing it later). The output ring
    // was primed with `latency_blocks` of silence at prepare(), so the read below
    // still returns the latency-delayed output — identical latency to process(),
    // which keeps host PDC aligned between realtime and offline renders.
    if (input_ring_.free_frames() >= n) {
        input_ring_.write(input, n);
    } else {
        input_dropped_blocks_.fetch_add(1, std::memory_order_relaxed);
    }
    pump(); // drains all ready input → node → output ring (blocking GPU readback)

    // Keep the fallback fed on the offline timeline too, so a backstop miss below
    // substitutes a correctly-continued, latency-aligned block.
    if (miss_policy_ == MissPolicy::CpuFallback)
        node_->prime_fallback(input, n);

    if (output_ring_.read(output, n))
        return;

    // Should not happen after a synchronous pump (the block was just produced),
    // but honor the miss policy as a backstop rather than emit a stale read.
    miss_blocks_.fetch_add(1, std::memory_order_relaxed);
    switch (miss_policy_) {
    case MissPolicy::Silence:
        output.clear();
        break;
    case MissPolicy::PassthroughDry:
        for (uint32_t c = 0; c < channels_; ++c) {
            const float* src = input.channel_ptr(c);
            float* dst = output.channel_ptr(c);
            for (uint32_t i = 0; i < n; ++i)
                dst[i] = src[i];
        }
        break;
    case MissPolicy::CpuFallback:
        node_->process_cpu_fallback(input, output, n);
        break;
    }
}

void GpuAudioTransport::pump(uint32_t max_blocks) noexcept {
    if (!prepared_)
        return;
    const uint32_t bs = block_size_;

    if (!synchronous_.load(std::memory_order_acquire) && realtime_gpu_service_ != nullptr) {
        const auto t0 = std::chrono::steady_clock::now();
        const auto produced = realtime_gpu_service_(
            realtime_gpu_context_,
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t0.time_since_epoch())
                    .count()));
        // The service step is where a node observes provider loss or failure;
        // publish what it saw before the next host/UI read.
        refresh_node_health();
        steward_reacquire();
        if (produced != detail::kRealtimeGpuServiceInactive) {
            const auto block_ns =
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                               std::chrono::steady_clock::now() - t0)
                                               .count());
            if (produced > 0) {
                last_block_ns_.store(block_ns, std::memory_order_relaxed);
                const std::uint64_t prev = avg_block_ns_.load(std::memory_order_relaxed);
                std::uint64_t next;
                if (prev == 0)
                    next = block_ns;
                else if (block_ns >= prev)
                    next = prev + (block_ns - prev) / 16;
                else
                    next = prev - (prev - block_ns) / 16;
                avg_block_ns_.store(next, std::memory_order_relaxed);
                produced_blocks_.fetch_add(produced, std::memory_order_relaxed);
            }
            (void)max_blocks;
            return;
        }
        // Losing a previously active provider does not authorize another GPU
        // path. The callback continues its already-prepared CPU fallback.
        return;
    }

    audio::BufferView<float> in_w(in_fptrs_.data(), channels_, bs);
    audio::BufferView<const float> in_c(in_cptrs_.data(), channels_, bs);
    audio::BufferView<float> out_w(out_fptrs_.data(), channels_, bs);

    uint32_t done = 0;
    while ((max_blocks == 0 || done < max_blocks) && input_ring_.available_frames() >= bs &&
           output_ring_.free_frames() >= bs) {
        if (!input_ring_.read(in_w, bs))
            break;
        const auto t0 = std::chrono::steady_clock::now();
        node_->process_block(in_c, out_w, bs);
        const auto block_ns =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           std::chrono::steady_clock::now() - t0)
                                           .count());
        if (output_ring_.write(out_w, bs) < bs)
            break;
        // Publish per-block timing: last value + an EWMA (alpha = 1/16) so the
        // UI sees a smooth, current figure without storing a history.
        last_block_ns_.store(block_ns, std::memory_order_relaxed);
        const std::uint64_t prev = avg_block_ns_.load(std::memory_order_relaxed);
        // EWMA toward block_ns with alpha = 1/16. Branch on direction so the
        // unsigned difference never underflows (block_ns can be below prev).
        std::uint64_t next;
        if (prev == 0)
            next = block_ns;
        else if (block_ns >= prev)
            next = prev + (block_ns - prev) / 16;
        else
            next = prev - (prev - block_ns) / 16;
        avg_block_ns_.store(next, std::memory_order_relaxed);
        produced_blocks_.fetch_add(1, std::memory_order_relaxed);
        ++done;
    }
    refresh_node_health();
}

void GpuAudioTransport::record_delivery(std::uint8_t disposition, bool worker_output) noexcept {
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    using D = detail::SharedIoDeliveryDisposition;
    switch (static_cast<D>(disposition)) {
    case D::GpuDelivered:
        (worker_output ? delivery_worker_ : delivery_gpu_).fetch_add(1, std::memory_order_relaxed);
        break;
    case D::CpuFallbackDelivered:
        delivery_fallback_.fetch_add(1, std::memory_order_relaxed);
        break;
    case D::SilenceDelivered:
        delivery_silence_.fetch_add(1, std::memory_order_relaxed);
        break;
    case D::PassthroughDelivered:
        delivery_passthrough_.fetch_add(1, std::memory_order_relaxed);
        break;
    case D::Priming:
        delivery_priming_.fetch_add(1, std::memory_order_relaxed);
        break;
    case D::InvalidRejected:
    case D::None:
        delivery_invalid_.fetch_add(1, std::memory_order_relaxed);
        break;
    }
}

GpuAudioTransport::DeliverySnapshot GpuAudioTransport::delivery_snapshot() const noexcept {
    return {
        delivery_gpu_.load(std::memory_order_relaxed),
        delivery_worker_.load(std::memory_order_relaxed),
        delivery_fallback_.load(std::memory_order_relaxed),
        delivery_silence_.load(std::memory_order_relaxed),
        delivery_passthrough_.load(std::memory_order_relaxed),
        delivery_priming_.load(std::memory_order_relaxed),
        delivery_invalid_.load(std::memory_order_relaxed),
    };
}

GpuAudioTransport::Stats GpuAudioTransport::stats() const noexcept {
    Stats s;
    s.produced_blocks = produced_blocks_.load(std::memory_order_relaxed);
    s.miss_blocks = miss_blocks_.load(std::memory_order_relaxed);
    s.input_dropped_frames = input_dropped_blocks_.load(std::memory_order_relaxed) * block_size_;
    s.resynced_blocks = resynced_blocks_.load(std::memory_order_relaxed);
    s.last_block_us = last_block_ns_.load(std::memory_order_relaxed) / 1000.0;
    s.avg_block_us = avg_block_ns_.load(std::memory_order_relaxed) / 1000.0;
    s.worker_workgroup_joined = worker_workgroup_joined_.load(std::memory_order_acquire);
    s.worker_workgroup_join_failures =
        worker_workgroup_join_failures_.load(std::memory_order_relaxed);
    s.worker_interactive_qos = worker_interactive_qos_.load(std::memory_order_acquire);
    return s;
}

namespace {

GpuAudioCapabilityReport
capability_from(const detail::GpuAudioTransportConfiguration& configuration) noexcept {
    GpuAudioCapabilityReport report;
    if (!configuration.prepared)
        return report;
    report.provider = configuration.provider;
    report.path = report.provider == GpuAudioProvider::Cpu ? GpuAudioExecutionPath::Cpu
                  : report.provider != GpuAudioProvider::Unknown
                      ? GpuAudioExecutionPath::SharedMemory
                      : GpuAudioExecutionPath::Staged;
    report.eligibility = GpuAudioEligibility::Eligible;
    report.fallback_policy = configuration.miss_policy;
    report.prepared_latency_blocks = configuration.latency_blocks;
    // `latency_blocks` is the transport's fixed PDC delay. It is also the
    // algorithmic lead for authenticated concrete shared providers, but a
    // generic staged node has no lead declaration at this boundary.
    report.prepared_lead_blocks =
        report.provider == GpuAudioProvider::Unknown ? 0 : configuration.latency_blocks;
    report.prepared = true;
    report.fallback_available = configuration.miss_policy == MissPolicy::CpuFallback;
    report.diagnostics_available = true;
    report.recovery_policy = configuration.recovery_policy;
    return report;
}

} // namespace

GpuAudioStatus GpuAudioTransport::status_snapshot() const noexcept {
    // Reads only transport-owned atomics and the two coherent copies in
    // `diagnostics_`; never `node_` or the plain prepared fields, which a
    // host may change concurrently by re-preparing or destroying the node.
    detail::GpuAudioTransportConfiguration configuration;
    if (!diagnostics_->configuration.read(configuration))
        configuration = {};
    const auto capability = capability_from(configuration);

    GpuAudioStatus status;
    status.provider = capability.provider;
    status.fallback_policy = capability.fallback_policy;
    status.prepared_lead_blocks = capability.prepared_lead_blocks;
    status.fallback_available = capability.fallback_available;
    status.diagnostics_available = capability.diagnostics_available;
    status.produced_blocks = produced_blocks_.load(std::memory_order_relaxed);
    status.missed_blocks = miss_blocks_.load(std::memory_order_relaxed);
    status.fallback_blocks = delivery_fallback_.load(std::memory_order_relaxed);
    status.resynced_blocks = resynced_blocks_.load(std::memory_order_relaxed);
    status.input_dropped_frames =
        input_dropped_blocks_.load(std::memory_order_relaxed) * configuration.block_size;
    status.last_block_us = last_block_ns_.load(std::memory_order_relaxed) / 1000.0;
    status.avg_block_us = avg_block_ns_.load(std::memory_order_relaxed) / 1000.0;

    if (!configuration.prepared) {
        status.selected_engine = GpuAudioEngine::Unknown;
        status.provider_state = GpuAudioProviderState::Uninitialized;
        return status;
    }

    status.sample_rate = configuration.sample_rate;
    status.block_size = configuration.block_size;
    status.latency_samples = configuration.latency_blocks * configuration.block_size;

    // A prepared transport is not, by itself, evidence that work executes on
    // a GPU. Generic GpuAudioNode implementations use the staged worker path
    // and may be entirely CPU-backed; that path deliberately reports an
    // unknown provider. Keep the public status honest instead of turning an
    // unknown provider into a false "GPU/Ready" indicator. Concrete providers
    // establish the engine only when their identity is authenticated.
    if (capability.path == GpuAudioExecutionPath::Cpu) {
        status.selected_engine = GpuAudioEngine::Cpu;
    } else if (capability.provider != GpuAudioProvider::Unknown &&
               capability.path != GpuAudioExecutionPath::Unavailable) {
        status.selected_engine = GpuAudioEngine::Gpu;
    } else {
        status.selected_engine = GpuAudioEngine::Unknown;
    }

    if (capability.eligibility != GpuAudioEligibility::Eligible) {
        status.provider_state = GpuAudioProviderState::Unavailable;
    } else if (status.selected_engine == GpuAudioEngine::Unknown) {
        // The transport is prepared, but its execution provider cannot be
        // identified. This is a degraded diagnostic state, not GPU readiness.
        status.provider_state = GpuAudioProviderState::Degraded;
    } else {
        status.provider_state = GpuAudioProviderState::Ready;
    }

    // Node health can only lower the state established above. The enum values
    // are ordered Ready < Degraded < Unavailable for a prepared transport.
    const auto lower_to = [&status](GpuAudioProviderState state) noexcept {
        if (static_cast<std::uint8_t>(state) > static_cast<std::uint8_t>(status.provider_state))
            status.provider_state = state;
    };
    detail::RealtimeGpuHealth health;
    const bool sampled = (configuration.realtime_health || configuration.staged_health) &&
                         diagnostics_->health.read(health);
    if (sampled) {
        lower_to(detail::provider_state_for_health(health));
        status.recovery_reason = detail::reported_recovery_reason(health);
        status.recovery_sequence = health.recovery_sequence;
        status.epoch = health.epoch;
        status.delivery_epoch_active = health.delivery_epoch_active;
        for (std::size_t i = 0; i < GpuAudioStatus::kTerminalDispositionCount; ++i)
            status.terminal_counts[i] = health.terminal_counts[i];
        status.shadow_mode = health.shadow_mode;
        status.shadow_inline_cost_pct = health.shadow_inline_cost_pct;
        status.shadow_delivered_blocks = health.shadow_delivered_blocks;
        status.shadow_late_blocks = health.shadow_late_blocks;
        status.shadow_double_miss_blocks = health.shadow_double_miss_blocks;
        status.shadow_model_differs = health.shadow_model_differs;
        status.gpu_kernel_available = health.gpu_kernel_available;
        status.completion_mode = health.completion_mode;
        status.timestamps_available = health.timestamps_available;
        status.last_delivery_ns = health.last_delivery_ns;
        if (health.reports_fallback) {
            status.fallback_policy = health.fallback_policy;
            status.fallback_available = health.fallback_available;
        }
    } else if (configuration.realtime_health) {
        // No coherent health copy: never report an unverified provider as
        // Ready, and claim no recovery cause that was not observed. A staged
        // node that reports no health keeps the staged state above.
        lower_to(GpuAudioProviderState::Degraded);
    }
    status.selected_engine = detail::delivering_engine(
        status.provider_state, status.selected_engine, status.fallback_policy,
        status.fallback_available);
    status.reacquire_attempts = reacquire_attempts_.load(std::memory_order_relaxed);
    status.reacquire_successes = reacquire_successes_.load(std::memory_order_relaxed);
    status.last_reacquire_to_gpu_ns = last_reacquire_to_gpu_ns_.load(std::memory_order_relaxed);
    return status;
}

GpuAudioCapabilityReport GpuAudioTransport::capability_report() const noexcept {
    detail::GpuAudioTransportConfiguration configuration;
    if (!diagnostics_->configuration.read(configuration))
        return {};
    auto report = capability_from(configuration);
    // The node's facts come from the same health sample status_snapshot()
    // reads: whether a GPU kernel is prepared, and the miss substitute in
    // effect. Without a sample neither is claimed.
    detail::RealtimeGpuHealth health;
    if (report.prepared && (configuration.realtime_health || configuration.staged_health) &&
        diagnostics_->health.read(health)) {
        report.gpu_kernel_available = health.gpu_kernel_available;
        if (health.reports_fallback) {
            report.fallback_policy = health.fallback_policy;
            report.fallback_available = health.fallback_available;
        }
    }
    return report;
}

} // namespace pulp::gpu_audio
