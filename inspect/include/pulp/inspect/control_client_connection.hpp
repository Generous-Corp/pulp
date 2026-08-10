#pragma once

#include <pulp/inspect/control_client.hpp>
#include <pulp/inspect/control_health.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace pulp::inspect {

struct ControlClientConnectionConfig {
    std::filesystem::path endpoint_path;
    ControlPeerExpectation expected_broker;
    /// Optional installed-binary trust anchor. Used when no exact live peer
    /// expectation has been preissued; the carrier still verifies the
    /// kernel-observed peer against this signed executable before any request.
    std::filesystem::path expected_broker_executable;
    std::chrono::milliseconds connect_timeout = std::chrono::seconds(3);
    std::chrono::milliseconds write_timeout = std::chrono::seconds(3);
    std::chrono::milliseconds frame_read_timeout = std::chrono::seconds(3);
};

/// Concrete OS-local transport for one authenticated control session.
///
/// The expected broker is verified from kernel-observed peer credentials before
/// any session or authority message is sent. Calls are synchronous and permit
/// one request in flight. The progress sink runs on the carrier read thread and
/// therefore must be bounded, nonblocking, and exception-free.
class ControlClientConnection final : public ControlClientTransport {
  public:
    using ProgressSink = std::function<void(const ControlProgressEnvelope&)>;

    explicit ControlClientConnection(ControlClientConnectionConfig config);
    ~ControlClientConnection() override;

    ControlClientConnection(const ControlClientConnection&) = delete;
    ControlClientConnection& operator=(const ControlClientConnection&) = delete;

    bool connect();
    ControlSessionOpenResult
    open_session(std::string_view admission_id,
                 std::chrono::milliseconds timeout = std::chrono::seconds(3));
    ControlManagementResult manage(std::string_view command, std::string_view params_json = "{}",
                                   std::chrono::milliseconds timeout = std::chrono::seconds(3));
    void disconnect() noexcept;

    bool is_connected() const;
    bool is_session_open() const;
    std::string last_error_code() const;
    std::string last_error_explanation() const;

    void set_progress_sink(ProgressSink sink);

    ControlTransportDispatchResult dispatch(std::string_view encoded_envelope,
                                            std::chrono::milliseconds timeout) override;
    ControlArtifactReadResult read_artifact(std::string_view artifact_id, std::uint64_t offset,
                                            std::size_t maximum_bytes,
                                            std::chrono::milliseconds timeout) override;

  private:
    friend ControlBrokerHealthProbeResult probe_control_broker(const ControlHealthProbeConfig&,
                                                               std::chrono::milliseconds);

    explicit ControlClientConnection(const ControlHealthProbeConfig& config);
    ControlTransportDispatchResult dispatch_probe(const ControlEnvelope& envelope,
                                                  std::chrono::milliseconds timeout);
    bool carrier_was_reached() const;
    bool broker_was_verified() const;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulp::inspect
