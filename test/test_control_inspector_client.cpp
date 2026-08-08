#include <pulp/inspect/control_inspector_client.hpp>

#include <pulp/inspect/control_protocol.hpp>

#include <catch2/catch_test_macros.hpp>
#include <choc/text/choc_JSON.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace pulp::inspect;

namespace {

struct FakeState {
    std::vector<ControlEnvelope> requests;
    std::function<ControlTransportDispatchResult(const ControlEnvelope&)> dispatch;
};

class FakeTransport final : public ControlClientTransport {
  public:
    explicit FakeTransport(std::shared_ptr<FakeState> state) : state_(std::move(state)) {}

    ControlTransportDispatchResult dispatch(std::string_view encoded,
                                            std::chrono::milliseconds) override {
        const auto envelope = decode_control_envelope(encoded);
        if (!envelope)
            return {.error_code = "fake-invalid-request", .explanation = "decode failed"};
        state_->requests.push_back(*envelope);
        return state_->dispatch(*envelope);
    }

    ControlArtifactReadResult read_artifact(std::string_view, std::uint64_t, std::size_t,
                                            std::chrono::milliseconds) override {
        return {};
    }

  private:
    std::shared_ptr<FakeState> state_;
};

ControlTransportDispatchResult encoded(ControlEnvelope envelope) {
    return {.encoded_response = encode_control_envelope(envelope)};
}

ControlTransportDispatchResult accepted_negotiation() {
    return encoded({
        .payload =
            ControlNegotiationResult{
                .status = ControlNegotiationStatus::Accepted,
                .selected_version = kControlProtocolVersion,
                .features = {"progress", "receipts"},
            },
    });
}

ControlReceiptEnvelope completed(const ControlRequestEnvelope& request) {
    const auto params = choc::json::parse(request.params_json);
    const auto start = params["action"].getString() == "start";
    return {
        .request_id = request.request_id,
        .receipt_id = "receipt-1",
        .operation_id = request.operation_id,
        .operation_version = request.operation_version,
        .state = ControlReceiptState::Completed,
        .detail_json = start ? R"({"active":true,"compiled_in":true,"ok":true})"
                             : R"({"ok":true,"out_path":"/tmp/trace.pftrace","trace_bytes":42})",
    };
}

std::shared_ptr<FakeState> successful_state() {
    auto state = std::make_shared<FakeState>();
    state->dispatch = [](const ControlEnvelope& envelope) {
        if (std::holds_alternative<ControlNegotiationOffer>(envelope.payload))
            return accepted_negotiation();
        const auto& request = std::get<ControlRequestEnvelope>(envelope.payload);
        return encoded({.payload = completed(request)});
    };
    return state;
}

class FakeOpener final : public InspectorControlSessionOpener {
  public:
    explicit FakeOpener(std::shared_ptr<FakeState> state) : state_(std::move(state)) {}

    std::optional<InspectorControlSession> open(std::chrono::milliseconds) override {
        ++calls;
        if (!available)
            return std::nullopt;
        return InspectorControlSession{
            .transport = std::make_unique<FakeTransport>(state_),
            .client_id = ControlClientId{"client-1"},
            .registration_id = ControlRegistrationId{"registration-1"},
            .grant_id = ControlGrantId{"grant-1"},
            .instance_generation = "generation-1",
            .target = {"session-1", "instance-1", "publication-1"},
            .expected_state_generation = 7,
        };
    }

    int calls = 0;
    bool available = true;

  private:
    std::shared_ptr<FakeState> state_;
};

const ControlRequestEnvelope& captured_request(const FakeState& state) {
    REQUIRE(state.requests.size() == 2);
    return std::get<ControlRequestEnvelope>(state.requests[1].payload);
}

ControlReceiptEnvelope failed(const ControlRequestEnvelope& request,
                              const ControlLegacyInspectorError& legacy) {
    const auto detail = encode_control_legacy_inspector_error(legacy);
    REQUIRE(detail.has_value());
    return {
        .request_id = request.request_id,
        .receipt_id = "receipt-failed",
        .operation_id = request.operation_id,
        .operation_version = request.operation_version,
        .state = ControlReceiptState::Failed,
        .result_code = ControlResultCode::LeaseConflict,
        .retry = ControlRetryClassification::AfterRefresh,
        .explanation = legacy.error_message,
        .detail_json = *detail,
    };
}

} // namespace

TEST_CASE("canonical Inspector client translates only trace session methods",
          "[inspect][control][client][trace]") {
    auto state = successful_state();
    FakeOpener opener(state);

    const auto started =
        request_control_inspector(opener, std::string(methods::kTraceStartSession),
                                  R"({"categories":["render","dsp"],"ring_mb":32})");
    REQUIRE(started.succeeded());
    CHECK_FALSE(started.publication.has_value());
    REQUIRE(started.target.has_value());
    CHECK(*started.target == InspectorClientTarget{"session-1", "instance-1", "publication-1"});
    CHECK(started.response.params_json == R"({"active": true, "compiled_in": true, "ok": true})");

    const auto& start = captured_request(*state);
    CHECK(start.client_id == "client-1");
    CHECK(start.registration_id == "registration-1");
    CHECK(start.grant_id == "grant-1");
    CHECK(start.instance_generation == "generation-1");
    CHECK(start.expected_state_generation == 7);
    CHECK(start.operation_id == "dev.pulp.trace/session-control@1");
    CHECK(start.operation_version == 1);
    CHECK(start.request_hash == control_request_hash(start));
    const auto start_params = choc::json::parse(start.params_json);
    CHECK(start_params["action"].getString() == "start");
    CHECK(start_params["ring_mb"].getInt64() == 32);

    state->requests.clear();
    const auto stopped = request_control_inspector(opener, std::string(methods::kTraceStopSession));
    REQUIRE(stopped.succeeded());
    CHECK(stopped.response.params_json ==
          R"({"ok": true, "out_path": "/tmp/trace.pftrace", "trace_bytes": 42})");
    CHECK(choc::json::parse(captured_request(*state).params_json)["action"].getString() == "stop");

    state->requests.clear();
    const auto unsupported = request_control_inspector(opener, "DOM.getDocument");
    CHECK_FALSE(unsupported.succeeded());
    CHECK(unsupported.response.error_code == "method_not_found");
    CHECK(opener.calls == 2);
    CHECK(state->requests.empty());
}

TEST_CASE("canonical Inspector client rejects invalid trace params before authority use",
          "[inspect][control][client][trace][negative]") {
    for (const auto& [method, params] : std::vector<std::pair<std::string, std::string>>{
             {std::string(methods::kTraceStartSession), "not-json"},
             {std::string(methods::kTraceStartSession), R"({"ring_mb":0})"},
             {std::string(methods::kTraceStartSession), R"({"action":"stop"})"},
             {std::string(methods::kTraceStartSession), R"({"out_path":"/tmp/x"})"},
             {std::string(methods::kTraceStopSession), R"({"extra":true})"},
         }) {
        auto state = successful_state();
        FakeOpener opener(state);
        const auto result = request_control_inspector(opener, method, params);
        CHECK_FALSE(result.succeeded());
        CHECK(result.response.error_code == "invalid_params");
        CHECK(result.response.error_data_json == R"({"mayHaveApplied":false})");
        CHECK(opener.calls == 0);
        CHECK(state->requests.empty());
    }
}

TEST_CASE("canonical Inspector client fails closed when no enrolled session opens",
          "[inspect][control][client][trace][opener]") {
    auto state = successful_state();
    FakeOpener opener(state);
    opener.available = false;

    const auto result = request_control_inspector(opener, std::string(methods::kTraceStartSession));

    CHECK_FALSE(result.succeeded());
    CHECK_FALSE(result.target.has_value());
    CHECK(result.response.error_code == "control_session_unavailable");
    CHECK(result.response.error_data_json == R"({"mayHaveApplied":false})");
    CHECK(opener.calls == 1);
    CHECK(state->requests.empty());
}

TEST_CASE("canonical Inspector client preserves failed trace compatibility tuples",
          "[inspect][control][client][trace][error-contract]") {
    const std::string opaque = R"({ "unknown" : [3,2,1], "nested":{"x":true} })";
    for (const auto& legacy : std::vector<ControlLegacyInspectorError>{
             {"trace_already_active", "a tracing session is already active", opaque},
             {"trace_owned_by_another_controller", "another controller owns tracing", "{}"},
         }) {
        auto state = successful_state();
        state->dispatch = [legacy](const ControlEnvelope& envelope) {
            if (std::holds_alternative<ControlNegotiationOffer>(envelope.payload))
                return accepted_negotiation();
            const auto& request = std::get<ControlRequestEnvelope>(envelope.payload);
            return encoded({.payload = failed(request, legacy)});
        };
        FakeOpener opener(state);
        const auto result =
            request_control_inspector(opener, std::string(methods::kTraceStartSession));
        REQUIRE(result.response.is_error);
        CHECK(result.response.error_code == legacy.error_code);
        CHECK(result.response.params_json == legacy.error_message);
        CHECK(result.response.error_data_json == legacy.error_data_json);
    }
}

TEST_CASE("canonical Inspector client fails closed on uncorrelated receipts",
          "[inspect][control][client][trace][correlation]") {
    enum class Mutation {
        Request,
        Operation,
        Version,
        Nonterminal,
        Artifact,
        ResultSchema,
        ErrorSchema
    };
    for (const auto mutation :
         {Mutation::Request, Mutation::Operation, Mutation::Version, Mutation::Nonterminal,
          Mutation::Artifact, Mutation::ResultSchema, Mutation::ErrorSchema}) {
        auto state = successful_state();
        state->dispatch = [mutation](const ControlEnvelope& envelope) {
            if (std::holds_alternative<ControlNegotiationOffer>(envelope.payload))
                return accepted_negotiation();
            const auto& request = std::get<ControlRequestEnvelope>(envelope.payload);
            auto receipt = completed(request);
            if (mutation == Mutation::Request)
                receipt.request_id = "different-request";
            if (mutation == Mutation::Operation)
                receipt.operation_id = "dev.pulp.instance/read@1";
            if (mutation == Mutation::Version)
                receipt.operation_version = 2;
            if (mutation == Mutation::Nonterminal)
                receipt.state = ControlReceiptState::Running;
            if (mutation == Mutation::Artifact)
                receipt.artifacts.push_back({"unexpected-artifact", "application/octet-stream", 1});
            if (mutation == Mutation::ResultSchema)
                receipt.detail_json = R"({"ok":true})";
            if (mutation == Mutation::ErrorSchema) {
                receipt.state = ControlReceiptState::Failed;
                receipt.result_code = ControlResultCode::LeaseConflict;
                receipt.detail_json = R"({"error_code":"trace_already_active"})";
            }
            return encoded({.payload = std::move(receipt)});
        };
        FakeOpener opener(state);
        const auto result =
            request_control_inspector(opener, std::string(methods::kTraceStartSession));
        CHECK_FALSE(result.succeeded());
        CHECK(result.response.error_code == "invalid_control_response");
        CHECK(result.response.error_data_json == R"({"mayHaveApplied":true})");
    }
}

TEST_CASE("canonical Inspector client preserves uncertain application state",
          "[inspect][control][client][trace][uncertain]") {
    SECTION("request transport failure may have applied") {
        auto state = successful_state();
        state->dispatch = [](const ControlEnvelope& envelope) {
            if (std::holds_alternative<ControlNegotiationOffer>(envelope.payload))
                return accepted_negotiation();
            return ControlTransportDispatchResult{
                .error_code = "connection_closed",
                .explanation = "peer disconnected before a receipt arrived",
            };
        };
        FakeOpener opener(state);
        const auto result =
            request_control_inspector(opener, std::string(methods::kTraceStartSession));
        CHECK(result.response.error_code == "connection_closed");
        CHECK(result.response.error_data_json == R"({"mayHaveApplied":true})");
    }

    SECTION("request timeout may have applied") {
        auto state = successful_state();
        state->dispatch = [](const ControlEnvelope& envelope) {
            if (std::holds_alternative<ControlNegotiationOffer>(envelope.payload))
                return accepted_negotiation();
            return ControlTransportDispatchResult{
                .error_code = "request_timeout",
                .explanation = "receipt deadline elapsed",
            };
        };
        FakeOpener opener(state);
        const auto result =
            request_control_inspector(opener, std::string(methods::kTraceStartSession));
        CHECK(result.response.error_code == "request_timeout");
        CHECK(result.response.error_data_json == R"({"mayHaveApplied":true})");
    }

    SECTION("unknown receipt may have applied") {
        auto state = successful_state();
        state->dispatch = [](const ControlEnvelope& envelope) {
            if (std::holds_alternative<ControlNegotiationOffer>(envelope.payload))
                return accepted_negotiation();
            const auto& request = std::get<ControlRequestEnvelope>(envelope.payload);
            auto receipt = completed(request);
            receipt.state = ControlReceiptState::UnknownNeedsRefresh;
            receipt.result_code = ControlResultCode::UnknownNeedsRefresh;
            receipt.retry = ControlRetryClassification::AfterRefresh;
            receipt.explanation = "refresh durable receipt state";
            return encoded({.payload = std::move(receipt)});
        };
        FakeOpener opener(state);
        const auto result =
            request_control_inspector(opener, std::string(methods::kTraceStartSession));
        CHECK(result.response.error_code == "unknown_needs_refresh");
        CHECK(result.response.error_data_json == R"({"mayHaveApplied":true})");
    }

    SECTION("negotiation failure cannot have applied") {
        auto state = successful_state();
        state->dispatch = [](const ControlEnvelope&) {
            return ControlTransportDispatchResult{
                .error_code = "connection_closed",
                .explanation = "negotiation disconnected",
            };
        };
        FakeOpener opener(state);
        const auto result =
            request_control_inspector(opener, std::string(methods::kTraceStartSession));
        CHECK(result.response.error_code == "connection_closed");
        CHECK(result.response.error_data_json == R"({"mayHaveApplied":false})");
        CHECK(state->requests.size() == 1);
    }

    SECTION("typed pre-execution failure cannot have applied") {
        auto state = successful_state();
        state->dispatch = [](const ControlEnvelope& envelope) {
            if (std::holds_alternative<ControlNegotiationOffer>(envelope.payload))
                return accepted_negotiation();
            const auto& request = std::get<ControlRequestEnvelope>(envelope.payload);
            auto receipt = completed(request);
            receipt.state = ControlReceiptState::Failed;
            receipt.result_code = ControlResultCode::DeadlineExceeded;
            receipt.explanation = "operation deadline elapsed before execution";
            receipt.detail_json = "{}";
            return encoded({.payload = std::move(receipt)});
        };
        FakeOpener opener(state);
        const auto result =
            request_control_inspector(opener, std::string(methods::kTraceStartSession));
        CHECK(result.response.error_code == "deadline_exceeded");
        CHECK(result.response.params_json == "operation deadline elapsed before execution");
        CHECK(result.response.error_data_json == R"({"mayHaveApplied":false})");
    }
}
