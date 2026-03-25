#pragma once

// SeqLock<T> — Lock-free reader/writer for coherent multi-field snapshots
//
// Use case: reading transport state (tempo + beat position + time sig) as a
// consistent snapshot without locks. The writer is assumed to be a single
// thread (audio thread or main thread). Readers retry if the data was
// modified during their read.
//
// Writer: seqlock.write(new_value);
// Reader: auto val = seqlock.read();
//
// Safe on ARM and x86. Uses acquire/release ordering.

#include <atomic>
#include <cstring>
#include <type_traits>

namespace pulp::runtime {

template <typename T>
class SeqLock {
    static_assert(std::is_trivially_copyable_v<T>,
                  "SeqLock<T> requires T to be trivially copyable");

public:
    SeqLock() = default;
    explicit SeqLock(const T& initial) : data_(initial) {}

    // Single-writer: store a new value. Not thread-safe with other writers.
    void write(const T& value) {
        seq_.fetch_add(1, std::memory_order_release); // odd = writing
        copy_bytes(reinterpret_cast<volatile char*>(&data_),
                   reinterpret_cast<const char*>(&value), sizeof(T));
        seq_.fetch_add(1, std::memory_order_release); // even = complete
    }

    // Multi-reader: read a consistent snapshot. Retries on torn reads.
    T read() const {
        T result;
        for (;;) {
            unsigned seq0 = seq_.load(std::memory_order_acquire);
            if (seq0 & 1) continue; // writer in progress, spin
            copy_bytes(reinterpret_cast<char*>(&result),
                       reinterpret_cast<const volatile char*>(&data_), sizeof(T));
            std::atomic_thread_fence(std::memory_order_acquire);
            unsigned seq1 = seq_.load(std::memory_order_relaxed);
            if (seq0 == seq1) return result;
        }
    }

private:
    // Byte-by-byte volatile copy prevents compiler from optimizing away or reordering
    static void copy_bytes(volatile char* dst, const char* src, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) dst[i] = src[i];
    }
    static void copy_bytes(char* dst, const volatile char* src, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) dst[i] = src[i];
    }

    alignas(64) T data_{};
    std::atomic<unsigned> seq_{0};
};

} // namespace pulp::runtime
