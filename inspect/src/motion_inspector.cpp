#include <pulp/inspect/motion_inspector.hpp>

#include <pulp/inspect/inspector_server.hpp>
#include <pulp/view/inspector.hpp>
#include <pulp/view/motion.hpp>
#include <pulp/view/motion_cost.hpp>
#include <pulp/view/ui_components.hpp>  // ScrollView (Motion.startTrace scroll-geometry)
#include <pulp/view/view.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <cmath>
#include <sstream>
#include <string>
#include <vector>

namespace pulp::inspect {

namespace {

using pulp::view::motion::Coordinator;
using pulp::view::motion::CostAttributor;
using pulp::view::motion::CostSample;
using pulp::view::motion::GeometryProperty;
using pulp::view::motion::GeometrySource;
using pulp::view::motion::GeometrySpace;
using pulp::view::motion::Provenance;
using pulp::view::motion::SampleEvent;
using pulp::view::motion::TraceBuilder;
using pulp::view::motion::TraceHandle;
using pulp::view::motion::TraceOptions;

GeometryProperty parse_geometry_property(std::string_view s) {
    if (s == "minX")   return GeometryProperty::MinX;
    if (s == "minY")   return GeometryProperty::MinY;
    if (s == "maxX")   return GeometryProperty::MaxX;
    if (s == "maxY")   return GeometryProperty::MaxY;
    if (s == "midX")   return GeometryProperty::MidX;
    if (s == "midY")   return GeometryProperty::MidY;
    if (s == "width")  return GeometryProperty::Width;
    if (s == "height") return GeometryProperty::Height;
    return GeometryProperty::MinX;
}

GeometrySpace parse_geometry_space(std::string_view s) {
    if (s == "view-local")  return GeometrySpace::ViewLocal;
    if (s == "view-global") return GeometrySpace::ViewGlobal;
    if (s == "window")      return GeometrySpace::Window;
    if (s == "screen")      return GeometrySpace::Screen;
    return GeometrySpace::Window;
}

// Phase 11c — camelCase property names mirror what TraceBuilder emits
// into fixtures, so callers can pass the same names back through the
// wire. Unknown names fall back to ContentOffsetX silently (defensive;
// the inspector should not crash on a typo).
pulp::view::motion::ScrollProperty parse_scroll_property(std::string_view s) {
    using SP = pulp::view::motion::ScrollProperty;
    if (s == "contentOffsetX")   return SP::ContentOffsetX;
    if (s == "contentOffsetY")   return SP::ContentOffsetY;
    if (s == "visibleRectMinX")  return SP::VisibleRectMinX;
    if (s == "visibleRectMinY")  return SP::VisibleRectMinY;
    if (s == "visibleRectWidth") return SP::VisibleRectWidth;
    if (s == "visibleRectHeight")return SP::VisibleRectHeight;
    if (s == "contentSizeWidth") return SP::ContentSizeWidth;
    if (s == "contentSizeHeight")return SP::ContentSizeHeight;
    if (s == "insetTop")         return SP::InsetTop;
    if (s == "insetBottom")      return SP::InsetBottom;
    if (s == "insetLeft")        return SP::InsetLeft;
    if (s == "insetRight")       return SP::InsetRight;
    if (s == "scrollableMaxX")   return SP::ScrollableMaxX;
    if (s == "scrollableMaxY")   return SP::ScrollableMaxY;
    return SP::ContentOffsetX;
}

GeometrySource parse_geometry_source(std::string_view s) {
    if (s == "layout")       return GeometrySource::Layout;
    if (s == "presentation") return GeometrySource::Presentation;
    return GeometrySource::Layout;
}

const char* sample_kind_to_string(SampleEvent::Kind k) {
    switch (k) {
        case SampleEvent::Kind::TraceStarted: return "trace-started";
        case SampleEvent::Kind::Baseline:     return "baseline";
        case SampleEvent::Kind::Sample:       return "sample";
        case SampleEvent::Kind::Start:        return "start";
        case SampleEvent::Kind::End:          return "end";
        case SampleEvent::Kind::Input:        return "input";
    }
    return "?";
}

const char* event_method_for_kind(SampleEvent::Kind k) {
    switch (k) {
        case SampleEvent::Kind::TraceStarted: return methods::kMotionStart;
        case SampleEvent::Kind::Baseline:     return methods::kMotionSample;
        case SampleEvent::Kind::Sample:       return methods::kMotionSample;
        case SampleEvent::Kind::Start:        return methods::kMotionStart;
        case SampleEvent::Kind::End:          return methods::kMotionEnd;
        case SampleEvent::Kind::Input:        return methods::kMotionSample;
    }
    return methods::kMotionSample;
}

// Match motion.cpp::format_number — NaN/Inf travel as quoted sentinels
// on the wire so a fixture round-trip and a live inspector broadcast see
// the same values. choc::value::createFloat64 would serialize non-finite
// as JSON `null`, silently dropping the misbehavior signal.
choc::value::Value wire_number(double v) {
    if (std::isnan(v))          return choc::value::createString("NaN");
    if (std::isinf(v) && v > 0) return choc::value::createString("Infinity");
    if (std::isinf(v) && v < 0) return choc::value::createString("-Infinity");
    return choc::value::createFloat64(v);
}

choc::value::Value components_to_object(
    const std::vector<std::pair<std::string, double>>& comps
) {
    auto obj = choc::value::createObject("");
    for (const auto& [k, v] : comps) {
        obj.addMember(k, wire_number(v));
    }
    return obj;
}

}  // namespace

// ── MotionInspector ──────────────────────────────────────────────────

MotionInspector::MotionInspector(pulp::view::View& root, InspectorServer* server)
    : root_(&root), server_(server) {
    sink_id_ = Coordinator::instance().add_sink(
        [this](const SampleEvent& e) { broadcast_event(e); });
    cost_sink_id_ = CostAttributor::instance().add_sink(
        [this](const CostSample& s) { broadcast_cost(s); });
}

MotionInspector::~MotionInspector() {
    if (sink_id_) {
        Coordinator::instance().remove_sink(sink_id_);
        sink_id_ = 0;
    }
    if (cost_sink_id_) {
        CostAttributor::instance().remove_sink(cost_sink_id_);
        cost_sink_id_ = 0;
    }
    std::lock_guard<std::mutex> lock(mtx_);
    traces_.clear();
}

std::size_t MotionInspector::active_trace_count() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return traces_.size();
}

InspectorMessage MotionInspector::handle(const InspectorMessage& req) {
    if (req.method == methods::kMotionStartTrace)  return start_trace(req);
    if (req.method == methods::kMotionStopTrace)   return stop_trace(req);
    if (req.method == methods::kMotionSnapshot)    return snapshot(req);
    if (req.method == methods::kMotionListTraces)  return list_traces(req);
    if (req.method == methods::kMotionEnableCost)  return enable_cost(req);
    if (req.method == methods::kMotionDisableCost) return disable_cost(req);
    return make_error(req.id, "Unknown Motion method: " + req.method);
}

InspectorMessage MotionInspector::start_trace(const InspectorMessage& req) {
    if (!root_) return make_error(req.id, "MotionInspector has no root view");

    choc::value::Value params;
    try {
        params = choc::json::parse(req.params_json);
    } catch (...) {
        return make_error(req.id, "Motion.startTrace: invalid params JSON");
    }

    std::string view_name = "Trace";
    if (params.isObject() && params.hasObjectMember("view_name")) {
        view_name = std::string(params["view_name"].getString());
    }
    int fps = 15;
    if (params.isObject() && params.hasObjectMember("fps")) {
        fps = static_cast<int>(params["fps"].getInt64());
    }

    TraceBuilder builder = Coordinator::instance().trace(view_name, {fps});

    if (!params.isObject() || !params.hasObjectMember("metrics") ||
        !params["metrics"].isArray()) {
        return make_error(req.id, "Motion.startTrace: 'metrics' array required");
    }

    const auto& metrics = params["metrics"];
    for (uint32_t i = 0; i < metrics.size(); ++i) {
        const auto& m = metrics[i];
        if (!m.isObject() || !m.hasObjectMember("kind")) {
            return make_error(req.id, "Motion.startTrace: metric requires 'kind'");
        }
        std::string kind(m["kind"].getString());
        std::string name = m.hasObjectMember("name")
                               ? std::string(m["name"].getString())
                               : kind;

        if (kind == "geometry") {
            if (!m.hasObjectMember("node_id")) {
                return make_error(req.id, "Motion.startTrace: geometry requires 'node_id'");
            }
            std::string node_id(m["node_id"].getString());
            auto* target = pulp::view::ViewInspector::find_by_id(*root_, node_id);
            if (!target) {
                return make_error(req.id,
                                  "Motion.startTrace: node not found: " + node_id);
            }
            std::vector<GeometryProperty> props;
            if (m.hasObjectMember("properties") && m["properties"].isArray()) {
                const auto& parr = m["properties"];
                for (uint32_t j = 0; j < parr.size(); ++j) {
                    props.push_back(parse_geometry_property(parr[j].getString()));
                }
            }
            if (props.empty()) {
                props = {GeometryProperty::MinX, GeometryProperty::MinY,
                         GeometryProperty::Width, GeometryProperty::Height};
            }
            const auto space = m.hasObjectMember("space")
                                   ? parse_geometry_space(m["space"].getString())
                                   : GeometrySpace::Window;
            const auto source = m.hasObjectMember("source")
                                    ? parse_geometry_source(m["source"].getString())
                                    : GeometrySource::Layout;
            builder.geometry(name, *target, std::move(props), space, source);
        } else if (kind == "scroll-geometry" || kind == "scrollGeometry") {
            // Phase 11c — scroll geometry tracing over a ScrollView.
            // Accept both "scroll-geometry" (kebab-case, matches our
            // other inspector spellings) and the camelCase form Swift
            // / Kotlin facades pass through verbatim.
            if (!m.hasObjectMember("node_id")) {
                return make_error(req.id,
                    "Motion.startTrace: scroll-geometry requires 'node_id'");
            }
            std::string node_id(m["node_id"].getString());
            auto* view_target = pulp::view::ViewInspector::find_by_id(*root_, node_id);
            if (!view_target) {
                return make_error(req.id,
                                  "Motion.startTrace: node not found: " + node_id);
            }
            auto* scroll_target = dynamic_cast<pulp::view::ScrollView*>(view_target);
            if (!scroll_target) {
                return make_error(req.id,
                    "Motion.startTrace: scroll-geometry node is not a ScrollView: "
                    + node_id);
            }
            std::vector<pulp::view::motion::ScrollProperty> props;
            if (m.hasObjectMember("properties") && m["properties"].isArray()) {
                const auto& parr = m["properties"];
                for (uint32_t j = 0; j < parr.size(); ++j) {
                    props.push_back(parse_scroll_property(parr[j].getString()));
                }
            }
            // Empty props → builder's default 4-property set.
            if (props.empty()) {
                builder.scroll_geometry(name, *scroll_target);
            } else {
                builder.scroll_geometry(name, *scroll_target, std::move(props));
            }
        } else {
            return make_error(req.id,
                              "Motion.startTrace: unsupported metric kind: " + kind);
        }
    }

    auto handle = builder.attach();
    if (!handle.is_attached()) {
        return make_error(req.id, "Motion.startTrace: attach failed");
    }
    Coordinator::instance().set_tracing_enabled(true);

    std::int64_t inspector_id = 0;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        inspector_id = next_inspector_id_++;
        traces_.emplace(inspector_id, std::move(handle));
    }

    auto out = choc::value::createObject("");
    out.addMember("trace_id", choc::value::createInt64(inspector_id));
    return make_response(req.id, choc::json::toString(out, false));
}

InspectorMessage MotionInspector::stop_trace(const InspectorMessage& req) {
    choc::value::Value params;
    try {
        params = choc::json::parse(req.params_json);
    } catch (...) {
        return make_error(req.id, "Motion.stopTrace: invalid params JSON");
    }
    if (!params.isObject() || !params.hasObjectMember("trace_id")) {
        return make_error(req.id, "Motion.stopTrace: 'trace_id' required");
    }
    const std::int64_t id = params["trace_id"].getInt64();
    bool removed = false;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = traces_.find(id);
        if (it != traces_.end()) {
            traces_.erase(it);
            removed = true;
        }
    }
    auto out = choc::value::createObject("");
    out.addMember("removed", choc::value::createBool(removed));
    return make_response(req.id, choc::json::toString(out, false));
}

InspectorMessage MotionInspector::snapshot(const InspectorMessage& req) {
    auto out = choc::value::createObject("");
    out.addMember("tracing_enabled",
                  choc::value::createBool(Coordinator::instance().tracing_enabled()));
    out.addMember("firehose",
                  choc::value::createBool(Coordinator::instance().firehose()));
    out.addMember("active_traces",
                  choc::value::createInt64(static_cast<int64_t>(
                      Coordinator::instance().active_trace_count())));
    out.addMember("inspector_traces",
                  choc::value::createInt64(static_cast<int64_t>(active_trace_count())));
    out.addMember("emitted_events",
                  choc::value::createInt64(static_cast<int64_t>(
                      Coordinator::instance().emitted_event_count())));
    out.addMember("cost_enabled",
                  choc::value::createBool(CostAttributor::instance().enabled()));
    out.addMember("cost_samples_emitted",
                  choc::value::createInt64(static_cast<int64_t>(
                      CostAttributor::instance().emitted_sample_count())));
    return make_response(req.id, choc::json::toString(out, false));
}

InspectorMessage MotionInspector::enable_cost(const InspectorMessage& req) {
    CostAttributor::instance().set_enabled(true);
    auto out = choc::value::createObject("");
    out.addMember("cost_enabled", choc::value::createBool(true));
    return make_response(req.id, choc::json::toString(out, false));
}

InspectorMessage MotionInspector::disable_cost(const InspectorMessage& req) {
    CostAttributor::instance().set_enabled(false);
    auto out = choc::value::createObject("");
    out.addMember("cost_enabled", choc::value::createBool(false));
    return make_response(req.id, choc::json::toString(out, false));
}

InspectorMessage MotionInspector::list_traces(const InspectorMessage& req) {
    auto out = choc::value::createObject("");
    auto arr = choc::value::createEmptyArray();
    {
        std::lock_guard<std::mutex> lock(mtx_);
        for (const auto& [id, handle] : traces_) {
            (void)handle;
            arr.addArrayElement(choc::value::createInt64(id));
        }
    }
    out.addMember("trace_ids", arr);
    return make_response(req.id, choc::json::toString(out, false));
}

void MotionInspector::broadcast_event(const SampleEvent& e) {
    if (!server_) return;

    auto params = choc::value::createObject("");
    params.addMember("view_name", choc::value::createString(e.view_name));
    params.addMember("metric_name", choc::value::createString(e.metric_name));
    params.addMember("kind", choc::value::createString(sample_kind_to_string(e.kind)));
    params.addMember("t", wire_number(e.t_seconds));
    params.addMember("frame", choc::value::createInt64(static_cast<int64_t>(e.frame)));
    // Phase 7 stable identifiers — let clients align bursts on the
    // wire the same way the fixture format aligns them.
    params.addMember("trace_id", choc::value::createInt64(e.trace_id));
    params.addMember("metric_id", choc::value::createInt64(e.metric_id));
    params.addMember("burst_id", choc::value::createInt64(e.burst_id));
    // Phase 10 Input events carry input_kind + view_id; without these
    // the wire form loses the entire payload of the event.
    if (e.kind == SampleEvent::Kind::Input) {
        params.addMember("input_kind", choc::value::createString(e.input_kind));
        params.addMember("view_id",    choc::value::createString(e.view_id));
    }
    if (!e.components.empty()) {
        params.addMember("components", components_to_object(e.components));
    }
    if (!e.deltas.empty()) {
        params.addMember("deltas", components_to_object(e.deltas));
    }
    if (e.provenance.is_set()) {
        auto prov = choc::value::createObject("");
        prov.addMember("source_kind", choc::value::createString(e.provenance.source_kind));
        prov.addMember("source_id",   choc::value::createString(e.provenance.source_id));
        prov.addMember("source_file", choc::value::createString(e.provenance.source_file));
        prov.addMember("source_line", choc::value::createInt64(e.provenance.source_line));
        params.addMember("provenance", prov);
    }

    InspectorMessage ev = make_event(event_method_for_kind(e.kind),
                                     choc::json::toString(params, false));
    server_->broadcast(ev);
}

void MotionInspector::broadcast_cost(const CostSample& s) {
    if (!server_) return;
    auto params = choc::value::createObject("");
    params.addMember("frame",
                     choc::value::createInt64(static_cast<int64_t>(s.frame)));
    params.addMember("t", wire_number(s.t_seconds));
    params.addMember("render_pass_duration_ms",
                     wire_number(s.render_pass_duration_ms));
    params.addMember("dirty_rect_area_px",
                     wire_number(s.dirty_rect_area_px));
    params.addMember("dirty_rect_count",
                     choc::value::createInt64(s.dirty_rect_count));

    auto ids = choc::value::createEmptyArray();
    for (int id : s.active_trace_ids) {
        ids.addArrayElement(choc::value::createInt64(id));
    }
    params.addMember("active_trace_ids", ids);

    auto provs = choc::value::createEmptyArray();
    for (const auto& p : s.active_provenance) {
        auto obj = choc::value::createObject("");
        obj.addMember("source_kind", choc::value::createString(p.source_kind));
        obj.addMember("source_id",   choc::value::createString(p.source_id));
        obj.addMember("source_file", choc::value::createString(p.source_file));
        obj.addMember("source_line", choc::value::createInt64(p.source_line));
        provs.addArrayElement(obj);
    }
    params.addMember("active_provenance", provs);

    InspectorMessage ev = make_event(methods::kMotionCost,
                                     choc::json::toString(params, false));
    server_->broadcast(ev);
}

} // namespace pulp::inspect
