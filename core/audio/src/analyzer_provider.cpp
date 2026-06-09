#include <pulp/audio/analyzer_provider.hpp>

#include <algorithm>
#include <cstddef>
#include <utility>

namespace pulp::audio {

namespace {

char lower_ascii(char ch) noexcept {
    return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch;
}

bool contains_ascii_case_insensitive(std::string_view text,
                                     std::string_view needle) noexcept {
    if (needle.empty()) return true;
    if (needle.size() > text.size()) return false;

    for (std::size_t offset = 0; offset <= text.size() - needle.size(); ++offset) {
        bool match = true;
        for (std::size_t i = 0; i < needle.size(); ++i) {
            if (lower_ascii(text[offset + i]) != lower_ascii(needle[i])) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

AnalyzerDescriptor built_in_descriptor(std::string id,
                                       std::string display_name,
                                       std::vector<AnalyzerCapability> capabilities,
                                       AnalyzerExecutionContext execution_context) {
    AnalyzerDescriptor descriptor;
    descriptor.id = std::move(id);
    descriptor.display_name = std::move(display_name);
    descriptor.version = "builtin";
    descriptor.license_id = "MIT";
    descriptor.backend = AnalyzerBackend::BuiltIn;
    descriptor.availability = AnalyzerAvailability::Available;
    descriptor.license_policy = AnalyzerLicensePolicy::Permissive;
    descriptor.capabilities = std::move(capabilities);
    descriptor.execution_context = execution_context;
    descriptor.supports_streaming_input = false;
    descriptor.supports_offline_buffers = true;
    return descriptor;
}

bool license_allowed(AnalyzerLicensePolicy license,
                     const AnalyzerSelectionPolicy& policy) noexcept {
    switch (license) {
        case AnalyzerLicensePolicy::Permissive:
        case AnalyzerLicensePolicy::NoticeRequired:
            return true;
        case AnalyzerLicensePolicy::Copyleft:
            return policy.include_copyleft;
        case AnalyzerLicensePolicy::Commercial:
            return policy.include_commercial;
        case AnalyzerLicensePolicy::Unknown:
            return policy.include_unknown_license;
    }
    return false;
}

}  // namespace

const char* analyzer_capability_name(AnalyzerCapability capability) noexcept {
    switch (capability) {
        case AnalyzerCapability::OnsetDetection:
            return "onset-detection";
        case AnalyzerCapability::BeatDetection:
            return "beat-detection";
        case AnalyzerCapability::PitchDetection:
            return "pitch-detection";
        case AnalyzerCapability::SliceAnalysis:
            return "slice-analysis";
        case AnalyzerCapability::LoopPointAnalysis:
            return "loop-point-analysis";
        case AnalyzerCapability::KeyDetection:
            return "key-detection";
        case AnalyzerCapability::TempoDetection:
            return "tempo-detection";
        case AnalyzerCapability::TransientClassification:
            return "transient-classification";
        case AnalyzerCapability::TimeStretch:
            return "time-stretch";
        case AnalyzerCapability::PitchShift:
            return "pitch-shift";
    }
    return "unknown";
}

const char* analyzer_backend_name(AnalyzerBackend backend) noexcept {
    switch (backend) {
        case AnalyzerBackend::BuiltIn:
            return "built-in";
        case AnalyzerBackend::Package:
            return "package";
        case AnalyzerBackend::ExternalProcess:
            return "external-process";
        case AnalyzerBackend::ImportedMetadata:
            return "imported-metadata";
    }
    return "unknown";
}

const char* analyzer_availability_name(AnalyzerAvailability availability) noexcept {
    switch (availability) {
        case AnalyzerAvailability::Available:
            return "available";
        case AnalyzerAvailability::MissingPackage:
            return "missing-package";
        case AnalyzerAvailability::LicenseNotAccepted:
            return "license-not-accepted";
        case AnalyzerAvailability::UnsupportedPlatform:
            return "unsupported-platform";
        case AnalyzerAvailability::Disabled:
            return "disabled";
        case AnalyzerAvailability::Unavailable:
            return "unavailable";
    }
    return "unknown";
}

const char* analyzer_license_policy_name(AnalyzerLicensePolicy policy) noexcept {
    switch (policy) {
        case AnalyzerLicensePolicy::Permissive:
            return "permissive";
        case AnalyzerLicensePolicy::NoticeRequired:
            return "notice-required";
        case AnalyzerLicensePolicy::Copyleft:
            return "copyleft";
        case AnalyzerLicensePolicy::Commercial:
            return "commercial";
        case AnalyzerLicensePolicy::Unknown:
            return "unknown";
    }
    return "unknown";
}

const char* analyzer_execution_context_name(AnalyzerExecutionContext context) noexcept {
    switch (context) {
        case AnalyzerExecutionContext::OfflineOnly:
            return "offline-only";
        case AnalyzerExecutionContext::BackgroundThread:
            return "background-thread";
        case AnalyzerExecutionContext::RealtimeSafe:
            return "realtime-safe";
    }
    return "unknown";
}

const char* analyzer_descriptor_status_name(AnalyzerDescriptorStatus status) noexcept {
    switch (status) {
        case AnalyzerDescriptorStatus::ok:
            return "ok";
        case AnalyzerDescriptorStatus::empty_id:
            return "empty-id";
        case AnalyzerDescriptorStatus::empty_display_name:
            return "empty-display-name";
        case AnalyzerDescriptorStatus::missing_capability:
            return "missing-capability";
        case AnalyzerDescriptorStatus::package_id_required:
            return "package-id-required";
        case AnalyzerDescriptorStatus::duplicate_id:
            return "duplicate-id";
    }
    return "unknown";
}

bool analyzer_descriptor_has_capability(const AnalyzerDescriptor& descriptor,
                                        AnalyzerCapability capability) noexcept {
    return std::find(descriptor.capabilities.begin(),
                     descriptor.capabilities.end(),
                     capability) != descriptor.capabilities.end();
}

AnalyzerDescriptorStatus validate_analyzer_descriptor(
    const AnalyzerDescriptor& descriptor) noexcept {
    if (descriptor.id.empty()) return AnalyzerDescriptorStatus::empty_id;
    if (descriptor.display_name.empty()) return AnalyzerDescriptorStatus::empty_display_name;
    if (descriptor.capabilities.empty()) return AnalyzerDescriptorStatus::missing_capability;
    if (descriptor.backend == AnalyzerBackend::Package && descriptor.package_id.empty()) {
        return AnalyzerDescriptorStatus::package_id_required;
    }
    return AnalyzerDescriptorStatus::ok;
}

bool analyzer_descriptor_selectable(const AnalyzerDescriptor& descriptor,
                                    AnalyzerCapability capability,
                                    const AnalyzerSelectionPolicy& policy) noexcept {
    if (validate_analyzer_descriptor(descriptor) != AnalyzerDescriptorStatus::ok) return false;
    if (!analyzer_descriptor_has_capability(descriptor, capability)) return false;
    if (!policy.include_unavailable &&
        descriptor.availability != AnalyzerAvailability::Available) {
        return false;
    }
    if (!license_allowed(descriptor.license_policy, policy)) return false;
    if (policy.require_real_time_safe &&
        descriptor.execution_context != AnalyzerExecutionContext::RealtimeSafe) {
        return false;
    }
    if (policy.require_offline_buffers && !descriptor.supports_offline_buffers) return false;
    if (!policy.allow_deferred_execution &&
        descriptor.execution_context != AnalyzerExecutionContext::RealtimeSafe) {
        return false;
    }
    return true;
}

AnalyzerLicensePolicy classify_analyzer_license_policy(std::string_view license_id) noexcept {
    if (license_id.empty()) return AnalyzerLicensePolicy::Unknown;
    if (contains_ascii_case_insensitive(license_id, "gpl") ||
        contains_ascii_case_insensitive(license_id, "agpl") ||
        contains_ascii_case_insensitive(license_id, "lgpl")) {
        return AnalyzerLicensePolicy::Copyleft;
    }
    if (contains_ascii_case_insensitive(license_id, "commercial") ||
        contains_ascii_case_insensitive(license_id, "proprietary")) {
        return AnalyzerLicensePolicy::Commercial;
    }
    if (contains_ascii_case_insensitive(license_id, "mit") ||
        contains_ascii_case_insensitive(license_id, "bsd") ||
        contains_ascii_case_insensitive(license_id, "apache") ||
        contains_ascii_case_insensitive(license_id, "isc") ||
        contains_ascii_case_insensitive(license_id, "zlib") ||
        contains_ascii_case_insensitive(license_id, "public-domain") ||
        contains_ascii_case_insensitive(license_id, "unlicense")) {
        return AnalyzerLicensePolicy::Permissive;
    }
    if (contains_ascii_case_insensitive(license_id, "mpl") ||
        contains_ascii_case_insensitive(license_id, "notice") ||
        contains_ascii_case_insensitive(license_id, "attribution")) {
        return AnalyzerLicensePolicy::NoticeRequired;
    }
    return AnalyzerLicensePolicy::Unknown;
}

AnalyzerAvailability infer_package_analyzer_availability(bool installed,
                                                         bool platform_supported,
                                                         bool license_accepted) noexcept {
    if (!platform_supported) return AnalyzerAvailability::UnsupportedPlatform;
    if (!installed) return AnalyzerAvailability::MissingPackage;
    if (!license_accepted) return AnalyzerAvailability::LicenseNotAccepted;
    return AnalyzerAvailability::Available;
}

AnalyzerDescriptor make_package_analyzer_descriptor(
    const PackageAnalyzerDescriptorInput& input) {
    AnalyzerDescriptor descriptor;
    descriptor.id = !input.provider_id.empty()
                        ? input.provider_id
                        : (input.package_id.empty() ? std::string{} : "package." + input.package_id);
    descriptor.display_name = input.display_name;
    descriptor.package_id = input.package_id;
    descriptor.version = input.version;
    descriptor.license_id = input.license_id;
    descriptor.url = input.url;
    descriptor.backend = AnalyzerBackend::Package;
    descriptor.availability = infer_package_analyzer_availability(input.installed,
                                                                  input.platform_supported,
                                                                  input.license_accepted);
    descriptor.license_policy = classify_analyzer_license_policy(input.license_id);
    descriptor.capabilities = input.capabilities;
    descriptor.execution_context = input.execution_context;
    descriptor.supports_streaming_input = input.supports_streaming_input;
    descriptor.supports_offline_buffers = input.supports_offline_buffers;
    return descriptor;
}

AnalyzerProvenance analyzer_provenance_from_descriptor(const AnalyzerDescriptor& descriptor,
                                                       std::string analysis_id) {
    AnalyzerProvenance provenance;
    provenance.provider_id = descriptor.id;
    provenance.package_id = descriptor.package_id;
    provenance.version = descriptor.version;
    provenance.analysis_id = std::move(analysis_id);
    provenance.backend = descriptor.backend;
    return provenance;
}

bool validate_analyzer_marker_provenance(
    std::size_t marker_count,
    std::span<const AnalyzerMarkerProvenance> provenance) noexcept {
    bool have_previous = false;
    std::uint32_t previous = 0;
    for (const auto& item : provenance) {
        if (item.marker_index >= marker_count) return false;
        if (item.provenance.provider_id.empty()) return false;
        if (have_previous && item.marker_index <= previous) return false;
        previous = item.marker_index;
        have_previous = true;
    }
    return true;
}

const AnalyzerProvenance* find_analyzer_marker_provenance(
    std::span<const AnalyzerMarkerProvenance> provenance,
    std::uint32_t marker_index) noexcept {
    for (const auto& item : provenance) {
        if (item.marker_index == marker_index) return &item.provenance;
    }
    return nullptr;
}

std::vector<AnalyzerDescriptor> built_in_analyzer_descriptors() {
    return {
        built_in_descriptor("builtin.onset-detector",
                            "Built-in Onset Detector",
                            {AnalyzerCapability::OnsetDetection},
                            AnalyzerExecutionContext::BackgroundThread),
        built_in_descriptor("builtin.slice-point-analyzer",
                            "Built-in Slice Point Analyzer",
                            {AnalyzerCapability::SliceAnalysis},
                            AnalyzerExecutionContext::BackgroundThread),
        built_in_descriptor("builtin.loop-point-analyzer",
                            "Built-in Loop Point Analyzer",
                            {AnalyzerCapability::LoopPointAnalysis},
                            AnalyzerExecutionContext::BackgroundThread),
    };
}

AnalyzerDescriptorStatus AnalyzerProviderRegistry::add(AnalyzerDescriptor descriptor) {
    const auto status = validate_analyzer_descriptor(descriptor);
    if (status != AnalyzerDescriptorStatus::ok) return status;
    if (find(descriptor.id) != nullptr) return AnalyzerDescriptorStatus::duplicate_id;
    descriptors_.push_back(std::move(descriptor));
    return AnalyzerDescriptorStatus::ok;
}

const AnalyzerDescriptor* AnalyzerProviderRegistry::find(std::string_view id) const noexcept {
    const auto it = std::find_if(descriptors_.begin(), descriptors_.end(), [id](const auto& item) {
        return item.id == id;
    });
    return it == descriptors_.end() ? nullptr : &*it;
}

std::vector<AnalyzerDescriptor> AnalyzerProviderRegistry::descriptors() const {
    return descriptors_;
}

std::vector<AnalyzerDescriptor> AnalyzerProviderRegistry::descriptors_for(
    AnalyzerCapability capability,
    const AnalyzerSelectionPolicy& policy) const {
    std::vector<AnalyzerDescriptor> selected;
    for (const auto& descriptor : descriptors_) {
        if (analyzer_descriptor_selectable(descriptor, capability, policy)) {
            selected.push_back(descriptor);
        }
    }
    return selected;
}

}  // namespace pulp::audio
