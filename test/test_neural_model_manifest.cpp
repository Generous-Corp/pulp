#include <catch2/catch_test_macros.hpp>

#include "detail/neural_model_manifest.hpp"

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
