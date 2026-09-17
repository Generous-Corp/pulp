#include "shared_io_stamped_bridge.hpp"

#include <algorithm>

namespace pulp::gpu_audio::detail {

bool SharedIoStampedBridge::prepare(Config config, std::uint64_t epoch,
                                    std::uint64_t first_sequence) {
    if (prepared_ || !config.capacity || !config.channels || !config.block_size ||
        !config.lead_blocks || config.capacity <= config.lead_blocks || !epoch ||
        first_sequence >= kSequenceLimit)
        return false;
    const auto maximum = std::numeric_limits<std::size_t>::max() / sizeof(float);
    if (std::size_t(config.channels) > maximum / config.block_size)
        return false;
    const auto count = std::size_t(config.channels) * config.block_size;
    if (count > maximum / config.capacity)
        return false;
    try {
        for (auto* queue : {&ingress_, &egress_}) {
            queue->stamps.resize(config.capacity);
            queue->samples.assign(count * config.capacity, 0.f);
        }
    } catch (...) {
        return false;
    }
    sample_count_ = count;
    capacity_ = config.capacity;
    lead_blocks_ = config.lead_blocks;
    next_sequence_ = first_sequence;
    prepared_ = true;
    return activate_epoch(epoch);
}

SharedIoStampedBridge::Publication
SharedIoStampedBridge::publish(Queue& queue, Stamp stamp, std::span<const float> samples) noexcept {
    const auto write = queue.write.load(std::memory_order_relaxed);
    if (write == kNoLease)
        return Publication::CounterExhausted;
    if (write - queue.read.load(std::memory_order_acquire) >= capacity_)
        return Publication::DroppedFull;
    const auto position = write % capacity_;
    queue.stamps[position] = stamp;
    std::copy(samples.begin(), samples.end(), queue.samples.begin() + position * sample_count_);
    queue.write.store(write + 1, std::memory_order_release);
    return Publication::Published;
}

std::optional<SharedIoStampedBridge::Lease> SharedIoStampedBridge::acquire(Queue& queue) noexcept {
    if (!prepared_ || queue.claimed != kNoLease)
        return std::nullopt;
    const auto read = queue.read.load(std::memory_order_relaxed);
    if (read == queue.write.load(std::memory_order_acquire))
        return std::nullopt;
    queue.claimed = read;
    Lease lease;
    lease.queue_ = &queue;
    lease.cursor_ = read;
    lease.stamp_ = queue.stamps[read % capacity_];
    lease.samples_ = {queue.samples.data() + (read % capacity_) * sample_count_, sample_count_};
    return lease;
}

bool SharedIoStampedBridge::release(Queue& queue, const Lease& lease) noexcept {
    if (lease.queue_ != &queue || queue.claimed == kNoLease || queue.claimed != lease.cursor_)
        return false;
    queue.claimed = kNoLease;
    queue.read.store(lease.cursor_ + 1, std::memory_order_release);
    return true;
}

SharedIoStampedBridge::Callback
SharedIoStampedBridge::begin_callback(std::span<const float> samples,
                                      std::uint64_t sequence) noexcept {
    if (!prepared_)
        return {};
    if (callback_open_ || delivery_pending_ || samples.size() != sample_count_) {
        request_recovery(SharedIoRecoveryReason::InvalidCallback);
        return {};
    }
    if (sequence < next_sequence_) {
        request_recovery(SharedIoRecoveryReason::InvalidCallback);
        return {};
    }
    if (sequence > next_sequence_)
        request_recovery(SharedIoRecoveryReason::SequenceGap);
    next_sequence_ = sequence;
    if (next_sequence_ >= kSequenceLimit)
        return {{}, Admission::SequenceExhausted};
    current_ = {delivery_epoch(), next_sequence_++};
    callback_open_ = true;
    observed_delivery_ = Delivery::Invalid;
    if (trace_telemetry_)
        trace_telemetry_->record_callback_block(false);
    trace_output_eligible_ = current_.sequence - epoch_first_sequence_ >= kLeadBlocks;
    if (trace_ && trace_output_eligible_) {
        SharedIoTraceRecord record;
        record.kind = SharedIoTraceKind::Eligible;
        record.generation = trace_->config().generation;
        record.sequence = current_.sequence - kLeadBlocks;
        (void)trace_->publish_callback(record);
    }
    if (current_.epoch == 0)
        return {current_, Admission::CpuOnly};
    const auto result = publish(ingress_, current_, samples);
    if (result != Publication::Published)
        request_recovery(SharedIoRecoveryReason::InputSaturated);
    return {current_, result == Publication::Published ? Admission::Accepted : Admission::Full};
}

bool SharedIoStampedBridge::current_callback(const Callback& callback) const noexcept {
    return prepared_ && callback_open_ && callback.valid() && callback.stamp == current_;
}

SharedIoStampedBridge::Claim
SharedIoStampedBridge::claim_output(const Callback& callback) noexcept {
    if (!current_callback(callback) || egress_.claimed != kNoLease)
        return {};
    Claim result;
    callback_open_ = false;
    if (callback.stamp.sequence - epoch_first_sequence_ < kLeadBlocks) {
        result.delivery = observed_delivery_ = Delivery::Priming;
        return result;
    }
    if (callback.stamp.epoch == 0 || delivery_epoch() != callback.stamp.epoch) {
        result.delivery = Delivery::EpochChanged;
        trace_delivery(result.delivery);
        return result;
    }
    const auto expected = callback.stamp.sequence - kLeadBlocks;
    result.delivery = Delivery::Missing;
    for (std::uint32_t examined = 0; examined < capacity_; ++examined) {
        auto lease = acquire(egress_);
        if (!lease)
            break;
        const auto stamp = lease->stamp();
        if (stamp.epoch < callback.stamp.epoch ||
            (stamp.epoch == callback.stamp.epoch && stamp.sequence < expected)) {
            release(egress_, *lease);
            ++result.stale_drained;
            continue;
        }
        if (stamp.epoch == callback.stamp.epoch && stamp.sequence == expected) {
            result.delivery = Delivery::Ready;
            result.lease = lease;
            // Keep the callback open until its outstanding sample lease returns.
            callback_open_ = true;
        } else {
            // A future front must remain queued. Only this consumer touches the
            // claim marker, so relinquishing the claim does not recycle storage.
            egress_.claimed = kNoLease;
        }
        break;
    }
    if (!result.lease)
        trace_delivery(result.delivery);
    return result;
}

SharedIoStampedBridge::Delivery
SharedIoStampedBridge::finish_output(const Lease& lease, std::span<float> output) noexcept {
    if (!callback_open_ || lease.queue_ != &egress_ || egress_.claimed == kNoLease ||
        egress_.claimed != lease.cursor_)
        return Delivery::Invalid;
    Delivery result = Delivery::Ready;
    if (output.size() != sample_count_)
        result = Delivery::Invalid;
    else if (delivery_epoch() != lease.stamp_.epoch)
        result = Delivery::EpochChanged;
    else {
        std::copy(lease.samples_.begin(), lease.samples_.end(), output.begin());
        if (delivery_epoch() != lease.stamp_.epoch)
            result = Delivery::EpochChanged;
    }
    if (result != Delivery::Ready)
        std::fill_n(output.begin(), std::min(output.size(), sample_count_), 0.f);
    release(egress_, lease);
    callback_open_ = false;
    trace_delivery(result);
    return result;
}

SharedIoStampedBridge::Delivery
SharedIoStampedBridge::consume_output(const Callback& callback, std::span<float> output,
                                      bool* finalized, bool defer_delivery) noexcept {
    const bool owns_callback = current_callback(callback) && egress_.claimed == kNoLease;
    if (finalized)
        *finalized = owns_callback;
    if (owns_callback) {
        defer_delivery_ = defer_delivery;
        delivery_pending_ = defer_delivery;
    }
    if (output.size() != sample_count_) {
        if (current_callback(callback) && egress_.claimed == kNoLease) {
            callback_open_ = false;
            trace_delivery(Delivery::Invalid);
        }
        return Delivery::Invalid;
    }
    const auto claim = claim_output(callback);
    if (claim.lease)
        return finish_output(*claim.lease, output);
    std::fill(output.begin(), output.end(), 0.f);
    return claim.delivery;
}

bool SharedIoStampedBridge::begin_worker_admission() noexcept {
    unsigned expected = 1u;
    return admission_gate_.compare_exchange_strong(expected, 3u, std::memory_order_acq_rel);
}

void SharedIoStampedBridge::end_worker_admission() noexcept {
    admission_gate_.fetch_and(~2u, std::memory_order_acq_rel);
}

std::optional<SharedIoStampedBridge::Lease> SharedIoStampedBridge::acquire_input() noexcept {
    return acquire(ingress_);
}

bool SharedIoStampedBridge::release_input(const Lease& lease) noexcept {
    return release(ingress_, lease);
}

SharedIoStampedBridge::Publication
SharedIoStampedBridge::publish_output(Stamp stamp, std::span<const float> samples) noexcept {
    if (!prepared_ || !stamp.epoch || stamp.epoch != delivery_epoch() ||
        stamp.sequence >= kSequenceLimit || samples.size() != sample_count_ ||
        (finalized_ && stamp.sequence <= finalized_->sequence))
        return Publication::Invalid;
    const auto result = publish(egress_, stamp, samples);
    // A dropped record is still a terminal delivery decision. Offline pumping
    // must not wait for a record that can no longer be published.
    if (result == Publication::Published || result == Publication::DroppedFull)
        finalized_ = stamp;
    return result;
}

void SharedIoStampedBridge::trace_delivery(Delivery delivery) noexcept {
    observed_delivery_ = delivery;
    if (defer_delivery_)
        return;
    if (!trace_output_eligible_)
        return;
    trace_output_eligible_ = false;
    if (trace_telemetry_) {
        if (delivery != Delivery::Ready)
            trace_telemetry_->record_deadline_miss();
        trace_telemetry_->record_delivery(false, false);
    }
    if (!trace_)
        return;
    SharedIoTraceRecord record;
    record.kind = SharedIoTraceKind::Delivery;
    record.generation = trace_->config().generation;
    record.sequence = current_.sequence - kLeadBlocks;
    record.output_eligible = true;
    // This bridge returns zeros on a miss. A higher-level transport may later
    // replace those samples with CPU fallback; this event cannot attest that.
    record.delivery = delivery == Delivery::Ready ? SharedIoDeliveryDisposition::GpuDelivered
                      : delivery == Delivery::Invalid
                          ? SharedIoDeliveryDisposition::InvalidRejected
                          : SharedIoDeliveryDisposition::SilenceDelivered;
    record.delivery_reason = delivery == Delivery::Ready ? SharedIoFallbackReason::None
                             : delivery == Delivery::EpochChanged
                                 ? SharedIoFallbackReason::Teardown
                                 : SharedIoFallbackReason::DeadlineExceeded;
    record.reason = record.delivery_reason;
    (void)trace_->publish_callback(record);
}

bool SharedIoStampedBridge::complete_callback_delivery(
    const Callback& callback, SharedIoDeliveryDisposition actual) noexcept {
    if (!delivery_pending_ || callback_open_ || !callback.valid() || callback.stamp != current_ ||
        actual == SharedIoDeliveryDisposition::None ||
        (actual == SharedIoDeliveryDisposition::Priming && trace_output_eligible_) ||
        (actual == SharedIoDeliveryDisposition::GpuDelivered &&
         observed_delivery_ != Delivery::Ready))
        return false;
    delivery_pending_ = defer_delivery_ = false;
    if (!trace_output_eligible_)
        return true;
    trace_output_eligible_ = false;
    if (trace_telemetry_) {
        if (actual != SharedIoDeliveryDisposition::GpuDelivered)
            trace_telemetry_->record_deadline_miss();
        trace_telemetry_->record_delivery(
            actual == SharedIoDeliveryDisposition::CpuFallbackDelivered, false);
    }
    if (!trace_)
        return true;
    SharedIoTraceRecord record;
    record.kind = SharedIoTraceKind::Delivery;
    record.generation = trace_->config().generation;
    record.sequence = current_.sequence - kLeadBlocks;
    record.output_eligible = true;
    record.delivery = actual;
    if (actual != SharedIoDeliveryDisposition::GpuDelivered) {
        switch (recovery_reason()) {
        case SharedIoRecoveryReason::SequenceGap:
            record.delivery_reason = SharedIoFallbackReason::SequenceGap;
            break;
        case SharedIoRecoveryReason::InputSaturated:
            record.delivery_reason = SharedIoFallbackReason::InputSaturated;
            break;
        case SharedIoRecoveryReason::ProviderLost:
            record.delivery_reason = SharedIoFallbackReason::DeviceLost;
            break;
        case SharedIoRecoveryReason::ProviderFailure:
            record.delivery_reason = SharedIoFallbackReason::CompletionFailed;
            break;
        case SharedIoRecoveryReason::InvalidCallback:
            record.delivery_reason = SharedIoFallbackReason::SequenceGap;
            break;
        case SharedIoRecoveryReason::OfflineFence:
            record.delivery_reason = SharedIoFallbackReason::Teardown;
            break;
        default:
            record.delivery_reason = SharedIoFallbackReason::DeadlineExceeded;
            break;
        }
    }
    record.reason = record.delivery_reason;
    (void)trace_->publish_callback(record);
    return true;
}

void SharedIoStampedBridge::request_recovery(SharedIoRecoveryReason reason) noexcept {
    if (reason == SharedIoRecoveryReason::None)
        return;
    suspend_delivery();
    auto expected = SharedIoRecoveryReason::None;
    (void)recovery_reason_.compare_exchange_strong(expected, reason, std::memory_order_acq_rel);
}

void SharedIoStampedBridge::suspend_delivery() noexcept {
    admission_gate_.fetch_and(~1u, std::memory_order_acq_rel);
    delivery_epoch_.store(0, std::memory_order_release);
}

bool SharedIoStampedBridge::activate_epoch(std::uint64_t epoch) noexcept {
    if (!prepared_ || !epoch || epoch <= last_epoch_ || delivery_epoch() != 0 || callback_open_ ||
        delivery_pending_ || (admission_gate_.load(std::memory_order_acquire) & 2u) ||
        ingress_.claimed != kNoLease || egress_.claimed != kNoLease)
        return false;
    // Both callers are quiescent and no lease is held. Retire the old queued
    // records before enabling admission, otherwise a full old ring can drop
    // the first sequence required by the new epoch's chronological collector.
    ingress_.read.store(ingress_.write.load(std::memory_order_relaxed), std::memory_order_relaxed);
    egress_.read.store(egress_.write.load(std::memory_order_relaxed), std::memory_order_relaxed);
    recovery_reason_.store(SharedIoRecoveryReason::None, std::memory_order_release);
    last_epoch_ = epoch;
    epoch_first_sequence_ = next_sequence_;
    finalized_.reset();
    admission_gate_.store(1u, std::memory_order_release);
    delivery_epoch_.store(epoch, std::memory_order_release);
    return true;
}

} // namespace pulp::gpu_audio::detail
