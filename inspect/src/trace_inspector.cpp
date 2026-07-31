#include <pulp/inspect/trace_inspector.hpp>

#include <pulp/runtime/trace.hpp>          // kTracingEnabled
#include <pulp/runtime/trace_session.hpp>  // Tracing, TraceStopResult

#include <choc/text/choc_JSON.h>

#include <cstdint>
#include <string>
#include <vector>

namespace pulp::inspect {

namespace {
using pulp::runtime::Tracing;
using pulp::runtime::kTracingEnabled;
constexpr std::int64_t kMinTraceRingMb = 1;
constexpr std::int64_t kMaxTraceRingMb = 512;
}  // namespace

bool TraceInspector::owns_method(const std::string& method) {
    return method == methods::kTraceStartSession
        || method == methods::kTraceStopSession
        || method == methods::kTraceSnapshot
        || method == methods::kTraceQuery
        || method == methods::kTraceExplain;
}

InspectorMessage TraceInspector::handle(const InspectorMessage& req) {
    if (req.method == methods::kTraceStartSession) return start_session(req);
    if (req.method == methods::kTraceStopSession)  return stop_session(req);
    if (req.method == methods::kTraceSnapshot)     return snapshot(req);
    if (req.method == methods::kTraceQuery)        return query(req);
    if (req.method == methods::kTraceExplain)      return explain(req);
    return make_error(req.id, "Unknown Trace method: " + req.method);
}

InspectorMessage TraceInspector::start_session(const InspectorMessage& req) {
    choc::value::Value params;
    try {
        params = choc::json::parse(req.params_json);
    } catch (...) {
        return make_error(req.id, "Trace.startSession: invalid params JSON");
    }

    std::vector<std::string> categories;
    if (params.isObject() && params.hasObjectMember("categories") &&
        params["categories"].isArray()) {
        const auto& arr = params["categories"];
        for (uint32_t i = 0; i < arr.size(); ++i)
            categories.emplace_back(arr[i].getString());
    }

    if (params.isObject() && params.hasObjectMember("out_path")) {
        return make_error(
            req.id,
            "Trace.startSession: out_path is unavailable over the inspector; "
            "the host owns the trace destination",
            "invalid_params");
    }

    // The CLI sizes the ring in megabytes; Tracing takes kilobytes. Absent →
    // Tracing's own 80 MB default.
    std::uint32_t ring_kb = 80u * 1024u;
    if (params.isObject() && params.hasObjectMember("ring_mb")) {
        const auto& ring = params["ring_mb"];
        if (!ring.isInt32() && !ring.isInt64()) {
            return make_error(
                req.id,
                "Trace.startSession: ring_mb must be an integer",
                "invalid_params");
        }
        const auto ring_mb = ring.getInt64();
        if (ring_mb < kMinTraceRingMb || ring_mb > kMaxTraceRingMb) {
            return make_error(
                req.id,
                "Trace.startSession: ring_mb must be between 1 and 512",
                "invalid_params");
        }
        ring_kb = static_cast<std::uint32_t>(ring_mb) * 1024u;
    }

    if (Tracing::active()) {
        return make_error(
            req.id, "Trace.startSession: a tracing session is already active",
            "trace_already_active");
    }

    // An authenticated peer can control capture, not host filesystem paths.
    // Empty delegates the destination to host-owned trace configuration.
    const bool started = Tracing::start(categories, {}, ring_kb);
    if (!started) {
        return make_error(
            req.id,
            kTracingEnabled
                ? "Trace.startSession: tracing could not be started"
                : "Trace.startSession: tracing is not compiled into this "
                  "build; rebuild with -DPULP_TRACING=ON",
            kTracingEnabled ? "trace_start_failed" : "tracing_unavailable");
    }

    auto out = choc::value::createObject("");
    out.addMember("compiled_in", choc::value::createBool(kTracingEnabled));
    out.addMember("active", choc::value::createBool(Tracing::active()));
    out.addMember("ok", choc::value::createBool(true));
    return make_response(req.id, choc::json::toString(out, false));
}

InspectorMessage TraceInspector::stop_session(const InspectorMessage& req) {
    if (!kTracingEnabled) {
        return make_error(
            req.id,
            "Trace.stopSession: tracing is not compiled into this build; "
            "rebuild with -DPULP_TRACING=ON",
            "tracing_unavailable");
    }
    if (!Tracing::active()) {
        return make_error(
            req.id, "Trace.stopSession: no active tracing session",
            "no_active_trace");
    }

    const auto result = Tracing::stop();
    if (!result.ok) {
        return make_error(
            req.id,
            "Trace.stopSession: the active trace could not be written",
            "trace_write_failed");
    }
    last_trace_path_ = result.path;

    auto out = choc::value::createObject("");
    out.addMember("ok", choc::value::createBool(true));
    out.addMember("out_path", choc::value::createString(result.path));
    out.addMember("trace_bytes",
                  choc::value::createInt64(static_cast<int64_t>(result.trace_bytes)));
    return make_response(req.id, choc::json::toString(out, false));
}

InspectorMessage TraceInspector::snapshot(const InspectorMessage& req) {
    auto out = choc::value::createObject("");
    out.addMember("compiled_in", choc::value::createBool(kTracingEnabled));
    out.addMember("active", choc::value::createBool(Tracing::active()));
    if (!last_trace_path_.empty())
        out.addMember("last_trace_path", choc::value::createString(last_trace_path_));
    return make_response(req.id, choc::json::toString(out, false));
}

InspectorMessage TraceInspector::query(const InspectorMessage& req) {
    return make_error(
        req.id,
        "Trace.query is unavailable in the live inspector; query a flushed "
        ".pftrace with `pulp trace query \"<sql>\" --trace <file>`",
        "capability_unavailable");
}

InspectorMessage TraceInspector::explain(const InspectorMessage& req) {
    return make_error(
        req.id,
        "Trace.explain is not implemented; capture a .pftrace and run the "
        "trace-analysis workflow over offline trace queries",
        "capability_unavailable");
}

} // namespace pulp::inspect
