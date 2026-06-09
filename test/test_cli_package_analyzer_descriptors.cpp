#include <catch2/catch_test_macros.hpp>

#include "package_analyzer_descriptors.hpp"

#include <pulp/audio/analyzer_provider.hpp>

#include <filesystem>
#include <span>
#include <string>
#include <vector>

using pulp::audio::AnalyzerAvailability;
using pulp::audio::AnalyzerCapability;
using pulp::audio::AnalyzerExecutionContext;
using pulp::audio::AnalyzerLicensePolicy;
using pulp::audio::analyzer_descriptor_has_capability;
using pulp::cli::pkg::LockFile;
using pulp::cli::pkg::LockedPackage;
using pulp::cli::pkg::PackageDescriptor;
using pulp::cli::pkg::PlatformTarget;
using pulp::cli::pkg::Registry;
using pulp::cli::pkg::load_registry;
using pulp::cli::pkg::package_analyzer_capabilities;
using pulp::cli::pkg::package_analyzer_descriptors;
using pulp::cli::pkg::package_has_analyzer_capabilities;
using pulp::cli::pkg::registry_analyzer_descriptors;

namespace {

std::filesystem::path registry_path() {
    return std::filesystem::path(PULP_SOURCE_DIR) / "tools" / "packages" /
           "registry.json";
}

const PackageDescriptor& package(const Registry& registry, const std::string& id) {
    const auto it = registry.packages.find(id);
    REQUIRE(it != registry.packages.end());
    return it->second;
}

LockFile lock_with(std::initializer_list<const char*> ids) {
    LockFile lock;
    for (const auto* id : ids) {
        LockedPackage locked;
        locked.version = "test";
        lock.packages[id] = locked;
    }
    return lock;
}

std::vector<PlatformTarget> default_descriptor_targets() {
    return {{"macOS", "arm64"}, {"Windows", "x64"}, {"Linux", "x64"}};
}

}  // namespace

TEST_CASE("Package analyzer descriptors map registry provides to core capabilities",
          "[cli][packages][analysis]") {
    const auto loaded = load_registry(registry_path());
    REQUIRE(loaded.error.empty());

    const auto& aubio = package(loaded.registry, "aubio");
    const auto capabilities = package_analyzer_capabilities(aubio);
    REQUIRE(package_has_analyzer_capabilities(aubio));
    REQUIRE(analyzer_descriptor_has_capability({.capabilities = capabilities},
                                               AnalyzerCapability::OnsetDetection));
    REQUIRE(analyzer_descriptor_has_capability({.capabilities = capabilities},
                                               AnalyzerCapability::BeatDetection));
    REQUIRE(analyzer_descriptor_has_capability({.capabilities = capabilities},
                                               AnalyzerCapability::PitchDetection));
    REQUIRE(analyzer_descriptor_has_capability({.capabilities = capabilities},
                                               AnalyzerCapability::TempoDetection));

    const auto lock = lock_with({"aubio"});
    const auto targets = default_descriptor_targets();
    const auto descriptors = package_analyzer_descriptors(
        aubio,
        lock,
        std::span<const PlatformTarget>(targets.data(), targets.size()));
    REQUIRE(descriptors.size() == 1);
    const auto& descriptor = descriptors[0];
    REQUIRE(descriptor.id == "package.aubio.mir");
    REQUIRE(descriptor.package_id == "aubio");
    REQUIRE(descriptor.license_policy == AnalyzerLicensePolicy::Copyleft);
    REQUIRE(descriptor.availability == AnalyzerAvailability::Disabled);
    REQUIRE(descriptor.execution_context == AnalyzerExecutionContext::RealtimeSafe);

    PackageDescriptor slice_loop;
    slice_loop.id = "slice-loop";
    slice_loop.name = "Slice Loop";
    slice_loop.license = "MIT";
    slice_loop.provides = {"slice-analysis", "loop-point-analysis"};
    const auto slice_loop_capabilities = package_analyzer_capabilities(slice_loop);
    REQUIRE(analyzer_descriptor_has_capability({.capabilities = slice_loop_capabilities},
                                               AnalyzerCapability::SliceAnalysis));
    REQUIRE(analyzer_descriptor_has_capability({.capabilities = slice_loop_capabilities},
                                               AnalyzerCapability::LoopPointAnalysis));
}

TEST_CASE("Package analyzer descriptors expose allowed time-pitch packages",
          "[cli][packages][analysis]") {
    const auto loaded = load_registry(registry_path());
    REQUIRE(loaded.error.empty());

    const auto& stretch = package(loaded.registry, "signalsmith-stretch");
    const auto lock = lock_with({"signalsmith-stretch"});
    const auto targets = default_descriptor_targets();
    const auto descriptors = package_analyzer_descriptors(
        stretch,
        lock,
        std::span<const PlatformTarget>(targets.data(), targets.size()));
    REQUIRE(descriptors.size() == 1);

    const auto& descriptor = descriptors[0];
    REQUIRE(descriptor.id == "package.signalsmith-stretch.time-pitch");
    REQUIRE(descriptor.package_id == "signalsmith-stretch");
    REQUIRE(descriptor.license_policy == AnalyzerLicensePolicy::Permissive);
    REQUIRE(descriptor.availability == AnalyzerAvailability::Available);
    REQUIRE(analyzer_descriptor_has_capability(descriptor, AnalyzerCapability::TimeStretch));
    REQUIRE(analyzer_descriptor_has_capability(descriptor, AnalyzerCapability::PitchShift));
}

TEST_CASE("Package analyzer descriptors report missing and unsupported packages",
          "[cli][packages][analysis]") {
    const auto loaded = load_registry(registry_path());
    REQUIRE(loaded.error.empty());

    const auto& stretch = package(loaded.registry, "signalsmith-stretch");
    const auto targets = default_descriptor_targets();
    const LockFile empty_lock;
    const auto missing = package_analyzer_descriptors(
        stretch,
        empty_lock,
        std::span<const PlatformTarget>(targets.data(), targets.size()));
    REQUIRE(missing.size() == 1);
    REQUIRE(missing[0].availability == AnalyzerAvailability::MissingPackage);

    const auto wasm_targets = std::vector<PlatformTarget>{{"WASM", "wasm32"}};
    const auto unsupported = package_analyzer_descriptors(
        stretch,
        lock_with({"signalsmith-stretch"}),
        std::span<const PlatformTarget>(wasm_targets.data(), wasm_targets.size()),
        true);
    REQUIRE(unsupported.size() == 1);
    REQUIRE(unsupported[0].availability == AnalyzerAvailability::UnsupportedPlatform);
}

TEST_CASE("Registry analyzer descriptors include only analyzer-capable packages",
          "[cli][packages][analysis]") {
    const auto loaded = load_registry(registry_path());
    REQUIRE(loaded.error.empty());

    const auto lock = lock_with({"signalsmith-stretch"});
    const auto targets = default_descriptor_targets();
    const auto descriptors = registry_analyzer_descriptors(
        loaded.registry,
        lock,
        std::span<const PlatformTarget>(targets.data(), targets.size()));

    bool saw_signalsmith = false;
    bool saw_non_analyzer_io = false;
    for (const auto& descriptor : descriptors) {
        if (descriptor.package_id == "signalsmith-stretch") saw_signalsmith = true;
        if (descriptor.package_id == "alac") saw_non_analyzer_io = true;
    }
    REQUIRE(saw_signalsmith);
    REQUIRE_FALSE(saw_non_analyzer_io);
}
