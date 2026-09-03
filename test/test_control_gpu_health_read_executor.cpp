#include <pulp/inspect/control_gpu_health_read_executor.hpp>
#include <pulp_tooling/gpu_health/health_read_result.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <string>

namespace inspect = pulp::inspect;
namespace gh = pulp::tooling::gpu_health;

namespace {

gh::HealthReadResult unavailable_snapshot() {
    gh::HealthReadResult result;
    result.health.run_id = "gpu-evidence-1";
    result.health.verdict = gh::Verdict::unavailable;
    result.health.health_state = gh::HealthState::unavailable;
    gh::ProbeEvidence probe;
    probe.probe_id = "startup-source";
    probe.verdict = gh::Verdict::unavailable;
    probe.events.push_back({0, gh::Stage::adapter, gh::Verdict::unavailable,
                            "gpu.adapter.unavailable", "adapter unavailable"});
    result.health.probes.push_back(std::move(probe));

    result.startup.status = gh::MeasurementStatus::unavailable;
    result.startup.verdict = gh::Verdict::unavailable;
    result.startup.budget.budget_id = "editor-first-visible-v1";
    result.startup.capture.event_capacity = 64;
    result.startup.identity.pulp_build_id = "pulp-build-1";
    result.startup.correlation.gpu_evidence_id = "gpu-evidence-1";
    result.startup.correlation.trace_evidence_id = "trace-evidence-1";
    return result;
}

inspect::ControlAdmissionPlan plan() {
    inspect::ControlAdmissionPlan value;
    value.registration_id = inspect::ControlRegistrationId{"registration-1"};
    value.instance_id = "instance-1";
    value.publication_id = "publication-1";
    value.operation_id = "dev.pulp.gpu/health.read@1";
    value.operation_version = 1;
    return value;
}

inspect::ControlRequestEnvelope request() {
    return {.registration_id = "registration-1",
            .instance_generation = "publication-1",
            .operation_id = "dev.pulp.gpu/health.read@1",
            .operation_version = 1,
            .params_json = "{}"};
}

inspect::ControlExecutionContext context(inspect::ControlExecutionCheckpoint checkpoint =
                                             inspect::ControlExecutionCheckpoint::Continue) {
    return {.checkpoint = [checkpoint] { return checkpoint; }};
}

inspect::ControlGpuHealthReadSource source(gh::HealthReadResult result) {
    return {.registration_id = inspect::ControlRegistrationId{"registration-1"},
            .instance_id = "instance-1",
            .publication_id = "publication-1",
            .read_result = [result = std::move(result)]() {
                return std::make_shared<const gh::HealthReadResult>(result);
            }};
}

} // namespace

TEST_CASE("GPU health read executor preserves exact source and evidence identity",
          "[inspect][control][gpu][read]") {
    const auto snapshot = unavailable_snapshot();
    std::string error;
    REQUIRE(gh::validate(snapshot, &error));
    auto executor = inspect::make_control_gpu_health_read_executor(
        [value = source(snapshot)](const auto&) { return value; });
    const auto result = executor(plan(), request(), context());

    CHECK(result.terminal_state == inspect::ControlReceiptState::Completed);
    CHECK_FALSE(result.result.result_code.has_value());
    CHECK(result.result.detail_json == gh::to_json(snapshot));
    CHECK(result.result.evidence_ids ==
          std::vector<std::string>{"gpu-evidence-1", "trace-evidence-1"});
}

TEST_CASE("GPU health read executor fails closed on source and schema drift",
          "[inspect][control][gpu][read]") {
    const auto snapshot = unavailable_snapshot();
    auto wrong = source(snapshot);
    wrong.publication_id = "other-publication";
    auto wrong_executor =
        inspect::make_control_gpu_health_read_executor([wrong](const auto&) { return wrong; });
    auto result = wrong_executor(plan(), request(), context());
    REQUIRE(result.result.result_code.has_value());
    CHECK(*result.result.result_code == inspect::ControlResultCode::HostUnavailable);
    CHECK(result.result.retry == inspect::ControlRetryClassification::AfterRefresh);

    auto schema_invalid = snapshot;
    schema_invalid.schema = "renamed";
    auto invalid_executor = inspect::make_control_gpu_health_read_executor(
        [value = source(std::move(schema_invalid))](const auto&) { return value; });
    result = invalid_executor(plan(), request(), context());
    REQUIRE(result.result.result_code.has_value());
    CHECK(*result.result.result_code == inspect::ControlResultCode::InternalError);

    auto semantic_invalid = snapshot;
    semantic_invalid.startup.verdict = gh::Verdict::pass;
    semantic_invalid.startup.disposition = gh::StartupDisposition::no_change;
    auto semantic_executor = inspect::make_control_gpu_health_read_executor(
        [value = source(std::move(semantic_invalid))](const auto&) { return value; });
    result = semantic_executor(plan(), request(), context());
    REQUIRE(result.result.result_code.has_value());
    CHECK(*result.result.result_code == inspect::ControlResultCode::InternalError);
}

TEST_CASE("GPU health read executor honors request and checkpoint boundaries",
          "[inspect][control][gpu][read]") {
    auto executor = inspect::make_control_gpu_health_read_executor(
        [value = source(unavailable_snapshot())](const auto&) { return value; });
    auto nonempty = request();
    nonempty.params_json = R"({"newest":true})";
    auto result = executor(plan(), nonempty, context());
    REQUIRE(result.result.result_code.has_value());
    CHECK(*result.result.result_code == inspect::ControlResultCode::InvalidRequest);

    result =
        executor(plan(), request(), context(inspect::ControlExecutionCheckpoint::DeadlineExceeded));
    REQUIRE(result.result.result_code.has_value());
    CHECK(*result.result.result_code == inspect::ControlResultCode::DeadlineExceeded);

    result = executor(plan(), request(), context(inspect::ControlExecutionCheckpoint::Cancelled));
    CHECK(result.terminal_state == inspect::ControlReceiptState::Cancelled);
    REQUIRE(result.result.result_code.has_value());
    CHECK(*result.result.result_code == inspect::ControlResultCode::Cancelled);
}
