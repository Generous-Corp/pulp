#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <choc/text/choc_JSON.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include <pulp/runtime/crypto.hpp>
#include <pulp/runtime/model_registry.hpp>

namespace pulp::gpu_audio::detail {

/// The sidecar is a control-plane persistence format.  It is deliberately
/// private to the GPU-audio implementation: ModelEntry remains the stable
/// catalog/download ABI, while this versioned document carries execution
/// metadata that must survive a ModelStore reload.
inline constexpr std::string_view kNeuralModelManifestSchema = "pulp.neural-model-manifest";
inline constexpr std::uint32_t kNeuralModelManifestSchemaVersion = 1;

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

/// Installed artifact provenance owned by the private neural admission seam.
/// Paths and strings belong to the control-plane installer; this view is never
/// retained by the realtime processor.
struct NeuralInstalledAsset {
    std::string_view asset_id{};
    std::filesystem::path path{};
    std::string_view sha256{};
    std::uint64_t size_bytes = 0;
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
    constexpr bool accepted() const noexcept {
        return error == NeuralModelManifestError::None;
    }
};

enum class NeuralModelUse : std::uint8_t {
    Shipped,
    LocalResearch,
};

constexpr NeuralModelManifestValidation
validate_neural_model_manifest(const NeuralModelManifest& manifest,
                               NeuralModelUse use = NeuralModelUse::Shipped) noexcept;

/// Owning form used only while reading/writing the sidecar.  A prepared
/// processor should keep using NeuralModelManifest's non-owning view instead;
/// this type is never touched by the realtime callback.
struct NeuralModelManifestRecord {
    std::string model_id;
    std::string architecture;
    std::string model_version;
    std::string artifact_id;
    std::string artifact_sha256;
    std::string license;
    std::string runtime;
    std::string state_schema;
    std::uint64_t artifact_size_bytes = 0;
    std::uint32_t sample_rate_hz = 0;
    std::uint64_t state_bytes = 0;
    std::uint32_t state_schema_version = 0;
    bool redistributable = false;

    [[nodiscard]] NeuralModelManifest view() const noexcept {
        return {.model_id = model_id,
                .architecture = architecture,
                .model_version = model_version,
                .artifact_id = artifact_id,
                .artifact_sha256 = artifact_sha256,
                .license = license,
                .runtime = runtime,
                .state_schema = state_schema,
                .artifact_size_bytes = artifact_size_bytes,
                .sample_rate_hz = sample_rate_hz,
                .state_bytes = state_bytes,
                .state_schema_version = state_schema_version,
                .redistributable = redistributable};
    }
};

/// Return the manifest sidecar corresponding to ModelStore's per-model
/// metadata file (`m1.json` -> `m1.neural.json`).  Keeping it beside, rather
/// than inside, the existing metadata preserves old ModelStore readers and
/// makes the extension removable if the manifest contract changes.
inline std::filesystem::path
neural_model_manifest_sidecar_path(const std::filesystem::path& install_metadata_path) {
    if (install_metadata_path.empty())
        return {};
    auto sidecar = install_metadata_path;
    sidecar.replace_filename(install_metadata_path.stem().string() + ".neural.json");
    return sidecar;
}

namespace manifest_store_detail {

inline void add_string(choc::value::Value& object, const char* key, std::string_view value) {
    object.addMember(key, choc::value::createString(std::string(value)));
}

inline std::string read_string(const choc::value::ValueView& object, const char* key) {
    if (!object.isObject() || !object.hasObjectMember(key) || !object[key].isString())
        return {};
    return std::string(object[key].toString());
}

inline bool read_u64(const choc::value::ValueView& object, const char* key,
                     std::uint64_t& destination) {
    if (!object.isObject() || !object.hasObjectMember(key) || !object[key].isInt())
        return false;
    const auto value = object[key].getInt64();
    if (value < 0)
        return false;
    destination = static_cast<std::uint64_t>(value);
    return true;
}

inline bool read_u32(const choc::value::ValueView& object, const char* key,
                     std::uint32_t& destination) {
    std::uint64_t value = 0;
    if (!read_u64(object, key, value) || value > 0xffffffffu)
        return false;
    destination = static_cast<std::uint32_t>(value);
    return true;
}

inline bool read_bool(const choc::value::ValueView& object, const char* key, bool& destination) {
    if (!object.isObject() || !object.hasObjectMember(key) || !object[key].isBool())
        return false;
    destination = object[key].getBool();
    return true;
}

} // namespace manifest_store_detail

/// Persist an execution manifest next to an installed model.  The write is
/// staged and renamed so a reader observes either the old complete sidecar or
/// the new complete sidecar, never a partial JSON document.  This is a
/// control-thread operation and must not be called from process().
inline bool write_neural_model_manifest(const std::filesystem::path& sidecar_path,
                                        const NeuralModelManifest& manifest, std::string& error,
                                        NeuralModelUse use = NeuralModelUse::Shipped) {
    error.clear();
    if (sidecar_path.empty()) {
        error = "empty neural manifest sidecar path";
        return false;
    }
    if (!validate_neural_model_manifest(manifest, use).accepted()) {
        error = "neural model manifest failed validation";
        return false;
    }
    if (manifest.artifact_size_bytes >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
        manifest.state_bytes >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        error = "neural model manifest size exceeds JSON integer range";
        return false;
    }

    auto object = choc::value::createObject("");
    manifest_store_detail::add_string(object, "schema", kNeuralModelManifestSchema);
    object.addMember("schema_version", choc::value::createInt64(kNeuralModelManifestSchemaVersion));
    manifest_store_detail::add_string(object, "model_id", manifest.model_id);
    manifest_store_detail::add_string(object, "architecture", manifest.architecture);
    manifest_store_detail::add_string(object, "model_version", manifest.model_version);
    manifest_store_detail::add_string(object, "artifact_id", manifest.artifact_id);
    manifest_store_detail::add_string(object, "artifact_sha256", manifest.artifact_sha256);
    manifest_store_detail::add_string(object, "license", manifest.license);
    manifest_store_detail::add_string(object, "runtime", manifest.runtime);
    manifest_store_detail::add_string(object, "state_schema", manifest.state_schema);
    object.addMember("artifact_size_bytes", choc::value::createInt64(static_cast<std::int64_t>(
                                                manifest.artifact_size_bytes)));
    object.addMember("sample_rate_hz",
                     choc::value::createInt64(static_cast<std::int64_t>(manifest.sample_rate_hz)));
    object.addMember("state_bytes",
                     choc::value::createInt64(static_cast<std::int64_t>(manifest.state_bytes)));
    object.addMember("state_schema_version", choc::value::createInt64(static_cast<std::int64_t>(
                                                 manifest.state_schema_version)));
    object.addMember("redistributable", choc::value::createBool(manifest.redistributable));

    const auto temporary = sidecar_path.string() + ".tmp";
    std::error_code ec;
    if (!sidecar_path.parent_path().empty())
        std::filesystem::create_directories(sidecar_path.parent_path(), ec);
    if (ec) {
        error = "failed to create neural manifest directory: " + ec.message();
        return false;
    }
    {
        std::ofstream output(temporary, std::ios::trunc);
        if (!output.is_open()) {
            error = "failed to open neural manifest temporary file";
            return false;
        }
        output << choc::json::toString(object, true);
        if (!output.good()) {
            error = "failed to write neural manifest temporary file";
            output.close();
            std::filesystem::remove(temporary, ec);
            return false;
        }
    }
#ifdef _WIN32
    // MoveFileEx with REPLACE_EXISTING and WRITE_THROUGH is the Windows
    // replace primitive; removing the destination first would expose a
    // missing/partial manifest to a concurrent reload.
    if (!::MoveFileExW(std::filesystem::path(temporary).c_str(), sidecar_path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        ec = std::error_code(static_cast<int>(::GetLastError()), std::system_category());
    }
#else
    // POSIX rename replaces an existing destination atomically within a file
    // system, which is the contract required by a cold reload.
    std::filesystem::rename(temporary, sidecar_path, ec);
#endif
    if (ec) {
        error = "failed to publish neural manifest: " + ec.message();
        std::filesystem::remove(temporary, ec);
        return false;
    }
    return true;
}

/// Read and validate a versioned sidecar. Unknown fields are intentionally
/// ignored so a newer producer can add optional metadata without breaking an
/// older reader; the schema id/version and all current required fields remain
/// strict.
inline bool read_neural_model_manifest(const std::filesystem::path& sidecar_path,
                                       NeuralModelManifestRecord& record, std::string& error,
                                       NeuralModelUse use = NeuralModelUse::Shipped) {
    error.clear();
    std::ifstream input(sidecar_path);
    if (!input.is_open()) {
        error = "failed to open neural manifest sidecar";
        return false;
    }
    std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (text.empty()) {
        error = "empty neural manifest sidecar";
        return false;
    }
    choc::value::Value document;
    try {
        document = choc::json::parse(text);
    } catch (...) {
        error = "failed to parse neural manifest sidecar";
        return false;
    }
    const auto root = document.getView();
    if (!root.isObject() ||
        manifest_store_detail::read_string(root, "schema") != kNeuralModelManifestSchema) {
        error = "unsupported neural manifest schema";
        return false;
    }
    std::uint32_t schema_version = 0;
    if (!manifest_store_detail::read_u32(root, "schema_version", schema_version) ||
        schema_version != kNeuralModelManifestSchemaVersion) {
        error = "unsupported neural manifest schema version";
        return false;
    }

    NeuralModelManifestRecord parsed;
    parsed.model_id = manifest_store_detail::read_string(root, "model_id");
    parsed.architecture = manifest_store_detail::read_string(root, "architecture");
    parsed.model_version = manifest_store_detail::read_string(root, "model_version");
    parsed.artifact_id = manifest_store_detail::read_string(root, "artifact_id");
    parsed.artifact_sha256 = manifest_store_detail::read_string(root, "artifact_sha256");
    parsed.license = manifest_store_detail::read_string(root, "license");
    parsed.runtime = manifest_store_detail::read_string(root, "runtime");
    parsed.state_schema = manifest_store_detail::read_string(root, "state_schema");
    if (!manifest_store_detail::read_u64(root, "artifact_size_bytes", parsed.artifact_size_bytes) ||
        !manifest_store_detail::read_u32(root, "sample_rate_hz", parsed.sample_rate_hz) ||
        !manifest_store_detail::read_u64(root, "state_bytes", parsed.state_bytes) ||
        !manifest_store_detail::read_u32(root, "state_schema_version",
                                         parsed.state_schema_version) ||
        !manifest_store_detail::read_bool(root, "redistributable", parsed.redistributable) ||
        !validate_neural_model_manifest(parsed.view(), use).accepted()) {
        error = "invalid neural model manifest metadata";
        return false;
    }
    record = std::move(parsed);
    return true;
}

constexpr bool is_hex(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

constexpr bool is_sha256(std::string_view hash) noexcept {
    if (hash.size() != 64)
        return false;
    for (const char c : hash)
        if (!is_hex(c))
            return false;
    return true;
}

constexpr NeuralModelManifestValidation
validate_neural_model_manifest(const NeuralModelManifest& manifest, NeuralModelUse use) noexcept {
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
inline NeuralModelManifestValidation
validate_neural_model_entry(const pulp::runtime::ModelEntry& entry, bool redistributable,
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

/// Verify the bytes on disk, rather than merely checking that a manifest hash
/// has SHA-256 shape. This is control-plane admission work and must complete
/// before a model is prepared or exposed to the realtime callback.
inline bool verify_neural_installed_assets(std::span<const NeuralInstalledAsset> assets,
                                           std::string& error) {
    error.clear();
    if (assets.empty()) {
        error = "neural model has no installed assets";
        return false;
    }

    for (const auto& asset : assets) {
        if (asset.asset_id.empty() || asset.path.empty() || asset.size_bytes == 0 ||
            !is_sha256(asset.sha256)) {
            error = "neural installed asset metadata is invalid";
            return false;
        }

        std::error_code ec;
        const auto actual_size = std::filesystem::file_size(asset.path, ec);
        if (ec || actual_size != asset.size_bytes) {
            error = "neural installed asset byte count mismatch: " + std::string(asset.asset_id);
            return false;
        }

        const auto actual_hash = pulp::runtime::sha256_file_hex(asset.path, asset.size_bytes);
        if (!actual_hash || *actual_hash != asset.sha256) {
            error = "neural installed asset SHA-256 mismatch: " + std::string(asset.asset_id);
            return false;
        }
    }
    return true;
}

/// Verify the primary artifact represented by a manifest. Additional bundle
/// assets are checked with verify_neural_installed_assets in the same admission
/// transaction.
inline bool verify_neural_manifest_artifact(const NeuralModelManifest& manifest,
                                            const std::filesystem::path& artifact_path,
                                            std::string& error) {
    const NeuralInstalledAsset asset{.asset_id = manifest.artifact_id,
                                     .path = artifact_path,
                                     .sha256 = manifest.artifact_sha256,
                                     .size_bytes = manifest.artifact_size_bytes};
    return verify_neural_installed_assets(std::span<const NeuralInstalledAsset>(&asset, 1), error);
}

} // namespace pulp::gpu_audio::detail
