#pragma once

#include <pulp/inspect/discovery.hpp>
#include <pulp/inspect/protocol.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>

namespace pulp::inspect {

/// Lightweight authenticated inspector client shared by CLI and MCP.
class InspectorClient {
public:
    using EventHandler = std::function<void(const InspectorMessage&)>;

    InspectorClient();
    ~InspectorClient();

    InspectorClient(const InspectorClient&) = delete;
    InspectorClient& operator=(const InspectorClient&) = delete;

    bool connect(const InspectorDiscoveryRecord& record,
                 const InspectorDiscoveryReader& discovery,
                 std::chrono::milliseconds timeout =
                     std::chrono::seconds(3));
    void disconnect();
    bool is_connected() const;

    /// A response timeout closes the connection and returns
    /// error_data_json={"mayHaveApplied":true}; the sent operation must not be
    /// treated as safely retryable.
    InspectorMessage request(
        std::string method,
        std::string params_json = "{}",
        std::chrono::milliseconds timeout = std::chrono::seconds(3));

    void set_event_handler(EventHandler handler);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulp::inspect
