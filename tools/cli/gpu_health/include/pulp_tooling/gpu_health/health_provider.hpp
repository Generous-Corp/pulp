#pragma once

#include <pulp_tooling/gpu_health/health_result.hpp>

#include <memory>
#include <string>

namespace pulp::tooling::gpu_health {

class HealthProvider {
public:
    virtual ~HealthProvider() = default;

    virtual ProbeEvidence probe_renderer3d() = 0;
    virtual ProbeEvidence probe_headless_surface() = 0;
    virtual ProbeEvidence probe_compute() = 0;
};

/// Test-only fault controls applied after real GPU work completes. These do
/// not replace device acquisition, submission, or readback with mocks.
struct HealthProbeOptions {
    bool seed_headless_content_mismatch = false;
};

std::unique_ptr<HealthProvider> make_default_health_provider(
    HealthProbeOptions options = {});

HealthResult run_health_check(HealthProvider& provider,
                              bool render_requested = true);

int exit_code(const HealthResult& result);
std::string render_human(const HealthResult& result);

} // namespace pulp::tooling::gpu_health
