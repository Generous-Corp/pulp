#include <pulp/signal/spectral_feature_frontends.hpp>

#include <array>
#include <cmath>
#include <cstddef>

int main() {
    constexpr std::size_t fft_size = 8;
    const std::array<float, fft_size> input{0.0f, 0.70710678f,  1.0f,  0.70710678f,
                                            0.0f, -0.70710678f, -1.0f, -0.70710678f};
    const float* channels[] = {input.data()};

    pulp::signal::SpectralFeatureFrontEndT<float, fft_size, 1> front_end;
    const pulp::signal::StreamingAnalysisConfig config{
        .sample_rate = 8000.0,
        .channels = 1,
        .fft_size = fft_size,
        .hop_size = fft_size,
    };
    if (!front_end.prepare(config))
        return 1;

    std::size_t frames = 0;
    bool valid = false;
    const auto sink = [&](const auto& frame) noexcept {
        ++frames;
        valid = frame.valid && std::isfinite(frame.centroid_hz) && std::isfinite(frame.flatness) &&
                std::isfinite(frame.rolloff_hz) && std::isfinite(frame.flux);
    };
    if (!front_end.process(channels, 1, input.size(), sink))
        return 2;
    if (frames != 1 || !valid)
        return 3;
    return 0;
}
