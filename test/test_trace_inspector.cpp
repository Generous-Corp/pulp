// TraceInspector bridge tests. Config-agnostic: the same suite runs with
// PULP_TRACING ON and OFF. OFF (the default, shipping config) verifies the
// bridge reports honestly that tracing is not compiled in — which is the
// "did I forget to enable it?" answer the CLI relays. ON verifies a real
// session round-trips: start → stop writes a non-empty .pftrace and snapshot
// reflects it.

#include <catch2/catch_test_macros.hpp>

#include <pulp/inspect/protocol.hpp>
#include <pulp/inspect/trace_inspector.hpp>
#include <pulp/runtime/trace.hpp>  // kTracingEnabled

#include <choc/text/choc_JSON.h>

#include <string>

using namespace pulp::inspect;

namespace {

InspectorMessage request(const std::string& method, const std::string& params = "{}") {
    return make_request(1, method, params);
}

choc::value::Value result_of(const InspectorMessage& resp) {
    return choc::json::parse(resp.params_json);
}

}  // namespace

TEST_CASE("TraceInspector recognizes exactly its Trace.* methods", "[tracing][inspect]") {
    CHECK(TraceInspector::owns_method(methods::kTraceStartSession));
    CHECK(TraceInspector::owns_method(methods::kTraceStopSession));
    CHECK(TraceInspector::owns_method(methods::kTraceSnapshot));
    CHECK(TraceInspector::owns_method(methods::kTraceQuery));
    CHECK(TraceInspector::owns_method(methods::kTraceExplain));
    CHECK_FALSE(TraceInspector::owns_method("Motion.startTrace"));
    CHECK_FALSE(TraceInspector::owns_method("Trace.bogus"));
}

TEST_CASE("TraceInspector rejects an unknown Trace method", "[tracing][inspect]") {
    TraceInspector insp;
    auto resp = insp.handle(request("Trace.bogus"));
    CHECK(resp.is_error);
}

TEST_CASE("TraceInspector snapshot reports compile-time tracing state", "[tracing][inspect]") {
    TraceInspector insp;
    auto out = result_of(insp.handle(request(methods::kTraceSnapshot)));
    REQUIRE(out.isObject());
    CHECK(out["compiled_in"].getBool() == pulp::runtime::kTracingEnabled);
    // No trace captured yet → no last_trace_path member.
    CHECK_FALSE(out.hasObjectMember("last_trace_path"));
}

TEST_CASE("TraceInspector rejects unavailable live query", "[tracing][inspect]") {
    TraceInspector insp;
    const auto response = insp.handle(request(
        methods::kTraceQuery,
        R"({"preset":"slowest-frames","format":"json"})"));
    CHECK(response.is_error);
    CHECK(response.error_code == "capability_unavailable");
    CHECK(response.params_json.find("--trace") != std::string::npos);
}

TEST_CASE("TraceInspector rejects unavailable live explanation", "[tracing][inspect]") {
    TraceInspector insp;
    const auto response = insp.handle(request(
        methods::kTraceExplain,
        R"({"question":"why is my plugin slow to open?"})"));
    CHECK(response.is_error);
    CHECK(response.error_code == "capability_unavailable");
    CHECK(response.params_json.find("not implemented") != std::string::npos);
}

TEST_CASE("TraceInspector keeps trace output under host authority",
          "[tracing][inspect][security]") {
    TraceInspector insp;
    const auto response = insp.handle(request(
        methods::kTraceStartSession,
        R"({"out_path":"/tmp/client-selected.pftrace"})"));
    CHECK(response.is_error);
    CHECK(response.error_code == "invalid_params");
    CHECK(response.params_json.find("host owns the trace destination") !=
          std::string::npos);
}

TEST_CASE("TraceInspector bounds the capture ring",
          "[tracing][inspect][resource-limit]") {
    TraceInspector insp;
    for (const auto* params : {
             R"({"ring_mb":-1})",
             R"({"ring_mb":0})",
             R"({"ring_mb":513})",
             R"({"ring_mb":"80"})",
         }) {
        INFO(params);
        const auto response = insp.handle(
            request(methods::kTraceStartSession, params));
        CHECK(response.is_error);
        CHECK(response.error_code == "invalid_params");
    }
}

#if defined(PULP_TRACING_ENABLED) && PULP_TRACING_ENABLED

TEST_CASE("TraceInspector round-trips a real session when tracing is ON", "[tracing][inspect]") {
    TraceInspector insp;

    auto started = result_of(insp.handle(request(
        methods::kTraceStartSession,
        R"({"categories":["render","state"],"ring_mb":8})")));
    REQUIRE(started.isObject());
    CHECK(started["compiled_in"].getBool());
    CHECK(started["ok"].getBool());
    CHECK(started["active"].getBool());

    const auto duplicate_start =
        insp.handle(request(methods::kTraceStartSession));
    CHECK(duplicate_start.is_error);
    CHECK(duplicate_start.error_code == "trace_already_active");

    // Emit a little so the trace is non-trivial.
    { PULP_TRACE_SCOPE_NAMED("render", "inspector_probe_frame"); }

    auto stopped = result_of(insp.handle(request(methods::kTraceStopSession)));
    REQUIRE(stopped.isObject());
    CHECK(stopped["ok"].getBool());
    CHECK_FALSE(std::string(stopped["out_path"].getString()).empty());
    CHECK(stopped["trace_bytes"].getInt64() > 0);

    // Snapshot now surfaces the flushed path.
    auto snap = result_of(insp.handle(request(methods::kTraceSnapshot)));
    CHECK(snap.hasObjectMember("last_trace_path"));
    CHECK_FALSE(insp.owns_method("Trace.nope"));

    const auto duplicate_stop =
        insp.handle(request(methods::kTraceStopSession));
    CHECK(duplicate_stop.is_error);
    CHECK(duplicate_stop.error_code == "no_active_trace");
}

#else

TEST_CASE("TraceInspector says tracing is not compiled in when OFF", "[tracing][inspect]") {
    TraceInspector insp;

    const auto started = insp.handle(request(methods::kTraceStartSession));
    CHECK(started.is_error);
    CHECK(started.error_code == "tracing_unavailable");
    CHECK(started.params_json.find("-DPULP_TRACING=ON") != std::string::npos);

    const auto stopped = insp.handle(request(methods::kTraceStopSession));
    CHECK(stopped.is_error);
    CHECK(stopped.error_code == "tracing_unavailable");
}

#endif
