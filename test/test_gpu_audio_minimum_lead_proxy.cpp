#include "detail/minimum_lead_proxy.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace pulp::gpu_audio::detail;

namespace {
MinimumLeadProxyAdmission admission() {
    return {.sample_rate = 48000,
            .block_size = 128,
            .requested_lead_blocks = 1,
            .pipeline_depth = 3,
            .provider_slots = 2,
            .max_inflight = 2,
            .batch_size = 2,
            .provider_identity_authenticated = true,
            .executable_identity_authenticated = true,
            .model_identity_authenticated = true,
            .provider_resources_resident = true,
            .cpu_fallback_prepared = true};
}

MinimumLeadProxySample sample(std::uint64_t sequence) {
    return {.generation = 7,
            .sequence = sequence,
            .admission_identity_matched = true,
            .terminal_present = true,
            .delivery_present = true,
            .gpu_completed = true,
            .prediction = {.version = 3,
                           .predicted_ns = 100,
                           .observed_ns = 110,
                           .deadline_ns = 200,
                           .available = true}};
}
} // namespace

TEST_CASE("minimum lead proxy accepts only complete one-block GPU evidence") {
    MinimumLeadProxyEvaluator evaluator(admission());
    for (std::uint64_t sequence = 0; sequence < MinimumLeadProxyEvaluator::minimum_samples;
         ++sequence)
        evaluator.observe(sample(sequence));

    const auto receipt = evaluator.finish();
    CHECK(receipt.admission_valid);
    CHECK(receipt.prediction_valid);
    CHECK(receipt.complete);
    CHECK(receipt.accepted);
    CHECK(receipt.gpu_completed == MinimumLeadProxyEvaluator::minimum_samples);
    CHECK(receipt.prediction_abs_error_max_ns == 10);
    CHECK(receipt.prediction_margin_min_ns == 90);
}

TEST_CASE("minimum lead proxy fails closed on fallback and incomplete prediction") {
    auto first = sample(0);
    first.cpu_fallback_delivered = true;
    first.gpu_completed = false;
    first.prediction.available = false;
    MinimumLeadProxyEvaluator evaluator(admission());
    evaluator.observe(first);
    for (std::uint64_t sequence = 1; sequence < MinimumLeadProxyEvaluator::minimum_samples;
         ++sequence)
        evaluator.observe(sample(sequence));

    const auto receipt = evaluator.finish();
    CHECK_FALSE(receipt.prediction_valid);
    CHECK_FALSE(receipt.accepted);
    CHECK(receipt.cpu_fallback == 1);
    CHECK(receipt.missing_evidence > 0);
}

TEST_CASE("minimum lead proxy rejects unauthenticated residency admission") {
    auto inputs = admission();
    inputs.provider_resources_resident = false;
    MinimumLeadProxyEvaluator evaluator(inputs);
    for (std::uint64_t sequence = 0; sequence < MinimumLeadProxyEvaluator::minimum_samples;
         ++sequence)
        evaluator.observe(sample(sequence));

    const auto receipt = evaluator.finish();
    CHECK_FALSE(receipt.admission_valid);
    CHECK_FALSE(receipt.accepted);
}
