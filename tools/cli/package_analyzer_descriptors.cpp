// SPDX-License-Identifier: MIT
#include "package_analyzer_descriptors.hpp"

#include <algorithm>
#include <string>
#include <string_view>

namespace pulp::cli::pkg {

namespace {

using pulp::audio::AnalyzerAvailability;
using pulp::audio::AnalyzerCapability;
using pulp::audio::AnalyzerDescriptor;
using pulp::audio::AnalyzerExecutionContext;
using pulp::audio::PackageAnalyzerDescriptorInput;

bool contains_string(const std::vector<std::string>& values, std::string_view value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

void add_unique(std::vector<AnalyzerCapability>& capabilities,
                AnalyzerCapability capability) {
    if (std::find(capabilities.begin(), capabilities.end(), capability) ==
        capabilities.end()) {
        capabilities.push_back(capability);
    }
}

struct CapabilityMapping {
    std::string_view token;
    AnalyzerCapability capability;
};

constexpr CapabilityMapping kCapabilityMappings[] = {
    {"onset-detection", AnalyzerCapability::OnsetDetection},
    {"beat-detection", AnalyzerCapability::BeatDetection},
    {"pitch-detection", AnalyzerCapability::PitchDetection},
    {"slice-analysis", AnalyzerCapability::SliceAnalysis},
    {"loop-point-analysis", AnalyzerCapability::LoopPointAnalysis},
    {"key-detection", AnalyzerCapability::KeyDetection},
    {"tempo-detection", AnalyzerCapability::TempoDetection},
    {"transient-classification", AnalyzerCapability::TransientClassification},
    {"time-stretch", AnalyzerCapability::TimeStretch},
    {"pitch-shift", AnalyzerCapability::PitchShift},
};

bool package_locked(const LockFile& lock_file, const std::string& package_id) {
    return lock_file.packages.find(package_id) != lock_file.packages.end();
}

bool platforms_supported(const PackageDescriptor& package,
                         std::span<const PlatformTarget> targets) {
    if (targets.empty()) return true;
    std::vector<PlatformTarget> target_vector(targets.begin(), targets.end());
    return unsupported_targets(package, target_vector).empty();
}

bool license_policy_allows_analyzer(const PackageDescriptor& package,
                                    bool allow_restricted_licenses) {
    const auto verdict = check_license(package.license);
    if (verdict == LicenseVerdict::allowed) return true;
    return allow_restricted_licenses;
}

std::string provider_suffix_for_capabilities(
    const std::vector<AnalyzerCapability>& capabilities) {
    const auto has = [&capabilities](AnalyzerCapability capability) {
        return std::find(capabilities.begin(), capabilities.end(), capability) !=
               capabilities.end();
    };
    if ((has(AnalyzerCapability::TimeStretch) || has(AnalyzerCapability::PitchShift)) &&
        capabilities.size() <= 2) {
        return "time-pitch";
    }
    if (has(AnalyzerCapability::OnsetDetection) ||
        has(AnalyzerCapability::BeatDetection) ||
        has(AnalyzerCapability::PitchDetection) ||
        has(AnalyzerCapability::TempoDetection) ||
        has(AnalyzerCapability::KeyDetection)) {
        return "mir";
    }
    return "analysis";
}

}  // namespace

std::vector<AnalyzerCapability> package_analyzer_capabilities(
    const PackageDescriptor& package) {
    std::vector<AnalyzerCapability> capabilities;
    for (const auto& mapping : kCapabilityMappings) {
        if (contains_string(package.provides, mapping.token)) {
            add_unique(capabilities, mapping.capability);
        }
    }
    return capabilities;
}

bool package_has_analyzer_capabilities(const PackageDescriptor& package) {
    return !package_analyzer_capabilities(package).empty();
}

std::vector<AnalyzerDescriptor> package_analyzer_descriptors(
    const PackageDescriptor& package,
    const LockFile& lock_file,
    std::span<const PlatformTarget> targets,
    bool allow_restricted_licenses) {
    const auto capabilities = package_analyzer_capabilities(package);
    if (capabilities.empty()) return {};

    PackageAnalyzerDescriptorInput input;
    input.provider_id = "package." + package.id + "." +
                        provider_suffix_for_capabilities(capabilities);
    input.package_id = package.id;
    input.display_name = package.name + " Analyzer";
    input.version = package.version;
    input.license_id = package.license;
    input.url = package.url;
    input.capabilities = capabilities;
    input.installed = package_locked(lock_file, package.id);
    input.platform_supported = platforms_supported(package, targets);
    const auto license_allowed = license_policy_allows_analyzer(package,
                                                               allow_restricted_licenses);
    input.license_accepted = license_allowed;
    input.execution_context = package.rt_safe ? AnalyzerExecutionContext::RealtimeSafe
                                              : AnalyzerExecutionContext::BackgroundThread;
    input.supports_streaming_input = package.rt_safe;
    input.supports_offline_buffers = true;

    auto descriptor = pulp::audio::make_package_analyzer_descriptor(input);
    if (!input.platform_supported) {
        descriptor.availability = AnalyzerAvailability::UnsupportedPlatform;
    } else if (input.installed && !license_allowed) {
        descriptor.availability = AnalyzerAvailability::Disabled;
    }
    return {descriptor};
}

std::vector<AnalyzerDescriptor> registry_analyzer_descriptors(
    const Registry& registry,
    const LockFile& lock_file,
    std::span<const PlatformTarget> targets,
    bool allow_restricted_licenses) {
    std::vector<AnalyzerDescriptor> descriptors;
    for (const auto& [_, package] : registry.packages) {
        auto package_descriptors = package_analyzer_descriptors(package,
                                                               lock_file,
                                                               targets,
                                                               allow_restricted_licenses);
        descriptors.insert(descriptors.end(),
                           package_descriptors.begin(),
                           package_descriptors.end());
    }
    return descriptors;
}

}  // namespace pulp::cli::pkg
