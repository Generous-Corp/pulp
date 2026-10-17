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
    BatchResidencySample result{.sequence = n,
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
    for (std::size_t i = 0; i < 4; ++i) {
        result.member_ids[i] = i + 1;
        result.member_terminal_ids[i] = i + 101;
    }
    return result;
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

TEST_CASE("G1 receipt fails closed on member evidence and dispatch storage overflow") {
    BatchResidencyEvaluator evaluator(admission());
    auto bad = sample(0);
    bad.member_ids[2] = 0;
    evaluator.observe(bad);
    REQUIRE(evaluator.finish().member_evidence_mismatches > 0);

    BatchResidencyEvaluator overflow(admission());
    for (std::uint64_t n = 0; n < 1025; ++n)
        overflow.observe(sample(n));
    REQUIRE(overflow.finish().dispatch_storage_overflow > 0);
    REQUIRE_FALSE(overflow.finish().complete);
}

TEST_CASE("G1 accepts width two and width four admission matrix") {
    for (const auto width : {2u, 4u}) {
        auto a = admission();
        a.batch_width = width;
        a.provider_slots = width;
        BatchResidencyEvaluator evaluator(a);
        for (std::uint64_t n = 0; n < BatchResidencyReceipt::minimum_samples; ++n) {
            auto s = sample(n);
            s.batch_width = width;
            s.member_count = width;
            s.member_terminal_count = width;
            for (std::size_t i = width; i < s.member_ids.size(); ++i) {
                s.member_ids[i] = 0;
                s.member_terminal_ids[i] = 0;
            }
            evaluator.observe(s);
        }
        REQUIRE(evaluator.finish().complete);
    }
}

TEST_CASE("G1 rejects duplicate member identities and broken terminal pairing") {
    BatchResidencyEvaluator duplicate_members(admission());
    auto bad = sample(0);
    bad.member_ids[1] = bad.member_ids[0];
    duplicate_members.observe(bad);
    REQUIRE(duplicate_members.finish().member_evidence_mismatches > 0);

    BatchResidencyEvaluator broken_pair(admission());
    bad = sample(0);
    bad.member_terminal_ids[0] = bad.member_ids[0];
    broken_pair.observe(bad);
    REQUIRE(broken_pair.finish().member_evidence_mismatches > 0);
}

TEST_CASE("G1 rejects rotating member identities within one stream") {
    BatchResidencyEvaluator evaluator(admission());
    evaluator.observe(sample(0));
    auto rotated = sample(1);
    rotated.member_ids[0] = 99;
    rotated.member_terminal_ids[0] = 199;
    evaluator.observe(rotated);
    REQUIRE(evaluator.finish().member_evidence_mismatches > 0);
}
