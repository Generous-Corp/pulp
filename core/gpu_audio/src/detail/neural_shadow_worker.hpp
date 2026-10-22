#pragma once

#include "streaming_model.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

namespace pulp::gpu_audio::detail {

/// Fixed single-producer/single-consumer ring of stamped planar blocks. The
/// producer writes a slot in place and commits it; the consumer reads the
/// front in place and pops it. Storage is allocated by prepare() only.
class NeuralShadowRing {
  public:
    bool prepare(std::uint32_t capacity, std::size_t samples_per_block);
    // Quiescent only: both sides stopped.
    void reset() noexcept;

    // Producer. Null when full.
    float* begin_push(StreamingBlockStamp stamp) noexcept;
    void commit_push() noexcept;

    // Consumer. Null when empty.
    const float* front(StreamingBlockStamp& stamp) const noexcept;
    void pop() noexcept;

    std::size_t samples_per_block() const noexcept {
        return samples_;
    }

  private:
    std::vector<StreamingBlockStamp> stamps_;
    std::vector<float> samples_storage_;
    std::size_t samples_ = 0;
    std::uint32_t capacity_ = 0;
    alignas(64) std::atomic<std::uint64_t> read_{0};
    alignas(64) std::atomic<std::uint64_t> write_{0};
};

/// Non-realtime CPU shadow for a GPU-backed neural node. The audio callback
/// copies each input block into `input`; a dedicated thread runs the CPU
/// model in stream order and publishes stamped results into `output`; the
/// callback copies a result out only when the GPU missed. Neither side ever
/// waits on the other.
class NeuralShadowWorker {
  public:
    // Runs the model on one planar block. Called only on the shadow side.
    using StepFn = void (*)(void* context, const float* input, float* output) noexcept;
    // Restarts the model's causal state after an input gap.
    using ResetFn = void (*)(void* context) noexcept;

    struct Config {
        std::uint32_t capacity = 16;
        std::size_t samples_per_block = 0;
        std::uint64_t poll_ns = 250'000;
        // Borrowed os_workgroup_t; null selects USER_INTERACTIVE QoS.
        void* audio_workgroup = nullptr;
        // False leaves stepping to pump() (deterministic tests).
        bool spawn_thread = true;
    };

    struct Counters {
        std::uint64_t processed = 0;
        std::uint64_t input_dropped = 0;
        std::uint64_t output_dropped = 0;
        std::uint64_t resyncs = 0;
        bool workgroup_joined = false;
    };

    NeuralShadowWorker() = default;
    ~NeuralShadowWorker() {
        stop();
    }
    NeuralShadowWorker(const NeuralShadowWorker&) = delete;
    NeuralShadowWorker& operator=(const NeuralShadowWorker&) = delete;

    // Host thread. Allocates both rings and, if configured, starts the thread.
    bool start(const Config& config, void* context, StepFn step, ResetFn reset);
    // Host thread. Joins the thread; queued blocks stay queued for pump().
    void stop() noexcept;
    bool running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    // Callback. Copy only; false (and a counted drop) when the ring is full.
    bool push_input(StreamingBlockStamp stamp, const float* const* channels,
                    std::uint32_t channel_count, std::uint32_t frames) noexcept;
    // Callback. Copies the result for `expected` into `output` if it is
    // ready. Older results are dropped and counted late.
    bool take_output(StreamingBlockStamp expected, float* output) noexcept;
    // Callback. Drops results through `expected` without copying (the GPU
    // delivered that slot). Older results are counted late.
    void discard_through(StreamingBlockStamp expected) noexcept;
    // Callback/host. Consumer-side access to the oldest published result.
    const float* front(StreamingBlockStamp& stamp) const noexcept {
        return output_.front(stamp);
    }
    void pop_front() noexcept {
        output_.pop();
    }

    // Shadow side: the thread, or the caller when no thread runs. Processes up
    // to `max_blocks` queued inputs (0 = all) and returns how many it did.
    std::size_t pump(std::size_t max_blocks = 0) noexcept;

    std::uint64_t late() const noexcept {
        return late_.load(std::memory_order_relaxed);
    }
    Counters counters() const noexcept;

  private:
    void loop() noexcept;

    NeuralShadowRing input_, output_;
    std::vector<float> scratch_;
    void* context_ = nullptr;
    StepFn step_ = nullptr;
    ResetFn reset_ = nullptr;
    Config config_{};
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::uint64_t next_expected_ = 0; // shadow side
    bool has_expected_ = false;       // shadow side
    std::atomic<std::uint64_t> processed_{0}, input_dropped_{0}, output_dropped_{0}, resyncs_{0},
        late_{0};
    std::atomic<bool> workgroup_joined_{false};
};

} // namespace pulp::gpu_audio::detail
