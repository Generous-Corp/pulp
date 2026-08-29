#pragma once

#include <pulp_tooling/gpu_health/health_result.hpp>

#include <memory>
#include <string>

namespace pulp::tooling::gpu_health {

/// Blocking producer interface for the three independent GPU health probes.
///
/// The caller owns the provider and must keep it alive for every virtual call.
/// Implementations return owning ProbeEvidence values and must not retain
/// references supplied by run_health_check(). Providers are not required to be
/// thread-safe. Device acquisition, submission, mapping, and readback may block
/// and allocate; invoke probes only from a control or worker thread, never an
/// audio callback or another real-time thread.
class HealthProvider {
public:
    virtual ~HealthProvider() = default;

    /// Acquire Renderer3D evidence, including a bounded render/readback oracle.
    virtual ProbeEvidence probe_renderer3d() = 0;
    /// Acquire a bounded headless Graphite render/readback observation.
    virtual ProbeEvidence probe_headless_surface() = 0;
    /// Acquire a bounded GPU compute/map observation and CPU-oracle comparison.
    virtual ProbeEvidence probe_compute() = 0;
};

/// Fault controls applied after real GPU work completes.
///
/// These controls are intended for deterministic negative acceptance tests and
/// do not replace device acquisition, submission, or readback with mocks.
struct HealthProbeOptions {
    bool seed_headless_content_mismatch = false;
};

/// Create the native blocking provider and transfer sole ownership to the caller.
std::unique_ptr<HealthProvider> make_default_health_provider(
    HealthProbeOptions options = {});

/// Execute the requested probes serially and derive one closed HealthResult.
///
/// `provider` is borrowed for the duration of the call. When
/// `render_requested` is false, the function performs no device acquisition or
/// GPU work and returns `unverified`; it never promotes inventory to `pass`.
HealthResult run_health_check(HealthProvider& provider,
                              bool render_requested = true);

/// Map `pass` to 0, `fail` to 1, and unavailable or unverified to 2.
int exit_code(const HealthResult& result);
/// Render an owning human-readable report; machine consumers use to_json().
std::string render_human(const HealthResult& result);

} // namespace pulp::tooling::gpu_health
