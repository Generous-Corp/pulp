#pragma once

/// @file audio_signal_generators.hpp
/// Deterministic stimulus generators and event scripts.
///
/// Test/tool layer only. Every generator here is deterministic by
/// construction: identical arguments produce bit-identical buffers on a
/// given platform. There is no `std::random_device`, no wall clock, and no
/// global state — noise generators take an explicit seed and document the
/// exact PRNG expression. Sine stimulus lives in audio_test_signals.hpp
/// (`make_sine` / `make_sine_vec`) and is re-exported via that include.
///
/// All multi-channel generators write the same signal to every channel
/// except the noise generators, which decorrelate channels by deriving a
/// per-channel sub-seed (documented below). Layering rule (see README.md):
/// this is the bottom "signals" layer — it must not include metrics,
/// assertions, artifacts, or scenario headers.

#include "audio_test_signals.hpp"

#include <pulp/midi/message.hpp>
#include <pulp/state/parameter.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace pulp::test::audio {

/// All-zero buffer (explicit name for scenario readability).
pulp::audio::Buffer<float> make_silence(int channels, int frames);

/// Constant `level` on every sample.
pulp::audio::Buffer<float> make_dc(int channels, int frames, float level);

/// Single `amplitude` sample at `position` (clamped to the buffer), zeros
/// elsewhere. The canonical impulse-response stimulus.
pulp::audio::Buffer<float> make_impulse(int channels, int frames,
                                        float amplitude = 1.0f,
                                        int position = 0);

/// Impulses of `amplitude` every `period_frames` starting at frame 0.
/// `period_frames` must be >= 1.
pulp::audio::Buffer<float> make_impulse_train(int channels, int frames,
                                              int period_frames,
                                              float amplitude = 1.0f);

/// Zeros before `onset_frame`, constant `level` from `onset_frame` on.
pulp::audio::Buffer<float> make_step(int channels, int frames, float level,
                                     int onset_frame = 0);

/// One component of a multi-sine stimulus.
struct SinePartial {
    double hz = 0.0;
    double amplitude = 0.0; ///< Linear (1.0 == full scale).
};

/// Sum of sine partials, each `amplitude * sin(2π hz i / sample_rate)`,
/// accumulated in double and narrowed to float on store. Phase starts at 0
/// for every partial.
pulp::audio::Buffer<float> make_multi_sine(int channels, int frames,
                                           std::span<const SinePartial> partials,
                                           double sample_rate);

/// Exponential (log-frequency) swept sine from `start_hz` to `end_hz`
/// across the whole buffer.
///
/// Determinism contract: double-precision phase accumulation,
/// `f(i) = start_hz * pow(end_hz / start_hz, i / (frames - 1))`,
/// `phase += f(i) / sample_rate`, sample = `amplitude * sin(2π phase)`.
/// No FFT, no randomness. Frequencies must be positive.
pulp::audio::Buffer<float> make_swept_sine(int channels, int frames,
                                           double start_hz, double end_hz,
                                           double sample_rate,
                                           float amplitude = 1.0f);

/// Naive (mathematically ideal) sawtooth ramp — **aliased by construction**.
///
/// This is the closed-form ideal saw, point-sampled with no bandlimiting:
/// every harmonic above Nyquist folds back into the baseband. That is the
/// point. It exists to be the naive reference an anti-aliased oscillator is
/// measured *against*, and to be broadband stimulus with energy at every
/// harmonic of `f0_hz`. Do not "fix" the aliasing here — an alias-free saw
/// would make it useless for both jobs.
///
/// Not to be confused with `WavetableT::make_saw` in
/// `core/signal/wavetable.hpp`, which is a **bandlimited** wavetable factory
/// for the shipped signal path. Same name, opposite contract: that one
/// suppresses aliases, this one guarantees them.
///
/// Determinism contract: closed form per sample (no phase accumulation, so
/// sample `i` never depends on the block partition):
/// `t = phase + f0_hz * i / sample_rate; p = t - floor(t);`
/// `sample = amplitude * (2p - 1)`. The ramp runs -amplitude → +amplitude
/// across each cycle and steps discontinuously back at the wrap. `phase` is
/// in **cycles** (turns), not radians, and is wrapped into [0, 1).
/// `sample_rate` must be > 0; `f0_hz` may be negative (reverses the ramp).
pulp::audio::Buffer<float> make_saw(int channels, int frames, double f0_hz,
                                    double sample_rate,
                                    float amplitude = 1.0f,
                                    double phase = 0.0);

/// Naive (mathematically ideal) square / pulse wave — **aliased by
/// construction**, for the same reasons and the same purpose as `make_saw`
/// above (naive reference + broadband stimulus; do not bandlimit it). The
/// bandlimited counterpart is `WavetableT::make_square` in
/// `core/signal/wavetable.hpp` — same name, opposite contract.
///
/// Determinism contract: closed form per sample, same phase expression as
/// `make_saw` (`p = t - floor(t)` with `t = phase + f0_hz * i / sample_rate`),
/// then `sample = p < pulse_width ? +amplitude : -amplitude`. The comparison
/// is strict, so a `pulse_width` of exactly 0.5 puts the sample at p == 0.5
/// on the *low* half — 48 kHz / 1 kHz gives exactly 24 high and 24 low
/// samples per cycle.
///
/// `pulse_width` is the duty cycle in [0, 1] and defaults to 0.5 (an even
/// square). The degenerate ends are legal DC and deliberately not clamped
/// away: 0 is constant -amplitude, 1 is constant +amplitude. `phase` is in
/// cycles as above. `sample_rate` must be > 0.
pulp::audio::Buffer<float> make_square(int channels, int frames, double f0_hz,
                                       double sample_rate,
                                       float amplitude = 1.0f,
                                       double pulse_width = 0.5,
                                       double phase = 0.0);

/// Deterministic uniform white noise in [-amplitude, amplitude).
///
/// PRNG (the whole determinism contract): xorshift64* seeded through one
/// SplitMix64 scramble of `seed + 0x9E3779B97F4A7C15 * (channel + 1)` — an
/// additive golden-ratio per-channel offset (NOT an XOR) — so channel 0 and
/// channel 1 are decorrelated but each is fully reproducible from `seed`.
/// Expression per sample: `x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
/// u = x * 0x2545F4914F6CDD1D; sample = amplitude * ((u >> 11) / 2^53 * 2 - 1)`.
/// A zero-state lock is impossible because SplitMix64 never yields 0 for
/// distinct inputs mapped through its finalizer (we also OR in 1).
pulp::audio::Buffer<float> make_white_noise(int channels, int frames,
                                            std::uint64_t seed,
                                            float amplitude = 1.0f);

/// Deterministic pink (1/f-ish) noise: the white generator above filtered
/// per channel by Paul Kellett's "economy" 3-pole pinking filter
/// (b0 = 0.99765 b0 + w·0.0990460; b1 = 0.96300 b1 + w·0.2965164;
/// b2 = 0.57000 b2 + w·1.0526913; pink = b0 + b1 + b2 + w·0.1848),
/// scaled by `amplitude * 0.25`. The result is *statistically* inside
/// ±amplitude (RMS ≈ 0.2·amplitude) but the filter gives no hard peak
/// bound — leave headroom rather than passing amplitude 1.0 into a clip
/// assertion. Same seed → same buffer; the filter is sample-rate-agnostic
/// (same sample sequence at any rate), so declare the rate in the
/// scenario if spectral shape matters.
pulp::audio::Buffer<float> make_pink_noise(int channels, int frames,
                                           std::uint64_t seed,
                                           float amplitude = 1.0f);

/// Deterministic brown (red) noise: leaky integration of the white
/// generator, `acc = 0.998 acc + w · 0.02`, then `amplitude * acc` with a
/// final clamp to ±amplitude (acc std ≈ 0.18, so the clamp at |acc| > 1 is
/// ~5.5σ — vanishingly rare). Same determinism contract as white noise.
pulp::audio::Buffer<float> make_brown_noise(int channels, int frames,
                                            std::uint64_t seed,
                                            float amplitude = 1.0f);

// ── Event scripts ───────────────────────────────────────────────────────
// Scripts schedule events at absolute frame positions within a render.
// RenderScenario (the layer above) translates them to per-block offsets,
// so a script is partition-independent by construction; see
// render_scenario.hpp for the exact application semantics.

/// One scheduled parameter change: `value` is applied to the StateStore at
/// the start of the host block containing `frame` (block-quantized — align
/// `frame` to a common multiple of the tested block sizes when a scenario
/// must be partition-invariant).
struct ParamStep {
    pulp::state::ParamID id = 0;
    std::int64_t frame = 0;
    float value = 0.0f;
};

/// One scheduled MIDI event. `event.sample_offset` is ignored; the
/// scenario rewrites it to `frame - block_start` (sample-accurate).
struct MidiScriptEvent {
    std::int64_t frame = 0;
    pulp::midi::MidiEvent event;
};

/// Stepped automation: `values[k]` applied at
/// `start_frame + k * interval_frames`. `interval_frames` must be >= 1.
std::vector<ParamStep> make_stepped_automation(pulp::state::ParamID id,
                                               std::span<const float> values,
                                               std::int64_t start_frame,
                                               std::int64_t interval_frames);

/// Note-on at `on_frame`, note-off at `off_frame` (omit the off event by
/// passing `off_frame < 0`).
std::vector<MidiScriptEvent> make_note_script(std::uint8_t note,
                                              std::uint8_t velocity,
                                              std::int64_t on_frame,
                                              std::int64_t off_frame = -1,
                                              std::uint8_t channel = 0);

} // namespace pulp::test::audio
