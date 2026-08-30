#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/**
 * @namespace pulp::tooling::gpu_probe
 * @brief Bounded recipe definitions, owned evidence, and native GPU probe runs.
 *
 * @par Ownership and lifetime
 * Probe results and recipe runs own their strings, vectors, and artifact bytes.
 * `find_recipe` pointers, `recipes` spans, and the string views inside each
 * RecipeDefinition refer to a process-lifetime static registry and must not be
 * freed or retained after module unload. Validators and serializers retain no
 * references to caller-owned inputs.
 *
 * @par Threading and real-time safety
 * Model, parsing, and serialization functions may allocate and provide no
 * synchronization for shared objects. Native recipe functions acquire devices,
 * submit work, wait for mapping/readback, and may access artifact runtimes.
 * Run them on an offline control or worker thread, never an audio callback or
 * another real-time thread.
 *
 * @par Determinism and units
 * Recipes bind fixed sources, seeds, dimensions, ordered semantic passes, and
 * independent oracles. Width and height are pixels, `work_items` and sample
 * counts are unitless counts, artifact sizes are bytes, SHA-256 values are
 * lowercase hexadecimal, and tolerances use the recipe's numeric sample units.
 * GPU scheduling time is not inferred from these result fields.
 *
 * @par Results, unavailable evidence, and errors
 * `pass` and `fail` mean the requested recipe executed and reached its oracle.
 * `unavailable` means the device, feature, runtime, or other required evidence
 * could not be acquired; `unverified` means the evidence cannot support a
 * claim. Neither is a pass. Validation and parsing report human diagnostics
 * through optional error outputs, while stable automation uses verdicts,
 * registered pass codes, and process exit codes.
 */
namespace pulp::tooling::gpu_probe {

/// Closed JSON schema identity for ProbeResult.
inline constexpr std::string_view kSchema = "pulp.gpu-probe-result.v1";
/// Schema version serialized in ProbeResult::version.
inline constexpr std::uint32_t kVersion = 1;
/// Maximum width or height in pixels for a registered recipe.
inline constexpr std::uint32_t kMaxDimension = 4096;
/// Maximum unitless work-item count for a registered recipe.
inline constexpr std::uint64_t kMaxWorkItems = 1'048'576;
/// Maximum number of numeric samples retained in one result.
inline constexpr std::uint32_t kMaxNumericSamples = 4096;
/// Maximum number of artifacts retained in one result.
inline constexpr std::uint32_t kMaxArtifacts = 16;
/// Maximum declared size in bytes for one artifact.
inline constexpr std::uint64_t kMaxArtifactBytes = 16 * 1024 * 1024;
/// Maximum combined declared artifact size in bytes for one result.
inline constexpr std::uint64_t kMaxTotalArtifactBytes = 64 * 1024 * 1024;

inline constexpr std::array kRecipeIds{
    std::string_view{"renderer3d.hardcoded-cube.v1"},
    std::string_view{"gpu-compute.magnitude.v1"},
    std::string_view{"gpu-audio.stft.v1"},
    std::string_view{"threejs.multi-pass.v1"},
};

/// Four-state recipe outcome that never folds missing evidence into success.
enum class Verdict {
    pass,        ///< Work completed and every semantic oracle passed.
    fail,        ///< Work completed and at least one measured oracle failed.
    unavailable, ///< Required execution capability or evidence was unavailable.
    unverified,  ///< Evidence exists but is insufficient for a measured claim.
};
/// Minimum adapter class accepted by a recipe definition.
enum class AdapterPolicy { hardware_required, hardware_preferred, any_supported };
/// Adapter class proven by native identity, never inferred from backend alone.
enum class AdapterClass { hardware, software, null_adapter, unknown };
/// Authenticity state for the optional native adapter identity fields.
enum class IdentityStatus { authentic, unverified, unavailable };
/// Machine-readable content kind for a declared artifact.
enum class ArtifactKind { json, image, numeric_samples, trace };

/// Native adapter identity. Missing optionals represent absent evidence.
struct AdapterIdentity {
    IdentityStatus status = IdentityStatus::unavailable;
    AdapterClass classification = AdapterClass::unknown;
    std::optional<std::string> backend;
    std::optional<std::string> name;
    std::optional<std::string> vendor;
    std::optional<std::string> architecture;
    std::optional<std::string> device;
};

/// Recipe dimensions in pixels and unitless dispatched work items.
struct Dimensions {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t work_items = 0;
};

/// Absolute and relative error limits in the recipe's numeric sample units.
struct Tolerance {
    double absolute = 0.0;
    double relative = 0.0;
};

/// Per-recipe caps for owned samples and artifact bytes.
struct RecipeBounds {
    std::uint32_t max_numeric_samples = kMaxNumericSamples;
    std::uint32_t max_artifacts = kMaxArtifacts;
    std::uint64_t max_artifact_bytes = kMaxArtifactBytes;
    std::uint64_t max_total_artifact_bytes = kMaxTotalArtifactBytes;
};

/// Borrowed process-lifetime definition of one deterministic recipe.
struct RecipeDefinition {
    std::string_view id;
    std::string_view source_identity;
    Dimensions dimensions;
    std::uint64_t seed = 0;
    std::string_view clock;
    std::string_view input_format;
    std::string_view output_format;
    std::string_view encoding;
    Tolerance tolerance;
    AdapterPolicy adapter_policy = AdapterPolicy::hardware_required;
    std::span<const std::string_view> semantic_passes;
    RecipeBounds bounds;
    std::string_view positive_control;
    std::string_view negative_mutation;
};

/// One ordered semantic oracle result.
struct PassResult {
    std::uint32_t sequence = 0;
    std::string name;
    Verdict verdict = Verdict::unverified;
    bool work_completed = false;
    std::optional<double> expected;
    std::optional<double> observed;
    std::optional<double> absolute_error;
    std::string code;
};

/// Bounded artifact descriptor; payload bytes are owned by ArtifactPayload.
struct Artifact {
    std::string name;
    ArtifactKind kind = ArtifactKind::json;
    std::string mime;
    std::uint64_t bytes = 0;
    std::string sha256;
};

/// Closed v1 metadata and verdict for one recipe execution.
struct ProbeResult {
    std::string schema{kSchema};
    std::uint32_t version = kVersion;
    std::string gpu_evidence_id;
    std::string recipe_id;
    std::string source_digest;
    std::string signature_digest;
    Dimensions dimensions;
    std::uint64_t seed = 0;
    std::string clock;
    std::string input_format;
    std::string output_format;
    std::string encoding;
    Tolerance tolerance;
    AdapterPolicy adapter_policy = AdapterPolicy::hardware_required;
    AdapterIdentity adapter;
    std::uint32_t numeric_sample_count = 0;
    std::optional<std::string> mutation;
    Verdict verdict = Verdict::unverified;
    std::vector<PassResult> passes;
    std::vector<Artifact> artifacts;
    std::vector<std::string> recommendations;
};

/// Return the canonical lowercase schema spelling from static storage.
std::string_view to_string(Verdict value);
std::string_view to_string(AdapterPolicy value);
std::string_view to_string(AdapterClass value);
std::string_view to_string(IdentityStatus value);
std::string_view to_string(ArtifactKind value);

/// Find a borrowed process-lifetime definition, or return null for an unknown ID.
const RecipeDefinition* find_recipe(std::string_view id);
/// Return a borrowed span over the immutable process-lifetime recipe registry.
std::span<const RecipeDefinition> recipes();

/// Validate bounds, recipe identity, ordered pass aggregation, and artifacts.
///
/// On failure, `error` receives a human diagnostic when non-null. The text is
/// not a stable machine-readable error code.
bool validate(const ProbeResult& result, std::string* error = nullptr);
/// Serialize without implicitly validating the result.
std::string to_json(const ProbeResult& result, bool pretty = false);
/// Parse the exact closed v1 shape and validate it before returning a value.
std::optional<ProbeResult> from_json(std::string_view json, std::string* error = nullptr);
/// Render an owning human-readable report; machine consumers use to_json().
std::string render_human(const ProbeResult& result);
/// Map `pass` to 0, `fail` to 1, and unavailable or unverified to 2.
int exit_code(const ProbeResult& result);

} // namespace pulp::tooling::gpu_probe
