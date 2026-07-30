#pragma once

#include <pulp/inspect/protocol.hpp>

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>

namespace pulp::inspect {

/// Bounded response-after-apply handoff from an inspector transport thread to
/// the registered host main thread. A request that times out before execution
/// is cancelled and cannot mutate later.
class InspectorMainThreadRpc {
public:
    using Operation = std::function<InspectorMessage()>;
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

    /// Stop accepting requests and cancel every queued operation that has not
    /// started. Safe to call repeatedly during host teardown.
    void cancel();
    bool active() const;

private:
    class Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace pulp::inspect
