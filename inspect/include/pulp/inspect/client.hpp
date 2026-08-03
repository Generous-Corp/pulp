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

    bool connect(const InspectorDiscoveryRecord& record, const InspectorDiscoveryReader& discovery,
                 std::chrono::milliseconds timeout = std::chrono::seconds(3));
    void disconnect();
    bool is_connected() const;

    /// A response timeout or disconnect after sending returns
    /// error_data_json={"mayHaveApplied":true}. A timeout also closes the
    /// connection; either result must not be treated as safely retryable.
    InspectorMessage request(std::string method, std::string params_json = "{}",
                             std::chrono::milliseconds timeout = std::chrono::seconds(3));

    void set_event_handler(EventHandler handler);

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

/// Exact selector for one published inspector generation. Empty fields ask
/// the discovery reader to select the only live publication; callers that can
/// act on more than one process should always provide all three fields.
struct InspectorClientTarget {
    std::string session_id;
    std::string instance_id;
    std::string publication_id;
};

/// Typed outcome for a one-shot inspector request. Client-side discovery and
/// authentication failures use the same stable InspectorMessage error shape as
/// protocol failures, so CLI and MCP do not need separate string parsers.
struct InspectorClientResult {
    std::optional<InspectorDiscoveryRecord> publication;
    InspectorMessage response;

    bool succeeded() const {
        return publication.has_value() && !response.is_error;
    }
};

/// Discover, select, authenticate, request, and disconnect in one bounded
/// operation. Controller-gated methods acquire and release a lease on the same
/// connection. This is the shared installed-client path for CLI and MCP.
InspectorClientResult
request_inspector(std::string method, std::string params_json = "{}",
                  InspectorClientTarget target = {},
                  std::chrono::milliseconds timeout = std::chrono::seconds(3),
                  const InspectorDiscoveryReader& discovery = InspectorDiscoveryReader{});

} // namespace pulp::inspect
