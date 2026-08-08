// motion_inspector.hpp — Bridges pulp::view::motion to the inspector
// protocol. Exposes Motion.startTrace / .stopTrace / .snapshot /
// .listTraces requests and broadcasts Motion.start / .sample / .end
// events to inspector clients.

#pragma once

#include <pulp/inspect/inspector_delivery.hpp>
#include <pulp/view/motion.hpp>
#include <pulp/view/motion_cost.hpp>

#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace pulp::view { class View; }

namespace pulp::inspect {

class MotionInspector {
public:
    /// Construct with a root view for node-id lookup and an optional event
    /// delivery callback. Without a callback, traces still register and tick;
    /// events fan out to sinks installed directly on the coordinator.
    explicit MotionInspector(pulp::view::View& root,
                             InspectorEventSink event_sink = {});
    ~MotionInspector();

    MotionInspector(const MotionInspector&) = delete;
    MotionInspector& operator=(const MotionInspector&) = delete;

    /// Handle a Motion.* request. Returns response message.
    InspectorMessage handle(const InspectorMessage& req);

    /// Number of currently-attached inspector traces.
    std::size_t active_trace_count() const;

private:
    pulp::view::View* root_ = nullptr;
    InspectorEventSink event_sink_;
    int sink_id_ = 0;
    int cost_sink_id_ = 0;

    mutable std::mutex mtx_;
    std::unordered_map<std::int64_t, pulp::view::motion::TraceHandle> traces_;
    std::int64_t next_inspector_id_ = 1;

    InspectorMessage start_trace(const InspectorMessage& req);
    InspectorMessage stop_trace(const InspectorMessage& req);
    InspectorMessage snapshot(const InspectorMessage& req);
    InspectorMessage list_traces(const InspectorMessage& req);
    InspectorMessage enable_cost(const InspectorMessage& req);
    InspectorMessage disable_cost(const InspectorMessage& req);

    void broadcast_event(const pulp::view::motion::SampleEvent& e);
    void broadcast_cost(const pulp::view::motion::CostSample& s);
};

} // namespace pulp::inspect
