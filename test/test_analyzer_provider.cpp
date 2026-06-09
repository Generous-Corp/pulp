#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/analyzer_provider.hpp>

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

using pulp::audio::AnalyzerAvailability;
using pulp::audio::AnalyzerBackend;
using pulp::audio::AnalyzerCapability;
using pulp::audio::AnalyzerDescriptor;
using pulp::audio::AnalyzerDescriptorStatus;
using pulp::audio::AnalyzerExecutionContext;
using pulp::audio::AnalyzerLicensePolicy;
using pulp::audio::AnalyzerMarkerProvenance;
using pulp::audio::AnalyzerProviderRegistry;
using pulp::audio::AnalyzerSelectionPolicy;
using pulp::audio::PackageAnalyzerDescriptorInput;
using pulp::audio::analyzer_capability_name;
using pulp::audio::analyzer_descriptor_selectable;
using pulp::audio::analyzer_execution_context_name;
using pulp::audio::analyzer_provenance_from_descriptor;
using pulp::audio::built_in_analyzer_descriptors;
using pulp::audio::classify_analyzer_license_policy;
using pulp::audio::find_analyzer_marker_provenance;
using pulp::audio::make_package_analyzer_descriptor;
using pulp::audio::validate_analyzer_descriptor;
using pulp::audio::validate_analyzer_marker_provenance;

namespace {

AnalyzerDescriptor valid_builtin_descriptor() {
    AnalyzerDescriptor descriptor;
    descriptor.id = "test.builtin.onset";
    descriptor.display_name = "Test Built-in Onset";
    descriptor.version = "builtin";
    descriptor.license_id = "MIT";
    descriptor.backend = AnalyzerBackend::BuiltIn;
    descriptor.availability = AnalyzerAvailability::Available;
    descriptor.license_policy = AnalyzerLicensePolicy::Permissive;
    descriptor.capabilities = {AnalyzerCapability::OnsetDetection};
    descriptor.execution_context = AnalyzerExecutionContext::BackgroundThread;
    descriptor.supports_offline_buffers = true;
    return descriptor;
}

}  // namespace

TEST_CASE("AnalyzerProvider validates stable descriptors",
          "[audio][analysis][provider]") {
    auto descriptor = valid_builtin_descriptor();
    REQUIRE(validate_analyzer_descriptor(descriptor) == AnalyzerDescriptorStatus::ok);
    REQUIRE(analyzer_descriptor_selectable(descriptor,
                                           AnalyzerCapability::OnsetDetection));
    REQUIRE_FALSE(analyzer_descriptor_selectable(descriptor,
                                                 AnalyzerCapability::TempoDetection));

    auto empty_id = descriptor;
    empty_id.id.clear();
    REQUIRE(validate_analyzer_descriptor(empty_id) == AnalyzerDescriptorStatus::empty_id);

    auto empty_name = descriptor;
    empty_name.display_name.clear();
    REQUIRE(validate_analyzer_descriptor(empty_name) ==
            AnalyzerDescriptorStatus::empty_display_name);

    auto no_capabilities = descriptor;
    no_capabilities.capabilities.clear();
    REQUIRE(validate_analyzer_descriptor(no_capabilities) ==
            AnalyzerDescriptorStatus::missing_capability);

    auto package_without_id = descriptor;
    package_without_id.backend = AnalyzerBackend::Package;
    package_without_id.package_id.clear();
    REQUIRE(validate_analyzer_descriptor(package_without_id) ==
            AnalyzerDescriptorStatus::package_id_required);
}

TEST_CASE("AnalyzerProviderRegistry rejects duplicate provider ids",
          "[audio][analysis][provider]") {
    AnalyzerProviderRegistry registry;
    auto descriptor = valid_builtin_descriptor();

    REQUIRE(registry.add(descriptor) == AnalyzerDescriptorStatus::ok);
    REQUIRE(registry.add(descriptor) == AnalyzerDescriptorStatus::duplicate_id);
    REQUIRE(registry.size() == 1);
    REQUIRE(registry.find("test.builtin.onset") != nullptr);
    REQUIRE(registry.find("missing") == nullptr);
}

TEST_CASE("AnalyzerProvider filters package availability and licensing outside RT paths",
          "[audio][analysis][provider][packages]") {
    AnalyzerProviderRegistry registry;
    REQUIRE(registry.add(valid_builtin_descriptor()) == AnalyzerDescriptorStatus::ok);

    PackageAnalyzerDescriptorInput aubio;
    aubio.package_id = "aubio";
    aubio.display_name = "Aubio";
    aubio.version = "0.4.9";
    aubio.license_id = "GPL-3.0";
    aubio.capabilities = {AnalyzerCapability::OnsetDetection,
                          AnalyzerCapability::BeatDetection,
                          AnalyzerCapability::TempoDetection};
    aubio.installed = true;
    aubio.platform_supported = true;
    aubio.license_accepted = true;
    aubio.provider_id = "package.aubio.onset-beat-tempo";
    aubio.execution_context = AnalyzerExecutionContext::RealtimeSafe;

    const auto aubio_descriptor = make_package_analyzer_descriptor(aubio);
    REQUIRE(aubio_descriptor.id == "package.aubio.onset-beat-tempo");
    REQUIRE(aubio_descriptor.backend == AnalyzerBackend::Package);
    REQUIRE(aubio_descriptor.package_id == "aubio");
    REQUIRE(aubio_descriptor.execution_context == AnalyzerExecutionContext::RealtimeSafe);
    REQUIRE(aubio_descriptor.availability == AnalyzerAvailability::Available);
    REQUIRE(aubio_descriptor.license_policy == AnalyzerLicensePolicy::Copyleft);
    REQUIRE(registry.add(aubio_descriptor) == AnalyzerDescriptorStatus::ok);

    auto aubio_pitch = aubio;
    aubio_pitch.provider_id = "package.aubio.pitch";
    aubio_pitch.capabilities = {AnalyzerCapability::KeyDetection};
    const auto aubio_pitch_descriptor = make_package_analyzer_descriptor(aubio_pitch);
    REQUIRE(aubio_pitch_descriptor.id == "package.aubio.pitch");
    REQUIRE(aubio_pitch_descriptor.package_id == "aubio");
    REQUIRE(registry.add(aubio_pitch_descriptor) == AnalyzerDescriptorStatus::ok);

    PackageAnalyzerDescriptorInput signalsmith;
    signalsmith.package_id = "signalsmith-stretch";
    signalsmith.display_name = "Signalsmith Stretch";
    signalsmith.version = "1.1.0";
    signalsmith.license_id = "MIT";
    signalsmith.capabilities = {AnalyzerCapability::TimeStretch,
                                AnalyzerCapability::PitchShift};
    signalsmith.installed = false;
    signalsmith.platform_supported = true;
    signalsmith.license_accepted = true;
    signalsmith.execution_context = AnalyzerExecutionContext::RealtimeSafe;
    REQUIRE(registry.add(make_package_analyzer_descriptor(signalsmith)) ==
            AnalyzerDescriptorStatus::ok);

    const auto default_onset = registry.descriptors_for(AnalyzerCapability::OnsetDetection);
    REQUIRE(default_onset.size() == 1);
    REQUIRE(default_onset[0].id == "test.builtin.onset");

    AnalyzerSelectionPolicy allow_copyleft;
    allow_copyleft.include_copyleft = true;
    const auto onset_with_copyleft =
        registry.descriptors_for(AnalyzerCapability::OnsetDetection, allow_copyleft);
    REQUIRE(onset_with_copyleft.size() == 2);

    const auto default_time_pitch = registry.descriptors_for(AnalyzerCapability::TimeStretch);
    REQUIRE(default_time_pitch.empty());

    AnalyzerSelectionPolicy include_missing;
    include_missing.include_unavailable = true;
    const auto missing_time_pitch =
        registry.descriptors_for(AnalyzerCapability::TimeStretch, include_missing);
    REQUIRE(missing_time_pitch.size() == 1);
    REQUIRE(missing_time_pitch[0].availability == AnalyzerAvailability::MissingPackage);

    AnalyzerSelectionPolicy realtime_only;
    realtime_only.allow_deferred_execution = false;
    REQUIRE(registry.descriptors_for(AnalyzerCapability::OnsetDetection,
                                     realtime_only)
                .empty());

    AnalyzerSelectionPolicy realtime_copyleft;
    realtime_copyleft.allow_deferred_execution = false;
    realtime_copyleft.include_copyleft = true;
    const auto realtime_onset = registry.descriptors_for(AnalyzerCapability::OnsetDetection,
                                                         realtime_copyleft);
    REQUIRE(realtime_onset.size() == 1);
    REQUIRE(realtime_onset[0].id == "package.aubio.onset-beat-tempo");
}

TEST_CASE("AnalyzerProvider exposes built-in analysis descriptors",
          "[audio][analysis][provider]") {
    const auto builtins = built_in_analyzer_descriptors();
    REQUIRE(builtins.size() == 5);

    AnalyzerProviderRegistry registry;
    for (const auto& descriptor : builtins) {
        REQUIRE(validate_analyzer_descriptor(descriptor) == AnalyzerDescriptorStatus::ok);
        REQUIRE(descriptor.backend == AnalyzerBackend::BuiltIn);
        REQUIRE(descriptor.package_id.empty());
        REQUIRE(descriptor.availability == AnalyzerAvailability::Available);
        REQUIRE(registry.add(descriptor) == AnalyzerDescriptorStatus::ok);
    }

    REQUIRE(registry.descriptors_for(AnalyzerCapability::OnsetDetection).size() == 1);
    REQUIRE(registry.descriptors_for(AnalyzerCapability::SliceAnalysis).size() == 1);
    REQUIRE(registry.descriptors_for(AnalyzerCapability::LoopPointAnalysis).size() == 1);
    const auto key_descriptors = registry.descriptors_for(AnalyzerCapability::KeyDetection);
    REQUIRE(key_descriptors.size() == 1);
    REQUIRE(key_descriptors[0].is_fallback);
    REQUIRE(registry.descriptors_for(AnalyzerCapability::TempoDetection).size() == 1);
    const auto transient_descriptors =
        registry.descriptors_for(AnalyzerCapability::TransientClassification);
    REQUIRE(transient_descriptors.size() == 1);
    REQUIRE(transient_descriptors[0].is_fallback);
    REQUIRE(registry.descriptors_for(AnalyzerCapability::BeatDetection).empty());
    REQUIRE(std::string_view(analyzer_capability_name(AnalyzerCapability::PitchShift)) ==
            "pitch-shift");
    REQUIRE(std::string_view(analyzer_capability_name(AnalyzerCapability::PitchDetection)) ==
            "pitch-detection");
    REQUIRE(static_cast<std::uint8_t>(AnalyzerCapability::PitchDetection) == 9);
    REQUIRE(std::string_view(analyzer_execution_context_name(
                AnalyzerExecutionContext::BackgroundThread)) == "background-thread");
}

TEST_CASE("AnalyzerProvider keeps marker provenance as sorted sidecar metadata",
          "[audio][analysis][provider][provenance]") {
    auto descriptor = valid_builtin_descriptor();
    const auto provenance = analyzer_provenance_from_descriptor(descriptor, "analysis-17");
    REQUIRE(provenance.provider_id == descriptor.id);
    REQUIRE(provenance.analysis_id == "analysis-17");
    REQUIRE(provenance.backend == AnalyzerBackend::BuiltIn);

    std::vector<AnalyzerMarkerProvenance> sidecar = {
        {0, AnalyzerCapability::OnsetDetection, provenance},
        {2, AnalyzerCapability::OnsetDetection, provenance},
    };
    REQUIRE(validate_analyzer_marker_provenance(3,
                                                std::span<const AnalyzerMarkerProvenance>(
                                                    sidecar.data(), sidecar.size())));

    const auto* marker_two = find_analyzer_marker_provenance(
        std::span<const AnalyzerMarkerProvenance>(sidecar.data(), sidecar.size()), 2);
    REQUIRE(marker_two != nullptr);
    REQUIRE(marker_two->provider_id == descriptor.id);
    REQUIRE(find_analyzer_marker_provenance(
                std::span<const AnalyzerMarkerProvenance>(sidecar.data(), sidecar.size()),
                1) == nullptr);

    auto out_of_range = sidecar;
    out_of_range.back().marker_index = 3;
    REQUIRE_FALSE(validate_analyzer_marker_provenance(
        3,
        std::span<const AnalyzerMarkerProvenance>(out_of_range.data(), out_of_range.size())));

    auto duplicate = sidecar;
    duplicate.back().marker_index = 0;
    REQUIRE_FALSE(validate_analyzer_marker_provenance(
        3,
        std::span<const AnalyzerMarkerProvenance>(duplicate.data(), duplicate.size())));

    auto missing_provider = sidecar;
    missing_provider.front().provenance.provider_id.clear();
    REQUIRE_FALSE(validate_analyzer_marker_provenance(
        3,
        std::span<const AnalyzerMarkerProvenance>(missing_provider.data(),
                                                  missing_provider.size())));
}

TEST_CASE("AnalyzerProvider classifies common package licenses",
          "[audio][analysis][provider][packages]") {
    REQUIRE(classify_analyzer_license_policy("MIT") == AnalyzerLicensePolicy::Permissive);
    REQUIRE(classify_analyzer_license_policy("Apache-2.0") ==
            AnalyzerLicensePolicy::Permissive);
    REQUIRE(classify_analyzer_license_policy("GPL-3.0") == AnalyzerLicensePolicy::Copyleft);
    REQUIRE(classify_analyzer_license_policy("LGPL-2.1") == AnalyzerLicensePolicy::Copyleft);
    REQUIRE(classify_analyzer_license_policy("Commercial") ==
            AnalyzerLicensePolicy::Commercial);
    REQUIRE(classify_analyzer_license_policy("") == AnalyzerLicensePolicy::Unknown);
}
