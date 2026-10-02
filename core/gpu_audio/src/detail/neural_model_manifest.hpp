#pragma once

#include <cstdint>
#include <string_view>

#include <pulp/runtime/model_registry.hpp>

namespace pulp::gpu_audio::detail {

/// Control-plane metadata for a neural artifact.  This is a non-owning view:
/// the model store owns the strings and must keep them immutable for the
/// lifetime of a prepared instance.  It deliberately stays private until the
/// runtime and packaging contracts are proven.
struct NeuralModelManifest {
    std::string_view model_id{};
    std::string_view architecture{};
    std::string_view model_version{};
    std::string_view artifact_id{};
    std::string_view artifact_sha256{};
    std::string_view license{};
    std::string_view runtime{};
    std::string_view state_schema{};
    std::uint64_t artifact_size_bytes = 0;
    std::uint32_t sample_rate_hz = 0;
    std::uint64_t state_bytes = 0;
    std::uint32_t state_schema_version = 0;
    bool redistributable = false;
};

enum class NeuralModelManifestError : std::uint8_t {
    None,
    MissingIdentity,
    InvalidArtifactHash,
    MissingLicense,
    NonRedistributableArtifact,
    InvalidArtifactSize,
    MissingCompatibilityMetadata,
    InvalidStateSchema,
};

struct NeuralModelManifestValidation {
    NeuralModelManifestError error = NeuralModelManifestError::None;
    constexpr bool accepted() const noexcept { return error == NeuralModelManifestError::None; }
};

enum class NeuralModelUse : std::uint8_t {
    Shipped,
    LocalResearch,
};

constexpr bool is_hex(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

constexpr bool is_sha256(std::string_view hash) noexcept {
    if (hash.size() != 64) return false;
    for (const char c : hash)
        if (!is_hex(c)) return false;
    return true;
}

constexpr NeuralModelManifestValidation
validate_neural_model_manifest(const NeuralModelManifest& manifest,
                               NeuralModelUse use = NeuralModelUse::Shipped) noexcept {
    if (manifest.model_id.empty() || manifest.architecture.empty() ||
        manifest.model_version.empty() || manifest.artifact_id.empty())
        return {NeuralModelManifestError::MissingIdentity};
    if (!is_sha256(manifest.artifact_sha256))
        return {NeuralModelManifestError::InvalidArtifactHash};
    if (manifest.artifact_size_bytes == 0)
        return {NeuralModelManifestError::InvalidArtifactSize};
    if (manifest.license.empty())
        return {NeuralModelManifestError::MissingLicense};
    if (use == NeuralModelUse::Shipped && !manifest.redistributable)
        return {NeuralModelManifestError::NonRedistributableArtifact};
    if (manifest.runtime.empty() || manifest.sample_rate_hz == 0)
        return {NeuralModelManifestError::MissingCompatibilityMetadata};
    if ((manifest.state_bytes != 0) != (!manifest.state_schema.empty()) ||
        (manifest.state_bytes != 0 && manifest.state_schema_version == 0))
        return {NeuralModelManifestError::InvalidStateSchema};
    return {};
}

/// A ModelStore entry can be admitted to the neural runtime only when its
/// downloadable primary asset has provenance and a declared redistribution
/// policy.  This seam does not change ModelEntry or weaken existing catalogs;
/// callers opt into strict neural admission after resolving their policy.
inline NeuralModelManifestValidation validate_neural_model_entry(
    const pulp::runtime::ModelEntry& entry, bool redistributable,
    std::uint32_t sample_rate_hz, std::string_view runtime) noexcept {
    NeuralModelManifest manifest{
        .model_id = entry.model_id,
        .architecture = entry.backend,
        .model_version = entry.model_id,
        .artifact_id = entry.checkpoint_ref,
        .artifact_sha256 = entry.sha256,
        .license = entry.license,
        .runtime = runtime,
        .state_schema = {},
        .artifact_size_bytes = entry.size_bytes,
        .sample_rate_hz = sample_rate_hz,
        .state_bytes = 0,
        .state_schema_version = 0,
        .redistributable = redistributable,
    };
    return validate_neural_model_manifest(manifest);
}

static_assert(is_sha256("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"));

} // namespace pulp::gpu_audio::detail
