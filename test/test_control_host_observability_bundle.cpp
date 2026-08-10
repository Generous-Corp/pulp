#include <catch2/catch_test_macros.hpp>

#include <pulp/inspect/control_host_observability_bundle.hpp>
#include <pulp/view/value_channel_set.hpp>

#include <choc/text/choc_JSON.h>

#include <chrono>
#include <limits>
#include <memory>
#include <thread>

using namespace std::chrono_literals;
using namespace pulp::inspect;

namespace {

ControlHostObservabilityBinding binding() {
    return {.registration_id = ControlRegistrationId{"registration-a"},
            .session_id = "session-a",
            .instance_id = "instance-a",
            .publication_id = "publication-a",
            .authentication_token = std::string(64, 'a')};
}

ControlAdmissionPlan plan(InspectorCapability capability, std::string operation) {
    ControlAdmissionPlan result;
    result.client_id = ControlClientId{"client-a"};
    result.registration_id = ControlRegistrationId{"registration-a"};
    result.grant_id = ControlGrantId{"grant-a"};
    result.session_id = "session-a";
    result.instance_id = "instance-a";
    result.publication_id = "publication-a";
    result.instance_generation = "generation-a";
    result.capability = capability;
    result.operation_id = std::move(operation);
    result.operation_version = 1;
    result.deadline_unix_ms = 1234567;
    return result;
}

ControlRequestEnvelope request(const ControlAdmissionPlan& admission, std::string params) {
    return {.request_id = "request-a",
            .client_id = admission.client_id.value,
            .registration_id = admission.registration_id.value,
            .grant_id = admission.grant_id.value,
            .instance_generation = admission.instance_generation,
            .operation_id = admission.operation_id,
            .operation_version = admission.operation_version,
            .deadline_unix_ms = admission.deadline_unix_ms,
            .params_json = std::move(params)};
}

ControlExecutionContext context() {
    return {.checkpoint = [] { return ControlExecutionCheckpoint::Continue; }};
}

} // namespace

TEST_CASE("observability bundle enforces exact authority heartbeat and disconnect lifetime",
          "[inspect][control][observability][security][lifetime]") {
    auto now = std::chrono::steady_clock::time_point{10s};
    auto telemetry = std::make_shared<ControlTelemetryTap>(ControlTelemetryTapConfig{});
    int trace_calls = 0;
    auto bundle = ControlHostObservabilityBundle::create({
        .binding = binding(),
        .trace_executor =
            [&](const ControlAdmissionPlan&, const ControlRequestEnvelope&,
                const ControlExecutionContext&) {
                ++trace_calls;
                return ControlExecutionOutcome{.result = {.detail_json = R"({"ok":true})"}};
            },
        .telemetry = telemetry,
        .heartbeat_ttl = 5s,
        .clock = [&] { return now; },
    });
    REQUIRE(bundle);
    REQUIRE(bundle->ready());
    const auto admission =
        plan(InspectorCapability::TraceSessionControl, "dev.pulp.trace/session-control@1");
    const auto envelope = request(admission, R"({"action":"stop"})");
    CHECK(bundle->executor()(admission, envelope, context()).terminal_state ==
          ControlReceiptState::Completed);
    CHECK(trace_calls == 1);

    auto wrong_grant = envelope;
    wrong_grant.grant_id = "grant-b";
    const auto denied = bundle->executor()(admission, wrong_grant, context());
    CHECK(denied.result.result_code == ControlResultCode::PolicyDenied);
    CHECK(trace_calls == 1);

    auto wrong_heartbeat = binding();
    wrong_heartbeat.authentication_token = "client-controlled-value";
    CHECK_FALSE(bundle->heartbeat(wrong_heartbeat));
    now += 4s;
    REQUIRE(bundle->heartbeat(binding()));
    now += 4s;
    CHECK(bundle->ready());
    now += 2s;
    CHECK_FALSE(bundle->heartbeat(binding()));
    CHECK_FALSE(bundle->ready());
    const auto expired = bundle->executor()(admission, envelope, context());
    CHECK(expired.result.result_code == ControlResultCode::SessionStale);
    CHECK_FALSE(bundle->heartbeat(binding()));

    bundle->disconnect();
    CHECK_FALSE(bundle->ready());
}

TEST_CASE("observability bundle exposes typed bounded telemetry lifecycle",
          "[inspect][control][observability][telemetry][loss][redaction]") {
    pulp::view::ValueChannelSet channels;
    auto* gain = channels.declare_scalar("gain");
    REQUIRE(gain);
    auto now = std::chrono::steady_clock::time_point{10s};
    auto telemetry = std::make_shared<ControlTelemetryTap>(
        ControlTelemetryTapConfig{.enabled = true}, [&] { return now; });
    REQUIRE(telemetry->attach(channels.attach_telemetry(), [](std::string_view) {
        return ControlTelemetrySensitivity::Observable;
    }));
    auto bundle = ControlHostObservabilityBundle::create({
        .binding = binding(),
        .trace_executor = [](const ControlAdmissionPlan&, const ControlRequestEnvelope&,
                             const ControlExecutionContext&) { return ControlExecutionOutcome{}; },
        .telemetry = telemetry,
        .heartbeat_ttl = 30s,
        .clock = [&] { return now; },
    });
    REQUIRE(bundle);
    const auto admission =
        plan(InspectorCapability::TelemetryStream, "dev.pulp.telemetry/subscribe@1");
    const auto subscribe = bundle->executor()(
        admission,
        request(admission,
                R"({"action":"subscribe","channel_ids":["gain"],"max_hz":15,"buffer_samples":32})"),
        context());
    REQUIRE(subscribe.terminal_state == ControlReceiptState::Completed);
    const auto subscribed = choc::json::parse(subscribe.result.detail_json);
    CHECK(subscribed["action"].getString() == "subscribed");
    const std::string stream_id{subscribed["stream_id"].getString()};

    gain->publish(0.5f);
    (void)gain->read();
    const auto poll =
        bundle->executor()(admission,
                           request(admission, std::string{"{\"action\":\"poll\",\"stream_id\":\""} +
                                                  stream_id + "\"}"),
                           context());
    REQUIRE(poll.terminal_state == ControlReceiptState::Completed);
    const auto polled = choc::json::parse(poll.result.detail_json);
    CHECK(polled["action"].getString() == "polled");
    CHECK(polled["available"].getBool());
    CHECK(polled["samples"].size() == 1);

    now += 100ms;
    gain->publish(std::numeric_limits<float>::quiet_NaN());
    (void)gain->read();
    const auto anomaly =
        bundle->executor()(admission,
                           request(admission, std::string{"{\"action\":\"poll\",\"stream_id\":\""} +
                                                  stream_id + "\"}"),
                           context());
    REQUIRE(anomaly.terminal_state == ControlReceiptState::Completed);
    const auto anomaly_result = choc::json::parse(anomaly.result.detail_json);
    REQUIRE(anomaly_result["samples"].size() == 1);
    CHECK(anomaly_result["samples"][0]["values"][0].getString() == "NaN");

    auto another_grant = admission;
    another_grant.grant_id = ControlGrantId{"grant-b"};
    const auto isolated = bundle->executor()(
        another_grant,
        request(another_grant,
                std::string{"{\"action\":\"poll\",\"stream_id\":\""} + stream_id + "\"}"),
        context());
    REQUIRE(isolated.terminal_state == ControlReceiptState::Completed);
    CHECK_FALSE(choc::json::parse(isolated.result.detail_json)["available"].getBool());

    const auto unsubscribe = bundle->executor()(
        admission,
        request(admission,
                std::string{"{\"action\":\"unsubscribe\",\"stream_id\":\""} + stream_id + "\"}"),
        context());
    REQUIRE(unsubscribe.terminal_state == ControlReceiptState::Completed);
    CHECK(telemetry->subscription_count() == 0);
}

TEST_CASE("trace-only observability does not require or expose a telemetry source",
          "[inspect][control][observability][telemetry][capability]") {
    auto bundle = ControlHostObservabilityBundle::create({
        .binding = binding(),
        .trace_executor = [](const ControlAdmissionPlan&, const ControlRequestEnvelope&,
                             const ControlExecutionContext&) {
            return ControlExecutionOutcome{.result = {.detail_json = R"({"ok":true})"}};
        },
        .heartbeat_ttl = 30s,
    });
    REQUIRE(bundle);
    REQUIRE(bundle->ready());

    const auto admission =
        plan(InspectorCapability::TelemetryStream, "dev.pulp.telemetry/subscribe@1");
    const auto unavailable = bundle->executor()(
        admission,
        request(admission,
                R"({"action":"subscribe","channel_ids":["gain"],"max_hz":15,"buffer_samples":32})"),
        context());
    CHECK(unavailable.result.result_code == ControlResultCode::NotImplemented);
}

TEST_CASE("observability bundle autonomously releases ownership after missed heartbeat",
          "[inspect][control][observability][heartbeat][expiry]") {
    auto telemetry = std::make_shared<ControlTelemetryTap>(ControlTelemetryTapConfig{});
    auto trace_owner = std::make_shared<int>(1);
    std::weak_ptr<int> released = trace_owner;
    auto bundle = ControlHostObservabilityBundle::create({
        .binding = binding(),
        .trace_executor =
            [trace_owner](const ControlAdmissionPlan&, const ControlRequestEnvelope&,
                          const ControlExecutionContext&) { return ControlExecutionOutcome{}; },
        .telemetry = telemetry,
        .heartbeat_ttl = 20ms,
    });
    REQUIRE(bundle);
    trace_owner.reset();
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (!released.expired() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    CHECK(released.expired());
    CHECK_FALSE(bundle->ready());
}
