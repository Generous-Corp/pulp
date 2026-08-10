// motion_inspector.hpp — Bridges pulp::view::motion to the inspector
// protocol. Exposes Motion.startTrace / .stopTrace / .snapshot /
// .listTraces requests and broadcasts Motion.start / .sample / .end
// events to inspector clients.

#pragma once

#include <pulp/inspect/inspector_delivery.hpp>
#include <pulp/view/motion.hpp>
#include <pulp/view/motion_cost.hpp>

#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pulp::view { class View; }

namespace pulp::inspect {

struct MotionGeometryMetric {
    std::string name;
    std::string node_id;
    std::vector<pulp::view::motion::GeometryProperty> properties;
    pulp::view::motion::GeometrySpace space = pulp::view::motion::GeometrySpace::Window;
    pulp::view::motion::GeometrySource source = pulp::view::motion::GeometrySource::Layout;
};

struct MotionScrollMetric {
    std::string name;
    std::string node_id;
    std::vector<pulp::view::motion::ScrollProperty> properties;
};

struct MotionTraceSpec {
    std::string view_name;
    int fps = 15;
    std::vector<MotionGeometryMetric> geometry;
    std::vector<MotionScrollMetric> scroll_geometry;
};

struct MotionTraceAttachResult {
    std::int64_t trace_id = 0;
    std::string error;
    bool resource_exhausted = false;

    explicit operator bool() const noexcept { return trace_id > 0 && error.empty(); }
};

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

    /// Transport-independent host operations used by canonical capability
    /// executors. They never emit or decode legacy protocol method names.
    MotionTraceAttachResult attach_trace(const MotionTraceSpec& spec, std::string owner,
                                         std::function<bool()> authority_live,
                                         std::function<std::shared_ptr<void>(
                                             std::function<void()>)> subscribe_authority_end);
    bool detach_trace(std::int64_t trace_id, std::string_view owner);
    bool begin_cost_observation(std::string owner, std::function<bool()> authority_live);
    bool end_cost_observation(std::string_view owner);
    std::vector<pulp::view::motion::CostSample>
    recent_cost_samples(std::size_t maximum_samples, std::string_view owner) const;

private:
    pulp::view::View* root_ = nullptr;
    InspectorEventSink event_sink_;
    int sink_id_ = 0;
    int cost_sink_id_ = 0;

    mutable std::mutex mtx_;
    std::unordered_map<std::int64_t, pulp::view::motion::TraceHandle> traces_;
    struct CanonicalTrace {
        pulp::view::motion::TraceHandle handle;
        std::string owner;
        std::function<bool()> authority_live;
        std::shared_ptr<void> authority_end_subscription;
    };
    std::unordered_map<std::int64_t, CanonicalTrace> canonical_traces_;
    std::unordered_map<std::int64_t, std::string> cost_trace_owners_;
    std::deque<pulp::view::motion::CostSample> recent_cost_samples_;
    std::string cost_owner_;
    std::function<bool()> cost_authority_live_;
    bool tracing_was_enabled_ = false;
    bool canonical_tracing_epoch_active_ = false;
    bool cost_was_enabled_ = false;
    std::int64_t next_inspector_id_ = 1;

    InspectorMessage start_trace(const InspectorMessage& req);
    InspectorMessage stop_trace(const InspectorMessage& req);
    InspectorMessage snapshot(const InspectorMessage& req);
    InspectorMessage list_traces(const InspectorMessage& req);
    InspectorMessage enable_cost(const InspectorMessage& req);
    InspectorMessage disable_cost(const InspectorMessage& req);

    void broadcast_event(const pulp::view::motion::SampleEvent& e);
    void broadcast_cost(const pulp::view::motion::CostSample& s);
    void prune_inactive_traces();
    void restore_tracing_if_unused_locked();
};

} // namespace pulp::inspect
