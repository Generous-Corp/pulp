#include <pulp/signal/spectral_mask_processor.hpp>

#include <array>
#include <complex>

int main() {
    pulp::signal::SpectralMaskProcessorConfig config;
    config.frame.fft_size = 256;
    config.frame.analysis_hop = 64;
    config.frame.channels = 2;
    config.frame.max_block = 64;

    pulp::signal::SpectralMaskProcessor processor;
    if (!processor.prepare(config) || processor.latency_samples() != 320)
        return 1;

    pulp::signal::SpectralBandLayout layout;
    layout.active_bands = 8;
    layout.min_hz = 20.0f;
    layout.max_hz = 20000.0f;
    layout.transition_frames = 0;
    for (auto& band : layout.bands) band.muted = true;
    layout.bands[1].muted = false;
    layout.bands[6].muted = false;
    if (!processor.publish_layout(layout)) return 2;

    std::array<std::complex<float>, 129> left;
    std::array<std::complex<float>, 129> right;
    left.fill({1.0f, -0.5f});
    right.fill({-0.25f, 0.75f});
    std::complex<float>* frames[] = {left.data(), right.data()};
    if (!processor.process_frame(frames, static_cast<int>(left.size())))
        return 3;
    pulp::signal::SpectralMaskTable expected;
    if (!pulp::signal::build_spectral_mask(layout, 256, 48000.0f, expected))
        return 4;
    bool saw_kept = false;
    bool saw_muted = false;
    for (std::size_t bin = 0; bin < left.size(); ++bin) {
        const auto gain = expected.gain_linear[bin];
        saw_kept |= gain == 1.0f;
        saw_muted |= gain == 0.0f;
        if (left[bin] != std::complex<float>{1.0f, -0.5f} * gain)
            return 5;
        if (right[bin] != std::complex<float>{-0.25f, 0.75f} * gain)
            return 6;
    }
    if (!saw_kept || !saw_muted) return 7;

    for (auto& band : layout.bands) band.muted = true;
    if (!processor.publish_layout(layout)) return 8;
    left.fill({1.0f, -0.5f});
    right.fill({-0.25f, 0.75f});
    if (!processor.process_frame(frames, static_cast<int>(left.size())))
        return 9;
    for (const auto value : left)
        if (value != std::complex<float>{}) return 10;
    for (const auto value : right)
        if (value != std::complex<float>{}) return 11;
    return 0;
}
