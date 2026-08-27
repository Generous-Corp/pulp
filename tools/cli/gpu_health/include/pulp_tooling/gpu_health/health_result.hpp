#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::tooling::gpu_health {

inline constexpr std::string_view kSchema = "pulp.gpu-health-result.v1";
inline constexpr std::uint32_t kVersion = 1;

// Canonical v1 diagnostic-code registry. The JSON Schema enum and fixture
// manifest are mechanically checked against this list so codes cannot drift
// between native, CLI, MCP, and agent consumers.
inline constexpr std::array kEvidenceCodes{
    std::string_view{"gpu.adapter.fail"},
    std::string_view{"gpu.adapter.null"},
    std::string_view{"gpu.adapter.pass"},
    std::string_view{"gpu.adapter.unavailable"},
    std::string_view{"gpu.adapter.unverified"},
    std::string_view{"gpu.compute.fail"},
    std::string_view{"gpu.compute.pass"},
    std::string_view{"gpu.compute.unavailable"},
    std::string_view{"gpu.compute.unverified"},
    std::string_view{"gpu.configuration.fail"},
    std::string_view{"gpu.configuration.pass"},
    std::string_view{"gpu.configuration.unavailable"},
    std::string_view{"gpu.configuration.unverified"},
    std::string_view{"gpu.content.fail"},
    std::string_view{"gpu.content.pass"},
    std::string_view{"gpu.content.unavailable"},
    std::string_view{"gpu.content.unverified"},
    std::string_view{"gpu.device_state.fail"},
    std::string_view{"gpu.device_state.pass"},
    std::string_view{"gpu.device_state.unavailable"},
    std::string_view{"gpu.device_state.unverified"},
    std::string_view{"gpu.pipeline_create.fail"},
    std::string_view{"gpu.pipeline_create.pass"},
    std::string_view{"gpu.pipeline_create.unavailable"},
    std::string_view{"gpu.pipeline_create.unverified"},
    std::string_view{"gpu.readback.fail"},
    std::string_view{"gpu.readback.pass"},
    std::string_view{"gpu.readback.unavailable"},
    std::string_view{"gpu.readback.unverified"},
    std::string_view{"gpu.render.fail"},
    std::string_view{"gpu.render.pass"},
    std::string_view{"gpu.render.unavailable"},
    std::string_view{"gpu.render.unverified"},
    std::string_view{"gpu.shader_compile.fail"},
    std::string_view{"gpu.shader_compile.pass"},
    std::string_view{"gpu.shader_compile.unavailable"},
    std::string_view{"gpu.shader_compile.unverified"},
    std::string_view{"gpu.submit.fail"},
    std::string_view{"gpu.submit.pass"},
    std::string_view{"gpu.submit.unavailable"},
    std::string_view{"gpu.submit.unverified"},
    std::string_view{"gpu_compute_adapter_acquired"},
    std::string_view{"gpu_compute_adapter_identity_unverified"},
    std::string_view{"gpu_compute_device_lost"},
    std::string_view{"gpu_compute_execution_failed"},
    std::string_view{"gpu_compute_initialization_unavailable"},
    std::string_view{"gpu_compute_not_built"},
    std::string_view{"gpu_compute_oracle_mismatch"},
    std::string_view{"gpu_compute_oracle_passed"},
    std::string_view{"render_not_requested"},
    std::string_view{"renderer3d_adapter_unavailable"},
    std::string_view{"renderer3d_blank_output"},
    std::string_view{"renderer3d_content_floor_passed"},
    std::string_view{"renderer3d_not_compiled"},
    std::string_view{"renderer3d_setup_failed"},
    std::string_view{"renderer3d_readback_completed"},
    std::string_view{"renderer3d_readback_failed"},
    std::string_view{"renderer3d_render_completed"},
    std::string_view{"renderer3d_submit_completed"},
    std::string_view{"skia_graphite_content_floor_passed"},
    std::string_view{"skia_graphite_content_mismatch"},
    std::string_view{"skia_graphite_frame_failed"},
    std::string_view{"skia_graphite_readback_completed"},
    std::string_view{"skia_graphite_render_completed"},
    std::string_view{"skia_graphite_unavailable"},
    std::string_view{"wgsl.async_uncaptured_error"},
};

constexpr bool is_known_evidence_code(std::string_view code) {
    for (const auto candidate : kEvidenceCodes)
        if (candidate == code) return true;
    return false;
}

enum class Verdict { pass, fail, unavailable, unverified };

enum class Stage {
    configuration,
    adapter,
    shader_compile,
    pipeline_create,
    render,
    submit,
    readback,
    content,
    compute,
    device_state,
};

enum class AdapterClass { hardware, software, null_adapter, unknown };

enum class HealthState { healthy, failed, unavailable, unverified, lost };

enum class IdentityStatus { authentic, unverified, unavailable };

struct AdapterIdentity {
    IdentityStatus status = IdentityStatus::unavailable;
    AdapterClass classification = AdapterClass::unknown;
    std::optional<std::string> backend;
    std::optional<std::string> name;
    std::optional<std::string> vendor;
    std::optional<std::string> architecture;
    std::optional<std::string> device;
};

struct ProbeMeasurements {
    std::optional<bool> command_submitted;
    std::optional<bool> readback_completed;
    std::optional<bool> pixel_output_produced;
    std::optional<bool> content_floor_passed;
    std::optional<bool> compute_initialized;
    std::optional<bool> compute_oracle_passed;
    std::optional<bool> device_lost;
    std::optional<std::uint64_t> non_transparent_pixel_count;
    std::optional<std::uint64_t> distinct_color_count;
    std::optional<std::uint64_t> rgba_fingerprint;
};

struct EvidenceEvent {
    std::uint32_t sequence = 0;
    Stage stage = Stage::configuration;
    Verdict verdict = Verdict::unverified;
    std::string code;
    std::string detail;
};

struct ProbeEvidence {
    std::string probe_id;
    /// Whether this probe contributes to the top-level verdict. Optional
    /// compile-time capabilities remain visible without making an otherwise
    /// healthy installed GPU stack unusable.
    bool required = true;
    Verdict verdict = Verdict::unverified;
    AdapterIdentity adapter;
    ProbeMeasurements measurements;
    std::vector<EvidenceEvent> events;
};

struct HealthResult {
    std::string schema{ kSchema };
    std::uint32_t version = kVersion;
    std::string run_id;
    bool render_requested = true;
    Verdict verdict = Verdict::unverified;
    HealthState health_state = HealthState::unverified;
    std::vector<ProbeEvidence> probes;
    std::vector<std::string> recommendations;
};

std::string_view to_string(Verdict value);
std::string_view to_string(Stage value);
std::string_view to_string(AdapterClass value);
std::string_view to_string(HealthState value);
std::string_view to_string(IdentityStatus value);

std::optional<Verdict> verdict_from_string(std::string_view value);
std::optional<Stage> stage_from_string(std::string_view value);
std::optional<AdapterClass> adapter_class_from_string(std::string_view value);
std::optional<HealthState> health_state_from_string(std::string_view value);
std::optional<IdentityStatus> identity_status_from_string(std::string_view value);

/// Validate relationships that JSON Schema cannot express, including event
/// ordering and whether the top-level verdict is supported by probe evidence.
bool validate(const HealthResult& result, std::string* error = nullptr);

std::string to_json(const HealthResult& result, bool pretty = false);

/// Parse only the v1 closed shape and apply the same semantic validation as
/// `validate`. Unknown members and newer versions fail instead of being ignored.
std::optional<HealthResult> from_json(std::string_view json,
                                      std::string* error = nullptr);

} // namespace pulp::tooling::gpu_health
