#include <pulp/signal/early_reflections.hpp>
#include <pulp/signal/graphic_eq.hpp>
#include <pulp/signal/headphone_crossfeed.hpp>
#include <pulp/signal/reverse_buffer.hpp>
#include <pulp/signal/signal.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <span>

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
    pulp::signal::Expander expander;
    if (expander.prepare(48000.0f) != pulp::signal::ExpanderStatus::ready)
        return 11;
    const auto output = expander.process(0.25f, -0.5f);
    if (!std::isfinite(output[0]) || !std::isfinite(output[1]))
        return 12;
    const auto exp2 = pulp::signal::FastMath::exp2(0.5f);
    if (!std::isfinite(exp2))
        return 13;
    pulp::signal::TransferCurve transfer_curve;
    if (transfer_curve.process(0.25f) != 0.25f)
        return 19;
    pulp::signal::SpectralBandLayout layout;
    pulp::signal::SpectralMaskTable mask;
    if (!pulp::signal::build_spectral_mask(layout, 1024, 48000.0f, mask))
        return 14;
    pulp::signal::SpectralMaskProcessor processor;
    pulp::signal::SpectralMaskProcessorConfig processor_config;
    processor_config.frame.fft_size = 256;
    processor_config.frame.analysis_hop = 64;
    processor_config.frame.channels = 1;
    processor_config.frame.max_block = 64;
    if (!processor.prepare(processor_config))
        return 15;
    const std::array<pulp::signal::FirDesignPoint, 2> points{
        pulp::signal::FirDesignPoint{0.0, 1.0, 1.0},
        pulp::signal::FirDesignPoint{3.141592653589793, 0.0, 1.0}};
    const auto design = pulp::signal::design_fir_least_squares(
        points, {.tap_count = 3u, .type = pulp::signal::LinearPhaseFirType::type_i_symmetric_odd});
    if (!design)
        return 16;
    pulp::signal::ReverseBuffer reverse;
    if (!reverse.configure({.window_samples = 4}) || !reverse.prepare(8))
        return 17;
    const float reversed = reverse.process_sample(1.0f);
    if (!(std::isfinite(reversed)))
        return 18;
    pulp::signal::EarlyReflections early_reflections;
    const pulp::signal::EarlyReflections::Tap reflection{.delay_ms = 10.0};
    if (!early_reflections.configure(std::span{&reflection, 1u}) ||
        !early_reflections.prepare(48000.0, 100.0))
        return 19;
    float reflected_left = 0.0f;
    float reflected_right = 0.0f;
    early_reflections.process_sample(1.0f, -1.0f, reflected_left, reflected_right);
    if (!std::isfinite(reflected_left) || !std::isfinite(reflected_right))
        return 20;
    pulp::signal::AutoDuckedSend ducked_send;
    if (ducked_send.prepare(48000.0f) != pulp::signal::AutoDuckedSendStatus::ready)
        return 21;
    const auto send_output = ducked_send.process(0.25f, -0.5f, 1.0f, -1.0f);
    if (!(std::isfinite(send_output[0]) && std::isfinite(send_output[1])))
        return 22;
    pulp::signal::CombFilter comb;
    if (!comb.prepare(8u) || !comb.configure({pulp::signal::CombFilterMode::feedback, 4u, 0.5}))
        return 23;
    if (!comb.process(1.0f))
        return 24;
    pulp::signal::GraphicEqT<float, 4> graphic_eq;
    if (graphic_eq.prepare(48000.0f, 4u) != pulp::signal::GraphicEqPrepareStatus::prepared)
        return 25;
    const pulp::signal::GraphicEqBandT<float> graphic_band{1000.0f, 3.0f, 1.0f};
    if (graphic_eq.configure(std::span{&graphic_band, 1u}) !=
        pulp::signal::GraphicEqConfigureStatus::configured)
        return 26;
    if (!std::isfinite(graphic_eq.process(0.25f)))
        return 27;
    pulp::signal::FormantFilterBank formants;
    if (!formants.prepare(48000.0))
        return 28;
    const std::array<pulp::signal::FormantFilterBank::FormantSpec, 1> recipe{{
        {800.0, 80.0, 0.0},
    }};
    return formants.configure(recipe) == pulp::signal::FormantConfigureStatus::configured ? 0 : 29;
}
