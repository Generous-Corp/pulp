#pragma once

#include <pulp/inspect/capabilities.hpp>
#include <pulp/inspect/protocol.hpp>

#include <chrono>
#include <functional>
#include <optional>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::inspect {

struct InspectorPolicyConfig {
    InspectorProfile profile = InspectorProfile::Off;
    std::vector<InspectorCapability> custom_capabilities;
    std::vector<InspectorCapability> available_capabilities;
    bool runtime_eval_enabled = false;
};

/// Resolves named profiles and exact custom grants against capabilities that
/// the host actually attached. Resolution is fail-closed: an empty availability
/// set exposes nothing, and runtime.eval additionally requires its separate
/// acknowledgement.
class InspectorAccessPolicy {
public:
    explicit InspectorAccessPolicy(InspectorPolicyConfig config = {});

    InspectorProfile profile() const { return profile_; }
    std::span<const InspectorCapability> available_capabilities() const {
        return available_;
    }
    std::span<const InspectorCapability> effective_capabilities() const {
        return effective_;
    }

    bool is_available(InspectorCapability capability) const;
    bool is_granted(InspectorCapability capability) const;

    /// Returns an error response when the request is unknown, unavailable, not
    /// granted, or requires a controller lease that the caller does not own.
    std::optional<InspectorMessage> authorize(const InspectorMessage& request,
                                              bool owns_controller_lease) const;

private:
    InspectorProfile profile_ = InspectorProfile::Off;
    std::vector<InspectorCapability> available_;
    std::vector<InspectorCapability> effective_;
};

enum class ControllerLeaseResult {
    Acquired,
    Renewed,
    HeldByOther,
    InvalidOwner,
};

/// One-controller lease for mutating inspector operations. The injected clock
/// keeps expiry deterministic in tests and avoids wall-clock jumps.
class InspectorControllerLease {
public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    explicit InspectorControllerLease(
        std::chrono::milliseconds ttl = std::chrono::seconds(15),
        Clock clock = [] { return std::chrono::steady_clock::now(); });

    ControllerLeaseResult acquire(std::string_view owner);
    ControllerLeaseResult renew(std::string_view owner);
    bool release(std::string_view owner);
    void disconnect(std::string_view owner);

    bool owns(std::string_view owner);
    std::optional<std::string> owner();
    std::chrono::milliseconds remaining();

private:
    friend class InspectorSession;

    void expire_if_needed();
    bool begin_operation(std::string_view owner);
    void end_operation(std::string_view owner);

    std::chrono::milliseconds ttl_;
    Clock clock_;
    std::string owner_;
    std::chrono::steady_clock::time_point expires_at_{};
    std::size_t active_operations_ = 0;
    bool release_pending_ = false;
};

struct InspectorSessionInfo {
    std::string session_id;
    std::string instance_id;
    std::string plugin_id;
    std::string protocol_version = "1";
};

/// Capability-enforcing dispatch facade. Transport supplies an authenticated
/// per-connection client identity; the session owns policy and the controller
/// lease, then serializes authorized domain requests before delegating to the
/// attached handler. Session control remains available while a handler runs.
class InspectorSession {
public:
    using RequestHandler =
        std::function<InspectorMessage(const InspectorMessage& request)>;

    InspectorSession(InspectorSessionInfo info,
                     InspectorPolicyConfig policy,
                     RequestHandler handler,
                     std::chrono::milliseconds lease_ttl =
                         std::chrono::seconds(15),
                     InspectorControllerLease::Clock clock =
                         [] { return std::chrono::steady_clock::now(); });

    InspectorMessage handle(std::string_view client_id,
                            const InspectorMessage& request);
    void disconnect(std::string_view client_id);

    const InspectorSessionInfo& info() const { return info_; }
    const InspectorAccessPolicy& policy() const { return policy_; }

private:
    InspectorMessage handle_session_method(std::string_view client_id,
                                           const InspectorMessage& request);

    InspectorSessionInfo info_;
    InspectorAccessPolicy policy_;
    RequestHandler handler_;
    InspectorControllerLease lease_;
    mutable std::mutex mutex_;
    std::recursive_mutex dispatch_mutex_;
};

} // namespace pulp::inspect
