#pragma once

#include <pulp/audio/analyzer_provider.hpp>

namespace pulp::audio {

// Background/offline key and tempo estimator built from Pulp's own onset and
// FFT primitives. Intended as a permissive baseline and fallback for package
// analyzers, not as a realtime callback component.
class BuiltInKeyTempoAnalyzer final : public KeyTempoAnalyzer {
public:
    BuiltInKeyTempoAnalyzer();

    [[nodiscard]] const AnalyzerDescriptor& descriptor() const noexcept override;
    [[nodiscard]] KeyTempoAnalysisResult analyze(
        BufferView<const float> source,
        const KeyTempoAnalysisConfig& config) override;

private:
    AnalyzerDescriptor descriptor_;
};

}  // namespace pulp::audio
