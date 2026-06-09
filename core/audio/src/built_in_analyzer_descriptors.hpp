#pragma once

#include <pulp/audio/analyzer_provider.hpp>

#include <string>
#include <utility>
#include <vector>

namespace pulp::audio::detail {

inline AnalyzerDescriptor make_built_in_analyzer_descriptor(
    std::string id,
    std::string display_name,
    std::vector<AnalyzerCapability> capabilities,
    AnalyzerExecutionContext execution_context,
    bool is_fallback = false) {
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
    descriptor.is_fallback = is_fallback;
    return descriptor;
}

inline AnalyzerDescriptor make_built_in_key_tempo_analyzer_descriptor() {
    return make_built_in_analyzer_descriptor(
        "builtin.key-tempo-analyzer",
        "Built-in Basic Key/Tempo Analyzer",
        {AnalyzerCapability::KeyDetection, AnalyzerCapability::TempoDetection},
        AnalyzerExecutionContext::BackgroundThread,
        true);
}

inline AnalyzerDescriptor make_built_in_transient_classifier_descriptor() {
    return make_built_in_analyzer_descriptor(
        "builtin.transient-classifier",
        "Built-in Basic Transient Classifier",
        {AnalyzerCapability::TransientClassification},
        AnalyzerExecutionContext::BackgroundThread,
        true);
}

}  // namespace pulp::audio::detail
