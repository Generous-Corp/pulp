#pragma once

/// @file wake_signal.hpp
/// Wakes a sleeping consumer thread from a realtime producer without a lock,
/// and without a system call while the consumer is busy.

#include <atomic>
#include <chrono>
#include <cstdint>

#if defined(__APPLE__)
#include <dispatch/dispatch.h>
#else
#include <semaphore>
#endif

namespace pulp::runtime {

/// A wake signal from one or more producers (typically the audio thread) to a
/// single consumer thread that sleeps until there is work.
///
/// The producer publishes its work first (a ring write, a release store of a
/// slot state) and then calls notify(). The consumer calls wait_until() or
/// wait_for() with a predicate that reads the same published state.
///
/// Realtime contract:
/// - notify() never allocates, never locks and never blocks. It is a
///   sequentially consistent fence and one atomic load; it reaches the kernel
///   (a dispatch semaphore on Apple, std::counting_semaphore elsewhere) only
///   while the consumer is parked, so a producer whose consumer is busy makes
///   no system call at all.
/// - wait_until() / wait_for() block in the kernel; call them from the
///   consumer thread only, never from the audio thread.
///
/// No lost wakeups: the consumer stores `parked`, fences, then re-checks its
/// predicate before sleeping; the producer publishes, fences, then loads
/// `parked`. The two sequentially consistent fences guarantee that either the
/// consumer sees the work in its re-check or the producer sees the consumer
/// parked and posts. An extra post (two notifies while parked) only costs the
/// consumer one more predicate check.
///
/// One consumer at a time. Destroy the signal only after the consumer has
/// stopped waiting (join the consumer thread first).
///
/// @code
/// // audio thread
/// ring.push(block);
/// wake.notify();
///
/// // worker thread
/// while (running) {
///     wake.wait_for([&] { return !ring.empty() || !running; }, 100ms);
///     drain(ring);
/// }
/// @endcode
class WakeSignal {
  public:
    using Clock = std::chrono::steady_clock;

    WakeSignal() noexcept
#if defined(__APPLE__)
        : semaphore_(dispatch_semaphore_create(0))
#endif
    {
    }

    ~WakeSignal() {
#if defined(__APPLE__)
        // libdispatch traps when a semaphore is released below its initial
        // value, i.e. with a thread still waiting. Surplus posts leave the
        // value above zero, which is allowed.
        if (semaphore_ != nullptr)
            dispatch_release(semaphore_);
#endif
    }

    WakeSignal(const WakeSignal&) = delete;
    WakeSignal& operator=(const WakeSignal&) = delete;

    /// Producer side, realtime safe. Call after the work is published.
    void notify() noexcept {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (parked_.load(std::memory_order_relaxed)) {
            posts_.fetch_add(1, std::memory_order_relaxed);
            post();
        }
    }

    /// Consumer side. Returns true once `ready()` holds, false if `deadline`
    /// passes first. `ready` runs before parking and after every wake, so
    /// spurious or surplus posts are harmless.
    template <typename Ready> bool wait_until(Ready&& ready, Clock::time_point deadline) {
        for (;;) {
            if (ready())
                return true;
            parked_.store(true, std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_seq_cst);
            if (ready()) {
                parked_.store(false, std::memory_order_relaxed);
                return true;
            }
            const auto now = Clock::now();
            if (now >= deadline) {
                parked_.store(false, std::memory_order_relaxed);
                return false;
            }
            park(deadline - now);
            parked_.store(false, std::memory_order_relaxed);
            wakes_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    /// wait_until() with a deadline `timeout` from now.
    template <typename Ready> bool wait_for(Ready&& ready, Clock::duration timeout) {
        return wait_until(static_cast<Ready&&>(ready), Clock::now() + timeout);
    }

    /// Diagnostics: kernel posts the producers made, and times the consumer
    /// woke from a park (a post or a timeout).
    std::uint64_t posts() const noexcept {
        return posts_.load(std::memory_order_relaxed);
    }
    std::uint64_t wakes() const noexcept {
        return wakes_.load(std::memory_order_relaxed);
    }
    /// Diagnostics: whether the consumer is currently parked (or about to be).
    bool consumer_parked() const noexcept {
        return parked_.load(std::memory_order_relaxed);
    }

  private:
    void post() noexcept {
#if defined(__APPLE__)
        (void)dispatch_semaphore_signal(semaphore_);
#else
        semaphore_.release();
#endif
    }

    void park(Clock::duration timeout) {
#if defined(__APPLE__)
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(timeout).count();
        (void)dispatch_semaphore_wait(semaphore_, dispatch_time(DISPATCH_TIME_NOW, ns));
#else
        (void)semaphore_.try_acquire_for(timeout);
#endif
    }

    std::atomic<bool> parked_{false};
    std::atomic<std::uint64_t> posts_{0};
    std::atomic<std::uint64_t> wakes_{0};
#if defined(__APPLE__)
    dispatch_semaphore_t semaphore_ = nullptr;
#else
    std::counting_semaphore<> semaphore_{0};
#endif
};

} // namespace pulp::runtime
