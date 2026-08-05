// TraceInspector bridge tests. Config-agnostic: the same suite runs with
// PULP_TRACING ON and OFF. OFF (the default, shipping config) verifies the
// bridge reports honestly that tracing is not compiled in — which is the
// "did I forget to enable it?" answer the CLI relays. ON verifies a real
// session round-trips: start → stop writes a non-empty .pftrace and snapshot
// reflects it.

#include <catch2/catch_test_macros.hpp>

#include <pulp/inspect/authentication.hpp>
#include <pulp/inspect/domain_handler.hpp>
#include <pulp/inspect/inspector_server.hpp>
#include <pulp/inspect/protocol.hpp>
#include <pulp/inspect/trace_inspector.hpp>
#include <pulp/runtime/trace.hpp>  // kTracingEnabled
#include <pulp/runtime/trace_session.hpp>

#include <choc/text/choc_JSON.h>

#include <filesystem>
#include <string>
#include <utility>

using namespace pulp::inspect;

namespace {

InspectorMessage request(const std::string& method, const std::string& params = "{}") {
    return make_request(1, method, params);
}

choc::value::Value result_of(const InspectorMessage& resp) {
    return choc::json::parse(resp.params_json);
}

TracePublicationOwner owner(std::string publication = "publication-a") {
    return {"session-a", "instance-a", std::move(publication)};
}

std::unique_ptr<InspectorPublicationLease> bind_trace(
    TraceInspector& inspector,
    TracePublicationOwner publication_owner = owner()) {
    InspectorDiscoveryRecord record;
    record.session_id = publication_owner.session_id;
    record.instance_id = publication_owner.instance_id;
    record.publication_id = publication_owner.publication_id;
    return inspector.bind_publication(record);
}

struct ScopedTestDirectory {
    std::filesystem::path path;
    ~ScopedTestDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

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

TEST_CASE("DomainHandler wires one TraceInspector into dispatch and publication",
          "[tracing][inspect][wiring]") {
    DomainHandler handler;
    InspectorServerConfig server_config;
    auto trace = std::make_shared<TraceInspector>();

    handler.set_trace_inspector(trace);
    server_config.domain_bindings = &handler;

    const auto bindings = handler.publication_bindings();
    REQUIRE(bindings.size() == 1);
    CHECK(bindings.front().capability ==
          InspectorCapability::TraceSessionControl);
    CHECK(bindings.front().binding.get() == trace.get());
    const auto response =
        handler.handle(request(methods::kTraceSnapshot));
    CHECK(response.is_error ==
          trace->handle(request(methods::kTraceSnapshot)).is_error);
}

TEST_CASE("trace-capable server validates its domain-owned controller",
          "[tracing][inspect][wiring][publication]") {
    const auto token = generate_inspector_secret();
    REQUIRE(token.has_value());
    std::string suffix;
    for (std::size_t index = 0; index < 8; ++index)
        suffix += "0123456789abcdef"[(*token)[index] & 0xf];
    ScopedTestDirectory temporary{
        std::filesystem::temp_directory_path() /
        ("pulp-trace-wiring-" + suffix)};
    InspectorDiscoveryPublisher publisher(temporary.path);
    InspectorPolicyConfig policy;
    policy.profile = InspectorProfile::Custom;
    policy.available_capabilities = {
        InspectorCapability::SessionControl,
        InspectorCapability::TraceSessionControl,
    };
    policy.custom_capabilities = policy.available_capabilities;
    DomainHandler handler;
    InspectorSession session(
        {"session-trace-wiring", "instance", "plugin", "1"},
        policy,
        [&handler](const auto& message) {
            return handler.handle(message);
        });
    InspectorDiscoveryRecord record;
    record.session_id = session.info().session_id;
    record.instance_id = session.info().instance_id;
    record.plugin_id = session.info().plugin_id;
    InspectorServer server;

    TraceInspector dispatch_only;
    handler.set_trace_inspector(&dispatch_only);
    InspectorServerConfig missing{
        &session, &publisher, record, *token};
    missing.domain_bindings = &handler;
    CHECK_FALSE(server.start_authenticated(std::move(missing)));

    auto trace = std::make_shared<TraceInspector>();
    handler.set_trace_inspector(trace);

    InspectorServerConfig valid{
        &session, &publisher, record, *token};
    valid.domain_bindings = &handler;
    REQUIRE(server.start_authenticated(std::move(valid)));
    CHECK(server.port() != 0);
    server.stop();
}

TEST_CASE("ungranted trace availability does not require a publication binding",
          "[tracing][inspect][wiring][policy]") {
    const auto token = generate_inspector_secret();
    REQUIRE(token.has_value());
    std::string suffix;
    for (std::size_t index = 0; index < 8; ++index)
        suffix += "0123456789abcdef"[(*token)[index] & 0xf];
    ScopedTestDirectory temporary{
        std::filesystem::temp_directory_path() /
        ("pulp-trace-observe-" + suffix)};
    InspectorDiscoveryPublisher publisher(temporary.path);
    InspectorPolicyConfig policy;
    policy.profile = InspectorProfile::Observe;
    policy.available_capabilities = {
        InspectorCapability::SessionDescribe,
        InspectorCapability::TraceSessionControl,
    };
    DomainHandler handler;
    TraceInspector dispatch_only;
    handler.set_trace_inspector(&dispatch_only);
    InspectorSession session(
        {"session-trace-observe", "instance", "plugin", "1"},
        policy,
        [&handler](const auto& message) {
            return handler.handle(message);
        });
    InspectorDiscoveryRecord record;
    record.session_id = session.info().session_id;
    record.instance_id = session.info().instance_id;
    record.plugin_id = session.info().plugin_id;
    InspectorServer server;
    REQUIRE(server.start_authenticated(
        InspectorServerConfig{
            &session, &publisher, record, *token}));
    server.stop();
}

TEST_CASE("TraceInspector rejects overlapping publication leases",
          "[tracing][inspect][security]") {
    TraceInspector inspector;
    auto first = bind_trace(inspector, owner("publication-first"));
    REQUIRE(first);
    CHECK_FALSE(bind_trace(inspector, owner("publication-first")));
    CHECK_FALSE(bind_trace(inspector, owner("publication-second")));

    first.reset();
    CHECK(bind_trace(inspector, owner("publication-second")));
}

TEST_CASE("TraceInspector rejects an unknown Trace method", "[tracing][inspect]") {
    TraceInspector insp;
    auto resp = insp.handle(request("Trace.bogus"));
    CHECK(resp.is_error);
}

TEST_CASE("TraceInspector enforces frozen category bounds before capture",
          "[tracing][inspect][security]") {
    TraceInspector inspector;
    const auto invalid = [&](std::string params) {
        const auto response = inspector.handle(
            request(methods::kTraceStartSession, std::move(params)));
        CHECK(response.is_error);
        CHECK(response.error_code == "invalid_params");
    };

    invalid("[]");
    invalid(R"({"categories":"render"})");
    invalid(R"({"categories":[1]})");
    invalid(R"({"categories":[""]})");
    invalid(R"({"categories":["render","render"]})");
    invalid(R"({"categories":["render"],"unexpected":true})");
    invalid("{\"categories\":[\"" + std::string(129, 'x') + "\"]}");

    std::string too_many = R"({"categories":[)";
    for (std::size_t index = 0; index < 129; ++index) {
        if (index != 0) too_many += ',';
        too_many += "\"category-" + std::to_string(index) + "\"";
    }
    too_many += "]}";
    invalid(std::move(too_many));
}

TEST_CASE("TraceInspector stop rejects undeclared params before state changes",
          "[tracing][inspect][security][control-contract]") {
    TraceInspector inspector;
    for (const auto* params : {
             R"({"unexpected":true})",
             R"([])",
             "not-json",
         }) {
        const auto response = inspector.handle(
            request(methods::kTraceStopSession, params));
        CHECK(response.is_error);
        CHECK(response.error_code == "invalid_params");
    }
}

TEST_CASE("TraceInspector snapshot reports compile-time tracing state", "[tracing][inspect]") {
    TraceInspector insp;
    auto out = result_of(insp.handle(request(methods::kTraceSnapshot)));
    REQUIRE(out.isObject());
    CHECK(out["compiled_in"].getBool() == pulp::runtime::kTracingEnabled);
    CHECK_FALSE(out["trace_control_available"].getBool());
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
             R"({"ring_mb":80.5})",
             R"({"ring_mb":"80"})",
         }) {
        INFO(params);
        const auto response = insp.handle(
            request(methods::kTraceStartSession, params));
        CHECK(response.is_error);
        CHECK(response.error_code == "invalid_params");
    }

    const auto integral_exponent = insp.handle(request(
        methods::kTraceStartSession, R"({"ring_mb":8e+1})"));
    CHECK(integral_exponent.error_code != "invalid_params");
}

#if defined(PULP_TRACING_ENABLED) && PULP_TRACING_ENABLED

TEST_CASE("TraceInspector round-trips a real session when tracing is ON", "[tracing][inspect]") {
    TraceInspector insp;
    auto lease = bind_trace(insp);
    REQUIRE(lease);

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

TEST_CASE("TraceInspector enforces process-global publication ownership",
          "[tracing][inspect][security]") {
    TraceInspector first;
    auto first_lease = bind_trace(first, owner("publication-first"));
    REQUIRE(first_lease);
    TraceInspector second;
    auto second_lease = bind_trace(second, owner("publication-second"));
    REQUIRE(second_lease);

    const auto started =
        first.handle(request(methods::kTraceStartSession, R"({"ring_mb":8})"));
    REQUIRE_FALSE(started.is_error);

    const auto other_stop =
        second.handle(request(methods::kTraceStopSession));
    CHECK(other_stop.is_error);
    CHECK(other_stop.error_code == "trace_owned_by_another_controller");

    const auto other_start =
        second.handle(request(methods::kTraceStartSession));
    CHECK(other_start.is_error);
    CHECK(other_start.error_code == "trace_owned_by_another_controller");

    const auto owner_snapshot =
        result_of(first.handle(request(methods::kTraceSnapshot)));
    const auto other_snapshot =
        result_of(second.handle(request(methods::kTraceSnapshot)));
    CHECK(owner_snapshot["trace_control_available"].getBool());
    CHECK_FALSE(other_snapshot["trace_control_available"].getBool());

    const auto stopped = first.handle(request(methods::kTraceStopSession));
    CHECK_FALSE(stopped.is_error);
    const auto available_snapshot =
        result_of(second.handle(request(methods::kTraceSnapshot)));
    CHECK(available_snapshot["trace_control_available"].getBool());
}

TEST_CASE("TraceInspector does not claim externally started captures",
          "[tracing][inspect][security]") {
    REQUIRE(pulp::runtime::Tracing::start());
    TraceInspector insp;
    auto lease = bind_trace(insp);
    REQUIRE(lease);

    const auto stop = insp.handle(request(methods::kTraceStopSession));
    CHECK(stop.is_error);
    CHECK(stop.error_code == "trace_owned_by_another_controller");

    CHECK(pulp::runtime::Tracing::stop().ok);
}

TEST_CASE("TraceInspector teardown stops an abandoned owned capture",
          "[tracing][inspect]") {
    {
        TraceInspector insp;
        auto lease = bind_trace(insp);
        REQUIRE(lease);
        const auto started =
            insp.handle(request(methods::kTraceStartSession, R"({"ring_mb":8})"));
        REQUIRE_FALSE(started.is_error);
        REQUIRE(pulp::runtime::Tracing::active());
    }
    CHECK_FALSE(pulp::runtime::Tracing::active());
}

TEST_CASE("TraceInspector stale ownership cannot stop a replacement capture",
          "[tracing][inspect][security]") {
    {
        TraceInspector insp;
        auto lease = bind_trace(insp);
        REQUIRE(lease);
        REQUIRE_FALSE(
            insp.handle(request(methods::kTraceStartSession)).is_error);
        REQUIRE(pulp::runtime::Tracing::stop().ok);
        REQUIRE(pulp::runtime::Tracing::start());

        const auto stale_stop =
            insp.handle(request(methods::kTraceStopSession));
        CHECK(stale_stop.is_error);
        CHECK(stale_stop.error_code == "trace_owned_by_another_controller");
        CHECK(pulp::runtime::Tracing::active());
    }

    // Destruction of the stale inspector owner also leaves the replacement
    // host capture intact.
    CHECK(pulp::runtime::Tracing::active());
    CHECK(pulp::runtime::Tracing::stop().ok);
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
