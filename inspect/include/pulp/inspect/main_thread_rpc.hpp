#pragma once

#include <pulp/inspect/protocol.hpp>

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>

namespace pulp::inspect {

/// Bounded response-after-apply handoff from an inspector transport thread to
/// the registered host main thread. A request that times out before execution
/// is cancelled and cannot mutate later. A request that times out after
/// execution starts returns a fenced `mayHaveApplied` error at the deadline;
/// the posted task retains its own state and discards any late response.
/// Operations must therefore own everything they may access until they return.
/// Calls already executing inline on the main thread cannot be preempted and
/// therefore cannot return at their deadline. If inline work crosses the
/// deadline, `call()` reports a fenced timeout only after that work returns.
/// A Post implementation owns every callable copy it accepts until that copy
/// is destroyed, including cancelled callables that will never execute.
class InspectorMainThreadRpc {
  public:
    using Operation = std::function<InspectorMessage()>;
    using Completion = std::function<void()>;
    using Post = std::function<bool(std::function<void()>)>;
    using IsMainThread = std::function<bool()>;

    struct Config {
        std::chrono::milliseconds timeout = std::chrono::seconds(2);
        std::size_t max_pending = 64;
    };

    InspectorMainThreadRpc();
    explicit InspectorMainThreadRpc(Config config);
    InspectorMainThreadRpc(Config config, Post post, IsMainThread is_main_thread);
    ~InspectorMainThreadRpc();

    InspectorMainThreadRpc(const InspectorMainThreadRpc&) = delete;
    InspectorMainThreadRpc& operator=(const InspectorMainThreadRpc&) = delete;

    InspectorMessage call(std::int64_t request_id, Operation operation);
    /// As call(), with a callback that runs exactly once when the operation is
    /// no longer capable of applying: immediately for work cancelled before it
    /// starts, or after a started operation actually returns. This distinction
    /// lets owners retain mutation leases across a fenced started timeout.
    InspectorMessage call(std::int64_t request_id, Operation operation, Completion completion);
    /// As call(), with a deadline chosen for this request. Non-positive values
    /// clamp to one millisecond, matching Config normalization. The completion
    /// and fenced-timeout lifetime semantics are otherwise identical.
    InspectorMessage call(std::int64_t request_id, Operation operation, Completion completion,
                          std::chrono::milliseconds timeout);
    /// Strict variant for callers that require an enforceable response
    /// deadline. It never executes inline: invocation from the registered main
    /// thread fails before apply and runs completion exactly once. Other
    /// callers use the normal bounded queued path.
    InspectorMessage call_queued_only(std::int64_t request_id, Operation operation,
                                      Completion completion, std::chrono::milliseconds timeout);

    /// Stop accepting requests and cancel every queued operation that has not
    /// started. Running operations retain their own state and are not waited
    /// for. Safe to call repeatedly during host teardown.
    void cancel();
    /// Cancel queued work and wait for every operation already executing on
    /// another thread to reach its completion callback. Accepted dispatcher
    /// closures that have not run are cancelled but remain owned by the
    /// dispatcher; this operation does not wait for their callable storage to
    /// be released. When called from the executing operation itself,
    /// cancellation is applied but waiting is skipped so reentrant host
    /// teardown cannot deadlock.
    void cancel_and_wait();
    /// Install callbacks bracketing the aggregate lifetime of each posted
    /// callable handed to Post. Copies made by Post share one lifetime. The end
    /// callback runs only when the final callable copy is destroyed. May be
    /// installed only once and while no posted callable lifetime is
    /// outstanding; returns false otherwise.
    bool set_posted_lifetime_callbacks(Completion begin, Completion end);
    bool executing_on_current_thread() const;
    /// Queue teardown work for the exact point at which the current operation
    /// has delivered its completion and left RPC execution. Returns false when
    /// the caller is not inside an operation owned by this RPC.
    bool after_current_operation(Completion completion);
    bool active() const;
    /// Normalized per-call ceiling configured for this dispatcher.
    std::chrono::milliseconds default_timeout() const;

  private:
    InspectorMessage call_with_inline_policy(std::int64_t request_id, Operation operation,
                                             Completion completion,
                                             std::chrono::milliseconds timeout, bool allow_inline);

    class Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace pulp::inspect
