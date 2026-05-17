// motion_inspector.hpp — Bridges pulp::view::motion to the inspector
// protocol. Exposes Motion.startTrace / .stopTrace / .snapshot /
// .listTraces requests and broadcasts Motion.start / .sample / .end
// events to inspector clients.

#pragma once

#include <pulp/inspect/protocol.hpp>
#include <pulp/view/motion.hpp>

#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace pulp::view { class View; }

namespace pulp::inspect {

class InspectorServer;

class MotionInspector {
public:
    /// Construct with a root view for node-id lookup and an optional
    /// InspectorServer for event broadcasting. Without a server, traces
    /// still register and tick; events fan out to any sinks installed
    /// directly on the coordinator.
    MotionInspector(pulp::view::View& root, InspectorServer* server);
    ~MotionInspector();

    MotionInspector(const MotionInspector&) = delete;
    MotionInspector& operator=(const MotionInspector&) = delete;

    /// Handle a Motion.* request. Returns response message.
    InspectorMessage handle(const InspectorMessage& req);

    /// Number of currently-attached inspector traces.
    std::size_t active_trace_count() const;

private:
    pulp::view::View* root_ = nullptr;
    InspectorServer* server_ = nullptr;
    int sink_id_ = 0;

    mutable std::mutex mtx_;
    std::unordered_map<std::int64_t, pulp::view::motion::TraceHandle> traces_;
    std::int64_t next_inspector_id_ = 1;

    InspectorMessage start_trace(const InspectorMessage& req);
    InspectorMessage stop_trace(const InspectorMessage& req);
    InspectorMessage snapshot(const InspectorMessage& req);
    InspectorMessage list_traces(const InspectorMessage& req);

    void broadcast_event(const pulp::view::motion::SampleEvent& e);
};

} // namespace pulp::inspect
