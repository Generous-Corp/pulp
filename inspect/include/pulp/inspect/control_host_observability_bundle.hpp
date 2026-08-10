#pragma once

#include <pulp/inspect/control_execution.hpp>
#include <pulp/inspect/control_identity.hpp>
#include <pulp/inspect/control_telemetry_tap.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace pulp::inspect {

struct ControlHostObservabilityBinding {
    ControlRegistrationId registration_id;
    std::string session_id;
    std::string instance_id;
    std::string publication_id;
    std::string authentication_token;

    friend bool operator==(const ControlHostObservabilityBinding&,
                           const ControlHostObservabilityBinding&) = default;
};

struct ControlHostObservabilityBundleConfig {
    ControlHostObservabilityBinding binding;
    ControlOperationExecutor trace_executor;
    /// Optional unless the host manifest declares telemetry/subscribe.
    std::shared_ptr<ControlTelemetryTap> telemetry;
    std::chrono::milliseconds heartbeat_ttl = std::chrono::seconds(30);
    std::function<std::chrono::steady_clock::time_point()> clock;
};

/// Registration-scoped canonical trace and telemetry executor bundle.
///
/// The host installs executor() before publishing the registration. Every
/// operation is then checked against the complete broker-minted authority
/// plan. The authentication token is an out-of-band host-connection secret;
/// it is never accepted from an operation request. Expiry or disconnect
/// destroys trace ownership and all telemetry subscriptions.
class ControlHostObservabilityBundle {
  public:
    static std::unique_ptr<ControlHostObservabilityBundle>
    create(ControlHostObservabilityBundleConfig config);
    ~ControlHostObservabilityBundle();

    ControlHostObservabilityBundle(const ControlHostObservabilityBundle&) = delete;
    ControlHostObservabilityBundle& operator=(const ControlHostObservabilityBundle&) = delete;

    ControlOperationExecutor executor() const;
    bool heartbeat(const ControlHostObservabilityBinding& authenticated_binding);
    void end_authority(std::string_view opaque_authority_id) noexcept;
    bool ready() const;
    void disconnect() noexcept;

  private:
    struct State;
    explicit ControlHostObservabilityBundle(std::shared_ptr<State> state);
    std::shared_ptr<State> state_;
};

} // namespace pulp::inspect
