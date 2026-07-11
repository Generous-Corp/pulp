#pragma once

/// @file slot.hpp
/// Publishing a value built off the audio thread, and reclaiming the old one
/// without ever calling a destructor on the audio thread.
///
/// This is the one rule both types below enforce:
///
///   **The audio thread never allocates, never blocks, and never frees.**
///
/// Deallocation is a blocking OS operation, so the retired value must always be
/// destroyed somewhere else. Pulp had four separate hand-rolled solutions to
/// this before these types existed; `Slot` and `Handoff` are the two shapes
/// those four collapse into.
///
/// Two publication modes, because there are genuinely two:
///
/// | | `Slot<T>` | `Handoff<T>` |
/// |---|---|---|
/// | audio thread | **reads** a shared value | **takes ownership** of the value |
/// | value is | immutable while published | mutable by its owner |
/// | reclamation | control thread, once readers drain | worker thread, via a retire ring |
/// | reader pin | yes (`ReadGuard`) | n/a — single owner |
///
/// Pick `Slot` when the audio thread only looks at the value (a compiled graph,
/// a wavetable, a lookup table). Pick `Handoff` when the audio thread owns and
/// mutates it (a convolver IR carrying partition state and input history).
///
/// Both are single-producer / audio-thread-consumer. Neither is a general MPMC
/// container, and neither tries to be.
///
/// @note `Slot` gives you *grace after retire*. If you need *quiescence before
///       publish* — proving the old value is not in `process()` so you can
///       serialize state out of it — that is a different requirement, and
///       `format::ProcessorHotSwapSlot` keeps its own mechanism for it.

#include <pulp/runtime/spsc_queue.hpp>

#include <atomic>
#include <cstddef>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace pulp::runtime {

/// Lock-free publication to a reader that must never block. The reader pins the
/// slot for the duration of its access; the writer swaps in a new value and
/// parks the old one until every reader has left.
///
/// **Slot manages the published object's LIFETIME, not its constness.** A pin
/// guarantees only that the object will not be destroyed while you hold it. RCU
/// is a reclamation scheme, not an immutability scheme.
///
/// That distinction is load-bearing. `SignalGraph`'s published `CompiledGraph`
/// is emphatically *not* immutable: the audio thread writes every node's scratch
/// buffer through it on every block, and control-thread readers drain telemetry
/// and inject MIDI through the same pin. What is immutable is the *topology*.
/// If you want a genuinely read-only value, say so in the type — `Slot<const T>`
/// — and the guard hands back `const T*` for free.
///
/// The audio thread's cost is one seq_cst increment, one atomic load, and one
/// seq_cst decrement. No allocation, no destructor, no lock.
///
/// @code
/// Slot<CompiledGraph>       graph;   // interior mutable: audio thread writes scratch
/// Slot<const ImpulseResponse> ir;    // genuinely read-only
///
/// // Control thread:
/// graph.publish(std::make_unique<CompiledGraph>(rebuild()));
///
/// // Audio thread:
/// if (auto pin = graph.read()) {
///     run(*pin);   // valid for the lifetime of `pin`
/// }
/// @endcode
///
/// @warning Exactly one publisher thread. Any number of readers. Whether two
///          readers may safely mutate the same interior state concurrently is
///          the published type's problem, not Slot's.
template <typename T>
class Slot {
public:
    /// RAII pin. While one exists, the value it points at cannot be reclaimed.
    /// Holding a guard across a block boundary is legal but pointless; hold it
    /// exactly as long as you dereference.
    ///
    /// `read()` is const because pinning does not change the *slot* — it bumps a
    /// `mutable` counter. The pinned object is handed back as `T*`, so a
    /// `Slot<const T>` yields `const T*` and a `Slot<T>` yields a mutable one.
    class ReadGuard {
    public:
        ReadGuard() noexcept = default;
        ReadGuard(const ReadGuard&) = delete;
        ReadGuard& operator=(const ReadGuard&) = delete;

        ReadGuard(ReadGuard&& other) noexcept
            : owner_(other.owner_), value_(other.value_) {
            other.owner_ = nullptr;
            other.value_ = nullptr;
        }

        ~ReadGuard() {
            if (owner_ != nullptr)
                owner_->readers_.fetch_sub(1, std::memory_order_seq_cst);
        }

        T* get() const noexcept { return value_; }
        T& operator*() const noexcept { return *value_; }
        T* operator->() const noexcept { return value_; }
        explicit operator bool() const noexcept { return value_ != nullptr; }

    private:
        friend class Slot;
        ReadGuard(const Slot* owner, T* value) noexcept : owner_(owner), value_(value) {}

        const Slot* owner_ = nullptr;
        T* value_ = nullptr;
    };

    Slot() = default;
    Slot(const Slot&) = delete;
    Slot& operator=(const Slot&) = delete;

    /// Blocks until every reader has left, then frees everything. Publisher
    /// thread only, and only once both sides have stopped racing to publish.
    ~Slot() { wait_and_clear(); }

    /// Audio-thread-callable. Pin and read the currently published value.
    /// Returns a falsy guard when nothing has been published yet.
    ///
    /// RT contract: never allocates, never blocks, never frees.
    [[nodiscard]] ReadGuard read() const noexcept {
        readers_.fetch_add(1, std::memory_order_seq_cst);
        // seq_cst pairs with the store in publish() and the load in
        // reclaim_if_quiescent(): either a publisher observes this pin and
        // declines to free, or this load observes the publisher's new pointer.
        T* value = live_raw_.load(std::memory_order_seq_cst);
        if (value == nullptr) {
            readers_.fetch_sub(1, std::memory_order_seq_cst);
            return ReadGuard{};
        }
        return ReadGuard{this, value};
    }

    /// Publisher thread. Swap in a new value; the displaced one is parked and
    /// freed as soon as no reader holds it. Publishing `nullptr` unpublishes.
    void publish(std::shared_ptr<T> next) {
        live_raw_.store(next.get(), std::memory_order_seq_cst);
        if (live_)
            retired_.push_back(std::move(live_));
        live_ = std::move(next);
        reclaim_if_quiescent();
    }

    void publish(std::unique_ptr<T> next) { publish(std::shared_ptr<T>(std::move(next))); }

    /// Publisher thread. Drop the published value: readers immediately see
    /// nothing, and the displaced value is reclaimed once they drain.
    ///
    /// Spelled out rather than `publish(nullptr)` — which is ambiguous between
    /// the two overloads above, and reads worse at the call site.
    void unpublish() { publish(std::shared_ptr<T>{}); }

    /// Publisher thread. Free any parked values if no reader is currently
    /// inside `read()`. Non-blocking: does nothing when a reader is active.
    /// Call it opportunistically (e.g. after each block) to bound memory.
    void reclaim_if_quiescent() {
        if (readers_.load(std::memory_order_seq_cst) == 0)
            retired_.clear();
    }

    /// Publisher thread, teardown only. Spins until readers drain, then frees.
    /// Never call this from the audio thread.
    void wait_and_clear() {
        while (readers_.load(std::memory_order_seq_cst) != 0)
            std::this_thread::yield();
        retired_.clear();
    }

    /// Publisher thread only. The currently published value, as a shared_ptr the
    /// publisher may keep. Readers must use `read()` — this accessor is not
    /// synchronized against a concurrent `publish()`.
    const std::shared_ptr<T>& live() const noexcept { return live_; }

    /// Number of displaced values still awaiting reclamation.
    std::size_t retired_count() const noexcept { return retired_.size(); }

    /// True once something has been published.
    bool has_value() const noexcept {
        return live_raw_.load(std::memory_order_acquire) != nullptr;
    }

private:
    // Readers dereference `live_raw_` only. `live_` and `retired_` keep the
    // pointed-to objects alive and are touched by the publisher thread alone.
    std::shared_ptr<T> live_;
    std::atomic<T*> live_raw_{nullptr};
    std::vector<std::shared_ptr<T>> retired_;
    mutable std::atomic<std::uint32_t> readers_{0};
};

/// Lock-free transfer of *ownership* to a reader that must never block, plus a
/// ring for handing the displaced value back so the audio thread never frees.
///
/// This is the pattern Timur Doumler and Ross Bencina describe as standard
/// practice for real-time audio: build the object off-thread, swap a pointer,
/// return the old one through a lock-free FIFO for a non-RT thread to destroy.
///
/// @code
/// Handoff<IrState> swapper;
///
/// // Worker thread:
/// swapper.publish(build_ir(...));
///
/// // Audio thread, at a block boundary:
/// if (swapper.has_pending() && swapper.has_retire_capacity()) {
///     auto next = swapper.try_consume();
///     if (next) {
///         auto displaced = std::exchange(current_, std::move(next));
///         (void)swapper.retire(displaced);   // capacity reserved above
///     }
/// }
///
/// // Worker thread, periodically:
/// swapper.drain_retired();
/// @endcode
///
/// @warning One producer thread, one consumer (audio) thread.
template <typename T, std::size_t RetireCapacity = 8>
class Handoff {
public:
    Handoff() = default;
    Handoff(const Handoff&) = delete;
    Handoff& operator=(const Handoff&) = delete;

    /// Reclaims anything still parked. Both threads must have stopped.
    ~Handoff() {
        delete pending_.exchange(nullptr, std::memory_order_acquire);
        while (T* raw = pop_retired_raw()) delete raw;
    }

    /// Producer thread. Publish a value for the audio thread to claim. If a
    /// previously published value has not been consumed yet, it is replaced and
    /// freed here — on the producer thread, never the audio thread.
    ///
    /// Returns false only for a null argument.
    bool publish(std::unique_ptr<T> next) {
        if (!next) return false;
        // Release ordering so the value's contents happen-before the audio
        // thread's acquire in try_consume().
        T* raw = next.release();
        delete pending_.exchange(raw, std::memory_order_acq_rel);
        return true;
    }

    /// Audio-thread-callable. Atomically claim the most recently published
    /// value, or nullptr if there is none.
    ///
    /// RT contract: never allocates, never blocks, never frees.
    [[nodiscard]] std::unique_ptr<T> try_consume() noexcept {
        return std::unique_ptr<T>(pending_.exchange(nullptr, std::memory_order_acquire));
    }

    /// Audio-thread-callable. Park the value the audio thread is displacing so
    /// a non-RT thread can free it.
    ///
    /// Returns false when the retire ring is full; on false the caller still
    /// owns `displaced` (the `unique_ptr` is left intact). Gate on
    /// `has_retire_capacity()` before consuming so this never happens in
    /// well-paced operation.
    ///
    /// RT contract: never allocates, never blocks, never frees.
    [[nodiscard]] bool retire(std::unique_ptr<T>& displaced) noexcept {
        if (!displaced) return true;  // nothing to park
        if (!retired_.try_push(displaced.get())) return false;
        (void)displaced.release();  // ownership transferred to the ring
        return true;
    }

    /// Room in the retire ring for @p n more values. Reserve before committing
    /// to a swap so a retire never fails inline and strands a value on the
    /// audio thread.
    bool has_retire_capacity(std::size_t n = 1) const noexcept {
        return retired_.size_approx() + n <= RetireCapacity;
    }

    /// Producer / worker thread. Free every parked value. Returns the count.
    std::size_t drain_retired() {
        std::size_t freed = 0;
        while (T* raw = pop_retired_raw()) {
            std::unique_ptr<T> owned(raw);
            ++freed;
        }
        return freed;
    }

    /// Producer / worker thread. Pop one parked value, for callers that want to
    /// inspect it before it dies. Returns nullptr when none are queued.
    std::unique_ptr<T> drain_retired_one() { return std::unique_ptr<T>(pop_retired_raw()); }

    bool has_pending() const noexcept {
        return pending_.load(std::memory_order_acquire) != nullptr;
    }
    bool has_retired() const noexcept { return retired_.size_approx() > 0; }

    static constexpr std::size_t retire_capacity() noexcept { return RetireCapacity; }

private:
    T* pop_retired_raw() {
        auto popped = retired_.try_pop();
        return popped ? *popped : nullptr;
    }

    // Producer -> consumer: latest value awaiting pickup.
    std::atomic<T*> pending_{nullptr};
    // Consumer -> producer: displaced values awaiting off-thread deletion.
    // A ring rather than a single slot so rapid swaps survive a missed drain.
    SpscQueue<T*, RetireCapacity> retired_;
};

}  // namespace pulp::runtime
