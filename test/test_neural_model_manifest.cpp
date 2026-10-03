#include <catch2/catch_test_macros.hpp>

#include "detail/neural_model_manifest.hpp"
#include "detail/neural_processor.hpp"

#include <pulp/runtime/crypto.hpp>
#include <pulp/runtime/model_store.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <limits>

#if defined(__unix__)
#include <sys/wait.h>
#include <unistd.h>
#endif

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

TEST_CASE("neural admission verifies every installed asset byte count and hash",
          "[gpu_audio][neural][manifest][provenance]") {
    const auto root = std::filesystem::temp_directory_path() / "pulp-neural-installed-assets";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    const auto weights = root / "weights.bin";
    const auto state = root / "state.bin";
    std::ofstream(weights, std::ios::binary) << std::string(128, 'w');
    std::ofstream(state, std::ios::binary) << "state";

    const auto weights_hash = pulp::runtime::sha256_file_hex(weights, 128);
    const auto state_hash = pulp::runtime::sha256_file_hex(state, 5);
    REQUIRE(weights_hash);
    REQUIRE(state_hash);

    const std::array assets{
        NeuralInstalledAsset{.asset_id = "weights", .path = weights, .sha256 = *weights_hash,
                             .size_bytes = 128},
        NeuralInstalledAsset{.asset_id = "state", .path = state, .sha256 = *state_hash,
                             .size_bytes = 5},
    };
    std::string error;
    REQUIRE(verify_neural_installed_assets(assets, error));
    REQUIRE(error.empty());

    // Fail-before plant: a same-sized replacement must be rejected by content hash.
    std::ofstream(weights, std::ios::binary | std::ios::trunc) << std::string(128, 'x');
    REQUIRE_FALSE(verify_neural_installed_assets(assets, error));
    CHECK(error.find("SHA-256") != std::string::npos);

    // Fail-before plant: a truncated asset must be rejected before hashing can admit it.
    std::ofstream(weights, std::ios::binary | std::ios::trunc) << std::string(128, 'w');
    std::ofstream(state, std::ios::binary | std::ios::trunc) << "bad";
    REQUIRE_FALSE(verify_neural_installed_assets(assets, error));
    CHECK(error.find("byte count") != std::string::npos);

    std::filesystem::remove_all(root);
}

#if defined(__unix__)
TEST_CASE("installed neural manifest reloads in a fresh process before prepare",
          "[gpu_audio][neural][manifest][cold_reload]") {
    const auto root = std::filesystem::temp_directory_path() / "pulp-neural-cold-reload";
    std::filesystem::remove_all(root);
    const auto home = root / "home";
    const auto weights = home / "audio" / "models" / "demo" / "weights.bin";
    const auto state = home / "audio" / "models" / "demo" / "state.bin";
    const auto metadata = pulp::runtime::model_install_path("audio", "demo", home);
    std::filesystem::create_directories(weights.parent_path());
    std::ofstream(weights, std::ios::binary) << std::string(128, 'w');
    std::ofstream(state, std::ios::binary) << "state";

    const auto weights_hash = pulp::runtime::sha256_file_hex(weights, 128);
    const auto state_hash = pulp::runtime::sha256_file_hex(state, 5);
    REQUIRE(weights_hash);
    REQUIRE(state_hash);
    std::ofstream(metadata) << R"({"model_id":"demo","backend":"tcn","checkpoint_ref":"weights.bin","resolved_checkpoint_path":")"
                            << weights.generic_string() << R"("})";

    auto manifest = valid_manifest();
    manifest.model_id = "demo";
    manifest.artifact_id = "weights";
    manifest.artifact_sha256 = *weights_hash;
    manifest.artifact_size_bytes = 128;
    const auto sidecar = neural_model_manifest_sidecar_path(metadata);
    std::string error;
    REQUIRE(write_neural_model_manifest(sidecar, manifest, error));

    const auto child = [&]() {
        const auto installed = pulp::runtime::read_installed_model("audio", "demo", home);
        if (!installed.metadata_found || !installed.checkpoint_exists)
            return 11;

        NeuralModelManifestRecord loaded;
        if (!read_neural_model_manifest(sidecar, loaded, error))
            return 12;
        const std::array assets{
            NeuralInstalledAsset{.asset_id = "weights", .path = weights,
                                 .sha256 = loaded.artifact_sha256,
                                 .size_bytes = loaded.artifact_size_bytes},
            NeuralInstalledAsset{.asset_id = "state", .path = state, .sha256 = *state_hash,
                                 .size_bytes = 5},
        };
        if (!verify_neural_installed_assets(assets, error))
            return 13;

        pulp::gpu_audio::detail::MicroTcnModel<1, 2> model;
        pulp::gpu_audio::detail::NeuralProcessor processor(model);
        const auto context = pulp::gpu_audio::detail::StreamingPrepareContext{
            .spec = &model.spec(), .artifact_id = loaded.artifact_id,
            .artifact_hash = loaded.artifact_sha256, .max_frames = 64};
        if (!processor.prepare(context) || !processor.publish())
            return 14;
        return 0;
    };

    const auto run_child = [&]() {
        const pid_t pid = ::fork();
        REQUIRE(pid >= 0);
        if (pid == 0)
            ::_exit(child());
        int status = 0;
        REQUIRE(::waitpid(pid, &status, 0) == pid);
        REQUIRE(WIFEXITED(status));
        return WEXITSTATUS(status);
    };

    CHECK(run_child() == 0);

    // Fail-before plant: cold reload must refuse tampered bytes before prepare.
    std::ofstream(weights, std::ios::binary | std::ios::trunc) << std::string(128, 'x');
    CHECK(run_child() == 13);

    std::filesystem::remove_all(root);
}
#endif
