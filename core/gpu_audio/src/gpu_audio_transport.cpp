#include <pulp/gpu_audio/gpu_audio_transport.hpp>

#include "detail/realtime_gpu_audio_path.hpp"
#include "detail/shared_io_trace.hpp"

#include <algorithm>

namespace pulp::gpu_audio {

bool GpuAudioTransport::prepare(GpuAudioNode* node, const Config& config) {
    release();
    if (node == nullptr)
        return false;

    // The descriptor is the single source of truth for channels/block/latency.
    const GpuAudioNodeDescriptor desc = node->descriptor(); // host thread — may allocate
    if (desc.input_channels == 0 || desc.output_channels == 0 ||
        desc.input_channels != desc.output_channels) {
        return false; // 3a: symmetric channel count only
    }
    if (desc.block_size == 0 || desc.latency_blocks == 0)
        return false;
    // CpuFallback miss policy requires a real fallback.
    if (desc.miss_policy == MissPolicy::CpuFallback && !desc.supports_cpu_fallback) {
        return false;
    }
    // Need the primed latency plus one in-flight block and one free slot.
    if (config.ring_blocks < desc.latency_blocks + 2)
        return false;

    node_ = node;
    channels_ = desc.output_channels;
    block_size_ = desc.block_size;
    latency_blocks_ = desc.latency_blocks;
    ring_blocks_ = config.ring_blocks;
    miss_policy_ = desc.miss_policy;
    const auto realtime_path = detail::realtime_gpu_node_path(node);
    if (realtime_path.active()) {
        realtime_gpu_context_ = realtime_path.context;
        realtime_gpu_process_ = realtime_path.process;
        realtime_gpu_service_ = realtime_path.service;
        realtime_gpu_fence_ = realtime_path.fence;
        realtime_gpu_delivered_ = realtime_path.delivered;
        callback_sequence_ = realtime_path.next_sequence(realtime_path.context);
    }

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
    // Wake-on-write only matters when we own the worker thread; when the caller
    // drives pump() there is nothing waiting on the semaphore to wake.
    wake_on_write_ = config.wake_on_write && config.run_worker_thread;
    prepared_ = true;

    if (config.run_worker_thread) {
        // Poll a few times per block so the worker reliably keeps the output
        // ring filled within the latency budget; clamp to a sane range.
        const double block_us = (desc.sample_rate > 0)
                                    ? (1.0e6 * static_cast<double>(block_size_) / desc.sample_rate)
                                    : 800.0;
        long iv = static_cast<long>(block_us / 4.0);
        if (iv < 50)
            iv = 50;
        if (iv > 2000)
            iv = 2000;
        poll_interval_ = std::chrono::microseconds(iv);
        worker_running_.store(true, std::memory_order_release);
        worker_ = std::thread([this] { worker_loop(); });
    }
    return true;
}

void GpuAudioTransport::worker_loop() noexcept {
    while (worker_running_.load(std::memory_order_acquire)) {
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
            (void)wake_sem_.try_acquire_for(poll_interval_);
        } else {
            std::this_thread::sleep_for(poll_interval_);
        }
    }
    std::lock_guard<std::mutex> lock(pump_mutex_);
    pump(); // final drain of any input left at stop
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
    for (uint32_t b = 0; b < latency_blocks_; ++b) {
        (void)output_ring_.write(silence, block_size_);
    }
}

void GpuAudioTransport::release() noexcept {
    // Stop the worker before tearing down the rings/buffers it touches.
    worker_running_.store(false, std::memory_order_release);
    if (worker_.joinable())
        worker_.join();

    input_ring_.release();
    output_ring_.release();
    in_fptrs_.clear();
    in_cptrs_.clear();
    out_fptrs_.clear();
    rejected_input_ptrs_.clear();
    rejected_output_ptrs_.clear();
    node_ = nullptr;
    realtime_gpu_context_ = nullptr;
    realtime_gpu_process_ = nullptr;
    realtime_gpu_service_ = nullptr;
    realtime_gpu_fence_ = nullptr;
    realtime_gpu_delivered_ = nullptr;
    callback_sequence_ = 0;
    realtime_gpu_fenced_for_offline_ = false;
    channels_ = block_size_ = latency_blocks_ = ring_blocks_ = 0;
    prepared_ = false;
}

void GpuAudioTransport::process(const audio::BufferView<const float>& input,
                                audio::BufferView<float>& output, uint32_t n) noexcept {
    if (!prepared_) {
        output.clear();
        return;
    }
    const auto sequence = callback_sequence_++;
    synchronous_.store(false, std::memory_order_release);
    if (n != block_size_ || input.num_channels() < channels_ || output.num_channels() < channels_ ||
        input.num_samples() < n || output.num_samples() < n) {
        process_invalid_position(output, sequence, false);
        return;
    }
    process_realtime_position(input, output, n, sequence, true);
}

void GpuAudioTransport::process_realtime_position(const audio::BufferView<const float>& input,
                                                  audio::BufferView<float>& output, uint32_t n,
                                                  std::uint64_t sequence,
                                                  bool input_valid) noexcept {
    if (realtime_gpu_process_ != nullptr) {
        process_shared(input, output, n, sequence, input_valid);
        return;
    }

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
            wake_sem_.release();
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
    // read. Counting misses is immune to that. We only drain while a block also
    // remains behind the dropped one (`avail_blocks > 1`), so a resync never
    // starves the read. drain() just advances the read cursor — no copy/alloc.
    if (blocks_owed_ > 0) {
        const std::uint64_t avail_blocks = output_ring_.available_frames() / n;
        if (avail_blocks > 1) {
            const std::uint64_t drop = std::min<std::uint64_t>(blocks_owed_, avail_blocks - 1);
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
    if (output_ring_.read(output, n))
        return;

    // Miss: the worker has not produced this block in time. Substitute per the
    // policy and record the debt so the late wet block is dropped once it lands.
    miss_blocks_.fetch_add(1, std::memory_order_relaxed);
    ++blocks_owed_;
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

void GpuAudioTransport::process_shared(const audio::BufferView<const float>& input,
                                       audio::BufferView<float>& output, std::uint32_t n,
                                       std::uint64_t sequence, bool input_valid) noexcept {
    using detail::SharedIoDeliveryDisposition;
    if (miss_policy_ == MissPolicy::CpuFallback)
        node_->prime_fallback(input, n);
    const auto status =
        realtime_gpu_process_(realtime_gpu_context_, input, output, n, sequence, input_valid);
    auto delivered = SharedIoDeliveryDisposition::GpuDelivered;
    if (status == detail::kRealtimeGpuPriming) {
        output.clear();
        delivered = SharedIoDeliveryDisposition::Priming;
    } else if (status != detail::kRealtimeGpuReady) {
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
    realtime_gpu_delivered_(realtime_gpu_context_, sequence, static_cast<std::uint8_t>(delivered));
    if (wake_on_write_)
        wake_sem_.release();
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
        process_realtime_position(zero_input, discarded_output, block_size_, sequence, false);
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
        if (!realtime_gpu_fenced_for_offline_)
            realtime_gpu_fenced_for_offline_ = realtime_gpu_fence_(realtime_gpu_context_);
        // A failed barrier retains its owner and stays CPU-only. The same
        // continuously primed fallback carries the RT/offline timeline.
        process_shared(input, output, n, sequence, input_valid);
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
}

GpuAudioTransport::Stats GpuAudioTransport::stats() const noexcept {
    Stats s;
    s.produced_blocks = produced_blocks_.load(std::memory_order_relaxed);
    s.miss_blocks = miss_blocks_.load(std::memory_order_relaxed);
    s.input_dropped_frames = input_dropped_blocks_.load(std::memory_order_relaxed) * block_size_;
    s.resynced_blocks = resynced_blocks_.load(std::memory_order_relaxed);
    s.last_block_us = last_block_ns_.load(std::memory_order_relaxed) / 1000.0;
    s.avg_block_us = avg_block_ns_.load(std::memory_order_relaxed) / 1000.0;
    return s;
}

} // namespace pulp::gpu_audio
