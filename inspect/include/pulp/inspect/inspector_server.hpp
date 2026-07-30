// inspector_server.hpp — TCP server for remote inspector access
// Accepts multiple clients, dispatches requests, broadcasts events.
#pragma once

#include <pulp/inspect/protocol.hpp>
#include <pulp/events/interprocess_connection.hpp>

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace pulp::inspect {

using RequestHandler = std::function<InspectorMessage(const InspectorMessage& request)>;

/// TCP server exposing the inspector protocol to external tools.
/// Wraps InterprocessConnectionServer for multi-client support.
class InspectorServer {
public:
    /// Create server. Does not start listening until start() is called.
    InspectorServer();
    ~InspectorServer();

    /// Start listening on the given port. Returns true on success.
    /// Default port: 9147. Configurable via PULP_INSPECTOR_PORT env var.
    bool start(int port = 0);

    /// Stop listening and disconnect all clients.
    void stop();

    /// Set the handler that processes incoming requests.
    /// Called on a background thread — handler must be thread-safe or
    /// marshal to UI thread internally.
    void set_request_handler(RequestHandler handler);

    /// Broadcast an event to all connected clients.
    void broadcast(const InspectorMessage& event);

    /// Number of connected clients.
    int client_count() const;

    /// The port we're actually listening on (may differ from requested if 0 was passed).
    int port() const { return port_; }

    /// Write port to discovery file so CLI tools can find us.
    void advertise_port() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
    int port_ = 0;
    RequestHandler handler_;
    mutable std::mutex clients_mutex_;
    mutable std::string discovery_file_;

    void on_message_received(const std::string& data, events::InterprocessConnection* sender);
};

} // namespace pulp::inspect
