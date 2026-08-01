#pragma once

#include <pulp/inspect/discovery.hpp>
#include <pulp/inspect/protocol.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace pulp::inspect {

/// Stable pre-session connection failure. The transport currently exposes a
/// single connect failure result, so socket refusal and socket timeout remain
/// grouped under transport_connection_failed.
struct InspectorClientConnectFailure {
    std::string code;
    std::string message;
};

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
    bool connect(const InspectorDiscoveryRecord& record,
                 const InspectorDiscoveryReader& discovery,
                 std::chrono::milliseconds timeout,
                 InspectorClientConnectFailure* failure);
    void disconnect();
    bool is_connected() const;

    /// A response timeout or disconnect after sending returns
    /// error_data_json={"mayHaveApplied":true}. A timeout also closes the
    /// connection; either result must not be treated as safely retryable.
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
