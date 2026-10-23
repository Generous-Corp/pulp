#include "neural_shadow_worker.hpp"

#include <pulp/audio/workgroup.hpp>
#include <pulp/signal/scoped_flush_denormals.hpp>

#include <algorithm>
#include <chrono>
#include <limits>

#if defined(__APPLE__)
#include <pthread/qos.h>
#endif

namespace pulp::gpu_audio::detail {

bool NeuralShadowRing::prepare(std::uint32_t capacity, std::size_t samples_per_block) {
    if (capacity == 0 || samples_per_block == 0)
        return false;
    try {
        stamps_.assign(capacity, {});
        samples_storage_.assign(static_cast<std::size_t>(capacity) * samples_per_block, 0.0f);
    } catch (...) {
        return false;
    }
    capacity_ = capacity;
    samples_ = samples_per_block;
    reset();
    return true;
}

void NeuralShadowRing::reset() noexcept {
    read_.store(0, std::memory_order_relaxed);
    write_.store(0, std::memory_order_relaxed);
}

float* NeuralShadowRing::begin_push(StreamingBlockStamp stamp) noexcept {
    if (capacity_ == 0)
        return nullptr;
    const auto write = write_.load(std::memory_order_relaxed);
    if (write - read_.load(std::memory_order_acquire) >= capacity_)
        return nullptr;
    const auto slot = write % capacity_;
    stamps_[slot] = stamp;
    return samples_storage_.data() + slot * samples_;
}

void NeuralShadowRing::commit_push() noexcept {
    write_.store(write_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
}

const float* NeuralShadowRing::front(StreamingBlockStamp& stamp) const noexcept {
    const auto read = read_.load(std::memory_order_relaxed);
    if (capacity_ == 0 || read == write_.load(std::memory_order_acquire))
        return nullptr;
    const auto slot = read % capacity_;
    stamp = stamps_[slot];
    return samples_storage_.data() + slot * samples_;
}

void NeuralShadowRing::pop() noexcept {
    read_.store(read_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
}

bool NeuralShadowWorker::start(const Config& config, void* context, StepFn step, ResetFn reset) {
    stop();
    if (step == nullptr || reset == nullptr || config.samples_per_block == 0 ||
        !input_.prepare(config.capacity, config.samples_per_block) ||
        !output_.prepare(config.capacity, config.samples_per_block))
        return false;
    try {
        scratch_.assign(config.samples_per_block, 0.0f);
        missed_slots_.assign(static_cast<std::size_t>(config.capacity) * 2,
                             std::numeric_limits<std::uint64_t>::max());
    } catch (...) {
        return false;
    }
    config_ = config;
    context_ = context;
    step_ = step;
    reset_ = reset;
    has_expected_ = false;
    next_expected_ = 0;
    paused_.store(0, std::memory_order_relaxed);
    parked_.store(0, std::memory_order_relaxed);
    for (auto* counter : {&processed_, &input_dropped_, &output_dropped_, &resyncs_, &late_})
        counter->store(0, std::memory_order_relaxed);
    workgroup_joined_.store(false, std::memory_order_relaxed);
    if (!config.spawn_thread)
        return true;
    running_.store(true, std::memory_order_release);
    try {
        thread_ = std::thread([this] { loop(); });
    } catch (...) {
        running_.store(false, std::memory_order_release);
        return false;
    }
    return true;
}

void NeuralShadowWorker::stop() noexcept {
    running_.store(false, std::memory_order_release);
    if (thread_.joinable())
        thread_.join();
}

void NeuralShadowWorker::pause() noexcept {
    const auto generation = ++pause_generation_;
    paused_.store(generation, std::memory_order_seq_cst);
    // An echo of an older request is stale; only this one proves the thread
    // has left pump() and will not enter it again until resume().
    while (running() && parked_.load(std::memory_order_acquire) != generation)
        std::this_thread::yield();
}

void NeuralShadowWorker::resume(std::uint64_t next_sequence) noexcept {
    next_expected_ = next_sequence;
    has_expected_ = true;
    paused_.store(0, std::memory_order_release);
}

bool NeuralShadowWorker::publish_output(StreamingBlockStamp stamp, const float* samples) noexcept {
    float* slot = output_.begin_push(stamp);
    if (slot == nullptr)
        return false;
    std::copy_n(samples, output_.samples_per_block(), slot);
    output_.commit_push();
    return true;
}

bool NeuralShadowWorker::push_input(StreamingBlockStamp stamp, const float* const* channels,
                                    std::uint32_t channel_count, std::uint32_t frames) noexcept {
    float* slot = input_.begin_push(stamp);
    if (slot == nullptr) {
        input_dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const auto expected_channels = input_.samples_per_block() / frames;
    for (std::size_t ch = 0; ch < expected_channels; ++ch) {
        float* destination = slot + ch * frames;
        if (ch < channel_count && channels[ch] != nullptr)
            std::copy_n(channels[ch], frames, destination);
        else
            std::fill_n(destination, frames, 0.0f);
    }
    input_.commit_push();
    return true;
}

void NeuralShadowWorker::drop_front(StreamingBlockStamp stamp) noexcept {
    output_.pop();
    if (missed_slots_[stamp.sequence % missed_slots_.size()] == stamp.sequence)
        late_.fetch_add(1, std::memory_order_relaxed);
}

bool NeuralShadowWorker::take_output(StreamingBlockStamp expected, float* output) noexcept {
    missed_slots_[expected.sequence % missed_slots_.size()] = expected.sequence;
    StreamingBlockStamp stamp;
    while (const float* samples = output_.front(stamp)) {
        if (stamp.epoch == expected.epoch && stamp.sequence == expected.sequence) {
            std::copy_n(samples, output_.samples_per_block(), output);
            output_.pop();
            return true;
        }
        if (stamp.epoch != expected.epoch || stamp.sequence > expected.sequence)
            return false;
        // Its slot has passed: a late result is dropped, never shifted later.
        drop_front(stamp);
    }
    return false;
}

void NeuralShadowWorker::discard_through(StreamingBlockStamp expected) noexcept {
    StreamingBlockStamp stamp;
    while (output_.front(stamp) != nullptr) {
        if (stamp.epoch != expected.epoch || stamp.sequence > expected.sequence)
            return;
        if (stamp.sequence < expected.sequence)
            drop_front(stamp);
        else
            output_.pop();
    }
}

std::size_t NeuralShadowWorker::pump(std::size_t max_blocks) noexcept {
    std::size_t done = 0;
    StreamingBlockStamp stamp;
    while (max_blocks == 0 || done < max_blocks) {
        const float* input = input_.front(stamp);
        if (input == nullptr)
            break;
        // A dropped input leaves a hole in the causal history; restart the
        // state rather than continue as though the hole were not there.
        if (has_expected_ && stamp.sequence != next_expected_) {
            reset_(context_);
            resyncs_.fetch_add(1, std::memory_order_relaxed);
        }
        float* output = output_.begin_push(stamp);
        const bool publish = output != nullptr;
        step_(context_, input, publish ? output : scratch_.data(), stamp);
        if (publish)
            output_.commit_push();
        else
            output_dropped_.fetch_add(1, std::memory_order_relaxed);
        input_.pop();
        has_expected_ = true;
        next_expected_ = stamp.sequence + 1;
        processed_.fetch_add(1, std::memory_order_relaxed);
        ++done;
    }
    return done;
}

void NeuralShadowWorker::loop() noexcept {
    // The inline shadow runs under the host's flush-to-zero; this thread
    // gets the same mode, so a decaying recurrent state cannot go subnormal.
    signal::ScopedFlushDenormals flush_denormals;
    audio::AudioWorkgroup workgroup;
#if defined(__APPLE__)
    if (config_.audio_workgroup != nullptr) {
        workgroup.set_workgroup(reinterpret_cast<os_workgroup_t>(config_.audio_workgroup));
        workgroup_joined_.store(workgroup.join_from_audio_thread(), std::memory_order_release);
    }
    if (!workgroup_joined_.load(std::memory_order_relaxed))
        (void)pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    const auto poll = std::chrono::nanoseconds(config_.poll_ns);
    while (running_.load(std::memory_order_acquire)) {
        if (const auto generation = paused_.load(std::memory_order_seq_cst)) {
            parked_.store(generation, std::memory_order_release);
            std::this_thread::sleep_for(poll);
            continue;
        }
        if (pump() == 0)
            std::this_thread::sleep_for(poll);
    }
    (void)workgroup.leave();
    workgroup_joined_.store(false, std::memory_order_release);
}

NeuralShadowWorker::Counters NeuralShadowWorker::counters() const noexcept {
    return {.processed = processed_.load(std::memory_order_relaxed),
            .input_dropped = input_dropped_.load(std::memory_order_relaxed),
            .output_dropped = output_dropped_.load(std::memory_order_relaxed),
            .resyncs = resyncs_.load(std::memory_order_relaxed),
            .workgroup_joined = workgroup_joined_.load(std::memory_order_acquire)};
}

} // namespace pulp::gpu_audio::detail
