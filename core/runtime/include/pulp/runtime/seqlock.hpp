#pragma once

/// @file seqlock.hpp
/// Lock-free sequence-lock for coherent multi-field snapshots.

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
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
    SeqLock() {
        const T initial{};
        store_payload(initial);
    }

    /// @param initial  Starting value.
    explicit SeqLock(const T& initial) {
        store_payload(initial);
    }

    /// Publish a new value. Must be called from a single writer thread only.
    /// @param value  The new value to publish.
    void write(const T& value) {
        // The opening increment needs acquire semantics as well as release:
        // on weakly ordered CPUs that prevents the upcoming data copy from
        // being observed before readers can see the odd "writer active" flag.
        seq_.fetch_add(1, std::memory_order_acq_rel); // odd = writing
        store_payload(value);
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
            load_payload(result);
            std::atomic_thread_fence(std::memory_order_acquire);
            unsigned seq1 = seq_.load(std::memory_order_acquire);
            if (seq0 == seq1) return result;
        }
    }

private:
    using PayloadWord = std::uintptr_t;
    static constexpr std::size_t kWordSize = sizeof(PayloadWord);
    static constexpr std::size_t kWordCount = sizeof(T) / kWordSize;
    static constexpr std::size_t kTailBytes = sizeof(T) % kWordSize;

    static_assert(std::atomic<PayloadWord>::is_always_lock_free,
                  "SeqLock payload word atomics must be lock-free");
    static_assert(std::atomic<std::uint8_t>::is_always_lock_free,
                  "SeqLock payload byte atomics must be lock-free");

    void store_payload(const T& value) noexcept {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
        for (std::size_t i = 0; i < kWordCount; ++i) {
            PayloadWord word = 0;
            std::memcpy(&word, bytes + i * kWordSize, kWordSize);
            payload_words_[i].store(word, std::memory_order_relaxed);
        }
        for (std::size_t i = 0; i < kTailBytes; ++i) {
            payload_tail_[i].store(bytes[kWordCount * kWordSize + i],
                                   std::memory_order_relaxed);
        }
    }

    void load_payload(T& result) const noexcept {
        auto* bytes = reinterpret_cast<std::uint8_t*>(&result);
        for (std::size_t i = 0; i < kWordCount; ++i) {
            const PayloadWord word =
                payload_words_[i].load(std::memory_order_relaxed);
            std::memcpy(bytes + i * kWordSize, &word, kWordSize);
        }
        for (std::size_t i = 0; i < kTailBytes; ++i) {
            bytes[kWordCount * kWordSize + i] =
                payload_tail_[i].load(std::memory_order_relaxed);
        }
    }

    alignas(64) std::array<std::atomic<PayloadWord>, kWordCount> payload_words_{};
    std::array<std::atomic<std::uint8_t>, kTailBytes> payload_tail_{};
    std::atomic<unsigned> seq_{0};
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

} // namespace pulp::runtime
