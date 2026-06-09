#pragma once

#include <pulp/audio/analyzer_provider.hpp>

namespace pulp::audio {

// Background/offline transient classifier built from simple energy and spectral
// heuristics. It is a permissive fallback metadata source, not a production MIR
// replacement for package-backed drum/source classifiers.
class BuiltInTransientClassifier final : public TransientClassifier {
public:
    BuiltInTransientClassifier();

    [[nodiscard]] const AnalyzerDescriptor& descriptor() const noexcept override;
    [[nodiscard]] std::vector<TransientClassification> classify(
        BufferView<const float> source,
        std::span<const std::uint64_t> candidate_frames) override;

private:
    AnalyzerDescriptor descriptor_;
};

}  // namespace pulp::audio
