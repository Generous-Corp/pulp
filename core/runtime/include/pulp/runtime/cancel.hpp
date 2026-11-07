#pragma once

// Cooperative cancellation.
//
//   CancelSource  - move-only owner. Calls `cancel()`, and cancels on destruction.
//   CancelToken   - copyable observer handed to the work being cancelled.
//   CancelRegistration - RAII handle for a callback registered on a token.
//
// A default-constructed CancelToken is never cancelled, so APIs can take a
// `const CancelToken& = {}` parameter and treat "no token" as "cannot be
// cancelled" without a null check.
//
// Threading: every member is safe to call from any thread. `is_cancelled()` is
// a single acquire load of a lock-free atomic. It never locks and never
// allocates, so it may be polled from the audio thread. Everything else
// (`wait_for_cancellation`, `register_callback`, `cancel`) takes a mutex and
// may allocate, and must stay off the audio thread.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>

namespace pulp::runtime {

class CancelToken;
class CancelSource;

namespace detail {
/// Public part of the shared state so `is_cancelled()` can inline to one load.
/// The rest (mutex, callbacks) lives in cancel.cpp.
struct CancelStateBase {
    std::atomic<bool> cancelled{false};
    virtual ~CancelStateBase() = default;
};
} // namespace detail

/// Callback run when a token is cancelled. It runs on the thread that calls
/// `CancelSource::cancel()`, or inline on the registering thread when the token
/// is already cancelled. It must not throw; an escaping exception is swallowed.
using CancelCallback = std::function<void()>;

/// RAII registration returned by `CancelToken::register_callback`.
///
/// Destroying (or `reset()`-ing) a registration guarantees the callback is not
/// running and will not run afterwards: if another thread is mid-callback, the
/// destructor waits for it to return. A registration destroyed from inside its
/// own callback does not wait.
class CancelRegistration {
  public:
    CancelRegistration() noexcept = default;
    ~CancelRegistration();

    CancelRegistration(const CancelRegistration&) = delete;
    CancelRegistration& operator=(const CancelRegistration&) = delete;
    CancelRegistration(CancelRegistration&& other) noexcept;
    CancelRegistration& operator=(CancelRegistration&& other) noexcept;

    /// True while the callback is still registered and has not yet been run.
    [[nodiscard]] explicit operator bool() const noexcept {
        return id_ != 0;
    }

    /// Unregister now. Idempotent.
    void reset() noexcept;

  private:
    friend class CancelToken;
    CancelRegistration(std::shared_ptr<detail::CancelStateBase> state, std::uint64_t id) noexcept
        : state_(std::move(state)), id_(id) {}

    std::shared_ptr<detail::CancelStateBase> state_;
    std::uint64_t id_ = 0;
};

class CancelToken {
  public:
    /// A token that can never be cancelled.
    CancelToken() noexcept = default;

    /// True once the owning source has been cancelled. Lock-free and
    /// allocation-free; safe to poll from the audio thread.
    [[nodiscard]] bool is_cancelled() const noexcept {
        return state_ && state_->cancelled.load(std::memory_order_acquire);
    }

    /// True if this token can ever become cancelled (it is bound to a source).
    [[nodiscard]] bool can_be_cancelled() const noexcept {
        return state_ != nullptr;
    }

    /// Block until cancelled or `timeout` elapses. Returns true when cancelled.
    /// On a token that cannot be cancelled this simply sleeps for `timeout`.
    /// Not for the audio thread.
    bool wait_for_cancellation(std::chrono::nanoseconds timeout) const;

    /// Run `fn` when the token is cancelled. If it is already cancelled, `fn`
    /// runs immediately on the calling thread and the returned registration is
    /// empty. On a token that cannot be cancelled `fn` never runs and the
    /// registration is empty. Callbacks run in registration order.
    [[nodiscard]] CancelRegistration register_callback(CancelCallback fn) const;

    /// True if both tokens observe the same underlying source.
    [[nodiscard]] bool shares(const CancelToken& other) const noexcept {
        return state_ == other.state_;
    }

  protected:
    using State = detail::CancelStateBase;
    explicit CancelToken(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}

    /// Mark the state cancelled, wake waiters, run callbacks. Returns true if
    /// this call performed the transition.
    static bool request_cancel(const std::shared_ptr<State>& state) noexcept;
    static std::shared_ptr<State> make_state();

    std::shared_ptr<State> state_;

    friend class CancelSource;
};

/// Move-only owner of a cancellation state. Cancels when destroyed, so work
/// observing a token never outlives the scope that owns the source without
/// being told to stop.
class CancelSource {
  public:
    CancelSource();
    ~CancelSource();

    CancelSource(const CancelSource&) = delete;
    CancelSource& operator=(const CancelSource&) = delete;
    CancelSource(CancelSource&& other) noexcept = default;
    /// Cancels the state this source currently owns, then takes over `other`.
    CancelSource& operator=(CancelSource&& other) noexcept;

    /// Observer for this source. Tokens remain valid after the source dies.
    [[nodiscard]] CancelToken token() const noexcept {
        return token_;
    }

    /// Signal cancellation. Idempotent. Returns true if this call was the one
    /// that cancelled. Wakes `wait_for_cancellation` callers and runs
    /// registered callbacks on the calling thread.
    bool cancel() noexcept;

    [[nodiscard]] bool is_cancelled() const noexcept {
        return token_.is_cancelled();
    }

  private:
    CancelToken token_;
};

/// Former name of the shared token. It both observes and requests cancellation:
/// every copy shares state and any copy may call `cancel()`. A default-constructed
/// instance owns live state. New code should hold a `CancelSource` and pass
/// `CancelToken` to the work being cancelled; a `CancellationToken` converts to
/// `CancelToken` implicitly. Unlike `CancelSource` it does not cancel on
/// destruction.
class CancellationToken : public CancelToken {
  public:
    CancellationToken();

    void cancel() noexcept {
        request_cancel(state_);
    }
};

} // namespace pulp::runtime
