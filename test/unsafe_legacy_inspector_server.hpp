#pragma once

#include <pulp/events/interprocess_connection.hpp>
#include <pulp/inspect/protocol.hpp>

#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace pulp::inspect::test {

/// Test-only unauthenticated transport fixture for legacy framing/broadcast
/// coverage. Production hosts must use InspectorServer::start_authenticated.
class UnsafeLegacyInspectorServer {
public:
    using RequestHandler =
        std::function<InspectorMessage(const InspectorMessage&)>;

    UnsafeLegacyInspectorServer();
    ~UnsafeLegacyInspectorServer();

    bool start(int port = 0);
    void stop();
    void set_request_handler(RequestHandler handler);
    void broadcast(const InspectorMessage& event);
    int client_count() const;
    int port() const { return port_; }
    void advertise_port() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
    int port_ = 0;
    RequestHandler handler_;
    mutable std::string discovery_file_;

    void on_message_received(const std::string& data,
                             events::InterprocessConnection* sender);
};

} // namespace pulp::inspect::test
