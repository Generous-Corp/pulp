#include <pulp/signal/signal.hpp>
#include <pulp/signal/headphone_crossfeed.hpp>

#include <cmath>
#include <limits>

int main() {
    pulp::signal::MirroredHistoryBuffer<float> mirrored_history;
    if (!mirrored_history.prepare(4u))
        return 1;
    mirrored_history.push(1.0f);
    if (mirrored_history.prepare(std::numeric_limits<std::size_t>::max() / 2u + 1u))
        return 2;
    if (mirrored_history.capacity() != 4u || mirrored_history.window().back() != 1.0f)
        return 3;

    pulp::signal::FractionalDelayHistory fractional_history;
    if (!fractional_history.prepare(8u))
        return 4;
    pulp::signal::FractionalDelayLine fractional_delay;
    if (!fractional_delay.prepare(8u, pulp::signal::FractionalDelayMethod::lagrange3))
        return 5;
    pulp::signal::AudioMatrixMixer matrix;
    if (!matrix.prepare(16u))
        return 6;
    pulp::signal::SpectralFrameBlur spectral_blur;
    if (!spectral_blur.prepare(1, 8, 2))
        return 7;
    pulp::signal::SpectralMorph spectral_morph;
    if (!spectral_morph.prepare(1, 129))
        return 8;
    pulp::signal::PathLatencyAligner aligner;
    if (!aligner.prepare(2u, 1u, 8u, 16u))
        return 9;
    pulp::signal::CrossFeedbackMultitapDelay multitap_delay;
    if (!multitap_delay.prepare(48000.0, 100.0))
        return 10;
    pulp::signal::WaveguideJunction waveguide_junction;
    const float waveguide_in[2] {0.25f, -0.5f};
    float waveguide_out[2] {};
    waveguide_junction.scatter(waveguide_in, waveguide_out, 2);
    if (!std::isfinite(waveguide_out[0]) || !std::isfinite(waveguide_out[1]))
        return 14;
    pulp::signal::Expander expander;
    if (expander.prepare(48000.0f) != pulp::signal::ExpanderStatus::ready)
        return 11;
    const auto output = expander.process(0.25f, -0.5f);
    if (!std::isfinite(output[0]) || !std::isfinite(output[1]))
        return 12;
    pulp::signal::SpectralBandLayout layout;
    pulp::signal::SpectralMaskTable mask;
    return pulp::signal::build_spectral_mask(layout, 1024, 48000.0f, mask)
        ? 0 : 13;
}
