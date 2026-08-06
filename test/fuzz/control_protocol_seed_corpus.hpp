#pragma once

#include <pulp/inspect/control_protocol.hpp>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pulp::test::control_protocol_fuzz {

struct SeedCorpusEntry {
    std::string_view filename;
    std::string bytes;
};

inline std::vector<SeedCorpusEntry> control_protocol_seed_corpus() {
    using namespace pulp::inspect;

    std::vector<SeedCorpusEntry> corpus;
    const auto add = [&](std::string_view filename, ControlEnvelope envelope) {
        corpus.push_back({filename, encode_control_envelope(envelope)});
    };

    add("negotiation-offer.json",
        {.payload = ControlNegotiationOffer{
             {1, 3}, {"receipts"}, {"artifacts", "cancellation", "progress"}}});
    add("negotiation-result.json",
        {.payload = ControlNegotiationResult{
             ControlNegotiationStatus::Accepted, 1, {"cancellation", "progress", "receipts"}, {}}});

    ControlRequestEnvelope request{
        .request_id = "request-fuzz",
        .client_id = "client-fuzz",
        .registration_id = "registration-fuzz",
        .grant_id = "grant-fuzz",
        .instance_generation = "generation-fuzz",
        .operation_id = "dev.pulp.state/read@1",
        .operation_version = 1,
        .idempotency_key = "idempotency-fuzz",
        .deadline_unix_ms = 4'102'444'800'000,
        .expected_state_generation = 7,
        .params_json = R"({"include_sensitive":false})",
    };
    request.request_hash = control_request_hash(request).value_or("");
    add("request.json", {.payload = std::move(request)});
    add("cancel.json",
        {.payload = ControlCancelEnvelope{"request-fuzz", "bounded fuzz cancellation"}});
    add("progress.json", {.payload = ControlProgressEnvelope{"request-fuzz", "receipt-fuzz", 2, 5,
                                                             10, R"({"stage":"render"})"}});
    add("receipt.json", {.payload = ControlReceiptEnvelope{
                             .request_id = "request-fuzz",
                             .receipt_id = "receipt-fuzz",
                             .operation_id = "dev.pulp.state/read@1",
                             .operation_version = 1,
                             .state = ControlReceiptState::UnknownNeedsRefresh,
                             .result_code = ControlResultCode::UnknownNeedsRefresh,
                             .retry = ControlRetryClassification::AfterRefresh,
                             .explanation = "fuzz seed terminal receipt",
                             .detail_json = R"({"generation":8})",
                             .artifacts = {{"artifact-fuzz", "application/json", 12}},
                         }});
    return corpus;
}

} // namespace pulp::test::control_protocol_fuzz
