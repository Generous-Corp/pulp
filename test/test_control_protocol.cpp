#include <pulp/inspect/control_manifest.hpp>
#include <pulp/inspect/control_protocol.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace pulp::inspect;

namespace {

ControlRequestEnvelope request_fixture() {
    ControlRequestEnvelope request{
        .request_id = "request-1",
        .client_id = "client-1",
        .registration_id = "registration-1",
        .grant_id = "grant-1",
        .instance_generation = "generation-7",
        .operation_id = "dev.pulp.state/write@1",
        .operation_version = 1,
        .idempotency_key = "idem-1",
        .deadline_unix_ms = 1786000000000,
        .expected_state_generation = 42,
        .params_json = R"({"z":2,"a":{"y":true,"x":1}})",
    };
    request.request_hash = control_request_hash(request).value();
    return request;
}

ControlArtifactWireMetadata artifact_metadata_fixture() {
    return {
        .artifact_id = "artifact-0123456789abcdef0123456789abcdef",
        .broker_id = "broker-1",
        .receipt_id = "receipt-1",
        .producer_client_id = "client-1",
        .producer_registration_id = "registration-1",
        .session_id = "session-1",
        .instance_id = "instance-1",
        .publication_id = "publication-1",
        .producer_capability_id = "dev.pulp.ui.observe",
        .producer_operation_id = "dev.pulp.ui/observe@1",
        .producer_operation_version = 1,
        .original_grant_id = "grant-1",
        .consent_decision_id = "consent-1",
        .manifest_digest = std::string(64, 'a'),
        .producer_artifact_digest = std::string(64, 'b'),
        .sha256 = std::string(64, 'c'),
        .byte_size = 3,
        .content_type = "application/json",
        .created_at_unix_ms = 1'786'000'000'000,
        .expires_at_unix_ms = 1'786'000'060'000,
        .sensitivity_id = "sensitive",
        .deletion_state_id = "active",
        .redaction_state_id = "original",
    };
}

template <typename Payload> Payload round_trip(Payload payload) {
    const auto encoded = encode_control_envelope(ControlEnvelope{
        .payload = payload,
    });
    INFO(encoded);
    REQUIRE_FALSE(encoded.empty());
    ControlProtocolDiagnostics diagnostics;
    const auto decoded = decode_control_envelope(encoded, &diagnostics);
    INFO(diagnostics.explanation);
    REQUIRE(decoded.has_value());
    return std::get<Payload>(decoded->payload);
}

std::string replace_once(std::string source, std::string_view from, std::string_view to) {
    const auto position = source.find(from);
    REQUIRE(position != std::string::npos);
    source.replace(position, from.size(), to);
    return source;
}

const ControlOperationDescriptor& operation(std::string_view id) {
    const auto* descriptor = resolve_control_operation(id, 1);
    REQUIRE(descriptor != nullptr);
    return *descriptor;
}

} // namespace

TEST_CASE("control protocol negotiates the highest safe common revision",
          "[inspect][control-protocol]") {
    const ControlNegotiationOffer local{{1, 4}, {"receipts"}, {"progress", "artifacts"}};
    const ControlNegotiationOffer peer{{2, 3}, {"artifacts"}, {"receipts"}};
    const auto result = negotiate_control_protocol(local, peer, 2);
    REQUIRE(result.status == ControlNegotiationStatus::Accepted);
    REQUIRE(result.selected_version == 3);
    REQUIRE(result.features == std::vector<std::string>{"artifacts", "receipts"});

    CHECK(negotiate_control_protocol(local, ControlNegotiationOffer{{5, 6}, {}, {}}, 1).status ==
          ControlNegotiationStatus::NoCommonVersion);
    CHECK(negotiate_control_protocol(local, ControlNegotiationOffer{{1, 1}, {}, {"receipts"}}, 2)
              .status == ControlNegotiationStatus::DowngradeRejected);
    CHECK(negotiate_control_protocol(local, ControlNegotiationOffer{{3, 2}, {}, {}}, 1).status ==
          ControlNegotiationStatus::InvalidOffer);
    CHECK(negotiate_control_protocol(local,
                                     ControlNegotiationOffer{{1, 4}, {"unknown-required"}, {}}, 1)
              .status == ControlNegotiationStatus::UnsupportedMandatoryFeature);
}

TEST_CASE("all closed control envelope variants round trip canonically",
          "[inspect][control-protocol]") {
    const ControlNegotiationOffer offer{{1, 2}, {"receipts"}, {"progress"}};
    CHECK(round_trip(offer) == offer);

    const ControlNegotiationResult negotiated{
        ControlNegotiationStatus::Accepted, 1, {"receipts"}, {}};
    CHECK(round_trip(negotiated) == negotiated);

    auto request = request_fixture();
    auto decoded_request = round_trip(request);
    request.params_json = canonicalize_control_json(request.params_json).value();
    CHECK(decoded_request == request);
    CHECK(decoded_request.expected_state_generation == 42);
    CHECK(control_request_hash(decoded_request) == request.request_hash);

    const ControlCancelEnvelope cancel{"request-1", "operator requested cancellation"};
    CHECK(round_trip(cancel) == cancel);

    const ControlCancelEnvelope maximum_cancel{
        "request-1", std::string(kControlReceiptMaximumCancellationReasonBytes, 'r')};
    CHECK(round_trip(maximum_cancel) == maximum_cancel);

    const ControlCancelEnvelope empty_cancel{"request-1", ""};
    CHECK(encode_control_envelope({kControlProtocolVersion, empty_cancel}).empty());
    const auto empty_cancel_json =
        replace_once(encode_control_envelope({kControlProtocolVersion, cancel}),
                     "operator requested cancellation", "");
    ControlProtocolDiagnostics empty_cancel_diagnostics;
    CHECK_FALSE(decode_control_envelope(empty_cancel_json, &empty_cancel_diagnostics));
    CHECK(empty_cancel_diagnostics.code == ControlProtocolError::InvalidValue);

    auto progress = ControlProgressEnvelope{
        .request_id = "request-1",
        .receipt_id = "receipt-1",
        .sequence = 3,
        .current = 40,
        .total = 100,
        .detail_json = R"({"stage":"render","pass":2})",
    };
    progress.detail_json = canonicalize_control_json(progress.detail_json).value();
    CHECK(round_trip(progress) == progress);

    const ControlSessionOpenEnvelope session_open{"session-request-1", "admission-1"};
    CHECK(round_trip(session_open) == session_open);

    const ControlSessionOpenResult session_opened{
        .request_id = "session-request-1",
        .accepted = true,
        .client_id = "client-1",
    };
    CHECK(round_trip(session_opened) == session_opened);

    const ControlSessionOpenResult session_rejected{
        .request_id = "session-request-2",
        .error_code = "admission-rejected",
        .explanation = "admission was not accepted",
    };
    CHECK(round_trip(session_rejected) == session_rejected);

    const ControlArtifactReadEnvelope artifact_read{
        .request_id = "artifact-request-1",
        .artifact_id = "artifact-0123456789abcdef0123456789abcdef",
        .offset = 64,
        .maximum_bytes = kControlMaximumArtifactReadBytes,
    };
    CHECK(round_trip(artifact_read) == artifact_read);

    const ControlArtifactReadResponseEnvelope artifact_response{
        .request_id = "artifact-request-1",
        .status_id = "read",
        .metadata = artifact_metadata_fixture(),
        .bytes_base64 = "AQID",
        .eof = true,
    };
    CHECK(round_trip(artifact_response) == artifact_response);

    const ControlArtifactReadResponseEnvelope missing_artifact{
        .request_id = "artifact-request-2",
        .status_id = "not-found",
        .explanation = "artifact does not exist",
    };
    CHECK(round_trip(missing_artifact) == missing_artifact);

    const ControlHealthEnvelope health{"health-request-1"};
    CHECK(round_trip(health) == health);

    const ControlHealthResult health_result{
        .request_id = "health-request-1",
        .sdk_version = "0.748.0",
        .protocol_versions = {1, 1},
        .broker_id = "broker-1",
        .process_generation = 47,
    };
    CHECK(round_trip(health_result) == health_result);

    const ControlErrorEnvelope protocol_error{
        .request_id = "artifact-request-1",
        .error_code = "request-not-supported",
        .explanation = "the request is not supported by this protocol version",
    };
    CHECK(round_trip(protocol_error) == protocol_error);

    const ControlReceiptEnvelope admitted{
        .request_id = "request-1",
        .receipt_id = "receipt-1",
        .operation_id = "dev.pulp.state/write@1",
        .operation_version = 1,
        .state = ControlReceiptState::Admitted,
        .retry = ControlRetryClassification::Never,
        .detail_json = "{}",
    };
    CHECK(round_trip(admitted) == admitted);

    auto completed = admitted;
    completed.state = ControlReceiptState::Completed;
    CHECK(round_trip(completed) == completed);

    auto terminal = ControlReceiptEnvelope{
        .request_id = "request-1",
        .receipt_id = "receipt-1",
        .operation_id = "dev.pulp.state/write@1",
        .operation_version = 1,
        .state = ControlReceiptState::UnknownNeedsRefresh,
        .result_code = ControlResultCode::UnknownNeedsRefresh,
        .retry = ControlRetryClassification::AfterRefresh,
        .explanation = "executor completion could not be confirmed",
        .detail_json = R"({"generation":43})",
        .artifacts = {{"artifact-1", "application/json", 29}},
    };
    terminal.detail_json = canonicalize_control_json(terminal.detail_json).value();
    CHECK(round_trip(terminal) == terminal);

    auto maximum_fields = terminal;
    maximum_fields.explanation = std::string(kControlReceiptMaximumExplanationBytes, 'x');
    maximum_fields.artifacts = {
        {std::string(kControlReceiptMaximumArtifactIdBytes, 'a'),
         std::string(kControlReceiptMaximumArtifactMediaTypeBytes, 'm'), 1},
    };
    CHECK(round_trip(maximum_fields) == maximum_fields);

    auto explanation_over = maximum_fields;
    explanation_over.explanation.push_back('x');
    CHECK(encode_control_envelope({kControlProtocolVersion, std::move(explanation_over)}).empty());
    auto media_type_over = maximum_fields;
    media_type_over.artifacts.front().media_type.push_back('m');
    CHECK(encode_control_envelope({kControlProtocolVersion, std::move(media_type_over)}).empty());
    auto explanation_with_nul = maximum_fields;
    explanation_with_nul.explanation = std::string{"x\0y", 3};
    CHECK(encode_control_envelope({kControlProtocolVersion, std::move(explanation_with_nul)})
              .empty());

    ControlNegotiationResult negotiation_with_nul{
        .status = ControlNegotiationStatus::InvalidOffer,
        .explanation = std::string{"x\0y", 3},
    };
    CHECK(encode_control_envelope({kControlProtocolVersion, std::move(negotiation_with_nul)})
              .empty());
    ControlCancelEnvelope cancellation_with_nul{
        .request_id = "request-1",
        .reason = std::string{"x\0y", 3},
    };
    CHECK(encode_control_envelope({kControlProtocolVersion, std::move(cancellation_with_nul)})
              .empty());
}

TEST_CASE("carrier management envelopes enforce bounded fail-closed fields",
          "[inspect][control-protocol][carrier]") {
    auto accepted_with_error = ControlSessionOpenResult{
        .request_id = "session-request-1",
        .accepted = true,
        .client_id = "client-1",
        .error_code = "unexpected",
    };
    CHECK(encode_control_envelope({.payload = accepted_with_error}).empty());

    auto rejected_without_error = ControlSessionOpenResult{
        .request_id = "session-request-1",
        .accepted = false,
    };
    CHECK(encode_control_envelope({.payload = rejected_without_error}).empty());

    auto oversized_read = ControlArtifactReadEnvelope{
        .request_id = "artifact-request-1",
        .artifact_id = "artifact-0123456789abcdef0123456789abcdef",
        .maximum_bytes = kControlMaximumArtifactReadBytes + 1,
    };
    CHECK(encode_control_envelope({.payload = oversized_read}).empty());
    oversized_read.maximum_bytes = 0;
    CHECK(encode_control_envelope({.payload = oversized_read}).empty());

    auto malformed_chunk = ControlArtifactReadResponseEnvelope{
        .request_id = "artifact-request-1",
        .status_id = "read",
        .metadata = artifact_metadata_fixture(),
        .bytes_base64 = "not base64",
    };
    CHECK(encode_control_envelope({.payload = malformed_chunk}).empty());
    malformed_chunk.bytes_base64 = "AQID";
    malformed_chunk.metadata->manifest_digest = std::string(64, 'z');
    CHECK(encode_control_envelope({.payload = malformed_chunk}).empty());

    auto invalid_health = ControlHealthResult{
        .request_id = "health-request-1",
        .sdk_version = "0.748.0",
        .protocol_versions = {2, 1},
        .broker_id = "broker-1",
        .process_generation = 1,
    };
    CHECK(encode_control_envelope({.payload = invalid_health}).empty());
    invalid_health.protocol_versions = {1, 1};
    invalid_health.process_generation = 0;
    CHECK(encode_control_envelope({.payload = invalid_health}).empty());

    const ControlErrorEnvelope empty_error{
        .request_id = "request-1",
        .error_code = "invalid-request",
    };
    CHECK(encode_control_envelope({.payload = empty_error}).empty());

    const auto valid_session = encode_control_envelope(
        {.payload = ControlSessionOpenResult{
             .request_id = "session-request-1", .accepted = true, .client_id = "client-1"}});
    REQUIRE_FALSE(valid_session.empty());
    ControlProtocolDiagnostics diagnostics;
    const auto wrong_accepted =
        replace_once(valid_session, R"("accepted": true)", R"("accepted": "true")");
    CHECK_FALSE(decode_control_envelope(wrong_accepted, &diagnostics));
    CHECK(diagnostics.code == ControlProtocolError::InvalidType);

    const auto valid_response =
        encode_control_envelope({.payload = ControlArtifactReadResponseEnvelope{
                                     .request_id = "artifact-request-1",
                                     .status_id = "read",
                                     .metadata = artifact_metadata_fixture(),
                                     .bytes_base64 = "AQID",
                                     .eof = true,
                                 }});
    REQUIRE_FALSE(valid_response.empty());
    const auto unknown_metadata =
        replace_once(valid_response, R"("artifact_id": )", R"("future": 1, "artifact_id": )");
    CHECK_FALSE(decode_control_envelope(unknown_metadata, &diagnostics));
    CHECK(diagnostics.code == ControlProtocolError::UnknownField);
}

TEST_CASE("control request hash binds authority operation and canonical content",
          "[inspect][control-protocol]") {
    const auto request = request_fixture();
    auto reordered = request;
    reordered.params_json = R"({"a":{"x":1,"y":true},"z":2})";
    CHECK(control_request_hash(reordered) == request.request_hash);

    auto changed = request;
    changed.params_json = R"({"a":{"x":9,"y":true},"z":2})";
    CHECK(control_request_hash(changed) != request.request_hash);
    changed = request;
    changed.grant_id = "grant-2";
    CHECK(control_request_hash(changed) != request.request_hash);
    changed = request;
    changed.operation_version = 2;
    CHECK(control_request_hash(changed) != request.request_hash);
    changed = request;
    ++changed.expected_state_generation;
    CHECK(control_request_hash(changed) != request.request_hash);

    CHECK(canonicalize_control_json(R"({"b":2,"a":[{"d":4,"c":3}]})") ==
          R"({"a": [{"c": 3, "d": 4}], "b": 2})");
    CHECK_FALSE(canonicalize_control_json("{").has_value());
    std::string too_deep(34, '[');
    too_deep += "0";
    too_deep.append(34, ']');
    CHECK_FALSE(canonicalize_control_json(too_deep).has_value());
}

TEST_CASE("control JSON Schema validator accepts representative registry contracts",
          "[inspect][control-protocol][json-schema]") {
    ControlJsonSchemaDiagnostics diagnostics;

    for (const auto& descriptor : control_operation_registry()) {
        CAPTURE(descriptor.id, descriptor.input_schema_id);
        const auto input_result =
            validate_control_json_schema("{}", descriptor.input_schema_json, &diagnostics);
        CHECK((input_result || diagnostics.code == ControlJsonSchemaError::ValidationFailed));
        CAPTURE(descriptor.output_schema_id);
        const auto output_result =
            validate_control_json_schema("{}", descriptor.output_schema_json, &diagnostics);
        CHECK((output_result || diagnostics.code == ControlJsonSchemaError::ValidationFailed));
    }

    const auto& state = operation("dev.pulp.state/read@1");
    CHECK(validate_control_json_schema(R"({"include_sensitive":false,"parameter_ids":[1,2]})",
                                       state.input_schema_json, &diagnostics));
    CHECK(validate_control_json_schema(
        R"({"generation":1,"parameters":[{"id":1,"normalized":0.5,"sensitive":false}]})",
        state.output_schema_json, &diagnostics));

    const auto& input = operation("dev.pulp.ui/input@1");
    CHECK(validate_control_json_schema(
        R"({"kind":"pointer","target_id":"gain","event":{"phase":"down","x":12.5,"y":3,"button":0}})",
        input.input_schema_json, &diagnostics));
    CHECK_FALSE(validate_control_json_schema(
        R"({"kind":"pointer","target_id":"gain","event":{"phase":"down","x":12.5}})",
        input.input_schema_json, &diagnostics));

    const auto& state_write = operation("dev.pulp.state/parameter-gesture@1");
    CHECK(validate_control_json_schema(
        R"({"parameter_id":15.0,"normalized_value":0.5,"idempotency_key":"write-1"})",
        state_write.input_schema_json, &diagnostics));
    CHECK_FALSE(validate_control_json_schema(
        R"({"parameter_id":15.5,"normalized_value":0.5,"idempotency_key":"write-1"})",
        state_write.input_schema_json, &diagnostics));

    const auto& trace = operation("dev.pulp.trace/control@1");
    CHECK(validate_control_json_schema(
        R"({"action":"motion-start-trace","metrics":[{"kind":"geometry","node_id":"meter","properties":["width","height"]}]})",
        trace.input_schema_json, &diagnostics));
    CHECK_FALSE(validate_control_json_schema(
        R"({"action":"motion-start-trace","metrics":[{"kind":"geometry","node_id":"meter","properties":["width","width"]}]})",
        trace.input_schema_json, &diagnostics));

    const auto& tweaks = operation("dev.pulp.authoring/tweaks@1");
    CHECK(validate_control_json_schema(
        R"({"anchor_id":"meter","changes":{"lock":true,"constants":{"gain_1":0.25}},"idempotency_key":"tweak-1"})",
        tweaks.input_schema_json, &diagnostics));
    CHECK_FALSE(
        validate_control_json_schema(R"({"changes":{"lock":true},"idempotency_key":"tweak-1"})",
                                     tweaks.input_schema_json, &diagnostics));
    CHECK_FALSE(validate_control_json_schema(
        R"({"changes":{"constants":{"1bad":0.25}},"idempotency_key":"tweak-1"})",
        tweaks.input_schema_json, &diagnostics));

    const auto& logs = operation("dev.pulp.logs/read@1");
    CHECK(validate_control_json_schema(R"({"after_sequence":9007199254740991})",
                                       logs.input_schema_json, &diagnostics));
    CHECK_FALSE(validate_control_json_schema(R"({"after_sequence":9007199254740992})",
                                             logs.input_schema_json, &diagnostics));

    const auto& evaluation = operation("dev.pulp.runtime/evaluate@1");
    CHECK(validate_control_json_schema(
        R"({"source":"return 42;","timeout_ms":100,"idempotency_key":"eval-1"})",
        evaluation.input_schema_json, &diagnostics));
}

TEST_CASE("control JSON Schema compares integral bounds without floating precision loss",
          "[inspect][control-protocol][json-schema][integer]") {
    ControlJsonSchemaDiagnostics diagnostics;
    constexpr std::string_view safe_maximum = R"({"type":"integer","maximum":9007199254740991})";
    CHECK(validate_control_json_schema("9007199254740991", safe_maximum, &diagnostics));
    CHECK_FALSE(validate_control_json_schema("9007199254740992", safe_maximum, &diagnostics));

    constexpr std::string_view collision_maximum =
        R"({"type":"integer","maximum":9007199254740992})";
    CHECK(validate_control_json_schema("9007199254740992", collision_maximum, &diagnostics));
    CHECK_FALSE(validate_control_json_schema("9007199254740993", collision_maximum, &diagnostics));

    constexpr std::string_view collision_minimum =
        R"({"type":"integer","minimum":-9007199254740992})";
    CHECK(validate_control_json_schema("-9007199254740992", collision_minimum, &diagnostics));
    CHECK_FALSE(validate_control_json_schema("-9007199254740993", collision_minimum, &diagnostics));

    constexpr std::string_view int64_maximum =
        R"({"type":"integer","maximum":9223372036854775806})";
    CHECK(validate_control_json_schema("9223372036854775806", int64_maximum, &diagnostics));
    CHECK_FALSE(validate_control_json_schema("9223372036854775807", int64_maximum, &diagnostics));

    constexpr std::string_view int64_minimum =
        R"({"type":"integer","minimum":-9223372036854775806})";
    CHECK(validate_control_json_schema("-9223372036854775806", int64_minimum, &diagnostics));
    CHECK_FALSE(validate_control_json_schema("-9223372036854775807", int64_minimum, &diagnostics));

    constexpr std::string_view mixed_maximum = R"({"type":"integer","maximum":10.5})";
    CHECK(validate_control_json_schema("10", mixed_maximum, &diagnostics));
    CHECK_FALSE(validate_control_json_schema("11", mixed_maximum, &diagnostics));

    constexpr std::string_view mixed_minimum = R"({"type":"integer","minimum":-10.5})";
    CHECK(validate_control_json_schema("-10", mixed_minimum, &diagnostics));
    CHECK_FALSE(validate_control_json_schema("-11", mixed_minimum, &diagnostics));

    CHECK(validate_control_json_schema("10.0", R"({"type":"number","maximum":10})", &diagnostics));
    CHECK_FALSE(
        validate_control_json_schema("10.5", R"({"type":"number","maximum":10})", &diagnostics));
    CHECK(
        validate_control_json_schema("-10.0", R"({"type":"number","minimum":-10})", &diagnostics));
    CHECK_FALSE(
        validate_control_json_schema("-10.5", R"({"type":"number","minimum":-10})", &diagnostics));
}

TEST_CASE("registry request and result maxima fit their bounded protocol budgets",
          "[inspect][control-protocol][json-schema]") {
    ControlJsonSchemaDiagnostics diagnostics;
    const auto& evaluation = operation("dev.pulp.runtime/evaluate@1");

    std::string request = R"({"idempotency_key":"eval-max","source":")";
    for (std::size_t index = 0; index < 65536; ++index)
        request += R"(\u0001)";
    request += R"(","timeout_ms":2000})";
    REQUIRE(request.size() < kControlMaximumRequestPayloadBytes);
    CHECK(validate_control_json_schema(request, evaluation.input_schema_json, &diagnostics));
    auto request_envelope = request_fixture();
    request_envelope.operation_id = std::string(evaluation.id);
    request_envelope.idempotency_key = "eval-max";
    request_envelope.params_json = request;
    request_envelope.request_hash = control_request_hash(request_envelope).value();
    const auto decoded_request = round_trip(request_envelope);
    CHECK(validate_control_json_schema(decoded_request.params_json, evaluation.input_schema_json,
                                       &diagnostics));

    std::string result = R"({"completed":true,"receipt_id":"receipt-max","result_json":")";
    for (std::size_t index = 0; index < 262144; ++index)
        result += R"(\u0001)";
    result += R"("})";
    REQUIRE(result.size() < kControlMaximumResultDetailBytes);
    CHECK(validate_control_output_json_schema(result, evaluation.output_schema_json, &diagnostics));

    const ControlReceiptEnvelope receipt{
        .request_id = "request-max",
        .receipt_id = "receipt-max",
        .operation_id = "dev.pulp.runtime/evaluate@1",
        .operation_version = 1,
        .state = ControlReceiptState::Completed,
        .detail_json = result,
    };
    const auto decoded_receipt = round_trip(receipt);
    CHECK(validate_control_output_json_schema(decoded_receipt.detail_json,
                                              evaluation.output_schema_json, &diagnostics));

    const auto& artifact_read = operation("dev.pulp.artifact/read@1");
    std::string artifact_result =
        R"({"artifact_id":"artifact-max","chunk_base64":")" + std::string(1398104, 'A') +
        R"(","eof":false,"sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"})";
    CHECK(validate_control_output_json_schema(artifact_result, artifact_read.output_schema_json,
                                              &diagnostics));
    auto artifact_receipt = receipt;
    artifact_receipt.operation_id = std::string(artifact_read.id);
    artifact_receipt.detail_json = std::move(artifact_result);
    const auto decoded_artifact_receipt = round_trip(std::move(artifact_receipt));
    CHECK(validate_control_output_json_schema(decoded_artifact_receipt.detail_json,
                                              artifact_read.output_schema_json, &diagnostics));

    const auto& state = operation("dev.pulp.state/read@1");
    std::string state_request = R"({"parameter_ids":[)";
    std::string state_result = R"({"generation":1,"parameters":[)";
    for (std::size_t index = 0; index < 4096; ++index) {
        if (index != 0) {
            state_request += ',';
            state_result += ',';
        }
        state_request += std::to_string(index);
        state_result += R"({"id":)" + std::to_string(index) +
                        R"(,"normalized":0.5,"sensitive":false})";
    }
    state_request += "]}";
    state_result += "]}";
    CHECK(validate_control_json_schema(state_request, state.input_schema_json, &diagnostics));
    CHECK(
        validate_control_output_json_schema(state_result, state.output_schema_json, &diagnostics));

    const std::string oversized_result(kControlMaximumResultDetailBytes + 1, 'x');
    CHECK_FALSE(validate_control_output_json_schema('"' + oversized_result + '"',
                                                    R"({"type":"string"})", &diagnostics));
    CHECK(diagnostics.code == ControlJsonSchemaError::InvalidDocument);
}

TEST_CASE("bulk registry outputs use artifact contracts",
          "[inspect][control-protocol][json-schema]") {
    for (const auto id :
         {"dev.pulp.ui/observe@1", "dev.pulp.diagnostics/read@1", "dev.pulp.logs/read@1"}) {
        const auto& descriptor = operation(id);
        CHECK(descriptor.result_kind == "artifact");
        CHECK(descriptor.artifact_binding.produced);
        CHECK(descriptor.output_schema_json.find("artifact_id") != std::string_view::npos);
        CHECK(descriptor.output_schema_json.find("sha256") != std::string_view::npos);
    }
    const auto& artifact_read = operation("dev.pulp.artifact/read@1");
    CHECK(artifact_read.result_kind == "artifact-chunk");
    CHECK_FALSE(artifact_read.artifact_binding.produced);
}

TEST_CASE("control JSON Schema validator rejects hostile documents and schemas",
          "[inspect][control-protocol][json-schema]") {
    ControlJsonSchemaDiagnostics diagnostics;
    constexpr std::string_view object_schema =
        R"({"type":"object","additionalProperties":false,"properties":{"value":{"type":"integer"}},"required":["value"]})";

    CHECK_FALSE(validate_control_json_schema("{", object_schema, &diagnostics));
    CHECK(diagnostics.code == ControlJsonSchemaError::InvalidDocument);

    CHECK_FALSE(validate_control_json_schema("{}", "{", &diagnostics));
    CHECK(diagnostics.code == ControlJsonSchemaError::InvalidSchema);
    CHECK_FALSE(validate_control_json_schema(
        "{}", R"({"type":"object","unevaluatedProperties":false})", &diagnostics));
    CHECK(diagnostics.code == ControlJsonSchemaError::UnsupportedKeyword);
    CHECK(diagnostics.path == "/unevaluatedProperties");
    CHECK_FALSE(validate_control_json_schema(
        "{}", R"({"type":"object","properties":{"ignored":{"futureKeyword":true}}})",
        &diagnostics));
    CHECK(diagnostics.code == ControlJsonSchemaError::UnsupportedKeyword);

    CHECK_FALSE(validate_control_json_schema("{}", R"({"type":["object"],"required":"value"})",
                                             &diagnostics));
    CHECK(diagnostics.code == ControlJsonSchemaError::InvalidSchema);
    CHECK_FALSE(
        validate_control_json_schema("{}", R"({"type":"object","type":"array"})", &diagnostics));
    CHECK(diagnostics.code == ControlJsonSchemaError::InvalidSchema);
    CHECK_FALSE(validate_control_json_schema("\"text\"", R"({"type":"string","pattern":"["})",
                                             &diagnostics));
    CHECK(diagnostics.code == ControlJsonSchemaError::InvalidSchema);

    const std::string oversized_regex_input(4097, 'a');
    CHECK_FALSE(validate_control_json_schema(
        '"' + oversized_regex_input + '"', R"({"type":"string","pattern":"^a+$"})", &diagnostics));
    CHECK(diagnostics.code == ControlJsonSchemaError::LimitExceeded);

    CHECK_FALSE(validate_control_json_schema(
        "1", R"({"oneOf":[{"type":"number"},{"type":"number"}]})", &diagnostics));
    CHECK(diagnostics.code == ControlJsonSchemaError::ValidationFailed);
    CHECK_FALSE(
        validate_control_json_schema(R"({"value":1,"extra":2})", object_schema, &diagnostics));
    CHECK(diagnostics.code == ControlJsonSchemaError::ValidationFailed);
    CHECK_FALSE(
        validate_control_json_schema(R"({"value":1,"value":2})", object_schema, &diagnostics));
    CHECK(diagnostics.code == ControlJsonSchemaError::InvalidDocument);

    std::string malformed_utf8{"\"", 1};
    malformed_utf8.push_back(static_cast<char>(0xc2));
    malformed_utf8 += "x\"";
    CHECK_FALSE(validate_control_json_schema(malformed_utf8, R"({"type":"string"})", &diagnostics));
    CHECK(diagnostics.code == ControlJsonSchemaError::InvalidDocument);

    std::string too_deep(34, '[');
    too_deep += "0";
    too_deep.append(34, ']');
    CHECK_FALSE(validate_control_json_schema(too_deep, R"({"type":"array"})", &diagnostics));
    CHECK(diagnostics.code == ControlJsonSchemaError::InvalidDocument);
}

TEST_CASE("control codec rejects unknown missing malformed and mistyped fields",
          "[inspect][control-protocol]") {
    const auto valid = encode_control_envelope(ControlEnvelope{.payload = request_fixture()});
    REQUIRE_FALSE(valid.empty());
    ControlProtocolDiagnostics diagnostics;

    auto unknown = replace_once(valid, R"("kind": )", R"("extra": 1, "kind": )");
    CHECK_FALSE(decode_control_envelope(unknown, &diagnostics));
    CHECK(diagnostics.code == ControlProtocolError::UnknownField);

    auto unknown_payload = replace_once(valid, R"("client_id": )", R"("extra": 1, "client_id": )");
    CHECK_FALSE(decode_control_envelope(unknown_payload, &diagnostics));
    CHECK(diagnostics.code == ControlProtocolError::UnknownField);

    auto missing = replace_once(valid, R"(, "request_id": "request-1")", "");
    CHECK_FALSE(decode_control_envelope(missing, &diagnostics));
    CHECK(diagnostics.code == ControlProtocolError::MissingField);

    auto wrong_type =
        replace_once(valid, R"("operation_version": 1)", R"("operation_version": "1")");
    CHECK_FALSE(decode_control_envelope(wrong_type, &diagnostics));
    CHECK(diagnostics.code == ControlProtocolError::InvalidType);

    CHECK_FALSE(decode_control_envelope("[]", &diagnostics));
    CHECK(diagnostics.code == ControlProtocolError::RootType);
    CHECK_FALSE(decode_control_envelope("{", &diagnostics));
    CHECK(diagnostics.code == ControlProtocolError::Parse);
    CHECK_FALSE(decode_control_envelope("", &diagnostics));
    CHECK(diagnostics.code == ControlProtocolError::Parse);

    const std::string invalid_utf8{"{\"kind\":\"", 9};
    auto malformed_utf8 = invalid_utf8;
    malformed_utf8.push_back(static_cast<char>(0xc2));
    malformed_utf8.push_back('x');
    CHECK_FALSE(decode_control_envelope(malformed_utf8, &diagnostics));
    CHECK(diagnostics.code == ControlProtocolError::Parse);
    CHECK_FALSE(canonicalize_control_json(malformed_utf8));
}

TEST_CASE("control codec rejects unsupported versions hashes and bounded values",
          "[inspect][control-protocol]") {
    const auto valid = encode_control_envelope(ControlEnvelope{.payload = request_fixture()});
    ControlProtocolDiagnostics diagnostics;

    auto version = replace_once(valid, R"("schema_version": 1)", R"("schema_version": 2)");
    CHECK_FALSE(decode_control_envelope(version, &diagnostics));
    CHECK(diagnostics.code == ControlProtocolError::UnsupportedVersion);

    auto hash = replace_once(valid, request_fixture().request_hash, std::string(64, '0'));
    CHECK_FALSE(decode_control_envelope(hash, &diagnostics));
    CHECK(diagnostics.code == ControlProtocolError::HashMismatch);

    auto state_generation = replace_once(valid, R"("expected_state_generation": 42)",
                                         R"("expected_state_generation": 43)");
    CHECK_FALSE(decode_control_envelope(state_generation, &diagnostics));
    CHECK(diagnostics.code == ControlProtocolError::HashMismatch);

    CHECK_FALSE(
        decode_control_envelope(std::string(kControlMaximumEnvelopeBytes + 1, 'x'), &diagnostics));
    CHECK(diagnostics.code == ControlProtocolError::EnvelopeTooLarge);

    auto overlong = request_fixture();
    overlong.client_id = std::string(129, 'a');
    overlong.request_hash = std::string(64, '0');
    CHECK(encode_control_envelope(ControlEnvelope{.payload = overlong}).empty());

    ControlNegotiationOffer duplicate{{1, 1}, {"receipts", "receipts"}, {}};
    CHECK(encode_control_envelope(ControlEnvelope{.payload = duplicate}).empty());
}

TEST_CASE("receipt transitions distinguish admission running and terminal truth",
          "[inspect][control-protocol]") {
    CHECK(valid_control_receipt_transition(ControlReceiptState::Admitted,
                                           ControlReceiptState::Running));
    CHECK(valid_control_receipt_transition(ControlReceiptState::Admitted,
                                           ControlReceiptState::Cancelled));
    CHECK(valid_control_receipt_transition(ControlReceiptState::Running,
                                           ControlReceiptState::Completed));
    CHECK(valid_control_receipt_transition(ControlReceiptState::Running,
                                           ControlReceiptState::UnknownNeedsRefresh));
    CHECK_FALSE(valid_control_receipt_transition(ControlReceiptState::Running,
                                                 ControlReceiptState::Admitted));
    CHECK(valid_control_receipt_transition(ControlReceiptState::Running,
                                           ControlReceiptState::CompletedAfterRevocation));
    CHECK_FALSE(valid_control_receipt_transition(ControlReceiptState::Completed,
                                                 ControlReceiptState::Failed));
    CHECK_FALSE(valid_control_receipt_transition(ControlReceiptState::Admitted,
                                                 ControlReceiptState::Admitted));

    auto invalid = ControlReceiptEnvelope{
        .request_id = "request-1",
        .receipt_id = "receipt-1",
        .operation_id = "dev.pulp.state/write@1",
        .state = ControlReceiptState::Failed,
    };
    CHECK(encode_control_envelope(ControlEnvelope{.payload = invalid}).empty());
    invalid.result_code = ControlResultCode::InternalError;
    invalid.state = ControlReceiptState::Running;
    CHECK(encode_control_envelope(ControlEnvelope{.payload = invalid}).empty());
}

TEST_CASE("progress transitions reject stale regressing and retargeted updates",
          "[inspect][control-protocol][progress]") {
    const ControlProgressEnvelope first{
        .request_id = "request-1",
        .receipt_id = "receipt-1",
        .sequence = 1,
        .current = 10,
        .total = 100,
    };
    auto next = first;
    next.sequence = 2;
    next.current = 25;
    CHECK(valid_control_progress_transition(first, next));

    auto stale = next;
    stale.sequence = first.sequence;
    CHECK_FALSE(valid_control_progress_transition(first, stale));
    auto regressed = next;
    regressed.current = 9;
    CHECK_FALSE(valid_control_progress_transition(first, regressed));
    auto changed_total = next;
    changed_total.total = 200;
    CHECK_FALSE(valid_control_progress_transition(first, changed_total));
    auto retargeted = next;
    retargeted.receipt_id = "receipt-2";
    CHECK_FALSE(valid_control_progress_transition(first, retargeted));
}

TEST_CASE("progress codec strictly rejects invalid shape and bounds",
          "[inspect][control-protocol][progress]") {
    const ControlProgressEnvelope valid{
        .request_id = "request-1",
        .receipt_id = "receipt-1",
        .sequence = 1,
        .current = 5,
        .total = 10,
        .detail_json = R"({"stage":"encode"})",
    };
    const auto encoded = encode_control_envelope(ControlEnvelope{.payload = valid});
    REQUIRE_FALSE(encoded.empty());
    ControlProtocolDiagnostics diagnostics;

    auto unknown = replace_once(encoded, R"("current": )", R"("extra": 1, "current": )");
    CHECK_FALSE(decode_control_envelope(unknown, &diagnostics));
    CHECK(diagnostics.code == ControlProtocolError::UnknownField);

    auto missing = replace_once(encoded, R"(, "total": 10)", "");
    CHECK_FALSE(decode_control_envelope(missing, &diagnostics));
    CHECK(diagnostics.code == ControlProtocolError::MissingField);

    auto wrong_type = replace_once(encoded, R"("sequence": 1)", R"("sequence": "1")");
    CHECK_FALSE(decode_control_envelope(wrong_type, &diagnostics));
    CHECK(diagnostics.code == ControlProtocolError::InvalidType);

    auto invalid = valid;
    invalid.sequence = 0;
    CHECK(encode_control_envelope(ControlEnvelope{.payload = invalid}).empty());
    invalid = valid;
    invalid.current = 11;
    CHECK(encode_control_envelope(ControlEnvelope{.payload = invalid}).empty());
    invalid = valid;
    invalid.total = 0;
    CHECK(encode_control_envelope(ControlEnvelope{.payload = invalid}).empty());
    invalid = valid;
    invalid.detail_json = std::string(33u * 1024u, 'x');
    CHECK(encode_control_envelope(ControlEnvelope{.payload = invalid}).empty());
}

TEST_CASE("host control frames round trip and enforce role direction",
          "[inspect][control-protocol][host]") {
    const ControlEnvelope execute{.payload = ControlHostExecuteEnvelope{
                                      .route_id = "route-1",
                                      .receipt_id = "receipt-1",
                                      .operation_id = "session.describe",
                                      .operation_version = 1,
                                      .deadline_unix_ms = 1786000000000,
                                      .expected_state_generation = 9,
                                      .params_json = R"({"scope":"host"})",
                                  }};
    const auto encoded = encode_control_envelope(execute);
    REQUIRE_FALSE(encoded.empty());
    const auto decoded = decode_control_envelope(encoded);
    REQUIRE(decoded.has_value());
    const auto* decoded_execute = std::get_if<ControlHostExecuteEnvelope>(&decoded->payload);
    REQUIRE(decoded_execute != nullptr);
    CHECK(decoded_execute->route_id == "route-1");
    CHECK(decoded_execute->receipt_id == "receipt-1");
    CHECK(decoded_execute->operation_id == "session.describe");
    CHECK(decoded_execute->expected_state_generation == 9);
    CHECK(control_envelope_allowed(execute, ControlEnvelopeDirection::BrokerToHost));
    CHECK_FALSE(control_envelope_allowed(execute, ControlEnvelopeDirection::BrokerToClient));

    const ControlEnvelope completed{.payload = ControlHostCompleteEnvelope{
                                        .route_id = "route-1",
                                        .terminal_state = ControlReceiptState::Completed,
                                    }};
    REQUIRE(decode_control_envelope(encode_control_envelope(completed)) == completed);
    CHECK(control_envelope_allowed(completed, ControlEnvelopeDirection::HostToBroker));
    CHECK_FALSE(control_envelope_allowed(completed, ControlEnvelopeDirection::ClientToBroker));

    auto invalid = std::get<ControlHostCompleteEnvelope>(completed.payload);
    invalid.result_code = ControlResultCode::InternalError;
    CHECK(encode_control_envelope(ControlEnvelope{.payload = invalid}).empty());
}

TEST_CASE("host open canonically selects exactly one admission mechanism",
          "[inspect][control-protocol][host][enrollment]") {
    const ControlEnvelope admission{.payload =
                                        ControlHostOpenEnvelope{.request_id = "host-open-admission",
                                                                .admission_id = "admission-1"}};
    const auto encoded_admission = encode_control_envelope(admission);
    REQUIRE_FALSE(encoded_admission.empty());
    CHECK(encoded_admission.find(R"("enrollment_id": "")") != std::string::npos);
    CHECK(decode_control_envelope(encoded_admission) == admission);

    const ControlEnvelope enrollment{
        .payload = ControlHostOpenEnvelope{.request_id = "host-open-enrollment",
                                           .enrollment_id = "enrollment-1"}};
    const auto encoded_enrollment = encode_control_envelope(enrollment);
    CHECK(decode_control_envelope(encoded_enrollment) == enrollment);

    const auto enrollment_without_admission =
        replace_once(encoded_enrollment, R"("admission_id": "", )", "");
    CHECK(decode_control_envelope(enrollment_without_admission) == enrollment);

    auto both = std::get<ControlHostOpenEnvelope>(enrollment.payload);
    both.admission_id = "admission-1";
    CHECK(encode_control_envelope(ControlEnvelope{.payload = both}).empty());
    auto neither = both;
    neither.admission_id.clear();
    neither.enrollment_id.clear();
    CHECK(encode_control_envelope(ControlEnvelope{.payload = neither}).empty());

    // Older preissued-admission clients did not emit the new empty field.
    const auto legacy = replace_once(encoded_admission, R"(, "enrollment_id": "")", "");
    CHECK(decode_control_envelope(legacy) == admission);
}

TEST_CASE("host preflight frames use the canonical codec and strict directions",
          "[inspect][control-protocol][host][preflight]") {
    const auto nonce = std::string(64, 'a');
    const ControlEnvelope challenge{.payload = ControlHostPreflightChallengeEnvelope{nonce}};
    const ControlEnvelope response{.payload = ControlHostPreflightResponseEnvelope{nonce}};
    const ControlEnvelope bootstrap{
        .payload = ControlHostPreflightBootstrapEnvelope{nonce, "Ym9vdHN0cmFw"}};

    CHECK(round_trip(std::get<ControlHostPreflightChallengeEnvelope>(challenge.payload)).nonce ==
          nonce);
    CHECK(round_trip(std::get<ControlHostPreflightResponseEnvelope>(response.payload)).nonce ==
          nonce);
    CHECK(round_trip(std::get<ControlHostPreflightBootstrapEnvelope>(bootstrap.payload)) ==
          std::get<ControlHostPreflightBootstrapEnvelope>(bootstrap.payload));

    CHECK(control_envelope_allowed(challenge, ControlEnvelopeDirection::LauncherToHost));
    CHECK(control_envelope_allowed(bootstrap, ControlEnvelopeDirection::LauncherToHost));
    CHECK_FALSE(control_envelope_allowed(challenge, ControlEnvelopeDirection::HostToLauncher));
    CHECK(control_envelope_allowed(response, ControlEnvelopeDirection::HostToLauncher));
    CHECK_FALSE(control_envelope_allowed(response, ControlEnvelopeDirection::LauncherToHost));

    ControlProtocolDiagnostics diagnostics;
    auto encoded = encode_control_envelope(bootstrap);
    auto unknown = replace_once(encoded, R"("nonce":)", R"("extra":1,"nonce":)");
    CHECK_FALSE(decode_control_envelope(unknown, &diagnostics));
    CHECK(diagnostics.code == ControlProtocolError::UnknownField);

    auto bad_nonce = std::get<ControlHostPreflightChallengeEnvelope>(challenge.payload);
    bad_nonce.nonce.pop_back();
    CHECK(encode_control_envelope(ControlEnvelope{.payload = bad_nonce}).empty());
    auto empty_bootstrap = std::get<ControlHostPreflightBootstrapEnvelope>(bootstrap.payload);
    empty_bootstrap.bootstrap_base64.clear();
    CHECK(encode_control_envelope(ControlEnvelope{.payload = empty_bootstrap}).empty());
    auto oversized = std::get<ControlHostPreflightBootstrapEnvelope>(bootstrap.payload);
    oversized.bootstrap_base64.assign(kControlHostPreflightMaximumBootstrapBase64Bytes + 1, 'x');
    CHECK(encode_control_envelope(ControlEnvelope{.payload = oversized}).empty());
}

TEST_CASE("management frames are typed bounded and direction constrained",
          "[inspect][control-protocol][management]") {
    const ControlEnvelope request{
        .payload = ControlManagementEnvelope{
            .request_id = "management-1",
            .command = "grant-request",
            .params_json = R"({"instance_id":"instance-1","profile":"inspect-readonly"})"}};
    const auto encoded_request = encode_control_envelope(request);
    REQUIRE_FALSE(encoded_request.empty());
    const auto decoded_request = decode_control_envelope(encoded_request);
    REQUIRE(decoded_request.has_value());
    const auto* management = std::get_if<ControlManagementEnvelope>(&decoded_request->payload);
    REQUIRE(management != nullptr);
    CHECK(management->request_id == "management-1");
    CHECK(management->command == "grant-request");
    CHECK(canonicalize_control_json(management->params_json) ==
          canonicalize_control_json(
              std::get<ControlManagementEnvelope>(request.payload).params_json));
    CHECK(control_envelope_allowed(request, ControlEnvelopeDirection::ClientToBroker));
    CHECK_FALSE(control_envelope_allowed(request, ControlEnvelopeDirection::BrokerToClient));

    const ControlEnvelope response{
        .payload = ControlManagementResult{.request_id = "management-1",
                                           .status_id = "consent-required",
                                           .data_json = "{}",
                                           .explanation = "trusted consent is unavailable"}};
    const auto decoded_response = decode_control_envelope(encode_control_envelope(response));
    REQUIRE(decoded_response.has_value());
    const auto* management_result =
        std::get_if<ControlManagementResult>(&decoded_response->payload);
    REQUIRE(management_result != nullptr);
    CHECK(management_result->status_id == "consent-required");
    CHECK(control_envelope_allowed(response, ControlEnvelopeDirection::BrokerToClient));
    CHECK_FALSE(control_envelope_allowed(response, ControlEnvelopeDirection::ClientToBroker));

    auto invalid = std::get<ControlManagementEnvelope>(request.payload);
    invalid.command = "raw-connect";
    CHECK(encode_control_envelope(ControlEnvelope{.payload = invalid}).empty());
}
