#include <pulp_tooling/gpu_health/health_read_result.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace gh = pulp::tooling::gpu_health;

namespace {

constexpr auto kSourceSignature =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr auto kShaderSignature =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
constexpr auto kTargetSignature =
    "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";

gh::HealthResult passing_health() {
    gh::HealthResult result;
    result.schema = gh::kSchemaV1;
    result.version = gh::kVersionV1;
    result.measured_at_utc.clear();
    result.run_id = "gpu-evidence-1";
    result.render_requested = true;
    result.verdict = gh::Verdict::pass;
    result.health_state = gh::HealthState::healthy;
    gh::ProbeEvidence probe;
    probe.probe_id = "headless";
    probe.verdict = gh::Verdict::pass;
    probe.adapter.status = gh::IdentityStatus::authentic;
    probe.adapter.classification = gh::AdapterClass::hardware;
    probe.adapter.backend = "Metal";
    probe.measurements.command_submitted = true;
    probe.measurements.readback_completed = true;
    probe.measurements.pixel_output_produced = true;
    probe.measurements.content_floor_passed = true;
    probe.events.push_back(
        {0, gh::Stage::render, gh::Verdict::pass, "gpu.render.pass", "rendered"});
    result.probes.push_back(std::move(probe));
    return result;
}

gh::HealthReadResult passing_read_result() {
    gh::HealthReadResult result;
    result.health = passing_health();
    auto& startup = result.startup;
    startup.status = gh::MeasurementStatus::complete;
    startup.verdict = gh::Verdict::pass;
    startup.budget.budget_id = "editor-first-visible-v1";
    startup.budget.status = gh::BudgetStatus::ratified;
    startup.budget.trial_count = 2;
    startup.budget.cold_trial_count = 1;
    startup.budget.warm_trial_count = 1;
    startup.budget.percentile = 95.0;
    startup.budget.threshold_ms = 20.0;
    startup.budget.threshold_source = "reference-host-study-1";
    startup.budget.reference_hosts.push_back({"forge-shell-m5", 60.0});
    startup.correlation.gpu_evidence_id = "gpu-evidence-1";
    startup.correlation.trace_evidence_id = "trace-evidence-1";
    startup.capture.event_capacity = 64;
    startup.capture.event_count = 8;
    startup.identity.pulp_build_id = "pulp-build-1";
    startup.identity.vellum_revision = "vellum-revision-1";
    startup.identity.source_signature_sha256 = kSourceSignature;
    startup.identity.shader_signature_sha256 = kShaderSignature;
    startup.identity.expected_target_signature_sha256 = kTargetSignature;
    startup.identity.adapter_class = gh::AdapterClass::hardware;
    startup.trials = {
        {.sequence = 0,
         .cache_state = gh::CacheState::cold,
         .lifecycle_id = "lifecycle-cold-0",
         .cache_provenance = gh::CacheProvenance::fresh_process,
         .editor_open_to_first_nonblank_ms = 10.0,
         .interaction_hitch_ms = 4.0,
         .shader_compile_ms = 2.0,
         .upload_ms = 1.0,
         .hidden_frame_ms = 1.0,
         .present_ms = 2.0,
         .observed_target_signature_sha256 = kTargetSignature,
         .content_floor_passed = true,
         .visible_state = gh::VisibleState::prepared,
         .verdict = gh::Verdict::pass,
         .diagnostic_code = "gpu.startup.pass"},
        {.sequence = 1,
         .cache_state = gh::CacheState::warm,
         .lifecycle_id = "lifecycle-warm-0",
         .cache_provenance = gh::CacheProvenance::same_process_editor_reopen,
         .editor_open_to_first_nonblank_ms = 12.0,
         .interaction_hitch_ms = 5.0,
         .shader_compile_ms = 0.0,
         .upload_ms = 1.0,
         .hidden_frame_ms = 1.0,
         .present_ms = 2.0,
         .observed_target_signature_sha256 = kTargetSignature,
         .content_floor_passed = true,
         .visible_state = gh::VisibleState::prepared,
         .verdict = gh::Verdict::pass,
         .diagnostic_code = "gpu.startup.pass"},
    };
    startup.observed_percentile_ms = 12.0;
    startup.interaction_hitch_percentile_ms = 5.0;
    startup.pipeline_contribution = gh::PipelineContribution::not_material;
    startup.causal_attribution = gh::CausalAttribution::complete;
    startup.disposition = gh::StartupDisposition::no_change;
    return result;
}

} // namespace

TEST_CASE("GPU health read v1 accepts derived correlated first-visible evidence",
          "[gpu][startup][contract]") {
    const auto result = passing_read_result();
    std::string error;
    REQUIRE(gh::validate(result, &error));
    const auto json = gh::to_json(result);
    CHECK(json.find(R"("schema":"pulp.gpu-health-read-result.v1")") != std::string::npos);
    CHECK(json.find(R"("trace_evidence_id":"trace-evidence-1")") != std::string::npos);
    CHECK(json.find(R"("measurement_endpoint":"native-compositor-presentation")") !=
          std::string::npos);
    CHECK(json.find(R"("disposition":"no-change")") != std::string::npos);
}

TEST_CASE("GPU health read v1 separates lossless capture from partial instrumentation",
          "[gpu][startup][contract]") {
    std::string error;
    auto passing = passing_read_result();
    passing.startup.identity.source_signature_sha256.reset();
    passing.startup.identity.shader_signature_sha256.reset();
    passing.startup.capture.missing_trace_categories = {"pipeline_compile", "resource_upload",
                                                        "hidden_frame", "native_present"};
    for (auto& trial : passing.startup.trials) {
        trial.shader_compile_ms.reset();
        trial.upload_ms.reset();
        trial.hidden_frame_ms.reset();
        trial.present_ms.reset();
    }
    passing.startup.causal_attribution = gh::CausalAttribution::unavailable;
    passing.startup.pipeline_contribution = gh::PipelineContribution::unverified;
    REQUIRE(gh::validate(passing, &error));

    auto investigation = passing;
    for (auto& trial : investigation.startup.trials) {
        trial.editor_open_to_first_nonblank_ms = 30.0;
        trial.verdict = gh::Verdict::fail;
        trial.diagnostic_code = "gpu.startup.budget_exceeded";
    }
    investigation.startup.observed_percentile_ms = 30.0;
    investigation.startup.verdict = gh::Verdict::fail;
    investigation.startup.pipeline_contribution = gh::PipelineContribution::unattributed;
    investigation.startup.causal_attribution = gh::CausalAttribution::incomplete;
    investigation.startup.disposition = gh::StartupDisposition::queue_b4_investigation;
    REQUIRE(gh::validate(investigation, &error));

    investigation.startup.capture.missing_trace_categories.clear();
    REQUIRE_FALSE(gh::validate(investigation, &error));
    CHECK(error.find("already attributed") != std::string::npos);
}

TEST_CASE("GPU health read v1 withholds claims before budget ratification",
          "[gpu][startup][contract]") {
    auto result = passing_read_result();
    result.startup.budget.status = gh::BudgetStatus::unratified;
    result.startup.budget.threshold_ms.reset();
    result.startup.budget.threshold_source.reset();
    result.startup.budget.reference_hosts.clear();
    result.startup.verdict = gh::Verdict::unverified;
    result.startup.pipeline_contribution = gh::PipelineContribution::unverified;
    result.startup.causal_attribution = gh::CausalAttribution::unverified;
    result.startup.disposition.reset();
    std::string error;
    REQUIRE(gh::validate(result, &error));

    result.startup.disposition = gh::StartupDisposition::no_change;
    REQUIRE_FALSE(gh::validate(result, &error));
    CHECK(error.find("final startup disposition") != std::string::npos);

    auto blank = passing_read_result();
    blank.startup.budget.status = gh::BudgetStatus::unratified;
    blank.startup.budget.threshold_ms.reset();
    blank.startup.budget.threshold_source.reset();
    blank.startup.budget.reference_hosts.clear();
    blank.startup.trials[0].verdict = gh::Verdict::fail;
    blank.startup.trials[0].diagnostic_code = "gpu.startup.blank";
    blank.startup.trials[0].content_floor_passed = false;
    blank.startup.verdict = gh::Verdict::fail;
    blank.startup.pipeline_contribution = gh::PipelineContribution::unverified;
    blank.startup.causal_attribution = gh::CausalAttribution::unverified;
    blank.startup.disposition.reset();
    REQUIRE_FALSE(gh::validate(blank, &error));
    blank.startup.verdict = gh::Verdict::unverified;
    REQUIRE(gh::validate(blank, &error));
}

TEST_CASE("GPU health read v1 rejects blank laundering and percentile drift",
          "[gpu][startup][contract]") {
    std::string error;
    auto blank = passing_read_result();
    blank.startup.trials[0].verdict = gh::Verdict::fail;
    blank.startup.trials[0].diagnostic_code = "gpu.startup.blank";
    blank.startup.verdict = gh::Verdict::fail;
    blank.startup.pipeline_contribution = gh::PipelineContribution::material;
    blank.startup.disposition = gh::StartupDisposition::queue_b4;
    REQUIRE_FALSE(gh::validate(blank, &error));
    CHECK(error.find("negative content control") != std::string::npos);

    blank.startup.trials[0].content_floor_passed = false;
    REQUIRE(gh::validate(blank, &error));

    auto drift = passing_read_result();
    drift.startup.observed_percentile_ms = 11.0;
    REQUIRE_FALSE(gh::validate(drift, &error));
    CHECK(error.find("derived") != std::string::npos);
}

TEST_CASE("GPU health read v1 rejects incomplete or unresolved complete captures",
          "[gpu][startup][contract]") {
    std::string error;
    auto loss = passing_read_result();
    loss.startup.capture.dropped_event_count = 1;
    loss.startup.capture.truncated = true;
    REQUIRE_FALSE(gh::validate(loss, &error));
    CHECK(error.find("complete startup measurement") != std::string::npos);

    auto timeout = passing_read_result();
    timeout.startup.trials[0].verdict = gh::Verdict::unavailable;
    timeout.startup.trials[0].diagnostic_code = "gpu.startup.timeout";
    REQUIRE_FALSE(gh::validate(timeout, &error));
    CHECK(error.find("unresolved trial") != std::string::npos);

    auto causal_loss = passing_read_result();
    causal_loss.startup.status = gh::MeasurementStatus::incomplete;
    causal_loss.startup.verdict = gh::Verdict::unverified;
    causal_loss.startup.capture.dropped_event_count = 1;
    causal_loss.startup.capture.truncated = true;
    causal_loss.startup.disposition.reset();
    REQUIRE_FALSE(gh::validate(causal_loss, &error));
    CHECK(error.find("complete causal attribution") != std::string::npos);

    auto incomplete_blank = passing_read_result();
    incomplete_blank.startup.status = gh::MeasurementStatus::incomplete;
    incomplete_blank.startup.verdict = gh::Verdict::fail;
    incomplete_blank.startup.capture.dropped_event_count = 1;
    incomplete_blank.startup.capture.truncated = true;
    incomplete_blank.startup.trials[0].verdict = gh::Verdict::fail;
    incomplete_blank.startup.trials[0].diagnostic_code = "gpu.startup.blank";
    incomplete_blank.startup.trials[0].content_floor_passed = false;
    incomplete_blank.startup.pipeline_contribution = gh::PipelineContribution::unverified;
    incomplete_blank.startup.causal_attribution = gh::CausalAttribution::incomplete;
    incomplete_blank.startup.disposition.reset();
    REQUIRE_FALSE(gh::validate(incomplete_blank, &error));
    CHECK(error.find("verdict is not supported") != std::string::npos);
    incomplete_blank.startup.verdict = gh::Verdict::unverified;
    REQUIRE(gh::validate(incomplete_blank, &error));

    auto uncorrelated = passing_read_result();
    uncorrelated.startup.correlation.gpu_evidence_id.reset();
    uncorrelated.startup.disposition.reset();
    REQUIRE_FALSE(gh::validate(uncorrelated, &error));
    CHECK(error.find("complete causal attribution") != std::string::npos);
}

TEST_CASE("GPU health read v1 enforces its frozen cold and warm trial composition",
          "[gpu][startup][contract]") {
    std::string error;
    auto warm_only = passing_read_result();
    warm_only.startup.trials[0].cache_state = gh::CacheState::warm;
    warm_only.startup.trials[0].cache_provenance =
        gh::CacheProvenance::same_process_editor_reopen;
    REQUIRE_FALSE(gh::validate(warm_only, &error));
    CHECK(error.find("cold/warm") != std::string::npos);

    auto budget_drift = passing_read_result();
    budget_drift.startup.budget.cold_trial_count = 2;
    REQUIRE_FALSE(gh::validate(budget_drift, &error));
    CHECK(error.find("composition") != std::string::npos);

    auto reused_lifecycle = passing_read_result();
    reused_lifecycle.startup.trials[1].lifecycle_id =
        reused_lifecycle.startup.trials[0].lifecycle_id;
    REQUIRE_FALSE(gh::validate(reused_lifecycle, &error));
    CHECK(error.find("lifecycle identity") != std::string::npos);

    auto fabricated_warm_boundary = passing_read_result();
    fabricated_warm_boundary.startup.trials[1].cache_provenance =
        gh::CacheProvenance::fresh_process;
    REQUIRE_FALSE(gh::validate(fabricated_warm_boundary, &error));
    CHECK(error.find("lifecycle/cache provenance") != std::string::npos);
}
