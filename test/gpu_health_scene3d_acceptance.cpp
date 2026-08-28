#include <pulp_tooling/gpu_health/health_provider.hpp>

#include <iostream>
#include <string>

namespace gh = pulp::tooling::gpu_health;

namespace {

bool proven_render(const gh::ProbeEvidence& probe) {
    return probe.required && probe.verdict == gh::Verdict::pass &&
           probe.measurements.command_submitted == true &&
           probe.measurements.readback_completed == true &&
           probe.measurements.pixel_output_produced == true &&
           probe.measurements.content_floor_passed == true;
}

int reject(std::string message, const gh::HealthResult& result) {
    std::cerr << message << "\n" << gh::render_human(result);
    return 1;
}

} // namespace

int main() {
    auto provider = gh::make_default_health_provider();
    const auto passing = gh::run_health_check(*provider, true);
    if (passing.verdict != gh::Verdict::pass || passing.probes.size() != 3)
        return reject("aggregate GPU-health pass was not proven", passing);
    if (!proven_render(passing.probes[0]))
        return reject("Renderer3D submit/readback/content proof was not proven", passing);
    if (!proven_render(passing.probes[1]))
        return reject("HeadlessSurface submit/readback/content proof was not proven", passing);
    if (passing.probes[2].adapter.status != gh::IdentityStatus::authentic ||
        passing.probes[2].measurements.compute_oracle_passed != true)
        return reject("authentic compute identity/oracle was not proven", passing);

    gh::HealthProbeOptions options;
    options.seed_headless_content_mismatch = true;
    auto seeded_provider = gh::make_default_health_provider(options);
    const auto failing = gh::run_health_check(*seeded_provider, true);
    const auto& seeded = failing.probes.at(1);
    if (failing.verdict != gh::Verdict::fail || gh::exit_code(failing) != 1 ||
        seeded.measurements.command_submitted != true ||
        seeded.measurements.readback_completed != true ||
        seeded.measurements.pixel_output_produced != true ||
        seeded.measurements.content_floor_passed != false ||
        seeded.events.empty() ||
        seeded.events.front().code != "skia_graphite_content_mismatch")
        return reject("seeded post-readback content failure was not proven", failing);

    std::cout << "gpu_health_scene3d_acceptance_verified=true\n"
              << gh::to_json(passing) << "\n";
    return 0;
}
