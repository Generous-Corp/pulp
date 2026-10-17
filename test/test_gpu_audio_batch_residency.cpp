#include "detail/batch_residency_contract.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace pulp::gpu_audio::detail;

namespace {
BatchResidencyAdmission admission() {
    return {.sample_rate = 48000,
            .block_size = 64,
            .batch_width = 4,
            .provider_slots = 4,
            .resident_bytes = 4096,
            .provider_id = 1,
            .executable_id = 2,
            .model_id = 3,
            .resident_plan_id = 4,
            .generation = 7,
            .authenticated = true,
            .true_batch = true,
            .resources_resident = true,
            .cpu_fallback_prepared = true};
}
BatchResidencySample sample(std::uint64_t n) {
    return {.sequence = n,
            .dispatch_id = n + 1,
            .batch_width = 4,
            .resident_bytes = 4096,
            .provider_id = 1,
            .executable_id = 2,
            .model_id = 3,
            .resident_plan_id = 4,
            .generation = 7,
            .batched_dispatch = true,
            .resources_resident = true,
            .gpu_completed = true,
            .authenticated = true,
            .member_count = 4,
            .member_terminal_count = 4};
}
} // namespace

TEST_CASE("G1 accepts authenticated width four resident dispatch receipts") {
    BatchResidencyEvaluator evaluator(admission());
    for (std::uint64_t n = 0; n < BatchResidencyReceipt::minimum_samples; ++n)
        evaluator.observe(sample(n));
    const auto receipt = evaluator.finish();
    REQUIRE(receipt.admission_valid);
    REQUIRE(receipt.complete);
    REQUIRE(receipt.gpu_completed == BatchResidencyReceipt::minimum_samples);
    REQUIRE(receipt.diagnostic_only);
    REQUIRE_FALSE(receipt.accepted);
}

TEST_CASE("G1 rejects width one and unauthenticated residency claims") {
    auto a = admission();
    a.batch_width = 1;
    BatchResidencyEvaluator width_one(a);
    width_one.observe(sample(0));
    REQUIRE_FALSE(width_one.finish().admission_valid);

    a = admission();
    a.authenticated = false;
    BatchResidencyEvaluator unauthenticated(a);
    for (std::uint64_t n = 0; n < BatchResidencyReceipt::minimum_samples; ++n)
        unauthenticated.observe(sample(n));
    REQUIRE_FALSE(unauthenticated.finish().admission_valid);
}

TEST_CASE("G1 receipt rejects residency drift, identity drift, and sequence gaps") {
    BatchResidencyEvaluator evaluator(admission());
    evaluator.observe(sample(0));
    auto bad = sample(2);
    bad.resident_bytes = 1;
    bad.model_id = 99;
    bad.batched_dispatch = false;
    evaluator.observe(bad);
    const auto receipt = evaluator.finish();
    REQUIRE(receipt.residency_mismatches > 0);
    REQUIRE(receipt.identity_mismatches > 0);
    REQUIRE(receipt.width_mismatches > 0);
    REQUIRE(receipt.sequence_gaps > 0);
    REQUIRE_FALSE(receipt.complete);
}

TEST_CASE("G1 rejects incomplete member terminals, unauthenticated rows, and sentinels") {
    BatchResidencyEvaluator evaluator(admission());
    auto bad = sample(0);
    bad.member_terminal_count = 3;
    bad.authenticated = false;
    bad.gpu_completed = false;
    evaluator.observe(bad);
    bad = sample(UINT64_MAX);
    evaluator.observe(bad);
    const auto receipt = evaluator.finish();
    REQUIRE(receipt.member_count_mismatches > 0);
    REQUIRE(receipt.invalid_samples > 0);
    REQUIRE_FALSE(receipt.complete);
}

TEST_CASE("G1 receipt rejects duplicate dispatch IDs") {
    BatchResidencyEvaluator evaluator(admission());
    evaluator.observe(sample(0));
    auto duplicate = sample(1);
    duplicate.dispatch_id = 1;
    evaluator.observe(duplicate);
    REQUIRE(evaluator.finish().duplicate_dispatch_ids > 0);
}
