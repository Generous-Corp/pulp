#pragma once

#include <atomic>
#include <cstdint>
#include <functional>

namespace pulp::format::detail {

// Marshals VST3's `IComponentHandler::restartComponent` off the audio thread.
//
// `restartComponent` is a HOST callback: hosts are free to take locks,
// allocate, and synchronously re-enter the plug-in (some deactivate and
// reactivate the component) from inside it. None of that is safe on the
// real-time audio thread, so it must never be invoked from `process()`.
//
// This publisher splits the work across threads with no audio-thread
// allocation or locking:
//
//   - The audio thread calls `note_pending(flags)` when the processor flags a
//     latency / tail change. It OR-accumulates the restart flags into one
//     atomic and arms a one-shot dispatch. Multiple changes between deliveries
//     coalesce into a single set of flags, so the host sees exactly one
//     `restartComponent` call per burst. Lock-free and allocation-free.
//
//   - A main-thread caller drains the publisher via `poll_main_thread(cb)`,
//     which swaps out the coalesced flags and, when non-zero, invokes `cb`
//     (the adapter's `restartComponent(flags)` wrapper) on that thread.
//
// The publisher itself owns no threading mechanism — the adapter decides how
// the main-thread drain is driven (a dispatcher post, or a main-thread host
// entrypoint that already runs regularly). This keeps the RT-safety contract
// in one small, directly testable unit.
class Vst3RestartPublisher {
public:
    using RestartFn = std::function<void(std::int32_t /*flags*/)>;

    // Audio-thread entry. OR-accumulates `flags` into the pending set and arms
    // a one-shot dispatch. Returns true exactly on the arming edge (the
    // transition from "no dispatch pending" to "dispatch pending"), so the
    // caller can kick a main-thread wakeup once per burst without re-posting on
    // every block. RT-safe: a relaxed/acq-rel atomic OR plus an exchange, no
    // allocation, no lock. A `flags` value of 0 is ignored.
    bool note_pending(std::int32_t flags) noexcept {
        if (flags == 0) return false;
        pending_flags_.fetch_or(flags, std::memory_order_acq_rel);
        // Arm the dispatch; report whether this call is the one that armed it.
        return !armed_.exchange(true, std::memory_order_acq_rel);
    }

    // Non-mutating check of whether a dispatch is currently armed. Used by
    // main-thread entrypoints to decide whether a drain is worthwhile.
    bool dispatch_armed() const noexcept {
        return armed_.load(std::memory_order_acquire);
    }

    // Main-thread drain. Atomically claims the coalesced flags, disarms the
    // dispatch, and — when the claimed flags are non-zero — invokes `restart`
    // with them on the calling (main) thread. Returns the flags that were
    // delivered (0 when there was nothing pending). Safe to call repeatedly;
    // a second call after a drain delivers nothing.
    std::int32_t poll_main_thread(const RestartFn& restart) {
        // Disarm BEFORE claiming the flags. A concurrent audio-thread
        // note_pending() that interleaves between these two operations will
        // re-arm the latch (armed_ -> true), so the late flags it OR-ed in are
        // never lost: either this drain's exchange claims them, or a later
        // drain does because the latch stays armed. Reversing the order would
        // let a late note_pending() set flags that this drain's armed_.store
        // then clobbers, stranding them behind a disarmed latch.
        armed_.store(false, std::memory_order_release);
        const std::int32_t flags =
            pending_flags_.exchange(0, std::memory_order_acq_rel);
        if (flags != 0 && restart) {
            restart(flags);
        }
        return flags;
    }

    // Test / teardown helper: drop any pending state without delivering it.
    void reset() noexcept {
        pending_flags_.store(0, std::memory_order_release);
        armed_.store(false, std::memory_order_release);
    }

private:
    // The audio thread touches both atomics, so they must be genuinely
    // lock-free (a libstdc++/libc++ fallback to an internal mutex would
    // reintroduce the exact RT-safety hazard this class exists to remove).
    static_assert(std::atomic<std::int32_t>::is_always_lock_free,
                  "Vst3RestartPublisher requires a lock-free atomic<int32_t> "
                  "on the audio thread");
    static_assert(std::atomic<bool>::is_always_lock_free,
                  "Vst3RestartPublisher requires a lock-free atomic<bool> on "
                  "the audio thread");

    // OR-accumulated restart flags awaiting main-thread delivery.
    std::atomic<std::int32_t> pending_flags_{0};
    // One-shot arming latch: true between an arming note_pending() and the
    // matching poll_main_thread() drain.
    std::atomic<bool> armed_{false};
};

}  // namespace pulp::format::detail
