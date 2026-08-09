#include <catch2/catch_test_macros.hpp>

#include "mcp_control_tools.hpp"
#include "mcp_server.hpp"
#include "support/thread_progress.hpp"

#include <pulp/inspect/control_inspector_client.hpp>
#include <pulp/inspect/control_manifest.hpp>
#include <pulp/inspect/control_protocol.hpp>

#include <choc/text/choc_JSON.h>

#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#if PULP_MCP_ENABLE_INSPECTOR_CLIENT
namespace {

using namespace pulp::inspect;
using namespace pulp_mcp;

struct FakeState {
    std::optional<ControlRequestEnvelope> request;
    std::optional<ControlNegotiationOffer> offer;
    ControlMcpSession::ProgressSink progress;
    bool revoke_during_call = false;
    bool artifact_allowed = true;
    bool artifact_wrong_instance = false;
    bool artifact_chunked = false;
    bool inventory_session_error_once = false;
    int grant_requests = 0;
    int session_opens = 0;
    std::chrono::milliseconds timeout_step_advance{};
    std::chrono::steady_clock::time_point timeout_clock{};
    std::vector<std::pair<std::string, std::chrono::milliseconds>> timeout_steps;
    std::mutex request_mutex;
    std::condition_variable request_condition;
    bool block_request = false;
    bool request_started = false;
    bool cancellation_seen = false;
};

class FakeTransport final : public ControlClientTransport {
  public:
    explicit FakeTransport(std::shared_ptr<FakeState> state) : state_(std::move(state)) {}

    ControlTransportDispatchResult dispatch(std::string_view encoded,
                                            std::chrono::milliseconds timeout) override {
        auto envelope = decode_control_envelope(encoded);
        if (!envelope)
            return {.error_code = "decode", .explanation = "invalid request"};
        if (const auto* offer = std::get_if<ControlNegotiationOffer>(&envelope->payload)) {
            record_timeout("negotiate", timeout);
            state_->offer = *offer;
            return encoded_response(ControlEnvelope{.payload = ControlNegotiationResult{
                                                        .status = ControlNegotiationStatus::Accepted,
                                                        .selected_version = kControlProtocolVersion,
                                                        .features = {"artifacts", "cancellation", "progress", "receipts"}}});
        }
        if (const auto* request = std::get_if<ControlRequestEnvelope>(&envelope->payload)) {
            record_timeout("request", timeout);
            state_->request = *request;
            if (state_->block_request) {
                std::unique_lock lock(state_->request_mutex);
                state_->request_started = true;
                state_->request_condition.notify_all();
                if (!state_->request_condition.wait_for(
                        lock, pulp::test::kProgressDeadline,
                        [&] { return state_->cancellation_seen; })) {
                    return {.error_code = "cancellation-timeout",
                            .explanation = "test transport did not observe cancellation"};
                }
            }
            if (state_->progress)
                state_->progress({.request_id = request->request_id,
                                  .receipt_id = "receipt-1",
                                  .sequence = 1,
                                  .current = 1,
                                  .total = 2,
                                  .detail_json = R"({"phase":"read"})"});
            const auto revoked = state_->revoke_during_call;
            const auto detail = request->operation_id == "dev.pulp.trace/session-control@1"
                                    ? R"({"active":true,"compiled_in":true,"ok":true})"
                                    : R"({"generation":1,"parameters":[]})";
            return encoded_response(ControlEnvelope{.payload = ControlReceiptEnvelope{
                                                        .request_id = request->request_id,
                                                        .receipt_id = "receipt-1",
                                                        .operation_id = request->operation_id,
                                                        .operation_version = request->operation_version,
                                                        .state = revoked ? ControlReceiptState::CompletedAfterRevocation
                                                                         : ControlReceiptState::Completed,
                                                        .result_code = revoked ? std::optional{ControlResultCode::CompletedAfterRevocation}
                                                                               : std::nullopt,
                                                        .explanation = revoked ? "grant revoked while applying" : "",
                                                        .detail_json = detail}});
        }
        if (const auto* cancel = std::get_if<ControlCancelEnvelope>(&envelope->payload)) {
            {
                std::lock_guard lock(state_->request_mutex);
                state_->cancellation_seen = true;
            }
            state_->request_condition.notify_all();
            return encoded_response(ControlEnvelope{.payload = ControlReceiptEnvelope{
                                                        .request_id = cancel->request_id,
                                                        .receipt_id = "receipt-cancel",
                                                        .operation_id = "dev.pulp.state/read@1",
                                                        .state = ControlReceiptState::Running,
                                                        .explanation = "cancellation requested",
                                                        .detail_json = "{}"}});
        }
        return {.error_code = "unexpected", .explanation = "unexpected envelope"};
    }

    ControlArtifactReadResult read_artifact(std::string_view artifact_id, std::uint64_t offset,
                                            std::size_t, std::chrono::milliseconds) override {
        if (!state_->artifact_allowed)
            return {.status = ControlArtifactStatus::Unauthorized,
                    .explanation = "artifact ACL denied this client"};
        ControlArtifactMetadata metadata;
        metadata.artifact_id = std::string(artifact_id);
        metadata.lineage.producer_registration_id = "registration-1";
        metadata.lineage.instance_id = state_->artifact_wrong_instance ? "instance-2" : "instance-1";
        metadata.lineage.publication_id = "publication-1";
        metadata.sha256 = std::string(64, 'a');
        metadata.byte_size = state_->artifact_chunked ? 6 : 3;
        metadata.content_type = "application/octet-stream";
        const auto second_chunk = state_->artifact_chunked && offset == 3;
        return {.status = ControlArtifactStatus::Read,
                .metadata = std::move(metadata),
                .bytes = second_chunk ? std::vector<std::uint8_t>{'d', 'e', 'f'}
                                      : std::vector<std::uint8_t>{'a', 'b', 'c'},
                .eof = !state_->artifact_chunked || second_chunk};
    }

  private:
    void record_timeout(std::string label, std::chrono::milliseconds timeout) {
        if (state_->timeout_step_advance <= std::chrono::milliseconds::zero())
            return;
        state_->timeout_steps.emplace_back(std::move(label), timeout);
        state_->timeout_clock += state_->timeout_step_advance;
    }

    static ControlTransportDispatchResult encoded_response(ControlEnvelope envelope) {
        return {.encoded_response = encode_control_envelope(envelope)};
    }
    std::shared_ptr<FakeState> state_;
};

class FakeSession final : public ControlMcpSession {
  public:
    explicit FakeSession(std::shared_ptr<FakeState> state)
        : state_(std::move(state)), transport_(state_) {}

    ControlClientTransport& transport() override { return transport_; }
    ControlManagementResult manage(std::string_view command,
                                   std::string_view params_json,
                                   std::chrono::milliseconds timeout) override {
        if (state_->timeout_step_advance > std::chrono::milliseconds::zero()) {
            state_->timeout_steps.emplace_back(std::string(command), timeout);
            state_->timeout_clock += state_->timeout_step_advance;
        }
        if (command == "instances") {
            if (state_->inventory_session_error_once) {
                state_->inventory_session_error_once = false;
                return {.status_id = "session-superseded",
                        .explanation = "test session expired"};
            }
            return {.status_id = "completed",
                    .data_json = R"({"schema":"pulp.control.instances.v1","instances":[{"instance_id":"instance-1","plugin_id":"dev.pulp.fixture","profile":"developer-local","publication_id":"publication-1","registration_id":"registration-1","session_id":"session-1"}]})"};
        }
        if (command == "grant-request") {
            ++state_->grant_requests;
            const auto params = choc::json::parse(params_json);
            REQUIRE(params["instance_id"].getString() == "instance-1");
            if (params.hasObjectMember("operation_id") &&
                params["operation_id"].getString() == "dev.pulp.runtime/evaluate@1")
                return {.status_id = "consent-required",
                        .explanation = "broker-owned consent is required"};
            return {.status_id = "granted",
                    .data_json = R"({"schema":"pulp.control.grant.v1","grant_id":"grant-1","instance_id":"instance-1"})"};
        }
        if (command == "revoke")
            return {.status_id = "revoked",
                    .data_json = R"({"schema":"pulp.control.revoke.v1","grant_id":"grant-1"})"};
        return {.status_id = "invalid-request", .explanation = "unsupported"};
    }
    std::string_view client_id() const override { return "client-1"; }
    void set_progress_sink(ProgressSink sink) override { state_->progress = std::move(sink); }

  private:
    std::shared_ptr<FakeState> state_;
    FakeTransport transport_;
};

class FakeTraceOpener final : public InspectorControlSessionOpener {
  public:
    explicit FakeTraceOpener(std::shared_ptr<FakeState> state) : state_(std::move(state)) {}

    std::optional<InspectorControlSession> open(std::chrono::milliseconds) override {
        return InspectorControlSession{
            .transport = std::make_unique<FakeTransport>(state_),
            .client_id = ControlClientId{"client-1"},
            .registration_id = ControlRegistrationId{"registration-1"},
            .grant_id = ControlGrantId{"grant-1"},
            .instance_generation = "publication-1",
            .target = {"session-1", "instance-1", "publication-1"},
        };
    }

  private:
    std::shared_ptr<FakeState> state_;
};

ControlMcpSessionFactory factory(std::shared_ptr<FakeState> state) {
    return [state = std::move(state)](std::chrono::milliseconds timeout) {
        ++state->session_opens;
        if (state->timeout_step_advance > std::chrono::milliseconds::zero()) {
            state->timeout_steps.emplace_back("session", timeout);
            state->timeout_clock += state->timeout_step_advance;
        }
        return ControlMcpOpenResult{.session = std::make_unique<FakeSession>(state)};
    };
}

std::string state_read_args(std::string_view grant = {}) {
    return std::string(R"({"instance_id":"instance-1",)") +
           (grant.empty() ? "" : R"("grant_id":")" + std::string(grant) + "\",") +
           R"("input":{"include_catalog":false,"include_sensitive":false}})";
}

} // namespace

TEST_CASE("control MCP bindings are generated from every canonical operation",
          "[mcp][control][protocol]") {
    auto state = std::make_shared<FakeState>();
    ControlMcpAdapter adapter(factory(state));
    const auto tools = adapter.tools_json_fragment();
    for (const auto& operation : control_operation_registry()) {
        INFO(operation.id);
        auto suffix = std::string(operation.id.substr(std::string_view("dev.pulp.").size()));
        suffix.resize(suffix.size() - 2);
        for (auto& character : suffix)
            if (!std::isalnum(static_cast<unsigned char>(character))) character = '_';
        if (capability_is_grantable(operation.capability) ||
            operation.capability == InspectorCapability::ArtifactRead) {
            REQUIRE(tools.find("\"name\":\"pulp_control_" + suffix + "\"") != std::string::npos);
            REQUIRE(tools.find(std::string(operation.input_schema_json)) != std::string::npos);
            REQUIRE(tools.find(std::string(operation.output_schema_json)) != std::string::npos);
        } else {
            REQUIRE(tools.find("\"name\":\"pulp_control_" + suffix + "\"") == std::string::npos);
        }
    }
    REQUIRE(tools.find("\"required\":[\"instance_id\",\"input\"]") != std::string::npos);
}

TEST_CASE("control MCP and CLI semantics produce the same canonical service request",
          "[mcp][control][parity][progress][negotiation]") {
    auto state = std::make_shared<FakeState>();
    std::vector<std::string> notifications;
    ControlMcpAdapter adapter(factory(state),
                              [&](std::string value) { notifications.push_back(std::move(value)); });
    const auto result = adapter.call_tool("pulp_control_state_read", state_read_args(), "progress-1");
    REQUIRE(result.find("\"isError\":true") == std::string::npos);
    REQUIRE(result.find("\"generation\"") != std::string::npos);
    REQUIRE(result.find("\"progress\"") != std::string::npos);
    REQUIRE(result.find("\"phase\"") != std::string::npos);
    REQUIRE(notifications.size() == 1);
    REQUIRE(notifications.front().find("notifications/progress") != std::string::npos);
    REQUIRE(state->grant_requests == 1);
    const auto repeated = adapter.call_tool("pulp_control_state_read", state_read_args());
    REQUIRE(repeated.find("\"isError\":true") == std::string::npos);
    REQUIRE(state->grant_requests == 1);
    REQUIRE(state->offer.has_value());
    REQUIRE(state->offer->mandatory_features == std::vector<std::string>{"receipts"});
    REQUIRE(state->request.has_value());
    CHECK(state->request->client_id == "client-1");
    CHECK(state->request->registration_id == "registration-1");
    CHECK(state->request->grant_id == "grant-1");
    CHECK(state->request->instance_generation == "publication-1");
    CHECK(state->request->operation_id == "dev.pulp.state/read@1");
    CHECK(state->request->operation_version == 1);
    CHECK(control_request_hash(*state->request) == state->request->request_hash);

    for (const auto malformed : {
             R"({"instance_id":"instance-1","grant_id":7,"input":{"include_catalog":false,"include_sensitive":false}})",
             R"({"instance_id":"instance-1","request_id":false,"input":{"include_catalog":false,"include_sensitive":false}})",
             R"({"instance_id":"instance-1","timeout_ms":"3000","input":{"include_catalog":false,"include_sensitive":false}})",
             R"({"instance_id":"instance-1","expected_state_generation":"1","input":{"include_catalog":false,"include_sensitive":false}})",
             R"({"instance_id":"instance-1","unexpected":true,"input":{"include_catalog":false,"include_sensitive":false}})"}) {
        const auto rejected = adapter.call_tool("pulp_control_state_read", malformed);
        REQUIRE(rejected.find("invalid-arguments") != std::string::npos);
    }
}

TEST_CASE("control MCP timeout is one deadline across the complete operation",
          "[mcp][control][timeout]") {
    auto state = std::make_shared<FakeState>();
    state->timeout_step_advance = std::chrono::milliseconds(10);
    ControlMcpAdapter adapter(factory(state), {}, [state] { return state->timeout_clock; });

    const auto result = adapter.call_tool(
        "pulp_control_state_read",
        R"({"instance_id":"instance-1","timeout_ms":100,"input":{"include_catalog":false,"include_sensitive":false}})");

    REQUIRE(result.find("\"isError\":true") == std::string::npos);
    const std::vector<std::pair<std::string, std::chrono::milliseconds>> expected{
        {"session", std::chrono::milliseconds(100)},
        {"instances", std::chrono::milliseconds(90)},
        {"grant-request", std::chrono::milliseconds(80)},
        {"negotiate", std::chrono::milliseconds(70)},
        {"request", std::chrono::milliseconds(60)}};
    REQUIRE(state->timeout_steps == expected);
}

TEST_CASE("control MCP stops when the shared operation deadline is exhausted",
          "[mcp][control][timeout]") {
    auto state = std::make_shared<FakeState>();
    state->timeout_step_advance = std::chrono::milliseconds(30);
    ControlMcpAdapter adapter(factory(state), {}, [state] { return state->timeout_clock; });

    const auto result = adapter.call_tool(
        "pulp_control_state_read",
        R"({"instance_id":"instance-1","timeout_ms":100,"input":{"include_catalog":false,"include_sensitive":false}})");

    REQUIRE(result.find("\"code\":\"timeout\"") != std::string::npos);
    const std::vector<std::pair<std::string, std::chrono::milliseconds>> expected{
        {"session", std::chrono::milliseconds(100)},
        {"instances", std::chrono::milliseconds(70)},
        {"grant-request", std::chrono::milliseconds(40)},
        {"negotiate", std::chrono::milliseconds(10)}};
    REQUIRE(state->timeout_steps == expected);
    REQUIRE_FALSE(state->request.has_value());
}

TEST_CASE("control MCP cold session cannot reset the inventory budget",
          "[mcp][control][timeout]") {
    auto state = std::make_shared<FakeState>();
    state->timeout_step_advance = std::chrono::milliseconds(100);
    ControlMcpAdapter adapter(factory(state), {}, [state] { return state->timeout_clock; });

    const auto result = adapter.call_tool(
        "pulp_control_state_read",
        R"({"instance_id":"instance-1","timeout_ms":100,"input":{"include_catalog":false,"include_sensitive":false}})");

    REQUIRE(result.find("\"code\":\"timeout\"") != std::string::npos);
    const std::vector<std::pair<std::string, std::chrono::milliseconds>> expected{
        {"session", std::chrono::milliseconds(100)}};
    REQUIRE(state->timeout_steps == expected);
    REQUIRE_FALSE(state->request.has_value());
}

TEST_CASE("critical MCP operations require broker-owned single-use consent",
          "[mcp][control][consent]") {
    auto state = std::make_shared<FakeState>();
    ControlMcpAdapter adapter(factory(state));
    const auto result = adapter.call_tool(
        "pulp_control_runtime_evaluate",
        R"({"instance_id":"instance-1","input":{"source":"1+1","timeout_ms":10,"idempotency_key":"eval-1"}})");
    REQUIRE(result.find("consent-required") != std::string::npos);
    REQUIRE(result.find("\"broker_owned\":true") != std::string::npos);
    REQUIRE(result.find("\"single_use\":true") != std::string::npos);
    REQUIRE(state->grant_requests == 1);
    REQUIRE_FALSE(state->request.has_value());
}

TEST_CASE("control MCP reports revocation during a call and supports cancellation",
          "[mcp][control][revocation][cancel]") {
    auto state = std::make_shared<FakeState>();
    state->revoke_during_call = true;
    ControlMcpAdapter adapter(factory(state));
    const auto revoked = adapter.call_tool("pulp_control_state_read", state_read_args("grant-1"));
    REQUIRE(revoked.find("\"isError\":true") != std::string::npos);
    REQUIRE(revoked.find("completed_after_revocation") != std::string::npos);

    state->revoke_during_call = false;
    state->block_request = true;
    auto active = std::async(std::launch::async, [&] {
        return adapter.call_tool(
            "pulp_control_state_read",
            R"({"instance_id":"instance-1","request_id":"request-1","grant_id":"grant-1","input":{}})");
    });
    {
        std::unique_lock lock(state->request_mutex);
        REQUIRE(state->request_condition.wait_for(
            lock, std::chrono::seconds(1), [&] { return state->request_started; }));
    }
    const auto opens_before_invalid_cancel = state->session_opens;
    const auto invalid_cancel = adapter.call_tool(
        "pulp_control_cancel",
        R"({"instance_id":"instance-1","request_id":"not-active","reason":"test"})");
    REQUIRE(invalid_cancel.find("request-instance-mismatch") != std::string::npos);
    REQUIRE(state->session_opens == opens_before_invalid_cancel);
    const auto cancelled = adapter.call_tool(
        "pulp_control_cancel",
        R"({"instance_id":"instance-1","request_id":"request-1","reason":"test"})");
    REQUIRE(cancelled.find("\"isError\":true") == std::string::npos);
    REQUIRE(cancelled.find("\"cancellation_accepted\": true") != std::string::npos);
    REQUIRE(cancelled.find("\"state\": \"running\"") != std::string::npos);
    REQUIRE(state->offer.has_value());
    REQUIRE(state->offer->mandatory_features ==
            std::vector<std::string>{"cancellation", "receipts"});
    REQUIRE(active.wait_for(pulp::test::kProgressDeadline) == std::future_status::ready);
    REQUIRE(active.get().find("\"isError\":true") == std::string::npos);
}

TEST_CASE("control MCP resources preserve broker artifact ACLs",
          "[mcp][control][resource][acl][subscription]") {
    auto state = std::make_shared<FakeState>();
    ControlMcpAdapter adapter(factory(state));
    const auto listed = adapter.resources_list_payload();
    REQUIRE(listed.find("pulp-control://instances/instance-1") != std::string::npos);

    const auto allowed = adapter.resource_read_payload(
        "pulp-control://artifacts/instance-1/artifact-1");
    REQUIRE(allowed.find("\"blob\":\"YWJj\"") != std::string::npos);
    REQUIRE(state->offer->mandatory_features ==
            std::vector<std::string>{"artifacts", "receipts"});
    state->artifact_chunked = true;
    const auto assembled = adapter.resource_read_payload(
        "pulp-control://artifacts/instance-1/artifact-1");
    REQUIRE(assembled.find("\"blob\":\"YWJjZGVm\"") != std::string::npos);
    state->artifact_chunked = false;
    state->artifact_wrong_instance = true;
    const auto mismatched = adapter.resource_read_payload(
        "pulp-control://artifacts/instance-1/artifact-1");
    REQUIRE(mismatched.find("artifact-instance-mismatch") != std::string::npos);
    state->artifact_wrong_instance = false;
    state->artifact_allowed = false;
    const auto denied = adapter.resource_read_payload(
        "pulp-control://artifacts/instance-1/artifact-1");
    REQUIRE(denied.find("unauthorized") != std::string::npos);
    REQUIRE(denied.find("\"isError\":true") != std::string::npos);
}

TEST_CASE("control MCP reopens an unhealthy long-lived broker session",
          "[mcp][control][session][renewal]") {
    auto state = std::make_shared<FakeState>();
    state->inventory_session_error_once = true;
    ControlMcpAdapter adapter(factory(state));
    const auto expired = adapter.call_tool("pulp_control_instances", "{}");
    REQUIRE(expired.find("session-superseded") != std::string::npos);
    const auto recovered = adapter.call_tool("pulp_control_instances", "{}");
    REQUIRE(recovered.find("\"isError\":true") == std::string::npos);
    REQUIRE(state->session_opens == 2);
}

TEST_CASE("MCP protocol advertises control resources and removed Inspector tools stay absent",
          "[mcp][control][protocol][break]") {
    auto state = std::make_shared<FakeState>();
    set_control_mcp_session_factory_for_test(factory(state));
    const auto initialize = pulp_mcp::server::handle_request(
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{"resources":{"subscribe":true}}}})");
    REQUIRE(initialize.find("\"resources\":{\"subscribe\":false") != std::string::npos);
    const auto tools = pulp_mcp::server::handle_request(
        R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})");
    REQUIRE(tools.find("pulp_control_state_read") != std::string::npos);
    REQUIRE(tools.find("pulp_control_runtime_evaluate") != std::string::npos);
    const auto state_read = tools.find("\"name\":\"pulp_control_state_read\"");
    const auto gesture = tools.find("\"name\":\"pulp_control_state_parameter_gesture\"");
    REQUIRE(tools.find("\"idempotentHint\":true", state_read) < gesture);
    REQUIRE(tools.find("\"idempotentHint\":false", gesture) != std::string::npos);
    for (const auto removed : {"pulp_inspect_set_param", "pulp_inspect_evaluate",
                               "pulp_inspect_screenshot"})
        REQUIRE(tools.find(std::string("\"name\":\"") + removed + "\"") == std::string::npos);
    const auto templates = pulp_mcp::server::handle_request(
        R"({"jsonrpc":"2.0","id":3,"method":"resources/templates/list"})");
    REQUIRE(templates.find("pulp-control://instances/{instance_id}") != std::string::npos);
    const auto resource_error = pulp_mcp::server::handle_request(
        R"({"jsonrpc":"2.0","id":5,"method":"resources/read","params":{"uri":"pulp-control://unknown"}})");
    REQUIRE(resource_error.find("\"error\":{") != std::string::npos);
    REQUIRE(resource_error.find("\"result\":") == std::string::npos);
    reset_control_mcp_session_factory_for_test();
}

TEST_CASE("MCP trace compatibility tools use an authorized canonical control session",
          "[mcp][control][trace][authorized]") {
    auto state = std::make_shared<FakeState>();
    pulp_mcp::server::set_trace_control_session_opener_for_test(
        std::make_shared<FakeTraceOpener>(state));
    const auto response = pulp_mcp::server::handle_request(
        R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"pulp_trace_start","arguments":{"categories":["dsp"],"ring_mb":8}}})");
    pulp_mcp::server::set_trace_control_session_opener_for_test({});
    INFO(response);
    REQUIRE(response.find("\"isError\":true") == std::string::npos);
    REQUIRE(response.find("\"active\": true") != std::string::npos);
    REQUIRE(state->request.has_value());
    CHECK(state->request->operation_id == "dev.pulp.trace/session-control@1");
    CHECK(state->request->registration_id == "registration-1");
    CHECK(state->request->grant_id == "grant-1");
}
#endif
