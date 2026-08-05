#pragma once

#include <pulp/inspect/audit.hpp>
#include <pulp/inspect/capabilities.hpp>
#include <pulp/inspect/protocol.hpp>
#include <pulp/inspect/test_input.hpp>

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <optional>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace pulp::inspect {

class InspectorMainThreadRpc;

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

struct InspectorControllerScopeEnd {
    std::string session_id;
    std::string instance_id;
    std::string client_id;
    TestInputReleaseReason reason = TestInputReleaseReason::ControllerReleased;
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

public:
    struct EndedScope {
        std::string owner;
        TestInputReleaseReason reason;
    };

private:

    void expire_if_needed();
    bool begin_operation(std::string_view owner);
    void end_operation(std::string_view owner);
    bool release_with_reason(std::string_view owner,
                             TestInputReleaseReason reason);
    void terminate();
    std::vector<EndedScope> take_ended_scopes();
    void finish_scope(TestInputReleaseReason reason);

    std::chrono::milliseconds ttl_;
    Clock clock_;
    std::string owner_;
    std::chrono::steady_clock::time_point expires_at_{};
    std::size_t active_operations_ = 0;
    bool release_pending_ = false;
    TestInputReleaseReason pending_reason_ =
        TestInputReleaseReason::ControllerReleased;
    std::vector<EndedScope> ended_scopes_;
};

struct InspectorSessionInfo {
    std::string session_id;
    std::string instance_id;
    std::string plugin_id;
    std::string protocol_version = "1";
};

/// Authenticated transport identity attached to a domain request.
///
/// Domain handlers that own per-client state (for example, telemetry
/// subscriptions) must key it from this context instead of trusting a client
/// identifier supplied in request JSON.
struct InspectorRequestContext {
    /// Borrowed for the duration of the handler call; copy it when retaining
    /// per-client state beyond the request.
    std::string_view client_id;
};

/// Capability-enforcing dispatch facade. Transport supplies an authenticated
/// per-connection client identity; the session owns policy and the controller
/// lease, then serializes authorized domain requests before delegating to the
/// attached handler. Session control remains available while a handler runs.
class InspectorSession {
public:
    using RequestHandler =
        std::function<InspectorMessage(const InspectorMessage& request)>;
    using ContextRequestHandler = std::function<InspectorMessage(
        const InspectorRequestContext& context,
        const InspectorMessage& request)>;
    using ControllerScopeEndHandler =
        std::function<void(const InspectorControllerScopeEnd& event)>;
    using ClientDisconnectHandler =
        std::function<void(std::string_view client_id)>;

    InspectorSession(InspectorSessionInfo info,
                     InspectorPolicyConfig policy,
                     RequestHandler handler,
                     std::chrono::milliseconds lease_ttl =
                         std::chrono::seconds(15),
                     InspectorControllerLease::Clock clock =
                         [] { return std::chrono::steady_clock::now(); });
    InspectorSession(InspectorSessionInfo info,
                     InspectorPolicyConfig policy,
                     ContextRequestHandler handler,
                     std::chrono::milliseconds lease_ttl =
                         std::chrono::seconds(15),
                     InspectorControllerLease::Clock clock =
                         [] { return std::chrono::steady_clock::now(); });
    ~InspectorSession();

    InspectorMessage handle(std::string_view client_id,
                            const InspectorMessage& request);
    /// Install the generation-scoped main-thread handoff used for domain
    /// requests. Session control methods remain synchronous on their caller.
    void set_main_thread_rpc(std::shared_ptr<InspectorMainThreadRpc> rpc);
    /// Route one exact authorized method through a handler that is safe to run
    /// concurrently on its transport thread. This is reserved for operations,
    /// such as interruption, that must reach work currently occupying the
    /// serialized main-thread lane. Passing an empty handler removes the route.
    void set_concurrent_request_handler(
        std::string method, ContextRequestHandler handler);
    /// Install the host cleanup hook for controller-scoped test input. The
    /// callback runs outside session locks and must transfer work to the host's
    /// owning thread when the caller is not already on it.
    void set_controller_scope_end_handler(ControllerScopeEndHandler handler);
    /// Install generation-scoped cleanup for per-client domain state. The
    /// callback runs outside session locks and may be called from a transport
    /// callback thread; it must hand off to its owner when necessary.
    void set_client_disconnect_handler(ClientDisconnectHandler handler);
    void set_audit_log(std::shared_ptr<InspectorAuditLog> audit_log);
    void disconnect(std::string_view client_id);
    /// Refuse new work without waiting for already-admitted concurrent work.
    /// Reentrant teardown uses this before deferring the actual drain.
    void close_dispatch_admission();
    /// Run completion after every already-admitted concurrent dispatch has
    /// returned. Teardown uses this to break main-thread RPC wait cycles.
    void after_concurrent_dispatches(std::function<void()> completion);
    /// Cancel queued domain handlers and reject new ones during server teardown.
    /// The handler already executing on the dispatch owner may finish.
    void suspend_dispatches();
    /// Reopen domain-handler admission for a new authenticated server generation.
    void resume_dispatches();
    /// Snapshot whether new domain dispatches are currently admitted.
    bool dispatches_accepting() const;

    const InspectorSessionInfo& info() const { return info_; }
    const InspectorAccessPolicy& policy() const { return policy_; }

private:
    class State;
    InspectorMessage handle_session_method(std::string_view client_id,
                                           const InspectorMessage& request);

    InspectorSessionInfo info_;
    InspectorAccessPolicy policy_;
    std::shared_ptr<State> state_;
};

} // namespace pulp::inspect
