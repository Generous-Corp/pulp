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

    std::string out_path;
    if (params.isObject() && params.hasObjectMember("out_path"))
        out_path = std::string(params["out_path"].getString());

    // The CLI sizes the ring in megabytes; Tracing takes kilobytes. Absent →
    // Tracing's own 80 MB default.
    std::uint32_t ring_kb = 80u * 1024u;
    if (params.isObject() && params.hasObjectMember("ring_mb"))
        ring_kb = static_cast<std::uint32_t>(params["ring_mb"].getInt64()) * 1024u;

    const bool started = Tracing::start(categories, out_path, ring_kb);

    auto out = choc::value::createObject("");
    out.addMember("compiled_in", choc::value::createBool(kTracingEnabled));
    out.addMember("active", choc::value::createBool(Tracing::active()));
    if (!started && !kTracingEnabled) {
        out.addMember("ok", choc::value::createBool(false));
        out.addMember("message", choc::value::createString(
            "Tracing is not compiled into this build. Rebuild with "
            "-DPULP_TRACING=ON to capture a trace."));
    } else {
        out.addMember("ok", choc::value::createBool(started));
        if (!out_path.empty())
            out.addMember("out_path", choc::value::createString(out_path));
    }
    return make_response(req.id, choc::json::toString(out, false));
}

InspectorMessage TraceInspector::stop_session(const InspectorMessage& req) {
    const auto result = Tracing::stop();
    if (result.ok)
        last_trace_path_ = result.path;

    auto out = choc::value::createObject("");
    out.addMember("ok", choc::value::createBool(result.ok));
    out.addMember("out_path", choc::value::createString(result.path));
    out.addMember("trace_bytes",
                  choc::value::createInt64(static_cast<int64_t>(result.trace_bytes)));
    if (!result.ok)
        out.addMember("message", choc::value::createString(
            kTracingEnabled
                ? "No active tracing session to stop."
                : "Tracing is not compiled into this build (-DPULP_TRACING=ON)."));
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
    // Running SQL means shelling out to the pinned trace_processor over the
    // flushed .pftrace — an offline step the `pulp trace` client owns, not the
    // in-process inspector. So a query request points the caller at the trace
    // file and defers to the trace-sql skill rather than executing here.
    auto out = choc::value::createObject("");
    out.addMember("available", choc::value::createBool(false));
    out.addMember("trace_path", choc::value::createString(last_trace_path_));
    out.addMember("hint", choc::value::createString(
        last_trace_path_.empty()
            ? "No trace captured yet — run `pulp trace start` then `pulp trace "
              "stop` first."
            : "Query the flushed .pftrace offline with trace_processor (see the "
              "trace-sql skill)."));
    return make_response(req.id, choc::json::toString(out, false));
}

InspectorMessage TraceInspector::explain(const InspectorMessage& req) {
    std::string question;
    try {
        auto params = choc::json::parse(req.params_json);
        if (params.isObject() && params.hasObjectMember("question"))
            question = std::string(params["question"].getString());
    } catch (...) {
        // A malformed explain request still gets the general guidance below.
    }

    const std::string explanation =
        last_trace_path_.empty()
            ? "No trace captured yet. Run `pulp trace start`, reproduce the slow "
              "moment, then `pulp trace stop` — that writes a .pftrace you can "
              "open in the Perfetto UI or query with the trace-sql skill."
            : "A trace is at " + last_trace_path_ + ". The narrated-answer path "
              "runs canned trace-sql queries over it; that query engine ships "
              "next. For now open the .pftrace in the Perfetto UI or run the "
              "trace-analysis skill's presets against it.";

    auto out = choc::value::createObject("");
    out.addMember("available", choc::value::createBool(false));
    out.addMember("question", choc::value::createString(question));
    out.addMember("explanation", choc::value::createString(explanation));
    if (!last_trace_path_.empty())
        out.addMember("trace_path", choc::value::createString(last_trace_path_));
    return make_response(req.id, choc::json::toString(out, false));
}

} // namespace pulp::inspect
