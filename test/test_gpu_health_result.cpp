#include <pulp_tooling/gpu_health/health_result.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace gh = pulp::tooling::gpu_health;

namespace {

gh::HealthResult passing_result() {
    gh::HealthResult result;
    result.run_id = "roundtrip";
    result.render_requested = true;
    result.verdict = gh::Verdict::pass;
    result.health_state = gh::HealthState::healthy;

    gh::ProbeEvidence probe;
    probe.probe_id = "render";
    probe.verdict = gh::Verdict::pass;
    probe.adapter.status = gh::IdentityStatus::authentic;
    probe.adapter.classification = gh::AdapterClass::hardware;
    probe.adapter.backend = "Metal";
    probe.adapter.name = "fixture-adapter";
    probe.measurements.command_submitted = true;
    probe.measurements.readback_completed = true;
    probe.measurements.pixel_output_produced = true;
    probe.measurements.content_floor_passed = true;
    probe.measurements.non_transparent_pixel_count = 64;
    probe.events.push_back({ 0, gh::Stage::render, gh::Verdict::pass,
                             "gpu.render.pass", "bounded fixture render" });
    result.probes.push_back(std::move(probe));
    return result;
}

} // namespace

TEST_CASE("GPU health v1 result round-trips through its closed JSON model",
          "[gpu][doctor][contract]") {
    const auto result = passing_result();
    std::string error;
    REQUIRE(gh::validate(result, &error));

    const auto json = gh::to_json(result, true);
    const auto parsed = gh::from_json(json, &error);
    REQUIRE(parsed.has_value());
    REQUIRE(error.empty());
    REQUIRE(gh::to_json(*parsed) == gh::to_json(result));
}

TEST_CASE("GPU health v1 parser rejects missing and unknown fields",
          "[gpu][doctor][contract]") {
    const auto json = gh::to_json(passing_result());
    std::string error;

    auto missing = json;
    const auto key = missing.find("\"run_id\":\"roundtrip\",");
    REQUIRE(key != std::string::npos);
    missing.erase(key, std::string("\"run_id\":\"roundtrip\",").size());
    REQUIRE_FALSE(gh::from_json(missing, &error).has_value());
    REQUIRE(error.find("run_id") != std::string::npos);

    auto unknown = json;
    unknown.insert(unknown.find('{') + 1, "\"unexpected\":true,");
    REQUIRE_FALSE(gh::from_json(unknown, &error).has_value());
    REQUIRE(error.find("unknown member") != std::string::npos);
}

TEST_CASE("GPU health v1 rejects unsupported identity and evidence claims",
          "[gpu][doctor][contract]") {
    std::string error;

    auto unverified_hardware = passing_result();
    unverified_hardware.probes[0].adapter.status = gh::IdentityStatus::unverified;
    REQUIRE_FALSE(gh::validate(unverified_hardware, &error));
    REQUIRE(error.find("hardware") != std::string::npos);

    auto null_pass = passing_result();
    null_pass.probes[0].adapter.classification = gh::AdapterClass::null_adapter;
    REQUIRE_FALSE(gh::validate(null_pass, &error));
    REQUIRE(error.find("null adapter") != std::string::npos);

    auto reordered = passing_result();
    reordered.probes[0].events[0].sequence = 1;
    REQUIRE_FALSE(gh::validate(reordered, &error));
    REQUIRE(error.find("contiguous") != std::string::npos);

    auto compute_without_oracle = passing_result();
    compute_without_oracle.probes[0].events[0].stage = gh::Stage::compute;
    compute_without_oracle.probes[0].events[0].code = "gpu.compute.pass";
    REQUIRE_FALSE(gh::validate(compute_without_oracle, &error));
    REQUIRE(error.find("compute") != std::string::npos);

    auto wrong_code_binding = passing_result();
    wrong_code_binding.probes[0].events[0].code = "gpu.readback.pass";
    REQUIRE_FALSE(gh::validate(wrong_code_binding, &error));
    REQUIRE(error.find("registered stage") != std::string::npos);

    auto unknown_code = passing_result();
    unknown_code.probes[0].events[0].code = "gpu.render.renamed";
    REQUIRE_FALSE(gh::validate(unknown_code, &error));
    REQUIRE(error.find("not registered") != std::string::npos);
}
