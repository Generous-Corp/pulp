// inspector_server.hpp — TCP server for remote inspector access
// Accepts multiple clients, dispatches requests, broadcasts events.
#pragma once

#include <pulp/inspect/discovery_publisher.hpp>
#include <pulp/inspect/main_thread_rpc.hpp>
#include <pulp/inspect/protocol.hpp>
#include <pulp/inspect/publication_binding.hpp>
#include <pulp/inspect/session.hpp>
#include <pulp/events/interprocess_connection.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace pulp::inspect {

/// Waitable proof that an InspectorServer's cleanup worker has exited. Capture
/// this before allowing publication/domain callbacks to destroy the server.
/// Waiting from the cleanup worker itself returns false instead of deadlocking.
class InspectorServerShutdownFence {
public:
    struct State;

    InspectorServerShutdownFence() = default;

    bool ready() const noexcept;
    bool wait() const;
    bool wait_for(std::chrono::milliseconds timeout) const;

private:
    friend class InspectorServer;
    explicit InspectorServerShutdownFence(std::shared_ptr<State> state)
        : state_(std::move(state)) {}

    std::shared_ptr<State> state_;
};

struct InspectorServerConfig {
    InspectorSession* session = nullptr;
    InspectorDiscoveryPublisher* discovery = nullptr;
    InspectorDiscoveryRecord record;
    std::vector<std::uint8_t> token;
    std::chrono::milliseconds authentication_timeout =
        std::chrono::seconds(3);
    std::chrono::milliseconds frame_read_timeout =
        std::chrono::seconds(3);
    // Discovery TTL is derived as at least three times this interval.
    std::chrono::milliseconds heartbeat_interval =
        std::chrono::seconds(10);
    std::size_t max_message_bytes = 1024u * 1024u;
    std::size_t max_clients = 16;
    // Appended to preserve positional initialization of the original fields.
    // The composition root supplies all publication-scoped domain bindings.
    InspectorDomainPublicationBindings* domain_bindings = nullptr;
    // Appended for source-compatible test/host injection. When absent, each
    // authenticated server generation creates a default main-thread RPC.
    std::shared_ptr<InspectorMainThreadRpc> main_thread_rpc;
};

/// TCP server exposing the inspector protocol to external tools.
/// Wraps InterprocessConnectionServer for multi-client support.
class InspectorServer {
public:
    /// Create server. Does not listen until start_authenticated() succeeds.
    InspectorServer();
    ~InspectorServer();
    InspectorServer(const InspectorServer&) = delete;
    InspectorServer& operator=(const InspectorServer&) = delete;
    InspectorServer(InspectorServer&&) = delete;
    InspectorServer& operator=(InspectorServer&&) = delete;

    /// Start an authenticated, ephemeral loopback session and publish its
    /// discovery record. The session and publisher must outlive the server.
    bool start_authenticated(InspectorServerConfig config);

    /// Stop listening and disconnect all clients.
    void stop();

    /// Obtain a lifetime fence before permitting callbacks to destroy this
    /// wrapper. After callback-triggered destruction, wait on a non-callback
    /// thread before tearing down attached sources or unloading module code.
    /// Ordinary external destruction already waits synchronously.
    InspectorServerShutdownFence shutdown_fence() const;

    /// Broadcast an event to all connected clients.
    void broadcast(const InspectorMessage& event);

    /// Number of connected clients.
    int client_count() const;

    /// The port we're actually listening on (may differ from requested if 0 was passed).
    int port() const;

private:
    class Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace pulp::inspect
