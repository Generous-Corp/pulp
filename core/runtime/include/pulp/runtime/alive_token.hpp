#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <utility>

namespace pulp::runtime {

/// Shared, fail-closed lifetime signal for callbacks that may outlive an owner.
///
/// The owner keeps the AliveToken and callbacks capture a Handle. Destroying or
/// explicitly retiring the token flips every handle to false and waits for
/// leases held by in-flight callbacks before the owner's referenced state is
/// released. A handle never extends the owner's lifetime; it only makes the
/// decision to avoid dereferencing it safe.
class AliveToken {
    struct State {
        std::atomic<bool> alive{true};
        std::atomic<bool> retired{false};
        std::mutex mutex;
        std::condition_variable quiesced;
        std::size_t active_leases = 0;

        // Keep Handle source compatibility for existing callback sites that
        // only need an atomic liveness read or fire-and-forget invalidation.
        // Owner teardown that must wait for in-flight users calls
        // AliveToken::retire(), which also drains active leases.
        bool load(std::memory_order order = std::memory_order_seq_cst) const noexcept {
            return alive.load(order);
        }

        void store(bool value, std::memory_order order = std::memory_order_seq_cst) noexcept {
            if (!value) {
                std::lock_guard lock(mutex);
                retired.store(true, std::memory_order_release);
                alive.store(false, order);
                return;
            }
            // A captured Handle can outlive its AliveToken. Never allow a
            // stale callback to resurrect a retired owner; reset() publishes
            // a fresh State when resurrection is actually intended.
            std::lock_guard lock(mutex);
            if (retired.load(std::memory_order_acquire))
                return;
            alive.store(true, order);
        }
    };

  public:
    using Handle = std::shared_ptr<State>;

    /// A temporary lease proving that an owner-backed callback may safely use
    /// its referenced state. The owner-side retire() waits until all leases
    /// have been released, so teardown cannot destroy that state in the
    /// middle of a leased callback.
    class Lease {
      public:
        Lease() noexcept = default;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;

        Lease(Lease&& other) noexcept : state_(std::move(other.state_)) {}

        Lease& operator=(Lease&& other) noexcept {
            if (this != &other) {
                release();
                state_ = std::move(other.state_);
            }
            return *this;
        }

        ~Lease() {
            release();
        }

        explicit operator bool() const noexcept {
            return static_cast<bool>(state_);
        }

        /// Release this callback's quiescence lease early.
        void reset() noexcept {
            release();
        }

      private:
        friend class AliveToken;

        explicit Lease(Handle state) noexcept : state_(std::move(state)) {}

        void release() noexcept {
            if (!state_)
                return;
            auto state = std::move(state_);
            {
                std::lock_guard lock(state->mutex);
                // A Lease can only be created by try_acquire(), which bumps
                // this counter while holding the same mutex.
                --state->active_leases;
            }
            state->quiesced.notify_all();
        }

        Handle state_;
    };

    AliveToken() : state_(std::make_shared<State>()) {}
    ~AliveToken() {
        retire();
    }

    AliveToken(const AliveToken&) = delete;
    AliveToken& operator=(const AliveToken&) = delete;
    AliveToken(AliveToken&&) = delete;
    AliveToken& operator=(AliveToken&&) = delete;

    Handle capture() const noexcept {
        return state_;
    }

    /// Retire from the owner teardown context, outside any callback that holds
    /// a Lease. A reentrant retire on the leased callback thread waits for that
    /// callback by design; returning early would make the owner's referenced
    /// state destructible while the callback is still using it.
    void retire() noexcept {
        const auto state = state_;
        if (!state)
            return;

        std::unique_lock lock(state->mutex);
        state->retired.store(true, std::memory_order_release);
        state->alive.store(false, std::memory_order_release);
        state->quiesced.wait(lock, [&state] { return state->active_leases == 0; });
    }

    void reset() {
        retire();
        state_ = std::make_shared<State>();
    }

    static bool is_alive(const Handle& handle) noexcept {
        return handle && handle->alive.load(std::memory_order_acquire);
    }

    /// Atomically admit one callback before teardown can retire the owner.
    ///
    /// Checking is_alive() and then using a captured owner reference is not a
    /// lifetime proof: retire() can run between those two operations. Callers
    /// that will dereference owner-backed state must hold the returned Lease
    /// for the complete callback. A retired or empty handle returns an empty
    /// lease and the callback must fail closed. Acquiring a lease takes a
    /// mutex, so this API is for editor/host control callbacks; realtime audio
    /// paths should keep using the lock-free is_alive() check and must not
    /// dereference state whose owner can be destroyed concurrently.
    static Lease try_acquire(const Handle& handle) noexcept {
        if (!handle)
            return {};
        std::lock_guard lock(handle->mutex);
        if (handle->retired.load(std::memory_order_acquire) ||
            !handle->alive.load(std::memory_order_acquire))
            return {};
        ++handle->active_leases;
        return Lease(handle);
    }

  private:
    Handle state_;
};

} // namespace pulp::runtime
