#pragma once

/// @file seqlock.hpp
/// Lock-free sequence-lock for coherent multi-field snapshots.

#include <atomic>
#include <cstring>
#include <type_traits>

namespace pulp::runtime {

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324) // intentional cache-line alignment for lock-free snapshots
#endif

/// Lock-free reader/writer for reading multi-field structs as consistent snapshots.
///
/// Use when a single writer (e.g., the audio thread) publishes a struct that
/// multiple readers need to consume atomically. Readers spin-retry if the data
/// was modified during their read, guaranteeing coherence without mutexes.
///
/// @tparam T  Must be trivially copyable.
///
/// @code
/// struct Transport { double tempo; double beat_pos; int time_sig_num; };
/// SeqLock<Transport> transport;
///
/// // Writer (audio thread):
/// transport.write({120.0, 4.5, 4});
///
/// // Reader (UI thread):
/// auto snap = transport.read(); // always a consistent snapshot
/// @endcode
///
/// @note Safe on ARM and x86. Uses acquire/release ordering internally.
/// @note Only one writer thread is allowed. Multiple concurrent readers are safe.
template <typename T>
class SeqLock {
    static_assert(std::is_trivially_copyable_v<T>,
                  "SeqLock<T> requires T to be trivially copyable");

public:
    SeqLock() = default;

    /// @param initial  Starting value.
    explicit SeqLock(const T& initial) : data_(initial) {}

    /// Publish a new value. Must be called from a single writer thread only.
    /// @param value  The new value to publish.
    void write(const T& value) {
        // The opening increment needs acquire semantics as well as release:
        // on weakly ordered CPUs that prevents the upcoming data copy from
        // being observed before readers can see the odd "writer active" flag.
        seq_.fetch_add(1, std::memory_order_acq_rel); // odd = writing
        copy_bytes(reinterpret_cast<volatile char*>(&data_),
                   reinterpret_cast<const char*>(&value), sizeof(T));
        seq_.fetch_add(1, std::memory_order_release); // even = complete
    }

    /// Read a consistent snapshot. Retries automatically on torn reads.
    /// Safe to call from any number of reader threads concurrently.
    /// @return A coherent copy of the stored value.
    T read() const {
        T result;
        for (;;) {
            unsigned seq0 = seq_.load(std::memory_order_acquire);
            if (seq0 & 1) continue; // writer in progress, spin
            copy_bytes(reinterpret_cast<char*>(&result),
                       reinterpret_cast<const volatile char*>(&data_), sizeof(T));
            std::atomic_thread_fence(std::memory_order_acquire);
            unsigned seq1 = seq_.load(std::memory_order_acquire);
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

#ifdef _MSC_VER
#pragma warning(pop)
#endif

} // namespace pulp::runtime
