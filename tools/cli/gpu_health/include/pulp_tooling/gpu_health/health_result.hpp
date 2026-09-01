#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/**
 * @namespace pulp::tooling::gpu_health
 * @brief Bounded, typed evidence returned by Pulp GPU health diagnostics.
 *
 * @par Ownership and lifetime
 * Result objects own their strings, optionals, and vectors. Parsing returns an
 * owning value. Views returned by `to_string` refer to static storage and must
 * not be freed. Validation and serialization retain no references to inputs.
 *
 * @par Threading and real-time safety
 * The value and serialization APIs provide no internal synchronization and may
 * allocate. Use separate result objects on concurrent threads or synchronize
 * shared access. Provider probes acquire GPU resources and block for submitted
 * work, so every API in this namespace belongs on a control or worker thread,
 * never an audio callback or another real-time thread.
 *
 * @par Determinism and units
 * Event sequences are zero-based and contiguous across a result. Pixel and
 * color fields are integer counts; fingerprints are opaque unsigned values.
 * Optional measurements distinguish an absent observation from an observed
 * `false` or zero. Serialization preserves the closed v1 field vocabulary.
 *
 * @par Results, unavailable evidence, and errors
 * `pass` and `fail` mean requested work completed and was measured.
 * `unavailable` means required work could not be acquired or executed, while
 * `unverified` means the available evidence cannot support a claim. Neither is
 * a pass. Validators and parsers return failure without throwing for malformed
 * contract data and write a human diagnostic when an error output is supplied.
 */
namespace pulp::tooling::gpu_health {

/// Closed JSON schema identity for HealthResult.
inline constexpr std::string_view kSchema = "pulp.gpu-health-result.v1";
/// Schema version serialized in HealthResult::version.
inline constexpr std::uint32_t kVersion = 1;

/// Canonical v1 diagnostic-code registry shared by native, CLI, MCP, and agents.
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

/// Four-state outcome that keeps missing or inconclusive evidence out of pass.
enum class Verdict {
    pass,        ///< Requested work completed and satisfied its oracle.
    fail,        ///< Requested work completed and failed a measured assertion.
    unavailable, ///< Requested evidence or execution capability was unavailable.
    unverified,  ///< Evidence exists but is insufficient to support a verdict.
};

/// Ordered semantic stage described by one EvidenceEvent.
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

/// Adapter class proven by native identity, never inferred from backend alone.
enum class AdapterClass { hardware, software, null_adapter, unknown };

/// Aggregate device state derived from required probe evidence.
enum class HealthState { healthy, failed, unavailable, unverified, lost };

/// Authenticity state for the optional native adapter identity fields.
enum class IdentityStatus { authentic, unverified, unavailable };

/// Native adapter identity. Missing optional fields represent absent evidence.
struct AdapterIdentity {
    IdentityStatus status = IdentityStatus::unavailable;
    AdapterClass classification = AdapterClass::unknown;
    std::optional<std::string> backend;
    std::optional<std::string> name;
    std::optional<std::string> vendor;
    std::optional<std::string> architecture;
    std::optional<std::string> device;
};

/// Optional measurements produced by one probe.
///
/// Counts are unitless pixel/color counts. `rgba_fingerprint` is an opaque
/// deterministic content fingerprint, not a color or duration.
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

/// One globally ordered stage observation with a registered diagnostic code.
struct EvidenceEvent {
    std::uint32_t sequence = 0;
    Stage stage = Stage::configuration;
    Verdict verdict = Verdict::unverified;
    std::string code;
    std::string detail;
};

/// Evidence and aggregate verdict for one independently executed probe.
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

/// Closed v1 aggregate returned by the GPU health diagnostic.
struct HealthResult {
    std::string schema{ kSchema };
    std::uint32_t version = kVersion;
    std::string run_id;
    /// UTC wall-clock time captured immediately before active probe execution.
    /// Empty preserves parsing compatibility with stored v1 results that
    /// predate authenticated run evidence.
    std::string measured_at_utc;
    bool render_requested = true;
    Verdict verdict = Verdict::unverified;
    HealthState health_state = HealthState::unverified;
    std::vector<ProbeEvidence> probes;
    std::vector<std::string> recommendations;
};

/// Return the canonical lowercase schema spelling from static storage.
std::string_view to_string(Verdict value);
std::string_view to_string(Stage value);
std::string_view to_string(AdapterClass value);
std::string_view to_string(HealthState value);
std::string_view to_string(IdentityStatus value);

/// Parse an exact canonical enum spelling; unknown text returns `std::nullopt`.
std::optional<Verdict> verdict_from_string(std::string_view value);
std::optional<Stage> stage_from_string(std::string_view value);
std::optional<AdapterClass> adapter_class_from_string(std::string_view value);
std::optional<HealthState> health_state_from_string(std::string_view value);
std::optional<IdentityStatus> identity_status_from_string(std::string_view value);

/// Validate bounds and cross-field relationships that JSON Schema cannot express.
///
/// The function retains no input references. On failure, `error` receives a
/// human diagnostic when non-null; callers must not treat that text as a stable
/// machine-readable error code.
bool validate(const HealthResult& result, std::string* error = nullptr);

/// Serialize one validated or diagnostic result to the closed v1 JSON shape.
///
/// Serialization does not implicitly call validate().
std::string to_json(const HealthResult& result, bool pretty = false);

/// Parse only the v1 closed shape and apply validate() before returning a value.
///
/// Unknown members, malformed values, and newer versions return `std::nullopt`.
/// When non-null, `error` receives a human diagnostic on failure.
std::optional<HealthResult> from_json(std::string_view json,
                                      std::string* error = nullptr);

} // namespace pulp::tooling::gpu_health
