#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace pulp::graph {

struct GraphRuntimeProbeSample {
    std::uint64_t sequence = 0;
    std::uint64_t timestamp_samples = 0;
    std::uint32_t edge_index = 0;
    std::uint32_t frames = 0;
    float peak = 0.0f;
    float rms = 0.0f;
};

// Single producer / single consumer bounded ring. An unattached probe is a
// no-op, keeping optional instrumentation allocation-free and lock-free.
template <std::size_t Capacity = 256> class GraphRuntimeProbe {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                  "probe capacity must be a power of two");

  public:
    void attach() noexcept {
        attached_.store(true, std::memory_order_release);
    }
    void detach() noexcept {
        attached_.store(false, std::memory_order_release);
    }
    bool attached() const noexcept {
        return attached_.load(std::memory_order_acquire);
    }

    bool try_push(GraphRuntimeProbeSample sample) noexcept {
        if (!attached())
            return false;
        const auto write = write_.load(std::memory_order_relaxed);
        const auto read = read_.load(std::memory_order_acquire);
        if (write - read == Capacity) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        sample.sequence = write;
        slots_[write & (Capacity - 1)] = sample;
        write_.store(write + 1, std::memory_order_release);
        return true;
    }

    bool try_pop(GraphRuntimeProbeSample& sample) noexcept {
        const auto read = read_.load(std::memory_order_relaxed);
        if (read == write_.load(std::memory_order_acquire))
            return false;
        sample = slots_[read & (Capacity - 1)];
        read_.store(read + 1, std::memory_order_release);
        return true;
    }

    std::uint64_t dropped() const noexcept {
        return dropped_.load(std::memory_order_relaxed);
    }

  private:
    alignas(64) std::atomic<std::uint64_t> write_{0};
    alignas(64) std::atomic<std::uint64_t> read_{0};
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<bool> attached_{false};
    GraphRuntimeProbeSample slots_[Capacity]{};
};

} // namespace pulp::graph
