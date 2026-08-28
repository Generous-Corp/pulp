#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::tooling::gpu_probe {

inline constexpr std::string_view kSchema = "pulp.gpu-probe-result.v1";
inline constexpr std::uint32_t kVersion = 1;
inline constexpr std::uint32_t kMaxDimension = 4096;
inline constexpr std::uint64_t kMaxWorkItems = 1'048'576;
inline constexpr std::uint32_t kMaxNumericSamples = 4096;
inline constexpr std::uint32_t kMaxArtifacts = 16;
inline constexpr std::uint64_t kMaxArtifactBytes = 16 * 1024 * 1024;
inline constexpr std::uint64_t kMaxTotalArtifactBytes = 64 * 1024 * 1024;

inline constexpr std::array kRecipeIds{
    std::string_view{"renderer3d.hardcoded-cube.v1"},
    std::string_view{"gpu-compute.magnitude.v1"},
    std::string_view{"threejs.multi-pass.v1"},
    std::string_view{"gpu-audio.stft.v1"},
};

enum class Verdict { pass, fail, unavailable, unverified };
enum class AdapterPolicy { hardware_required, hardware_preferred, any_supported };
enum class AdapterClass { hardware, software, null_adapter, unknown };
enum class IdentityStatus { authentic, unverified, unavailable };
enum class ArtifactKind { json, image, numeric_samples, trace };

struct AdapterIdentity {
    IdentityStatus status = IdentityStatus::unavailable;
    AdapterClass classification = AdapterClass::unknown;
    std::optional<std::string> backend;
    std::optional<std::string> name;
    std::optional<std::string> vendor;
    std::optional<std::string> architecture;
    std::optional<std::string> device;
};

struct Dimensions {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t work_items = 0;
};

struct Tolerance {
    double absolute = 0.0;
    double relative = 0.0;
};

struct RecipeBounds {
    std::uint32_t max_numeric_samples = kMaxNumericSamples;
    std::uint32_t max_artifacts = kMaxArtifacts;
    std::uint64_t max_artifact_bytes = kMaxArtifactBytes;
    std::uint64_t max_total_artifact_bytes = kMaxTotalArtifactBytes;
};

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

struct Artifact {
    std::string name;
    ArtifactKind kind = ArtifactKind::json;
    std::string mime;
    std::uint64_t bytes = 0;
    std::string sha256;
};

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

std::string_view to_string(Verdict value);
std::string_view to_string(AdapterPolicy value);
std::string_view to_string(AdapterClass value);
std::string_view to_string(IdentityStatus value);
std::string_view to_string(ArtifactKind value);

const RecipeDefinition* find_recipe(std::string_view id);
std::span<const RecipeDefinition> recipes();

bool validate(const ProbeResult& result, std::string* error = nullptr);

} // namespace pulp::tooling::gpu_probe
