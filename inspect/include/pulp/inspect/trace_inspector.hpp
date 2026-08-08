// trace_inspector.hpp — Bridges the process-global pulp::runtime::Tracing
// controller to the inspector protocol's Trace.* methods, so the `pulp trace`
// CLI can start / stop / inspect a Perfetto session over the inspector wire.
//
// Session state lives in pulp::runtime::Tracing (a process singleton — a DAW
// hosts one session per process, not per plugin instance). Each bridge is bound
// to one exact inspector publication generation so one plugin instance cannot
// stop a capture owned by another.
// Query and explain are reserved protocol methods that fail explicitly until
// an analysis backend exists. Safe to construct and call in any build config:
// with PULP_TRACING=OFF lifecycle requests return protocol errors.
#pragma once

#include <pulp/inspect/protocol.hpp>
#include <pulp/runtime/trace_session.hpp>

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

namespace pulp::inspect {

class TraceOwnerLease {
public:
    virtual ~TraceOwnerLease() = default;
    virtual InspectorMessage handle(const InspectorMessage& request) = 0;
};

struct TracePublicationOwner {
    std::string session_id;
    std::string instance_id;
    std::string publication_id;

    bool complete() const {
        return !session_id.empty() && !instance_id.empty() &&
               !publication_id.empty();
    }

    friend bool operator==(const TracePublicationOwner&,
                           const TracePublicationOwner&) = default;
};

/// Handles Trace.* protocol requests by driving pulp::runtime::Tracing.
class TraceInspector {
public:
    TraceInspector() = default;
    ~TraceInspector();

    TraceInspector(const TraceInspector&) = delete;
    TraceInspector& operator=(const TraceInspector&) = delete;
    TraceInspector(TraceInspector&&) = delete;
    TraceInspector& operator=(TraceInspector&&) = delete;

    /// Bind process-global trace ownership to one authenticated control
    /// registration. Releasing the lease also stops an abandoned capture.
    std::unique_ptr<TraceOwnerLease> bind_control_registration(
        std::string registration_id);

    /// Handle a Trace.* request. Returns a response message.
    InspectorMessage handle(const InspectorMessage& req);

    /// Whether this inspector recognizes the method (used by DomainHandler
    /// dispatch, mirroring MotionScrubber::owns_method).
    static bool owns_method(const std::string& method);

private:
    enum class OwnerKind { None, ControlRegistration };
    class OwnerLease;
    std::unique_ptr<TraceOwnerLease> bind_owner(OwnerKind kind);
    void release_owner(const std::shared_ptr<void>& owner_token) noexcept;
    InspectorMessage handle_with_owner(
        const InspectorMessage& request,
        const std::shared_ptr<void>& owner_token);

    std::shared_ptr<void> owner_token_;
    OwnerKind owner_kind_ = OwnerKind::None;
    std::optional<pulp::runtime::TraceOwnership> ownership_;
    std::mutex mutex_;

    // The last .pftrace flushed by stopSession, so snapshot can report it.
    std::string last_trace_path_;

    InspectorMessage start_session(
        const InspectorMessage& req,
        const std::shared_ptr<void>* owner_token);
    InspectorMessage stop_session(
        const InspectorMessage& req,
        const std::shared_ptr<void>* owner_token);
    InspectorMessage snapshot(const InspectorMessage& req);
    InspectorMessage query(const InspectorMessage& req);
    InspectorMessage explain(const InspectorMessage& req);
};

} // namespace pulp::inspect
