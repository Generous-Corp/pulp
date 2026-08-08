#pragma once

#include <pulp/inspect/client.hpp>
#include <pulp/inspect/control_client.hpp>
#include <pulp/inspect/control_grants.hpp>
#include <pulp/inspect/control_identity.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <string>

namespace pulp::inspect {

/// Authority material returned by an enrollment-aware client integration.
///
/// The transport is already connected, peer-authenticated, and has an open
/// broker session. The IDs and target are broker-minted outputs; this façade
/// only binds them into a canonical operation request.
struct InspectorControlSession {
    std::unique_ptr<ControlClientTransport> transport;
    ControlClientId client_id;
    ControlRegistrationId registration_id;
    ControlGrantId grant_id;
    std::string instance_generation;
    InspectorClientTarget target;
    std::uint64_t expected_state_generation = 0;
};

/// Injected enrollment/session boundary for canonical Inspector operations.
/// Implementations own discovery, broker authentication, enrollment, and
/// grant acquisition. This interface does not create a second authority path.
class InspectorControlSessionOpener {
  public:
    virtual ~InspectorControlSessionOpener() = default;

    virtual std::optional<InspectorControlSession> open(std::chrono::milliseconds timeout) = 0;
};

/// Execute one already-supported trace Inspector method through the canonical
/// capability-control carrier. Only Trace.startSession and Trace.stopSession
/// are accepted. Progress is carrier-validated and deliberately not surfaced.
InspectorClientResult
request_control_inspector(InspectorControlSessionOpener& opener, std::string method,
                          std::string params_json = "{}",
                          std::chrono::milliseconds timeout = std::chrono::seconds(3));

/// Installed-process entry point for trace lifecycle adapters. Production is
/// default-denied until the broker installs an enrollment/session opener;
/// callers never fall back to legacy discovery or a raw host/port connection.
InspectorClientResult
request_control_inspector(std::string method, std::string params_json = "{}",
                          std::chrono::milliseconds timeout = std::chrono::seconds(3));

} // namespace pulp::inspect
