#include <pulp_tooling/gpu_health/health_provider.hpp>

#include <catch2/catch_test_macros.hpp>

#include <utility>

namespace gh = pulp::tooling::gpu_health;

namespace {

gh::ProbeEvidence probe(std::string id, gh::Verdict verdict,
                        gh::Stage stage, std::string code) {
    gh::ProbeEvidence result;
    result.probe_id = std::move(id);
    result.verdict = verdict;
    result.adapter.status = gh::IdentityStatus::unavailable;
    result.adapter.classification = gh::AdapterClass::unknown;
    result.events.push_back({ 0, stage, verdict, std::move(code), "fixture evidence" });
    return result;
}

gh::ProbeEvidence passing_render(std::string id) {
    auto result = probe(std::move(id), gh::Verdict::pass,
                        gh::Stage::render, "gpu.render.pass");
    result.measurements.command_submitted = true;
    result.measurements.readback_completed = true;
    result.measurements.pixel_output_produced = true;
    result.measurements.content_floor_passed = true;
    return result;
}

class FakeProvider final : public gh::HealthProvider {
public:
    FakeProvider() {
        compute.adapter.status = gh::IdentityStatus::authentic;
        compute.adapter.classification = gh::AdapterClass::hardware;
        compute.adapter.backend = "Metal";
        compute.measurements.compute_initialized = true;
        compute.measurements.compute_oracle_passed = true;
    }

    gh::ProbeEvidence renderer = passing_render("renderer3d");
    gh::ProbeEvidence headless = passing_render("skia-graphite-headless");
    gh::ProbeEvidence compute = probe("gpu-compute-magnitude", gh::Verdict::pass,
                                      gh::Stage::compute, "gpu.compute.pass");
    int calls = 0;

    gh::ProbeEvidence probe_renderer3d() override {
        ++calls;
        return renderer;
    }
    gh::ProbeEvidence probe_headless_surface() override {
        ++calls;
        return headless;
    }
    gh::ProbeEvidence probe_compute() override {
        ++calls;
        return compute;
    }
};

} // namespace

TEST_CASE("GPU health provider aggregates real-work evidence into a pass",
          "[gpu][doctor][provider]") {
    FakeProvider provider;
    const auto result = gh::run_health_check(provider, true);
    std::string error;
    REQUIRE(provider.calls == 3);
    REQUIRE(result.verdict == gh::Verdict::pass);
    REQUIRE_FALSE(result.measured_at_utc.empty());
    REQUIRE(result.health_state == gh::HealthState::healthy);
    REQUIRE(gh::exit_code(result) == 0);
    REQUIRE(gh::validate(result, &error));
}

TEST_CASE("GPU health provider keeps observed failure above unavailable state",
          "[gpu][doctor][provider]") {
    FakeProvider provider;
    provider.renderer = probe("renderer3d", gh::Verdict::fail,
                              gh::Stage::readback, "renderer3d_readback_failed");
    provider.headless = probe("skia-graphite-headless", gh::Verdict::unavailable,
                              gh::Stage::configuration, "skia_graphite_unavailable");
    const auto result = gh::run_health_check(provider, true);
    std::string error;
    REQUIRE(result.verdict == gh::Verdict::fail);
    REQUIRE(result.health_state == gh::HealthState::failed);
    REQUIRE(gh::exit_code(result) == 1);
    REQUIRE_FALSE(result.recommendations.empty());
    REQUIRE(gh::validate(result, &error));
}

TEST_CASE("GPU health provider reports optional compile-time probes without poisoning health",
          "[gpu][doctor][provider]") {
    FakeProvider provider;
    provider.renderer = probe("renderer3d", gh::Verdict::unavailable,
                              gh::Stage::configuration, "renderer3d_not_compiled");
    provider.renderer.required = false;
    const auto result = gh::run_health_check(provider, true);
    std::string error;
    REQUIRE(result.verdict == gh::Verdict::pass);
    REQUIRE(result.health_state == gh::HealthState::healthy);
    REQUIRE(gh::exit_code(result) == 0);
    REQUIRE(gh::validate(result, &error));
}

TEST_CASE("GPU health no-render mode acquires no device and stays unverified",
          "[gpu][doctor][provider]") {
    FakeProvider provider;
    const auto result = gh::run_health_check(provider, false);
    std::string error;
    REQUIRE(provider.calls == 0);
    REQUIRE_FALSE(result.render_requested);
    REQUIRE(result.verdict == gh::Verdict::unverified);
    REQUIRE(result.health_state == gh::HealthState::unverified);
    REQUIRE(gh::exit_code(result) == 2);
    REQUIRE(gh::validate(result, &error));
}

TEST_CASE("GPU health seeded content failure preserves real device work",
          "[gpu][doctor][provider][real-device]") {
    auto passing_provider = gh::make_default_health_provider();
    const auto passing = gh::run_health_check(*passing_provider, true);
    INFO(gh::render_human(passing));
    if (passing.verdict != gh::Verdict::pass)
        SKIP("The seeded real-device mutation requires a passing baseline: " +
             gh::render_human(passing));
    REQUIRE(passing.verdict == gh::Verdict::pass);
    REQUIRE(gh::exit_code(passing) == 0);
    const auto& passing_headless = passing.probes.at(1);
    REQUIRE(passing_headless.verdict == gh::Verdict::pass);
    REQUIRE(passing_headless.measurements.command_submitted == true);
    REQUIRE(passing_headless.measurements.readback_completed == true);
    REQUIRE(passing_headless.measurements.pixel_output_produced == true);
    REQUIRE(passing_headless.measurements.content_floor_passed == true);
#if defined(PULP_GPU_HEALTH_REQUIRE_RENDERER3D)
    const auto& passing_renderer = passing.probes.at(0);
    REQUIRE(passing_renderer.required);
    REQUIRE(passing_renderer.verdict == gh::Verdict::pass);
    REQUIRE(passing_renderer.measurements.command_submitted == true);
    REQUIRE(passing_renderer.measurements.readback_completed == true);
    REQUIRE(passing_renderer.measurements.pixel_output_produced == true);
    REQUIRE(passing_renderer.measurements.content_floor_passed == true);
#endif

    gh::HealthProbeOptions options;
    options.seed_headless_content_mismatch = true;
    auto failing_provider = gh::make_default_health_provider(options);
    const auto failing = gh::run_health_check(*failing_provider, true);
    INFO(gh::render_human(failing));
    REQUIRE(failing.verdict == gh::Verdict::fail);
    REQUIRE(gh::exit_code(failing) == 1);
    const auto& headless = failing.probes.at(1);
    REQUIRE(headless.measurements.command_submitted == true);
    REQUIRE(headless.measurements.readback_completed == true);
    REQUIRE(headless.measurements.pixel_output_produced == true);
    REQUIRE(headless.measurements.content_floor_passed == false);
    REQUIRE(headless.events.at(0).stage == gh::Stage::content);
    REQUIRE(headless.events.at(0).code == "skia_graphite_content_mismatch");
}
