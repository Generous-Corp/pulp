#pragma once

/// @file audio_doctor.hpp
/// Scenario-driven offline "Audio Doctor" analyzers.
///
/// The Doctor is the top harness layer: it sits ABOVE scenarios and drives a
/// processor through `RenderScenario`, then runs the buffer-level spectrum
/// analyzers in `pulp-audio-analysis` (audio_spectrum.hpp) over the rendered
/// output to answer the "is this DSP behaving correctly?" questions an
/// interactive analyzer answers visually — magnitude/frequency response and
/// harmonic distortion.
///
/// This header owns only the scenario wiring (stimulus synthesis + render);
/// the windowing/FFT/THD/response math lives in `audio_spectrum.hpp` so the
/// `pulp audio validate doctor` CLI can run the same analysis over decoded
/// WAVs without a processor. The result/option structs (`ResponseCurve`,
/// `ThdResult`, `ResponseOptions`, `ThdOptions`, `Window`, ...) come straight
/// from `audio_spectrum.hpp`; this header re-exports them so existing test
/// includes are unchanged.
///
/// Layering (see README.md): signals → metrics → assertions → artifacts →
/// scenarios → contracts, and the Doctor on top. It includes the scenario
/// layer and `audio_spectrum.hpp`; nothing below scenarios may include this
/// header. Target-boundary rule (harness plan §9): the heavy FFT lives in the
/// tool-only `pulp-audio-analysis` lib; this scenario wiring is test-only.
///
/// Test/tool layer only — analysis happens entirely off the audio thread.

#include "render_scenario.hpp"

#include <pulp/audio/analysis/audio_spectrum.hpp>
#include <pulp/state/parameter.hpp>

#include <span>

namespace pulp::test::audio {

/// Drive `scenario` with a unit impulse and return the magnitude response,
/// sampled at `checkpoints_hz` plus the full-resolution curve. The scenario's
/// own input/duration are overridden: a single impulse is rendered for
/// `options.analysis_offset + options.fft_length` samples, so the captured
/// segment beginning at `analysis_offset` is a full `fft_length` of impulse
/// response rather than a zero-padded tail. Parameter/MIDI scripts and
/// `set_param` calls are preserved.
///
/// Delegates the spectral math to the buffer-level
/// `response_relative_to_input(input, output, ...)` in audio_spectrum.hpp.
ResponseCurve response_relative_to_input(const RenderScenario& scenario,
                                         std::span<const double> checkpoints_hz,
                                         const ResponseOptions& options = {});

/// Drive `scenario` with a unit impulse and return the phase / group-delay
/// curve, sampled at `checkpoints_hz` plus the full-resolution curve. As with
/// the magnitude response, the scenario's own input/duration are overridden:
/// the impulse is rendered for `options.analysis_offset + options.fft_length`
/// samples, so the analysis segment beginning at `analysis_offset` is a full
/// `fft_length` long. Parameter/MIDI scripts and `set_param` calls are
/// preserved.
///
/// `options.analysis_offset` must be 0 here — the reference impulse sits at
/// frame 0, so any nonzero offset leaves the reference window silent and
/// throws. See `GroupDelayOptions::analysis_offset`.
///
/// Group delay is reported in samples at the scenario's sample rate, positive
/// for a causal delay. Bins in a stopband are reported undefined rather than
/// given a number read out of the noise floor — read `defined_at(hz)` first.
///
/// Delegates the spectral math to the buffer-level
/// `measure_group_delay(input, output, ...)` in audio_spectrum.hpp, which
/// documents the estimator, the unwrapping method, and the stopband contract.
PhaseCurve measure_group_delay(const RenderScenario& scenario,
                               std::span<const double> checkpoints_hz,
                               const GroupDelayOptions& options = {});

/// Drive `scenario` with a steady sine at `fundamental_hz` and measure THD /
/// THD+N from the rendered output. The scenario's input/duration are
/// overridden with the analysis sine; parameter/MIDI scripts are preserved
/// (so a clipping/waveshaping setup driven by `set_param` is honored).
///
/// Delegates the spectral math to the buffer-level
/// `measure_thd(signal, fundamental_hz, sample_rate, ...)` in audio_spectrum.hpp.
/// `options.amplitude` sets the synthesized sine level driven into the input.
ThdResult measure_thd(const RenderScenario& scenario, double fundamental_hz,
                      const ThdOptions& options = {});

} // namespace pulp::test::audio
