#pragma once

#include <pulp/inspect/control_execution.hpp>
#include <pulp/inspect/control_peer.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace pulp::inspect {

struct ControlHostConnectionConfig {
    std::filesystem::path endpoint_path;
    ControlPeerExpectation expected_broker;
    std::chrono::milliseconds connect_timeout = std::chrono::seconds(3);
    std::chrono::milliseconds write_timeout = std::chrono::seconds(3);
    std::chrono::milliseconds frame_read_timeout = std::chrono::seconds(3);
    std::size_t maximum_queued_executions = 8;
};

/// Authenticated host-role carrier over the canonical ControlEnvelope stream.
///
/// Broker-supplied authority is deliberately projected down to execution data:
/// the host cannot see or mint client/grant identities. The injected executor
/// remains the sole owner of legal-thread scheduling and operation semantics.
class ControlHostConnection {
  public:
    ControlHostConnection(ControlHostConnectionConfig config, ControlOperationExecutor executor);
    ~ControlHostConnection();

    ControlHostConnection(const ControlHostConnection&) = delete;
    ControlHostConnection& operator=(const ControlHostConnection&) = delete;

    bool connect();
    ControlHostOpenResult open_host(std::string_view admission_id,
                                    std::chrono::milliseconds timeout = std::chrono::seconds(3));
    ControlHostOpenResult
    open_host_enrollment(std::string_view enrollment_id,
                         std::chrono::milliseconds timeout = std::chrono::seconds(3));
    void disconnect() noexcept;

    bool is_connected() const;
    bool is_host_open() const;
    std::string last_error_code() const;
    std::string last_error_explanation() const;

  private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace pulp::inspect
