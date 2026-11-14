#include "tools/cli/cli_graph_reload_receipt.hpp"

#include <catch2/catch_test_macros.hpp>

namespace {
constexpr const char* kAdmissionDenied =
    R"({"error": "admission-denied", "explanation": "permission-denied", )"
    R"("schema": "dev.pulp.control/cli-error@1"})";
} // namespace

TEST_CASE("Graph reload receipts map to resend, stop or move on", "[cli][graph-file-reload]") {
    std::uint64_t generation = 4;
    SECTION("completed publishes and adopts the receipt generation") {
        REQUIRE(classify_graph_reload_receipt(
                    0,
                    R"({"state": "completed", "detail": {"code": "committed", "generation": 5}})",
                    generation) == GraphReloadOutcome::Published);
        REQUIRE(generation == 5);
    }
    SECTION("a typed executor refusal is final") {
        REQUIRE(classify_graph_reload_receipt(
                    1, R"({"state": "failed", "detail": {"code": "invalid-document"}})",
                    generation) == GraphReloadOutcome::Refused);
        REQUIRE(generation == 4);
    }
    SECTION("a stale-base refusal is its own outcome and keeps the generation") {
        REQUIRE(classify_graph_reload_receipt(
                    1, R"({"state": "failed", "detail": {"code": "stale-base", "generation": 9}})",
                    generation) == GraphReloadOutcome::StaleGeneration);
        REQUIRE(generation == 4);
    }
    SECTION("a failure before the executor ran is not applied") {
        REQUIRE(classify_graph_reload_receipt(
                    1, R"({"state": "failed", "detail": {}, "explanation": "timed out"})",
                    generation) == GraphReloadOutcome::NotApplied);
    }
    SECTION("an admission refusal of the grant stops resending") {
        REQUIRE(classify_graph_reload_receipt(1, kAdmissionDenied, generation) ==
                GraphReloadOutcome::AuthorityRefused);
        REQUIRE(graph_reload_authority_refused(kAdmissionDenied));
    }
    SECTION("anything without a terminal receipt is unknown") {
        REQUIRE(classify_graph_reload_receipt(1, R"({"state": "unknown_needs_refresh"})",
                                              generation) == GraphReloadOutcome::Unknown);
        REQUIRE(classify_graph_reload_receipt(1, R"({"error": "connect-failed"})", generation) ==
                GraphReloadOutcome::Unknown);
        REQUIRE(classify_graph_reload_receipt(1, "not json", generation) ==
                GraphReloadOutcome::Unknown);
        REQUIRE_FALSE(graph_reload_authority_refused(R"({"error": "connect-failed"})"));
    }
}

TEST_CASE("Repeated lease refusals share one key across receipt ids", "[cli][graph-file-reload]") {
    const auto first = graph_reload_refusal_key(
        R"({"detail": {}, "explanation": "controller lease has expired", "receipt_id": "a", "state": "failed"})");
    const auto second = graph_reload_refusal_key(
        R"({"detail": {}, "explanation": "controller lease has expired", "receipt_id": "b", "state": "failed"})");
    const auto other = graph_reload_refusal_key(
        R"({"detail": {}, "explanation": "controller lease is not owned by this authority", "receipt_id": "c", "state": "failed"})");
    REQUIRE(first == second);
    REQUIRE(first != other);
}

TEST_CASE("Retry budget is independent per distinct file content", "[cli][graph-file-reload]") {
    GraphReloadRetryBudget budget(3);
    budget.offer("first");
    CHECK_FALSE(budget.record_failure());
    CHECK_FALSE(budget.record_failure());
    // A different change starts fresh: one failure must not hit the limit.
    budget.offer("second");
    CHECK(budget.failures() == 0);
    CHECK_FALSE(budget.record_failure());
    // Retrying the same content keeps counting toward the limit.
    budget.offer("second");
    CHECK_FALSE(budget.record_failure());
    CHECK(budget.record_failure());
    budget.reset();
    CHECK(budget.failures() == 0);
}
