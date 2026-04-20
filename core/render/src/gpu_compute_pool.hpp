// gpu_compute_pool.hpp — internal header, not installed, not exported.
//
// StagingBufferPool: pre-allocated ring of persistent wgpu::Buffer objects
// keyed by wgpu::BufferUsage bitmask. Replaces malloc-per-call buffer
// creation in DawnGpuCompute to eliminate allocator churn on Dawn backends
// (Metal / D3D12 / Vulkan).
//
// Design reference: planning/zero-copy-slice-1-design-2026-04-20.md
//
// Threading: acquire() / release() are mutex-guarded. The mutex is only
// held for pool bookkeeping — no GPU calls are made under the lock, so
// contention is minimal in the audio/compute hot path.
//
// Lifetime: buffers returned from acquire() are expected to be returned
// via release() once the GPU has confirmed the submission completed
// (typically from an OnSubmittedWorkDone callback). The pool does NOT
// track in-flight submissions itself — the caller is responsible for
// that sequencing.

#pragma once

#ifdef PULP_HAS_SKIA

#include "webgpu/webgpu_cpp.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pulp::render::detail {

// Round `n` up to the next power of two (minimum 16 bytes so we never
// hand out a buffer too small to be useful). Uses 64-bit arithmetic so
// this matches wgpu::Buffer::GetSize()'s return type.
inline uint64_t next_pow2(uint64_t n) {
    if (n <= 16) return 16;
    // Subtract 1 so that an already-power-of-two input stays the same.
    --n;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    n |= n >> 32;
    return n + 1;
}

// Hook for tests: incremented every time the pool creates a new
// wgpu::Buffer via device_.CreateBuffer(). Tests assert this stays
// bounded under repeated acquire/release cycles. Declared as a
// pointer so the hook is opt-in and introduces no cost in the
// normal case.
struct StagingBufferPoolHooks {
    std::size_t* created_count = nullptr;
};

class StagingBufferPool {
public:
    using InstrumentationHooks = StagingBufferPoolHooks;

    explicit StagingBufferPool(wgpu::Device device,
                               std::size_t pool_size = 8,
                               InstrumentationHooks hooks = InstrumentationHooks{})
        : device_(std::move(device)),
          cap_(pool_size == 0 ? 1 : pool_size),
          hooks_(hooks) {}

    // Non-copyable, non-movable. Owns wgpu::Buffer handles.
    StagingBufferPool(const StagingBufferPool&) = delete;
    StagingBufferPool& operator=(const StagingBufferPool&) = delete;
    StagingBufferPool(StagingBufferPool&&) = delete;
    StagingBufferPool& operator=(StagingBufferPool&&) = delete;

    ~StagingBufferPool() { clear(); }

    // Borrow a buffer with at least `min_bytes` capacity and the given
    // usage mask. Returns either a recycled free-list buffer (whose
    // capacity is already >= min_bytes) or a newly-created buffer sized
    // to the next power-of-two >= min_bytes.
    //
    // The returned buffer is a reference-counted wgpu::Buffer handle;
    // the pool also retains a reference via in_flight_ until release()
    // is called with the same handle.
    wgpu::Buffer acquire(std::size_t min_bytes, wgpu::BufferUsage usage) {
        const std::uint64_t key = usage_key(usage);
        const std::uint64_t want = next_pow2(static_cast<std::uint64_t>(min_bytes));

        std::lock_guard<std::mutex> lock(mu_);

        auto& free_list = free_[key];
        // Pick the smallest buffer in the free list that's large enough.
        // Scan is O(N) but N is bounded by cap_ (default 8), so this is
        // effectively constant time.
        auto best = free_list.end();
        std::uint64_t best_size = 0;
        for (auto it = free_list.begin(); it != free_list.end(); ++it) {
            const std::uint64_t sz = it->GetSize();
            if (sz >= want && (best == free_list.end() || sz < best_size)) {
                best = it;
                best_size = sz;
            }
        }

        if (best != free_list.end()) {
            wgpu::Buffer buf = std::move(*best);
            free_list.erase(best);
            in_flight_[key].push_back(buf);
            return buf;
        }

        // No suitable free buffer — create a new one. If the in-flight
        // + free-list total is already at cap_, evict the oldest free
        // buffer (FIFO) to bound resource use.
        if (free_list.size() + in_flight_[key].size() >= cap_ && !free_list.empty()) {
            free_list.pop_front();
        }

        wgpu::BufferDescriptor desc{};
        desc.size = want;
        desc.usage = usage;
        wgpu::Buffer buf = device_.CreateBuffer(&desc);
        if (hooks_.created_count) {
            ++(*hooks_.created_count);
        }
        in_flight_[key].push_back(buf);
        return buf;
    }

    // Return a buffer to the pool's free list. Must be called only
    // once per acquire(), and only after the GPU has confirmed the
    // submission that used this buffer is complete.
    //
    // Matching is done by CType handle identity (wgpu::Buffer is a
    // reference-counted ObjectBase; multiple instances can share the
    // same underlying handle).
    void release(const wgpu::Buffer& buf) {
        if (!buf) return;
        const std::uint64_t key = usage_key(buf.GetUsage());

        std::lock_guard<std::mutex> lock(mu_);
        auto& in_flight = in_flight_[key];
        for (auto it = in_flight.begin(); it != in_flight.end(); ++it) {
            if (it->Get() == buf.Get()) {
                wgpu::Buffer recovered = std::move(*it);
                in_flight.erase(it);

                auto& free_list = free_[key];
                // Enforce cap on the free list; oldest evicted first.
                while (free_list.size() + in_flight_[key].size() >= cap_
                       && !free_list.empty()) {
                    free_list.pop_front();
                }
                free_list.push_back(std::move(recovered));
                return;
            }
        }
        // Unknown buffer — ignore. Releasing a handle the pool doesn't
        // own is a programming error; failing silently is safer than
        // aborting from inside a queue callback.
    }

    // Drop a buffer from in_flight_ WITHOUT returning it to the free
    // list. Use when a caller cannot guarantee the GPU is done with
    // the buffer (e.g. MapAsync timed out) but the pool must still
    // reclaim its in_flight_ slot so subsequent acquire()s don't
    // accumulate tracked handles and grow the pool unboundedly.
    //
    // Codex 2026-04-21 review on #560: the previous fix for #536
    // skipped release() on timeout but left the buffer tracked in
    // in_flight_ forever; over a long run of timeouts (the exact
    // scenario #536 targets) the map grew without bound. discard()
    // drops the pool's reference so the wgpu::Buffer lands on the
    // normal refcounted teardown path once the GPU stops mapping it.
    void discard(const wgpu::Buffer& buf) {
        if (!buf) return;
        const std::uint64_t key = usage_key(buf.GetUsage());

        std::lock_guard<std::mutex> lock(mu_);
        auto& in_flight = in_flight_[key];
        for (auto it = in_flight.begin(); it != in_flight.end(); ++it) {
            if (it->Get() == buf.Get()) {
                in_flight.erase(it);
                return;
            }
        }
        // Unknown buffer — ignore (same policy as release()).
    }

    // Test accessors — read-only snapshot of pool state. Safe to call
    // concurrently with acquire()/release() but the returned counts are
    // a point-in-time snapshot.
    std::size_t capacity() const { return cap_; }

    std::size_t free_count() const {
        std::lock_guard<std::mutex> lock(mu_);
        std::size_t n = 0;
        for (const auto& [_, q] : free_) n += q.size();
        return n;
    }

    std::size_t in_flight_count() const {
        std::lock_guard<std::mutex> lock(mu_);
        std::size_t n = 0;
        for (const auto& [_, q] : in_flight_) n += q.size();
        return n;
    }

    // Destroy all buffers held by the pool. Called from the destructor;
    // also exposed for tests that want to verify deterministic teardown.
    void clear() {
        std::lock_guard<std::mutex> lock(mu_);
        free_.clear();
        in_flight_.clear();
    }

private:
    static std::uint64_t usage_key(wgpu::BufferUsage usage) {
        return static_cast<std::uint64_t>(usage);
    }

    wgpu::Device device_;
    std::size_t cap_;
    InstrumentationHooks hooks_;

    // Keyed by the raw BufferUsage bitmask (cast to uint64_t). Dawn
    // does not permit reuse of a buffer with a different usage mask,
    // so each mask gets its own ring.
    std::unordered_map<std::uint64_t, std::deque<wgpu::Buffer>> free_;
    std::unordered_map<std::uint64_t, std::deque<wgpu::Buffer>> in_flight_;

    mutable std::mutex mu_;
};

}  // namespace pulp::render::detail

#endif  // PULP_HAS_SKIA
