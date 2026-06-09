#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <pulp/audio/buffer.hpp>

namespace pulp::audio {

enum class OnsetDetectionMethod : std::uint8_t {
    EnergyFlux,
    SpectralFlux,
    HighFrequencyContent,
};

struct OnsetDetectionConfig {
    OnsetDetectionMethod method = OnsetDetectionMethod::EnergyFlux;
    // SpectralFlux and HighFrequencyContent use the existing radix-2 FFT,
    // so `frame_size` must be a power of two for those methods.
    std::uint32_t frame_size = 1024;
    std::uint32_t hop_size = 256;
    std::uint32_t adaptive_window_frames = 8;
    std::uint64_t min_spacing_frames = 1024;
    double threshold_multiplier = 1.5;
    double min_confidence = 0.05;
    std::size_t max_markers = 1024;
};

struct OnsetMarker {
    std::uint64_t frame = 0;
    double confidence = 0.0;
    OnsetDetectionMethod method = OnsetDetectionMethod::EnergyFlux;
};

struct OnsetDetectionResult {
    bool ok = false;
    std::uint64_t analyzed_frames = 0;
    std::vector<OnsetMarker> markers;
};

class OnsetDetector {
public:
    // Off-real-time/background analysis. Package-backed analyzers should
    // produce OnsetMarker data for SlicePointAnalyzer rather than entering
    // core/audio as package-specific APIs.
    OnsetDetectionResult detect(BufferView<const float> source,
                                const OnsetDetectionConfig& config = {}) const;
};

}  // namespace pulp::audio
