#include "detail/minimum_lead_proxy.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace pulp::gpu_audio::detail;
namespace {
MinimumLeadProxyAdmission admission() {
    return {.sample_rate=48000, .block_size=64, .requested_lead_blocks=1, .pipeline_depth=3,
            .provider_slots=4, .max_inflight=4, .capacity=8, .batch_size=2, .true_batch_semantics=true,
            .provider_resources_resident=true, .cpu_fallback_prepared=true,
            .identity={1,2,3,4}, .cell_id=64000, .contention_level=1, .thermal_level=1,
            .observer_capacity=MinimumLeadProxyEvaluator::diagnostic_minimum_samples};
}
MinimumLeadProxySample sample(std::uint64_t n) {
    return {.generation=7, .sequence=n, .admission_id=n+1, .terminal_id=n+1001, .delivery_id=n+2001,
            .admission_generation=7, .admission_sequence=n, .terminal_generation=7, .terminal_sequence=n,
            .delivery_generation=7, .delivery_sequence=n, .identity={1,2,3,4}, .terminal_present=true,
            .delivery_present=true, .gpu_completed=true, .callback_timing_available=true,
            .callback_duration_ns=10, .callback_budget_ns=20,
            .prediction={.version=3, .provenance=3, .predicted_ns=100, .observed_ns=110,
                         .bound_ns=5, .uncertainty_ns=5, .safety_reserve_ns=10, .deadline_ns=200, .available=true}};
}
MinimumLeadProxyReceipt run(MinimumLeadProxySample bad, bool bad_first=true) {
    MinimumLeadProxyEvaluator e(admission());
    if (bad_first) e.observe(bad);
    for (std::uint64_t n=bad_first ? 1 : 0; n<MinimumLeadProxyEvaluator::diagnostic_minimum_samples; ++n) e.observe(sample(n));
    return e.finish();
}
}

TEST_CASE("minimum lead proxy remains diagnostic below authoritative campaign") {
    auto r = run(sample(0));
    CHECK(r.admission_valid); CHECK(r.prediction_valid); CHECK(r.complete);
    CHECK(r.diagnostic_only); CHECK_FALSE(r.authoritative_campaign); CHECK_FALSE(r.accepted);
}
TEST_CASE("minimum lead proxy rejects missing per-sample IDs") { auto s=sample(0); s.delivery_id=0; auto r=run(s); CHECK(r.missing_evidence>0); CHECK_FALSE(r.complete); }
TEST_CASE("minimum lead proxy rejects sequence gap and duplicate") { auto s=sample(0); auto r=run(s); CHECK(r.sequence_gaps==0); MinimumLeadProxyEvaluator e(admission()); e.observe(sample(0)); e.observe(sample(2)); e.observe(sample(2)); auto x=e.finish(); CHECK(x.sequence_gaps>0); CHECK(x.duplicate_records>0); }
TEST_CASE("minimum lead proxy rejects generation mismatch") { auto s=sample(0); s.terminal_generation=8; auto r=run(s); CHECK(r.generation_mismatches>0); CHECK_FALSE(r.complete); }
TEST_CASE("minimum lead proxy rejects GPU and fallback overlap") { auto s=sample(0); s.cpu_fallback_delivered=true; auto r=run(s); CHECK_FALSE(r.complete); }
TEST_CASE("minimum lead proxy rejects neither GPU nor fallback") { auto s=sample(0); s.gpu_completed=false; auto r=run(s); CHECK_FALSE(r.complete); }
TEST_CASE("minimum lead proxy enforces late and drop consistency") { auto s=sample(0); s.late=true; auto r=run(s); CHECK_FALSE(r.complete); s=sample(0); s.dropped=true; r=run(s); CHECK_FALSE(r.complete); }
TEST_CASE("minimum lead proxy rejects predictor under-estimate") { auto s=sample(0); s.prediction.observed_ns=200; s.prediction.deadline_ns=300; auto r=run(s); CHECK(r.predictor_underestimates>0); CHECK_FALSE(r.prediction_valid); }
TEST_CASE("minimum lead proxy rejects predictor provenance") { auto s=sample(0); s.prediction.provenance=99; auto r=run(s); CHECK_FALSE(r.prediction_valid); }
TEST_CASE("minimum lead proxy binds provider executable model and resident plan") { auto s=sample(0); s.identity.model=99; auto r=run(s); CHECK(r.identity_mismatches>0); CHECK_FALSE(r.complete); }
TEST_CASE("minimum lead proxy validates slots lead capacity and true batch") { auto a=admission(); a.capacity=1; a.true_batch_semantics=false; MinimumLeadProxyEvaluator e(a); e.observe(sample(0)); CHECK_FALSE(e.finish().admission_valid); }
TEST_CASE("minimum lead proxy requires explicit cell scope") { auto a=admission(); a.cell_id=0; MinimumLeadProxyEvaluator e(a); e.observe(sample(0)); CHECK_FALSE(e.finish().admission_valid); }
TEST_CASE("minimum lead proxy detects observer overflow") { auto a=admission(); a.observer_capacity=1; MinimumLeadProxyEvaluator e(a); e.observe(sample(0)); e.observe(sample(1)); CHECK(e.finish().observer_overflow>0); CHECK_FALSE(e.finish().complete); }
TEST_CASE("minimum lead proxy validates callback timing") { auto s=sample(0); s.callback_timing_available=false; auto r=run(s); CHECK(r.callback_deadline_misses>0); CHECK_FALSE(r.complete); s=sample(0); s.callback_duration_ns=21; r=run(s); CHECK(r.callback_deadline_misses>0); }
TEST_CASE("minimum lead proxy covers diagnostic sample rate block contention thermal cell") {
    auto a=admission(); a.sample_rate=44100; a.block_size=128; a.contention_level=2; a.thermal_level=2; a.cell_id=44100128;
    MinimumLeadProxyEvaluator e(a); e.observe(sample(0)); CHECK(e.finish().cell_id==44100128);
}
