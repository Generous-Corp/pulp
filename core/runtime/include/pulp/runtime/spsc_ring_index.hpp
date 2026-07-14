#pragma once

// SpscRingIndex — index-pair cursor management for a single-producer
// single-consumer (SPSC) ring buffer whose storage the CALLER owns.
//
// This class does NOT own a storage buffer. Instead it tracks the read and
// write positions inside a caller-owned circular buffer (a contiguous array
// the caller allocates separately — e.g. an `std::vector<float>` for an audio
// sample ring, a `std::vector<uint8_t>` for a byte stream, etc.). The caller
// uses the index ranges returned by `prepare_to_write()` / `prepare_to_read()`
// to copy into / out of its own buffer; SpscRingIndex only deals with the
// modular arithmetic of "where to read/write next, and how many items can
// be touched without wrapping past the producer/consumer cursor."
//
// Because a single linear range of items in the underlying buffer may wrap
// around the end of the array, `prepare_to_*` returns **two** index ranges
// (each `start` + `size`). The second range is empty (`size_2 == 0`) when
// no wrap occurs. Callers iterate or copy both ranges in sequence.
//
// Lock-free for one producer thread + one consumer thread (SPSC) — the
// reader and writer cursors are independent `std::atomic<int>` values with
// acquire/release ordering. Multi-producer / multi-consumer usage requires
// external synchronization.
//
// Pick this over its two siblings when the payload is bulk samples/bytes you
// want to memcpy in place rather than element-wise values you want to push and
// pop:
//
//   * `SpscRingIndex`   — cursors only; you own the array and copy into the two
//                         returned index ranges. Zero-copy bulk transfer.
//   * `SpscQueue<T,N>`  — a real queue: owns its storage, moves elements in and
//                         out one at a time.
//   * `TripleBuffer<T>` — no queue at all: publishes a single latest value,
//                         dropping anything the consumer did not pick up.

#include <algorithm>
#include <atomic>
#include <cstddef>

namespace pulp::runtime {

class SpscRingIndex {
public:
    /// Construct an SpscRingIndex that manages indices in `[0, capacity)`.
    /// `capacity` is the size of the caller-owned buffer in items. One slot
    /// is reserved internally as the empty/full sentinel, so the maximum
    /// number of items that can ever be queued is `capacity - 1`. Pick a
    /// capacity one larger than the worst-case queue depth you need.
    explicit SpscRingIndex(int capacity) noexcept
        : capacity_(capacity < 1 ? 1 : capacity) {}

    /// Total size of the managed buffer in items.
    int capacity() const noexcept { return capacity_; }

    /// Number of items currently available to read.
    int num_ready() const noexcept {
        const int w = write_pos_.load(std::memory_order_acquire);
        const int r = read_pos_.load(std::memory_order_acquire);
        const int diff = w - r;
        return diff >= 0 ? diff : diff + capacity_;
    }

    /// Free space (in items) available for new writes. The fifo reserves
    /// one slot internally, so this is at most `capacity - 1`.
    int free_space() const noexcept {
        return capacity_ - num_ready() - 1;
    }

    /// Reset the cursors back to empty. Not thread-safe — the caller must
    /// guarantee neither producer nor consumer is active.
    void reset() noexcept {
        read_pos_.store(0, std::memory_order_relaxed);
        write_pos_.store(0, std::memory_order_relaxed);
    }

    /// Plan a write of up to `num_to_write` items. Returns two index ranges
    /// `[start_1, start_1+size_1)` and `[start_2, start_2+size_2)` covering
    /// contiguous regions of the caller-owned buffer that may be filled.
    /// `size_2` is non-zero only when the write wraps past `capacity`.
    /// The sum `size_1 + size_2` is `min(num_to_write, free_space())`.
    /// The cursor is NOT advanced until `finish_write()` is called.
    void prepare_to_write(int num_to_write,
                          int& start_1, int& size_1,
                          int& start_2, int& size_2) const noexcept {
        const int w = write_pos_.load(std::memory_order_relaxed);
        const int r = read_pos_.load(std::memory_order_acquire);
        const int free = (r > w ? r - w : capacity_ - w + r) - 1;
        const int can_write = std::min(num_to_write < 0 ? 0 : num_to_write,
                                       free < 0 ? 0 : free);

        if (can_write <= 0) {
            start_1 = w;
            size_1 = 0;
            start_2 = 0;
            size_2 = 0;
            return;
        }

        start_1 = w;
        const int contiguous = capacity_ - w;
        if (can_write <= contiguous) {
            size_1 = can_write;
            start_2 = 0;
            size_2 = 0;
        } else {
            size_1 = contiguous;
            start_2 = 0;
            size_2 = can_write - contiguous;
        }
    }

    /// Advance the write cursor by `num_written` items. Must be called after
    /// the caller has copied data into the ranges returned by
    /// `prepare_to_write()`. Passing more than the prepared sum is a
    /// programming error and is clamped to keep the fifo coherent.
    void finish_write(int num_written) noexcept {
        if (num_written <= 0) return;
        const int w = write_pos_.load(std::memory_order_relaxed);
        // True modulo wrap. The previous `next -= capacity_` only handled
        // a single overflow span, so an oversized `num_written` (caller
        // error past the prepared sum) could leave `next` >= capacity_
        // and subsequent prepare_* calls would return invalid indices.
        int next = (w + num_written) % capacity_;
        if (next < 0) next += capacity_;
        write_pos_.store(next, std::memory_order_release);
    }

    /// Plan a read of up to `num_to_read` items. Returns two ranges the
    /// caller may copy out of its buffer. Sum is `min(num_to_read, num_ready())`.
    /// The cursor is NOT advanced until `finish_read()` is called.
    void prepare_to_read(int num_to_read,
                         int& start_1, int& size_1,
                         int& start_2, int& size_2) const noexcept {
        const int r = read_pos_.load(std::memory_order_relaxed);
        const int w = write_pos_.load(std::memory_order_acquire);
        const int available = (w >= r ? w - r : capacity_ - r + w);
        const int can_read = std::min(num_to_read < 0 ? 0 : num_to_read,
                                      available);

        if (can_read <= 0) {
            start_1 = r;
            size_1 = 0;
            start_2 = 0;
            size_2 = 0;
            return;
        }

        start_1 = r;
        const int contiguous = capacity_ - r;
        if (can_read <= contiguous) {
            size_1 = can_read;
            start_2 = 0;
            size_2 = 0;
        } else {
            size_1 = contiguous;
            start_2 = 0;
            size_2 = can_read - contiguous;
        }
    }

    /// Advance the read cursor by `num_read` items.
    void finish_read(int num_read) noexcept {
        if (num_read <= 0) return;
        const int r = read_pos_.load(std::memory_order_relaxed);
        // True modulo wrap — mirror finish_write so an oversized num_read
        // does not leave the cursor out of range.
        int next = (r + num_read) % capacity_;
        if (next < 0) next += capacity_;
        read_pos_.store(next, std::memory_order_release);
    }

    SpscRingIndex(const SpscRingIndex&) = delete;
    SpscRingIndex& operator=(const SpscRingIndex&) = delete;
    SpscRingIndex(SpscRingIndex&&) = delete;
    SpscRingIndex& operator=(SpscRingIndex&&) = delete;

private:
    const int capacity_;
    std::atomic<int> read_pos_{0};
    std::atomic<int> write_pos_{0};
};

/// Deprecated spelling of `SpscRingIndex`. Kept so existing code keeps
/// compiling; the type is identical. Prefer `SpscRingIndex` — the class manages
/// ring INDICES into a buffer it does not own, which is the one thing the old
/// name never said.
using AbstractFifo [[deprecated("renamed to pulp::runtime::SpscRingIndex")]] = SpscRingIndex;

}  // namespace pulp::runtime
