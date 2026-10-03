#include <catch2/catch_test_macros.hpp>

#include "detail/neural_model_manifest.hpp"

#include <filesystem>
#include <fstream>
#include <limits>

using namespace pulp::gpu_audio::detail;

namespace {
constexpr auto kHash = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

NeuralModelManifest valid_manifest() {
    return {.model_id = "demo.tcn",
            .architecture = "tcn",
            .model_version = "1",
            .artifact_id = "weights.bin",
            .artifact_sha256 = kHash,
            .license = "MIT",
            .runtime = "cpu-tcn",
            .state_schema = "causal-ring-v1",
            .artifact_size_bytes = 128,
            .sample_rate_hz = 48000,
            .state_bytes = 64,
            .state_schema_version = 1,
            .redistributable = true};
}
} // namespace

TEST_CASE("neural manifest accepts immutable, redistributable artifact metadata",
          "[gpu_audio][neural][manifest]") {
    CHECK(validate_neural_model_manifest(valid_manifest()).accepted());
}

TEST_CASE("neural manifest rejects missing or malformed provenance",
          "[gpu_audio][neural][manifest]") {
    auto manifest = valid_manifest();
    manifest.model_id = {};
    CHECK(validate_neural_model_manifest(manifest).error ==
          NeuralModelManifestError::MissingIdentity);
    manifest = valid_manifest();
    manifest.artifact_sha256 = "short";
    CHECK(validate_neural_model_manifest(manifest).error ==
          NeuralModelManifestError::InvalidArtifactHash);
    manifest = valid_manifest();
    manifest.license = {};
    CHECK(validate_neural_model_manifest(manifest).error ==
          NeuralModelManifestError::MissingLicense);
    manifest = valid_manifest();
    manifest.artifact_size_bytes = 0;
    CHECK(validate_neural_model_manifest(manifest).error ==
          NeuralModelManifestError::InvalidArtifactSize);
    manifest = valid_manifest();
    manifest.redistributable = false;
    CHECK(validate_neural_model_manifest(manifest).error ==
          NeuralModelManifestError::NonRedistributableArtifact);
}

TEST_CASE("neural manifest requires a versioned schema for retained state",
          "[gpu_audio][neural][manifest]") {
    auto manifest = valid_manifest();
    manifest.state_schema = {};
    CHECK(validate_neural_model_manifest(manifest).error ==
          NeuralModelManifestError::InvalidStateSchema);
    manifest = valid_manifest();
    manifest.state_schema_version = 0;
    CHECK(validate_neural_model_manifest(manifest).error ==
          NeuralModelManifestError::InvalidStateSchema);
    manifest = valid_manifest();
    manifest.state_schema = "causal-ring-v1";
    manifest.state_schema_version = 0;
    CHECK(validate_neural_model_manifest(manifest).error ==
          NeuralModelManifestError::InvalidStateSchema);
    manifest = valid_manifest();
    manifest.state_bytes = 0;
    manifest.state_schema = {};
    manifest.state_schema_version = 0;
    CHECK(validate_neural_model_manifest(manifest).accepted());
}

TEST_CASE("neural manifest permits licensed local research weights",
          "[gpu_audio][neural][manifest]") {
    auto manifest = valid_manifest();
    manifest.redistributable = false;
    CHECK(validate_neural_model_manifest(manifest, NeuralModelUse::LocalResearch).accepted());
    CHECK(validate_neural_model_manifest(manifest, NeuralModelUse::Shipped).error ==
          NeuralModelManifestError::NonRedistributableArtifact);
}

TEST_CASE("neural manifest requires runtime compatibility metadata",
          "[gpu_audio][neural][manifest]") {
    auto manifest = valid_manifest();
    manifest.runtime = {};
    CHECK(validate_neural_model_manifest(manifest).error ==
          NeuralModelManifestError::MissingCompatibilityMetadata);
    manifest = valid_manifest();
    manifest.sample_rate_hz = 0;
    CHECK(validate_neural_model_manifest(manifest).error ==
          NeuralModelManifestError::MissingCompatibilityMetadata);
}

TEST_CASE("ModelStore entry admits only verified neural artifacts",
          "[gpu_audio][neural][manifest]") {
    pulp::runtime::ModelEntry entry{.model_id = "demo.tcn",
                                    .display_name = "Demo",
                                    .description = {},
                                    .backend = "tcn",
                                    .checkpoint_ref = "hf://pulp/demo/weights.bin",
                                    .task_tags = {},
                                    .size_bytes = 128,
                                    .download_url = {},
                                    .sha256 = kHash,
                                    .auto_downloadable = true,
                                    .assets = {},
                                    .is_recommended = false,
                                    .license = "MIT"};
    CHECK(validate_neural_model_entry(entry, true, 48000, "cpu-tcn").accepted());
    CHECK(validate_neural_model_entry(entry, false, 48000, "cpu-tcn").error ==
          NeuralModelManifestError::NonRedistributableArtifact);
    entry.sha256 = "";
    CHECK(validate_neural_model_entry(entry, true, 48000, "cpu-tcn").error ==
          NeuralModelManifestError::InvalidArtifactHash);
    entry.sha256 = kHash;
    entry.size_bytes = 0;
    CHECK(validate_neural_model_entry(entry, true, 48000, "cpu-tcn").error ==
          NeuralModelManifestError::InvalidArtifactSize);
}

TEST_CASE("neural manifest sidecar round-trips execution metadata without changing ModelEntry",
          "[gpu_audio][neural][manifest][persistence]") {
    const auto root = std::filesystem::temp_directory_path() / "pulp-neural-manifest-sidecar";
    std::filesystem::remove_all(root);
    const auto metadata = root / "models" / "demo.tcn.json";
    const auto sidecar = neural_model_manifest_sidecar_path(metadata);
    REQUIRE(sidecar == root / "models" / "demo.tcn.neural.json");

    const auto source = valid_manifest();
    std::string error;
    REQUIRE(write_neural_model_manifest(sidecar, source, error));
    REQUIRE(error.empty());
    REQUIRE(std::filesystem::exists(sidecar));
    REQUIRE_FALSE(std::filesystem::exists(sidecar.string() + ".tmp"));

    NeuralModelManifestRecord loaded;
    REQUIRE(read_neural_model_manifest(sidecar, loaded, error));
    REQUIRE(error.empty());
    CHECK(loaded.model_id == source.model_id);
    CHECK(loaded.architecture == source.architecture);
    CHECK(loaded.model_version == source.model_version);
    CHECK(loaded.artifact_id == source.artifact_id);
    CHECK(loaded.artifact_sha256 == source.artifact_sha256);
    CHECK(loaded.license == source.license);
    CHECK(loaded.runtime == source.runtime);
    CHECK(loaded.state_schema == source.state_schema);
    CHECK(loaded.artifact_size_bytes == source.artifact_size_bytes);
    CHECK(loaded.sample_rate_hz == source.sample_rate_hz);
    CHECK(loaded.state_bytes == source.state_bytes);
    CHECK(loaded.state_schema_version == source.state_schema_version);
    CHECK(loaded.redistributable == source.redistributable);
    CHECK(validate_neural_model_manifest(loaded.view()).accepted());

    std::filesystem::remove_all(root);
}

TEST_CASE("neural manifest sidecar rejects schema drift and invalid reloads",
          "[gpu_audio][neural][manifest][persistence]") {
    const auto root =
        std::filesystem::temp_directory_path() / "pulp-neural-manifest-sidecar-invalid";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto sidecar = root / "demo.neural.json";
    NeuralModelManifestRecord loaded;
    std::string error;

    std::ofstream(sidecar) << R"({"schema":"pulp.neural-model-manifest","schema_version":99})";
    REQUIRE_FALSE(read_neural_model_manifest(sidecar, loaded, error));
    CHECK(error.find("schema version") != std::string::npos);

    // A recognized schema with missing required metadata must fail closed rather
    // than produce a partially populated record that could reach a model loader.
    std::ofstream(sidecar)
        << R"({"schema":"pulp.neural-model-manifest","schema_version":1,"model_id":"x"})";
    REQUIRE_FALSE(read_neural_model_manifest(sidecar, loaded, error));
    CHECK(error.find("invalid") != std::string::npos);

    std::filesystem::remove_all(root);
}

TEST_CASE("neural manifest sidecar rejects unusable paths and unrepresentable sizes",
          "[gpu_audio][neural][manifest][persistence]") {
    const auto source = valid_manifest();
    std::string error;
    REQUIRE(neural_model_manifest_sidecar_path({}).empty());
    REQUIRE_FALSE(write_neural_model_manifest({}, source, error));
    CHECK(error.find("path") != std::string::npos);

    const auto root = std::filesystem::temp_directory_path() / "pulp-neural-manifest-sidecar-range";
    std::filesystem::remove_all(root);
    auto oversized = source;
    oversized.artifact_size_bytes = std::numeric_limits<std::uint64_t>::max();
    REQUIRE_FALSE(write_neural_model_manifest(root / "demo.neural.json", oversized, error));
    CHECK(error.find("integer range") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(root / "demo.neural.json.tmp"));
    std::filesystem::remove_all(root);
}
