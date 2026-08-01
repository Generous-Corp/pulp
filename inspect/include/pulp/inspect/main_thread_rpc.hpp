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
/// Calls already executing inline on the main thread cannot be preempted.
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
    InspectorMessage call(std::int64_t request_id,
                          Operation operation,
                          Completion completion);

    /// Stop accepting requests and cancel every queued operation that has not
    /// started. Running operations retain their own state and are not waited
    /// for. Safe to call repeatedly during host teardown.
    void cancel();
    bool active() const;

private:
    class Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace pulp::inspect
