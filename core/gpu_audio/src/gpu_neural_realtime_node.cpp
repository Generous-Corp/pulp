#include "detail/gpu_neural_model.hpp"
#include "detail/gpu_neural_realtime_test_access.hpp"
#include "detail/neural_input_sanitizer.hpp"
#include "detail/neural_processor.hpp"
#include "detail/neural_shadow_worker.hpp"
#include "detail/realtime_gpu_audio_path.hpp"
#include "detail/realtime_gpu_health_mirror.hpp"
#include "detail/shared_io_execution_contract.hpp"
#include "detail/shared_io_stamped_bridge.hpp"

#include <pulp/gpu_audio/gpu_neural_realtime_node.hpp>

#include <algorithm>
#include <array>
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
    case detail::SharedIoRecoveryReason::ProviderUnresponsive:
        return GpuAudioRecoveryReason::ProviderUnresponsive;
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

    // GPU delivery, present only while a backend is prepared. A fence
    // quiesces the backend but keeps the bridge, so the callback stays on the
    // fenced shared path; `backend_quiesced` records that the kernel is gone.
    bool backend_active = false;
    bool production_backend = false; // cached at prepare; read by host threads
    std::atomic<bool> backend_quiesced{false};
    // Prewarm leases outlive any failed prewarm until the backend releases.
    std::vector<float> prewarm_input, prewarm_output;
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
    // GPU loss and hang detection, as in GpuWaveNetRealtimeNode. The device
    // flag is written by the worker and read by host status reads. The
    // callback publishes the sequence after the last callback that finished
    // consuming its output; the watchdog counts silence in those blocks.
    std::atomic<bool> device_lost{false};
    std::atomic<std::uint64_t> callback_next{0};
    std::uint64_t watchdog_limit = 0, completion_mark = 0; // worker only
    std::atomic<std::uint64_t> cpu_recovery_sequence{std::numeric_limits<std::uint64_t>::max()};

    bool prepared = false;
    PrepareError last_error = PrepareError::None;
    // Non-finite input samples replaced with zero, counted once per callback.
    std::atomic<std::uint64_t> sanitized_samples{0};
    // CPU-only path: invalid or out-of-sequence callbacks observed.
    std::atomic<std::uint64_t> contract_violations{0};

    // Shadow placement. `shadow_mode` is the policy in effect; only prepare(),
    // release() and the offline fence change it, each with the callback idle.
    // Atomic because shadow_status() may read them from a host or UI thread
    // while an offline fence moves Worker to Inline.
    std::atomic<ShadowPolicy> shadow_mode{ShadowPolicy::None};
    std::atomic<float> inline_cost_pct{-1.0f};
    float injected_cost_pct = -1.0f; // test seam; negative measures
    bool manual_shadow = false;      // test seam; no thread, pumped explicitly
    GpuNeuralModelHandle shadow_model;
    bool shadow_model_owner = false;
    std::uint64_t shadow_epoch = 0;
    std::uint64_t first_sequence = 0;
    std::vector<float> shadow_take; // Worker result copied out by the callback
    bool shadow_hit = false;
    std::atomic<std::uint64_t> shadow_delivered{0}, shadow_double_miss{0};
    // Declared last so its thread is joined before the processors it steps die.
    detail::NeuralShadowWorker worker;

    explicit Impl(Config source)
        : config(std::move(source)), model(config.model), shadow_model(config.shadow_model) {
        if (shadow_model == model)
            shadow_model.reset();
    }

    // The shadow_* projection shared by status_snapshot() and the health hook
    // the transport mirrors. Counters are atomics; mode and cost change only
    // in prepare(), release() and the offline fence.
    void fill_shadow(detail::RealtimeGpuHealth& out) const noexcept {
        switch (shadow_mode.load(std::memory_order_relaxed)) {
        case ShadowPolicy::Worker:
            out.shadow_mode = GpuAudioShadowMode::Worker;
            break;
        case ShadowPolicy::None:
            out.shadow_mode = GpuAudioShadowMode::None;
            return; // None carries no shadow counts
        default:
            out.shadow_mode = GpuAudioShadowMode::Inline;
            break;
        }
        out.shadow_inline_cost_pct =
            std::max(0.0f, inline_cost_pct.load()) * static_cast<float>(config.channels);
        out.shadow_delivered_blocks = shadow_delivered.load(std::memory_order_relaxed);
        out.shadow_late_blocks = worker.late();
        out.shadow_double_miss_blocks = shadow_double_miss.load(std::memory_order_relaxed);
        out.shadow_model_differs = shadow_model != nullptr;
    }

    // Worker and None both keep the model primed on the shadow thread; only
    // Worker substitutes its results for realtime GPU misses.
    bool worker_fed() const noexcept {
        const auto mode = shadow_mode.load(std::memory_order_relaxed);
        return mode == ShadowPolicy::Worker || mode == ShadowPolicy::None;
    }

    const GpuNeuralModel& shadow_source() const noexcept {
        return shadow_model ? *shadow_model : *model;
    }

    std::uint32_t block_size() const noexcept {
        return model ? model->block_size : 0;
    }

    PrepareError config_error() const noexcept {
        const auto& c = config;
        if (model == nullptr || c.family == NeuralFamily::Unknown)
            return PrepareError::InvalidConfig;
        if (c.family != model->family)
            return PrepareError::FamilyMismatch;
        if (c.channels == 0 || c.channels > 64 || c.channels != model->channels())
            return PrepareError::ChannelMismatch;
        if (model->block_size == 0 || c.lead_blocks == 0 || c.capacity <= c.lead_blocks ||
            c.max_inflight == 0 || c.max_inflight > c.capacity - c.lead_blocks ||
            c.completion_service_wait_ns > 1'000'000 || c.miss_policy != MissPolicy::CpuFallback ||
            epoch == UINT64_MAX || sequence >= Bridge::kSequenceLimit ||
            c.prewarm_blocks >= Bridge::kSequenceLimit - sequence)
            return PrepareError::InvalidConfig;
        if (!backend_admits(c.max_inflight))
            return PrepareError::BackendInflightExceeded;
        if (c.shadow_policy == ShadowPolicy::None && !model->backend)
            return PrepareError::ShadowRequired;
        if (c.shadow_queue_blocks < c.lead_blocks + 2 || c.shadow_queue_blocks > 4096)
            return PrepareError::InvalidConfig;
        if (shadow_model && (shadow_model->channels() != c.channels ||
                             shadow_model->block_size != model->block_size ||
                             shadow_model->sample_rate != model->sample_rate))
            return PrepareError::ShadowModelMismatch;
        return PrepareError::None;
    }

    // A node must never admit more blocks than the backend can hold: a
    // backpressure rejection would otherwise fence the GPU as a failure.
    bool backend_admits(std::uint32_t inflight) const noexcept {
        if (!model->backend)
            return true;
        const auto* wavenet =
            dynamic_cast<const detail::WaveNetStreamingBackend*>(model->backend.get());
        return wavenet != nullptr && inflight <= wavenet->max_inflight();
    }

    detail::StreamingPrepareContext
    context_for(const detail::StreamingModelSpec& spec, bool worker_backend,
                const GpuNeuralModel* source = nullptr) const noexcept {
        if (source == nullptr)
            source = model.get();
        return {.spec = &spec,
                .artifact_id = source->artifact_id,
                .artifact_hash = source->artifact_sha256,
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
                    std::uint32_t n, std::uint64_t at) noexcept {
        for (std::uint32_t ch = 0; ch < config.channels; ++ch) {
            const float* x = shadow_input.data();
            if (ch < input_channels) {
                // Same policy as the GPU submission; process() counts it.
                const float* source = inputs[ch];
                if (detail::count_nonfinite(source, n) == 0)
                    x = source;
                else
                    (void)detail::sanitize_nonfinite(source, shadow_input.data(), n);
            } else {
                std::fill_n(shadow_input.data(), n, 0.0f);
            }
            const float* in_ptr[1] = {x};
            float* out_ptr[1] = {outputs[ch]};
            audio::BufferView<const float> in(in_ptr, 1, n);
            audio::BufferView<float> out(out_ptr, 1, n);
            processors[ch]->process_cpu(in, out, n, {.epoch = shadow_epoch, .sequence = at});
        }
    }

    // Host thread: time one block of channel 0's shadow, then restart its
    // state so the measurement leaves no trace in the stream.
    bool measure_inline_cost() {
        if (injected_cost_pct >= 0.0f) {
            inline_cost_pct = injected_cost_pct;
            return true;
        }
        const auto n = block_size();
        std::vector<float> x(n, 0.0f), y(n, 0.0f);
        const float* in_ptr[1] = {x.data()};
        float* out_ptr[1] = {y.data()};
        audio::BufferView<const float> in(in_ptr, 1, n);
        audio::BufferView<float> out(out_ptr, 1, n);
        processors[0]->process_cpu(in, out, n, {}); // warm caches
        std::array<std::uint64_t, 3> elapsed{};
        for (auto& sample : elapsed) {
            const auto start = now_ns();
            processors[0]->process_cpu(in, out, n, {});
            sample = now_ns() - start;
        }
        std::sort(elapsed.begin(), elapsed.end());
        const double period_ns = 1e9 * static_cast<double>(n) / model->sample_rate;
        inline_cost_pct = static_cast<float>(100.0 * static_cast<double>(elapsed[1]) / period_ns);
        return processors[0]->reset(detail::StreamingResetReason::TransportRestart);
    }

    // Shadow side of the Worker: one planar block through every channel.
    static void shadow_step(void* self, const float* input, float* output,
                            detail::StreamingBlockStamp stamp) noexcept {
        auto& s = *static_cast<Impl*>(self);
        const auto n = s.block_size();
        const float* inputs[64];
        float* outputs[64];
        for (std::uint32_t ch = 0; ch < s.config.channels; ++ch) {
            inputs[ch] = input + static_cast<std::size_t>(ch) * n;
            outputs[ch] = output + static_cast<std::size_t>(ch) * n;
        }
        s.run_shadow(inputs, s.config.channels, outputs, n, stamp.sequence);
    }
    // Shadow side, after an input gap. The shadow thread owns the processors
    // exclusively in Worker mode, so restarting their state there is safe.
    static void shadow_reset(void* self) noexcept {
        for (auto& processor : static_cast<Impl*>(self)->processors)
            (void)processor->reset(detail::StreamingResetReason::TransportRestart);
    }

    // Offline fence: finish the queued shadow work on this thread, then let
    // the Worker's unconsumed results seed the inline delay ring so the next
    // callbacks continue the same timeline.
    void switch_worker_to_inline() noexcept {
        worker.stop();
        (void)worker.pump();
        const auto n = block_size();
        const auto channels = config.channels;
        const auto lead = config.lead_blocks;
        const auto block_samples = static_cast<std::size_t>(channels) * n;
        // The next callback is shadow_sequence; slot (cursor + k) must hold the
        // result for shadow_sequence - lead + k.
        for (std::uint32_t k = 0; k < lead; ++k) {
            float* slot = shadow_ring.data() + ((shadow_cursor + k) % lead) * block_samples;
            std::fill_n(slot, block_samples, 0.0f);
            if (shadow_sequence + k < first_sequence + lead)
                continue;
            const auto target = shadow_sequence + k - lead;
            detail::StreamingBlockStamp stamp;
            while (const float* result = worker.front(stamp)) {
                if (stamp.epoch == shadow_epoch && stamp.sequence < target) {
                    worker.pop_front();
                    continue;
                }
                if (stamp.epoch == shadow_epoch && stamp.sequence == target) {
                    std::copy_n(result, block_samples, slot);
                    worker.pop_front();
                }
                break;
            }
        }
        shadow_mode = ShadowPolicy::Inline;
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
        prewarm_input.assign(count, 0.0f);
        prewarm_output.assign(count, 0.0f);
        for (std::uint32_t block = 0; block < config.prewarm_blocks; ++block) {
            const detail::StreamingBlock request{
                .stamp = {.epoch = epoch, .sequence = sequence + block},
                .input = prewarm_input,
                .output = prewarm_output,
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
        callback_next.store(sequence, std::memory_order_relaxed);
        watchdog_limit = c.watchdog_blocks != 0 ? c.watchdog_blocks : c.capacity - 1;
        completion_mark = sequence;
        device_lost.store(false, std::memory_order_relaxed);
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

    // The block the provider has not delivered: the oldest in flight, or the
    // next to be admitted when nothing is in flight.
    std::uint64_t first_undelivered() const noexcept {
        return pending_count != 0 ? pending_ring[pending_head].stamp.sequence : next_input;
    }
    void latch_loss(std::uint64_t at) noexcept {
        device_lost.store(true, std::memory_order_release);
        fail(detail::SharedIoRecoveryReason::ProviderLost, at);
    }
    // Device loss is the root cause of the failures and silence it produces,
    // so once observed it replaces those latched symptoms. Independent causes
    // (a sequence gap, an invalid callback) stay first.
    detail::SharedIoRecoveryReason recovery() const noexcept {
        using Recovery = detail::SharedIoRecoveryReason;
        const auto latched =
            bridge ? bridge->recovery_reason() : cpu_recovery.load(std::memory_order_acquire);
        if (device_lost.load(std::memory_order_acquire) &&
            (latched == Recovery::ProviderFailure || latched == Recovery::ProviderUnresponsive))
            return Recovery::ProviderLost;
        return latched;
    }

    std::uint32_t collect(std::uint64_t deadline, bool* progressed = nullptr) noexcept {
        auto& backend = *model->backend;
        if (backend.service_until(deadline) != 0 && progressed != nullptr)
            *progressed = true;
        // Loss outranks the failed completions it causes, so it is latched
        // before any of this pass's terminals can latch ProviderFailure.
        if (backend.device_lost() && !device_lost.load(std::memory_order_relaxed))
            latch_loss(first_undelivered());
        const auto failure = device_lost.load(std::memory_order_relaxed)
                                 ? detail::SharedIoRecoveryReason::ProviderLost
                                 : detail::SharedIoRecoveryReason::ProviderFailure;
        detail::StreamingTerminal terminal;
        while (backend.receive(terminal)) {
            auto* pending = find_pending(terminal.stamp);
            if (pending == nullptr) {
                fail(failure, terminal.stamp.sequence);
                continue;
            }
            pending->done = true;
            pending->disposition = terminal.disposition;
            if (terminal.disposition == Terminal::ProviderFailed)
                fail(failure, terminal.stamp.sequence);
            else if (terminal.disposition == Terminal::DeviceLost)
                latch_loss(terminal.stamp.sequence);
        }
        std::uint32_t produced = 0;
        while (pending_count != 0) {
            auto& pending = pending_ring[pending_head];
            if (!pending.done)
                break;
            // Late is a stream-position fact: the callback that would consume
            // this block has finished, so publishing it would only fill the
            // egress ring with a result nobody can take.
            const bool consumer_passed = callback_next.load(std::memory_order_acquire) >
                                         pending.stamp.sequence + config.lead_blocks;
            if (pending.disposition == Terminal::Completed && !consumer_passed &&
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

    // A provider is unresponsive when it completes nothing for
    // `watchdog_limit` callback blocks while work is in flight. Silence counts
    // from the later of the last completion and the oldest in-flight
    // admission, so an idle stretch never counts against it, and a provider
    // that keeps completing, however late, is slow rather than unresponsive.
    void watch_provider(bool progressed) noexcept {
        const auto position = callback_next.load(std::memory_order_acquire);
        if (progressed) {
            completion_mark = position;
            return;
        }
        if (pending_count == 0 || bridge->delivery_epoch() == 0)
            return;
        // The oldest in-flight block was admitted after its own callback.
        const auto since = std::max(completion_mark, first_undelivered() + 1);
        // A preempted callback can leave the position behind; never wrap.
        const auto silent = position > since ? position - since : 0;
        if (silent >= watchdog_limit)
            fail(detail::SharedIoRecoveryReason::ProviderUnresponsive, first_undelivered());
    }
};

GpuNeuralRealtimeNode::GpuNeuralRealtimeNode(Config config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

GpuNeuralRealtimeNode::~GpuNeuralRealtimeNode() {
    // A failed release leaves the backend holding this node's work, so the
    // model stays bound and no other node can adopt that state.
    if (!release())
        return;
    if (impl_->model_owner)
        impl_->model->bound.store(false, std::memory_order_release);
    if (impl_->shadow_model_owner)
        impl_->shadow_model->bound.store(false, std::memory_order_release);
}

GpuAudioNodeDescriptor GpuNeuralRealtimeNode::descriptor() const {
    const auto& c = impl_->config;
    return {.name = "Neural realtime",
            .input_channels = c.channels,
            .output_channels = c.channels,
            .block_size = impl_->block_size(),
            .sample_rate = impl_->model ? impl_->model->sample_rate : 0,
            .latency_blocks = c.lead_blocks,
            // CpuFallback for every policy: the transport must keep feeding the
            // model and call back on a miss even when None answers silence.
            .miss_policy = MissPolicy::CpuFallback,
            .supports_cpu_fallback = true};
}

bool GpuNeuralRealtimeNode::prepare() {
    auto& s = *impl_;
    const auto fail = [&s, this](PrepareError error) {
        (void)release();
        s.last_error = error;
        return false;
    };
    if (!release()) {
        s.last_error = PrepareError::ReleaseFailed;
        return false;
    }
    s.last_error = s.config_error();
    if (s.last_error != PrepareError::None)
        return false;
    if (!s.model_owner) {
        bool expected = false;
        if (!s.model->bound.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            s.last_error = PrepareError::ModelBound;
            return false;
        }
        s.model_owner = true;
    }
    if (s.shadow_model && !s.shadow_model_owner) {
        bool expected = false;
        if (!s.shadow_model->bound.compare_exchange_strong(expected, true,
                                                           std::memory_order_acq_rel)) {
            s.last_error = PrepareError::ModelBound;
            return false;
        }
        s.shadow_model_owner = true;
    }
    try {
        const auto n = s.block_size();
        const auto channels = s.config.channels;
        s.shadow_ring.assign(static_cast<std::size_t>(s.config.lead_blocks) * channels * n, 0.0f);
        s.shadow_due.assign(static_cast<std::size_t>(channels) * n, 0.0f);
        s.shadow_input.assign(n, 0.0f);
        s.shadow_cursor = 0;
        s.shadow_published = true;
        s.inline_cost_pct = -1.0f;
        ++s.shadow_epoch;
        const auto& source = s.shadow_source();
        for (std::uint32_t ch = 0; ch < channels; ++ch) {
            auto& model = *source.channel_models[ch];
            auto processor = std::make_unique<detail::NeuralProcessor>(model);
            if (!processor->prepare(
                    s.context_for(model.spec(), s.model->backend != nullptr, &source),
                    detail::NeuralProviderPreference::CpuOnly)) {
                (void)processor->release();
                return fail(PrepareError::ShadowPrepareFailed);
            }
            if (s.test_skip_publish || !processor->publish())
                s.shadow_published = false;
            s.processors.push_back(std::move(processor));
        }
        // The shadow is the miss policy, so a node without a published shadow
        // must not reach the transport.
        if (!cpu_fallback_prepared())
            return fail(PrepareError::ShadowUnpublished);

        if (s.model->backend) {
            auto& backend = *s.model->backend;
            ++s.epoch;
            const auto context = s.context_for(s.model->backend_spec, true);
            if (backend.prepare(context) &&
                backend.begin_epoch(s.epoch, detail::StreamingResetReason::TransportRestart) &&
                s.prewarm_backend(backend)) {
                s.backend_active = true;
                s.production_backend = s.model->production_backend();
                s.backend_quiesced.store(false, std::memory_order_release);
            } else if (!backend.release()) {
                return fail(PrepareError::ReleaseFailed);
            }
        }
        // A GPU family whose provider could not be prepared runs on the CPU,
        // and says why. Latched once the CPU path is reset below.
        const bool gpu_unavailable = s.model->backend && !s.backend_active;
        if (!s.measure_inline_cost())
            return fail(PrepareError::ShadowPrepareFailed);
        // Without a GPU backend the CPU model is the output, so it runs
        // inline whatever was requested. Auto weighs the whole callback: one
        // channel's cost times the channel count.
        const auto requested = s.config.shadow_policy;
        const float total_cost = s.inline_cost_pct.load() * static_cast<float>(channels);
        if (!s.backend_active || requested == ShadowPolicy::Inline)
            s.shadow_mode = ShadowPolicy::Inline;
        else if (requested == ShadowPolicy::Worker || requested == ShadowPolicy::None)
            s.shadow_mode = requested;
        else
            s.shadow_mode =
                total_cost <= kAutoInlineMaxCostPct ? ShadowPolicy::Inline : ShadowPolicy::Worker;

        // Silent prewarm runs through the shadow too, so its causal history
        // starts at the same stream position as the GPU history.
        if (s.config.prewarm_blocks != 0) {
            std::vector<float> zero(n, 0.0f), discard(n, 0.0f);
            std::vector<const float*> inputs(channels, zero.data());
            std::vector<float*> outputs(channels, discard.data());
            for (std::uint32_t block = 0; block < s.config.prewarm_blocks; ++block)
                s.run_shadow(inputs.data(), channels, outputs.data(), n, s.sequence + block);
        }
        s.sequence += s.config.prewarm_blocks;
        s.shadow_sequence = s.sequence;
        s.first_sequence = s.sequence;
        s.shadow_hit = false;
        s.shadow_delivered.store(0, std::memory_order_relaxed);
        s.shadow_double_miss.store(0, std::memory_order_relaxed);
        if (s.worker_fed()) {
            s.shadow_take.assign(static_cast<std::size_t>(channels) * n, 0.0f);
            const auto period_ns =
                static_cast<std::uint64_t>(1e9 * static_cast<double>(n) / s.model->sample_rate);
            if (!s.worker.start({.capacity = s.config.shadow_queue_blocks,
                                 .samples_per_block = static_cast<std::size_t>(channels) * n,
                                 .poll_ns = std::max<std::uint64_t>(period_ns / 4, 20'000),
                                 .audio_workgroup = s.config.shadow_audio_workgroup,
                                 .spawn_thread = !s.manual_shadow},
                                &s, &Impl::shadow_step, &Impl::shadow_reset))
                return fail(PrepareError::ShadowPrepareFailed);
        }

        if (s.backend_active) {
            if (!s.initialize_bridge())
                return fail(PrepareError::BridgeFailed);
        } else {
            s.cpu_next_sequence = s.sequence;
            s.cpu_blocks.store(0, std::memory_order_relaxed);
            s.cpu_recovery.store(detail::SharedIoRecoveryReason::None, std::memory_order_release);
            s.cpu_recovery_sequence.store(std::numeric_limits<std::uint64_t>::max(),
                                          std::memory_order_release);
            if (gpu_unavailable)
                s.fail(detail::SharedIoRecoveryReason::ProviderFailure, s.sequence);
        }
        s.sanitized_samples.store(0, std::memory_order_relaxed);
        s.contract_violations.store(0, std::memory_order_relaxed);
        s.prepared = true;
        return true;
    } catch (...) {
    }
    return fail(PrepareError::InvalidConfig);
}

bool GpuNeuralRealtimeNode::release() noexcept {
    auto& s = *impl_;
    if (s.prepared) {
        // Keep the absolute stream position across reprepare.
        s.sequence = s.bridge ? s.bridge->next_sequence() : s.cpu_next_sequence;
    }
    s.prepared = false;
    // The shadow thread steps the processors; join it before releasing them.
    s.worker.stop();
    s.shadow_mode = ShadowPolicy::None;
    if (s.bridge)
        s.bridge->request_recovery(detail::SharedIoRecoveryReason::OfflineFence);
    if (s.backend_active) {
        if (!s.model->backend->release())
            return false;
        s.backend_active = false;
        s.production_backend = false;
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
    if (s.worker_fed()) {
        // Copy only: the shadow thread runs the model.
        s.shadow_hit = false;
        const float* inputs[64];
        const auto count =
            std::min<std::uint32_t>(static_cast<std::uint32_t>(input.num_channels()), channels);
        for (std::uint32_t ch = 0; ch < count; ++ch)
            inputs[ch] = input.channel_ptr(ch);
        (void)s.worker.push_input({.epoch = s.shadow_epoch, .sequence = s.shadow_sequence}, inputs,
                                  count, n);
        ++s.shadow_sequence;
        return;
    }
    if (s.shadow_mode != ShadowPolicy::Inline)
        return;
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
    s.run_shadow(inputs, input_channels, outputs, n, s.shadow_sequence);
    ++s.shadow_sequence;
    s.shadow_cursor = (s.shadow_cursor + 1) % s.config.lead_blocks;
}

void GpuNeuralRealtimeNode::process_cpu_fallback(const audio::BufferView<const float>&,
                                                 audio::BufferView<float>& output,
                                                 std::uint32_t n) noexcept {
    auto& s = *impl_;
    for (std::size_t ch = 0; ch < output.num_channels(); ++ch)
        std::fill_n(output.channel_ptr(ch), std::min<std::size_t>(n, output.num_samples()), 0.0f);
    if (!s.prepared || n != s.block_size() || output.num_samples() < n ||
        s.shadow_mode == ShadowPolicy::None)
        return;
    const float* source = s.shadow_due.data();
    if (s.shadow_mode == ShadowPolicy::Worker) {
        if (!s.shadow_hit) {
            // Both the GPU and the shadow missed this slot: stay silent.
            s.shadow_double_miss.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        s.shadow_hit = false;
        source = s.shadow_take.data();
    }
    s.shadow_delivered.fetch_add(1, std::memory_order_relaxed);
    const auto channels = std::min<std::uint32_t>(
        s.config.channels, static_cast<std::uint32_t>(output.num_channels()));
    for (std::uint32_t ch = 0; ch < channels; ++ch)
        std::copy_n(source + static_cast<std::size_t>(ch) * n, n, output.channel_ptr(ch));
}

void GpuNeuralRealtimeNode::process_block(const audio::BufferView<const float>&,
                                          audio::BufferView<float>&, std::uint32_t) {
    throw std::logic_error("GpuNeuralRealtimeNode requires the stamped transport");
}

NeuralFamily GpuNeuralRealtimeNode::family() const noexcept {
    return impl_->config.family;
}

bool GpuNeuralRealtimeNode::gpu_kernel_available() const noexcept {
    return impl_->prepared && impl_->backend_active &&
           !impl_->backend_quiesced.load(std::memory_order_acquire);
}

std::uint64_t GpuNeuralRealtimeNode::epoch() const noexcept {
    return impl_->bridge ? impl_->bridge->delivery_epoch() : 0;
}

bool GpuNeuralRealtimeNode::fenced() const noexcept {
    if (!impl_->prepared)
        return true;
    // CPU-only execution has no GPU delivery to fence.
    return impl_->bridge && impl_->bridge->delivery_epoch() == 0;
}

std::uint64_t GpuNeuralRealtimeNode::callback_contract_violations() const noexcept {
    return impl_->contract_violations.load(std::memory_order_relaxed);
}

std::uint64_t GpuNeuralRealtimeNode::sanitized_input_samples() const noexcept {
    return impl_->sanitized_samples.load(std::memory_order_relaxed);
}

GpuNeuralRealtimeNode::PrepareError GpuNeuralRealtimeNode::last_prepare_error() const noexcept {
    return impl_->last_error;
}

GpuAudioProvider GpuNeuralRealtimeNode::transport_provider() const noexcept {
    if (authenticated_provider())
        return GpuAudioProvider::Dawn;
    return impl_->prepared && !impl_->backend_active ? GpuAudioProvider::Cpu
                                                     : GpuAudioProvider::Unknown;
}

GpuAudioRecoveryReason GpuNeuralRealtimeNode::recovery_reason() const noexcept {
    return public_reason(impl_->recovery());
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
    // Under None a realtime miss is silent; the primed model serves offline
    // renders only, so it is not a fallback a host can rely on in realtime.
    const bool silent_miss = s.shadow_mode.load(std::memory_order_relaxed) == ShadowPolicy::None;
    report.fallback_policy = silent_miss ? MissPolicy::Silence : MissPolicy::CpuFallback;
    report.prepared_lead_blocks = s.config.lead_blocks;
    report.prepared = true;
    report.fallback_available = !silent_miss && cpu_fallback_prepared();
    report.diagnostics_available = true;
    report.prepared_latency_blocks = s.config.lead_blocks;
    report.gpu_kernel_available = gpu_kernel_available();
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
    const bool silent_miss = s.shadow_mode.load(std::memory_order_relaxed) == ShadowPolicy::None;
    status.fallback_policy = silent_miss ? MissPolicy::Silence : MissPolicy::CpuFallback;
    status.sample_rate = s.model->sample_rate;
    status.block_size = n;
    status.latency_samples = s.config.lead_blocks * n;
    status.prepared_lead_blocks = s.config.lead_blocks;
    status.fallback_available = !silent_miss && cpu_fallback_prepared();
    status.diagnostics_available = true;
    status.recovery_reason = recovery_reason();
    status.recovery_sequence = recovery_sequence();
    status.epoch = epoch();
    status.delivery_epoch_active = epoch() != 0;
    status.gpu_kernel_available = gpu_kernel_available();
    detail::RealtimeGpuHealth shadow;
    s.fill_shadow(shadow);
    status.shadow_mode = shadow.shadow_mode;
    status.shadow_inline_cost_pct = shadow.shadow_inline_cost_pct;
    status.shadow_delivered_blocks = shadow.shadow_delivered_blocks;
    status.shadow_late_blocks = shadow.shadow_late_blocks;
    status.shadow_double_miss_blocks = shadow.shadow_double_miss_blocks;
    status.shadow_model_differs = shadow.shadow_model_differs;
    // A model with a GPU kernel asked for the GPU; the others asked for CPU.
    status.requested_engine = s.model->backend ? GpuAudioEngine::Gpu : GpuAudioEngine::Cpu;
    if (!s.backend_active) {
        // CPU-only execution is a prepared CPU provider. It is Ready when
        // that is what was requested and Degraded when a GPU was requested
        // but its provider could not be prepared.
        status.selected_engine = GpuAudioEngine::Cpu;
        status.provider = GpuAudioProvider::Cpu;
        status.provider_state =
            s.model->backend || status.recovery_reason != GpuAudioRecoveryReason::None
                ? GpuAudioProviderState::Degraded
                : GpuAudioProviderState::Ready;
        status.fallback_blocks = s.cpu_blocks.load(std::memory_order_relaxed);
        return status;
    }
    const auto telemetry = s.telemetry.snapshot();
    status.produced_blocks = telemetry.delivered_blocks;
    status.missed_blocks = telemetry.deadline_misses;
    // Substitutions actually taken from the shadow; a double miss is silent.
    status.fallback_blocks = s.shadow_delivered.load(std::memory_order_relaxed);
    status.resynced_blocks = telemetry.resync_drops;
    status.input_dropped_frames = telemetry.input_drops;
    status.last_block_us = static_cast<double>(telemetry.submit_to_completion_ns) / 1000.0;
    status.provider = authenticated_provider() ? GpuAudioProvider::Dawn : GpuAudioProvider::Unknown;
    status.selected_engine =
        authenticated_provider() ? GpuAudioEngine::Gpu : GpuAudioEngine::Unknown;
    // Same projection the transport applies to this node's health: a lost or
    // unresponsive provider is Unavailable, any other latched cause Degraded.
    detail::RealtimeGpuHealth health;
    (void)GpuNeuralRealtimeNode::health(this, health);
    status.provider_state = detail::provider_state_for_health(health);
    if (status.provider_state == GpuAudioProviderState::Ready && !authenticated_provider())
        status.provider_state = GpuAudioProviderState::Degraded;
    return status;
}

bool GpuNeuralRealtimeNode::ready() const noexcept {
    return impl_->prepared;
}

bool GpuNeuralRealtimeNode::authenticated_provider() const noexcept {
    return impl_->prepared && impl_->backend_active && impl_->production_backend;
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
        if (shape_ok) {
            std::uint64_t replaced = 0;
            for (std::uint32_t ch = 0; ch < s.config.channels; ++ch)
                replaced += detail::count_nonfinite(input.channel_ptr(ch), n);
            if (replaced != 0)
                s.sanitized_samples.fetch_add(replaced, std::memory_order_relaxed);
        }
        // Nothing is fenced on this path and the CPU model keeps delivering,
        // so a contract violation is counted rather than latched as a
        // recovery that would leave the node Degraded for good.
        if (!shape_ok || sequence != s.cpu_next_sequence)
            s.contract_violations.fetch_add(1, std::memory_order_relaxed);
        if (sequence >= s.cpu_next_sequence && sequence < Bridge::kSequenceLimit)
            s.cpu_next_sequence = sequence + 1;
        return detail::kRealtimeGpuMissed;
    }
    if (!shape_ok) {
        s.fail(detail::SharedIoRecoveryReason::InvalidCallback, sequence);
        std::fill(s.callback_input.begin(), s.callback_input.end(), 0.0f);
    } else {
        // The GPU gets exactly what the shadow gets: non-finite samples as 0.
        std::uint64_t replaced = 0;
        for (std::uint32_t ch = 0; ch < s.config.channels; ++ch)
            replaced += detail::sanitize_nonfinite(input.channel_ptr(ch),
                                                   s.callback_input.data() + ch * n, n);
        if (replaced != 0)
            s.sanitized_samples.fetch_add(replaced, std::memory_order_relaxed);
    }
    s.callback = s.bridge->begin_callback(s.callback_input, sequence, start);
    const auto delivery = s.bridge->consume_output(s.callback, s.callback_output, nullptr, true);
    // Consumption is over: the watchdog may count this block.
    s.callback_next.store(sequence + 1, std::memory_order_release);
    // The Worker shadow's result for the slot this callback emits, if any.
    const bool worker = s.worker_fed() && sequence >= s.first_sequence + s.config.lead_blocks;
    const bool substitute = s.shadow_mode.load(std::memory_order_relaxed) == ShadowPolicy::Worker;
    const detail::StreamingBlockStamp expected{
        .epoch = s.shadow_epoch, .sequence = worker ? sequence - s.config.lead_blocks : 0};
    if (delivery == Bridge::Delivery::Ready) {
        if (worker)
            s.worker.discard_through(expected);
        for (std::uint32_t ch = 0; ch < s.config.channels; ++ch)
            std::copy_n(s.callback_output.data() + ch * n, n, output.channel_ptr(ch));
        return detail::kRealtimeGpuReady;
    }
    if (delivery == Bridge::Delivery::Priming)
        return detail::kRealtimeGpuPriming;
    // GPU miss: the shadow egress is consulted next; process_cpu_fallback()
    // copies the result or, on a double miss, stays silent.
    // None keeps the egress drained but never substitutes in realtime.
    if (worker && substitute)
        s.shadow_hit = s.worker.take_output(expected, s.shadow_take.data());
    else if (worker)
        s.worker.discard_through(expected);
    return detail::kRealtimeGpuMissed;
}

std::uint32_t GpuNeuralRealtimeNode::service(void* self, std::uint64_t) noexcept {
    auto& s = *static_cast<GpuNeuralRealtimeNode*>(self)->impl_;
    if (!s.prepared || !s.backend_active)
        return detail::kRealtimeGpuServiceInactive;
    const auto n = s.block_size();
    const auto pump_now = now_ns();
    const auto wait_ns = s.config.completion_service_wait_ns;
    const auto deadline = wait_ns != 0 && pump_now <= UINT64_MAX - wait_ns ? pump_now + wait_ns : 0;
    // Collect before watching, so this pump's completions count as progress.
    bool progressed = false;
    auto produced = s.collect(deadline, &progressed);
    s.watch_provider(progressed);
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
            if (s.model->backend->device_lost())
                s.latch_loss(stamp.sequence);
            else
                s.fail(detail::SharedIoRecoveryReason::ProviderFailure, stamp.sequence);
        } else {
            admitted = true;
        }
        ++s.next_input;
        ++s.pending_count;
    }
    s.bridge->end_worker_admission();
    // A completion seen only in this tail pass is progress too: the next
    // pump's watchdog check must count silence from it.
    progressed = false;
    produced += s.collect(deadline != 0 && admitted ? deadline : 0, &progressed);
    if (progressed)
        s.watch_provider(true);
    return produced;
}

bool GpuNeuralRealtimeNode::fence(void* self) noexcept {
    auto& s = *static_cast<GpuNeuralRealtimeNode*>(self)->impl_;
    // An offline render is synchronous; the asynchronous shadow cannot keep
    // up with it, so it continues inline from the same timeline.
    if (s.worker_fed())
        s.switch_worker_to_inline();
    if (!s.bridge || !s.backend_active)
        return true;
    // The callback is idle during the offline fence, so its position is stable.
    s.fail(detail::SharedIoRecoveryReason::OfflineFence, s.bridge->next_sequence());
    if (!s.model->backend->quiesce())
        return false;
    s.backend_quiesced.store(true, std::memory_order_release);
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
    out.delivery_epoch_active = out.epoch != 0;
    out.device_lost = node.impl_->device_lost.load(std::memory_order_acquire);
    out.gpu_kernel_available = node.gpu_kernel_available();
    if (node.impl_->prepared)
        node.impl_->fill_shadow(out);
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

GpuNeuralRealtimeNode::ShadowStatus GpuNeuralRealtimeNode::shadow_status() const noexcept {
    const auto& s = *impl_;
    ShadowStatus status;
    status.requested = s.config.shadow_policy;
    const float per_channel = s.inline_cost_pct.load();
    status.inline_cost_per_channel_pct = per_channel;
    status.inline_cost_total_pct =
        per_channel < 0.0f ? per_channel : per_channel * static_cast<float>(s.config.channels);
    const auto counters = s.worker.counters();
    status.shadow_input_dropped = counters.input_dropped;
    status.shadow_resyncs = counters.resyncs;
    status.workgroup_joined = counters.workgroup_joined;
    return status;
}

void detail::NeuralRealtimeTestAccess::inject_shadow_cost(GpuNeuralRealtimeNode& node,
                                                          float pct) noexcept {
    node.impl_->injected_cost_pct = pct;
}

void detail::NeuralRealtimeTestAccess::manual_shadow(GpuNeuralRealtimeNode& node,
                                                     bool manual) noexcept {
    node.impl_->manual_shadow = manual;
}

std::size_t detail::NeuralRealtimeTestAccess::pump_shadow(GpuNeuralRealtimeNode& node,
                                                          std::size_t max_blocks) noexcept {
    return node.impl_->worker.pump(max_blocks);
}

std::uint64_t
detail::NeuralRealtimeTestAccess::shadow_processed(const GpuNeuralRealtimeNode& node) noexcept {
    return node.impl_->worker.counters().processed;
}

void detail::NeuralRealtimeTestAccess::skip_shadow_publish(GpuNeuralRealtimeNode& node,
                                                           bool skip) noexcept {
    node.impl_->test_skip_publish = skip;
}

void detail::NeuralRealtimeTestAccess::request_recovery(GpuNeuralRealtimeNode& node,
                                                        SharedIoRecoveryReason reason) noexcept {
    auto& s = *node.impl_;
    s.fail(reason, s.bridge ? s.bridge->next_sequence() : s.cpu_next_sequence);
}

detail::RealtimeGpuNodePath
detail::NeuralRealtimeTestAccess::path(GpuNeuralRealtimeNode& node) noexcept {
    return realtime_gpu_node_path(&node);
}

} // namespace pulp::gpu_audio
