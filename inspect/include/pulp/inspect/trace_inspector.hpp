// trace_inspector.hpp — Bridges the process-global pulp::runtime::Tracing
// controller to the inspector protocol's Trace.* methods, so the `pulp trace`
// CLI can start / stop / inspect a Perfetto session over the inspector wire.
//
// Session state lives in pulp::runtime::Tracing (a process singleton — a DAW
// hosts one session per process, not per plugin instance), so this bridge holds
// no session; it only remembers the last flushed trace path to answer snapshot
// and to point query / explain at a file. Safe to construct and call in any
// build config: with PULP_TRACING=OFF every Tracing call no-ops and the
// responses say so plainly, which is the "did I forget to enable it?" answer.
#pragma once

#include <pulp/inspect/protocol.hpp>

#include <string>

namespace pulp::inspect {

/// Handles Trace.* protocol requests by driving pulp::runtime::Tracing.
class TraceInspector {
public:
    TraceInspector() = default;

    /// Handle a Trace.* request. Returns a response message.
    InspectorMessage handle(const InspectorMessage& req);

    /// Whether this inspector recognizes the method (used by DomainHandler
    /// dispatch, mirroring MotionScrubber::owns_method).
    static bool owns_method(const std::string& method);

private:
    // The last .pftrace flushed by stopSession, so snapshot can report it and
    // query / explain can point the caller at a real file to analyze offline.
    std::string last_trace_path_;

    InspectorMessage start_session(const InspectorMessage& req);
    InspectorMessage stop_session(const InspectorMessage& req);
    InspectorMessage snapshot(const InspectorMessage& req);
    InspectorMessage query(const InspectorMessage& req);
    InspectorMessage explain(const InspectorMessage& req);
};

} // namespace pulp::inspect
