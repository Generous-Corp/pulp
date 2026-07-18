// audio_doctor.cpp — scenario-driven Audio Doctor wiring. Synthesizes the
// stimulus, drives the processor through RenderScenario, then delegates all
// spectral math to the buffer-level
// analyzers in pulp-audio-analysis (audio_spectrum.cpp). The per-analyzer
// determinism contracts live in audio_spectrum.hpp.

#include "audio_doctor.hpp"

#include <algorithm>
#include <utility>

namespace pulp::test::audio {

ResponseCurve response_relative_to_input(const RenderScenario& scenario,
                                         std::span<const double> checkpoints_hz,
                                         const ResponseOptions& options) {
    // The captured segment must fit in the render. Render the impulse for the
    // whole analysis span (offset + N) so the late part of the response is
    // present rather than zero-padded by the scenario.
    const int render_len = options.analysis_offset + options.fft_length;

    // A 2-channel unit impulse at frame 0 (input() sets the input channel
    // count to match). The standard effects here are stereo; mono-only
    // processors read channel 0.
    auto impulse = make_impulse(/*channels=*/2, render_len, 1.0f, 0);
    auto driven = scenario;
    auto result = driven.input(impulse)
                      .duration_frames(render_len)
                      .render();

    return response_relative_to_input(std::as_const(impulse).view(),
                                      std::as_const(result.output).view(),
                                      result.sample_rate, checkpoints_hz,
                                      options);
}

PhaseCurve measure_group_delay(const RenderScenario& scenario,
                               std::span<const double> checkpoints_hz,
                               const GroupDelayOptions& options) {
    // Same stimulus and render budget as the magnitude response: the impulse
    // must be rendered across the whole analysis span (offset + N) so the tail
    // of the response is measured rather than zero-padded by the scenario.
    const int render_len = options.analysis_offset + options.fft_length;

    auto impulse = make_impulse(/*channels=*/2, render_len, 1.0f, 0);
    auto driven = scenario;
    auto result = driven.input(impulse)
                      .duration_frames(render_len)
                      .render();

    return measure_group_delay(std::as_const(impulse).view(),
                               std::as_const(result.output).view(),
                               result.sample_rate, checkpoints_hz, options);
}

ThdResult measure_thd(const RenderScenario& scenario, double fundamental_hz,
                      const ThdOptions& options) {
    const int render_len = options.analysis_offset + options.fft_length;

    // Drive the analysis sine through the scenario. A generator input ties the
    // tone to the scenario's effective sample rate, so coherence is judged
    // against the real bin grid the render uses.
    auto driven = scenario;
    const float amplitude = options.amplitude;
    driven.input([fundamental_hz, amplitude](double sr, int ch,
                                              std::int64_t frames) {
        return make_sine(ch, static_cast<int>(frames),
                         static_cast<float>(fundamental_hz), sr, amplitude);
    });
    auto result = driven.duration_frames(render_len).render();

    return measure_thd(std::as_const(result.output).view(), fundamental_hz,
                       result.sample_rate, options);
}

} // namespace pulp::test::audio
