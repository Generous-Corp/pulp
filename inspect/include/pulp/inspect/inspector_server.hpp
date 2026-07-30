// inspector_server.hpp — TCP server for remote inspector access
// Accepts multiple clients, dispatches requests, broadcasts events.
#pragma once

#include <pulp/inspect/discovery.hpp>
#include <pulp/inspect/protocol.hpp>
#include <pulp/inspect/session.hpp>
#include <pulp/events/interprocess_connection.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace pulp::inspect {

struct InspectorServerConfig {
    InspectorSession* session = nullptr;
    InspectorDiscoveryPublisher* discovery = nullptr;
    InspectorDiscoveryRecord record;
    std::vector<std::uint8_t> token;
    std::chrono::milliseconds authentication_timeout =
        std::chrono::seconds(3);
    std::chrono::milliseconds frame_read_timeout =
        std::chrono::seconds(3);
    std::chrono::milliseconds heartbeat_interval =
        std::chrono::seconds(10);
    std::size_t max_message_bytes = 1024u * 1024u;
    std::size_t max_clients = 16;
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
