#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <pulp/audio/buffer.hpp>
#include <pulp/signal/analysis_frontends.hpp>

namespace pulp::audio {

using OnsetDetectionMethod = signal::OnsetDetectionMethod;

struct OnsetDetectionConfig {
    OnsetDetectionMethod method = OnsetDetectionMethod::EnergyFlux;
    // SpectralFlux, HighFrequencyContent and SuperFlux use the existing radix-2
    // FFT, so `frame_size` must be a power of two for those methods.
    std::uint32_t frame_size = 1024;
    std::uint32_t hop_size = 256;
    std::uint32_t adaptive_window_frames = 8;
    std::uint64_t min_spacing_frames = 1024;
    double threshold_multiplier = 1.5;
    double min_confidence = 0.05;
    std::size_t max_markers = 1024;
    // Sample rate of `source`. Only SuperFlux reads it (its filterbank is laid
    // out in hertz). The default keeps the other methods' behavior unchanged.
    double sample_rate = 48000.0;
    // SuperFlux settings; ignored by the other methods. SuperFlux needs
    // `frame_size` <= 65536, at most 256 channels and `hop_size` <= `frame_size`,
    // and reports each marker at the centre of the analysis window that
    // produced it, where its novelty peaks (the other methods report the
    // window start).
    signal::SuperFluxConfig superflux{};
};

// Published SuperFlux settings for `sample_rate`: 2048-sample Hann window, a
// 5 ms hop, a longer adaptive-threshold neighbourhood than the default, and a
// minimum confidence of 0.1 so the weak tails of a decaying hit are not picked.
inline OnsetDetectionConfig superflux_onset_config(double sample_rate) {
    OnsetDetectionConfig config;
    config.method = OnsetDetectionMethod::SuperFlux;
    config.sample_rate = sample_rate;
    config.frame_size = 2048;
    config.hop_size =
        static_cast<std::uint32_t>(sample_rate > 0.0 ? sample_rate / 200.0 + 0.5 : 256);
    config.adaptive_window_frames = 16;
    config.min_spacing_frames =
        static_cast<std::uint64_t>(sample_rate > 0.0 ? sample_rate * 0.03 : 1024);
    config.threshold_multiplier = 2.0;
    config.min_confidence = 0.1;
    return config;
}

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

} // namespace pulp::audio
