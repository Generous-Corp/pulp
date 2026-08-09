#pragma once

#include <pulp/inspect/control_connection_admission.hpp>
#include <pulp/inspect/control_host_enrollment.hpp>
#include <pulp/inspect/control_service.hpp>
#include <pulp/inspect/control_trusted_host_launcher.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace pulp::inspect {

class ControlHostRouter;
class ControlBroker;

/// Explicit injection seam for endpoint-owned enrollment. It is null in the
/// production daemon until an endpoint-owned private preflight rendezvous can
/// supply an exact VerifiedControlPeerIdentity; a PID-only launcher is
/// intentionally insufficient.
struct ControlEndpointEnrollmentContext {
    ControlHostEnrollmentStore& enrollments;
    ControlBroker& broker;
    ControlConnectionAdmissionStore& admissions;
};

using ControlAdmissionConsumer =
    std::function<std::optional<ControlConnectionAdmission>(std::string_view admission_id)>;

struct ControlTrustedHostManagementLaunchResult {
    ControlTrustedHostLaunchStatus status = ControlTrustedHostLaunchStatus::InvalidConfiguration;
    std::string explanation;
};

struct ControlTrustedHostManagement {
    std::function<ControlTrustedHostInventoryPrepareResult(const ControlTrustedHostLaunchIntent&)>
        prepare;
    std::function<ControlTrustedHostManagementLaunchResult(std::string_view)> launch;
};

struct ControlEndpointConfig {
    std::filesystem::path endpoint_path;
    std::string sdk_version;
    std::string broker_id;
    std::uint64_t process_generation = 0;
    std::size_t maximum_connections = 16;
    std::size_t maximum_queued_frames_per_connection = 16;
    std::size_t maximum_queued_bytes_per_connection = 2 * kControlMaximumEnvelopeBytes;
    std::chrono::milliseconds write_timeout = std::chrono::seconds(3);
    std::chrono::milliseconds frame_read_timeout = std::chrono::seconds(3);
    /// Installed adapters may self-enroll only when this broker-owned policy
    /// accepts kernel-observed, code-signed peer evidence.
    std::function<bool(const ControlPeerEvidence&)> authorize_client;
    /// Maps an already-authorized, kernel-observed peer to a broker-owned
    /// reconnectable principal. Absence keeps enrollment connection-scoped.
    struct DurableClientPrincipal {
        std::string value;
        ControlDurableClientLifetime lifetime = ControlDurableClientLifetime::Broker;
    };
    std::function<std::optional<DurableClientPrincipal>(const ControlPeerEvidence&)>
        durable_client_principal;
    /// Optional trusted consent source. Absence means grant requests return
    /// consent-required; request payloads can never claim this authority.
    std::function<ControlConsentDecision(const ControlGrantConsentRequest&)> decide_consent;
    /// Typed broker-owned host launch seam. It remains unreachable until the
    /// local peer has completed canonical client enrollment.
    ControlTrustedHostManagement trusted_hosts;
};

/// OS-local carrier for one ControlService composition root.
///
/// Each InterprocessConnection frame contains one canonical ControlEnvelope.
/// The endpoint consumes a one-time broker-owned admission before verifying
/// the kernel-observed peer and opening a connection-bound service session.
class ControlEndpoint {
  public:
    ControlEndpoint(ControlService& service, ControlAdmissionConsumer consume_admission,
                    ControlEndpointConfig config, ControlHostRouter* host_router = nullptr,
                    ControlEndpointEnrollmentContext* enrollment_context = nullptr,
                    ControlBroker* management_broker = nullptr);
    ~ControlEndpoint();

    ControlEndpoint(const ControlEndpoint&) = delete;
    ControlEndpoint& operator=(const ControlEndpoint&) = delete;

    bool start();
    void stop() noexcept;
    bool is_listening() const noexcept;
    const std::filesystem::path& endpoint_path() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulp::inspect
