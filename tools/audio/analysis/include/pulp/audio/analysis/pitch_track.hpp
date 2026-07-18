#pragma once

/// @file pitch_track.hpp
/// Accurate fundamental-frequency estimation for harmonically-dense, bandlimited
/// oscillator output, plus an f0(t) trajectory extractor over a render.
///
/// Why this exists: `audio_metrics.hpp`'s `estimate_frequency` is a
/// positive-going zero-crossing detector, and its own contract disclaims
/// harmonically-dense material. A saw, pulse, or any real oscillator IS dense —
/// intra-period wiggle and Gibbs ripple add spurious crossings, so zero-crossing
/// returns a harmonic multiple or noise. The oscillator suite cannot gate VCO
/// drift/jitter or a DCO pitch-quantization error without a fundamental estimate
/// accurate to a fraction of a cent; that is what `estimate_pitch` provides, and
/// `track_pitch` turns it into the f0(t) demodulator the drift/jitter statistics
/// (Allan deviation, Theil-Sen slope — owned by the Quality Lab, not here)
/// consume.
///
/// Method (two shipped primitives, no new FFT):
///   1. Coarse: the loudest non-DC bin of `magnitude_spectrum_curve`
///      (audio_spectrum.hpp), refined to sub-bin by a parabolic (quadratic) fit
///      over the peak and its two neighbours in the log-magnitude domain.
///   2. Refine: golden-section search that MAXIMISES the shipped `fit_tone`'s
///      fitted energy over a one-bin bracket around the coarse peak. Because
///      `fit_tone` projects a known frequency out of the whole segment by least
///      squares, it has no leakage skirt and no bin quantisation — the fitted
///      energy peaks exactly at the true fundamental, and the search resolves it
///      to well under a cent on a clean tone (proven in the tests).
///
/// Fail-closed discipline (mirrors the rest of the harness): silence or content
/// with no clear fundamental is REFUSED (`voiced = false`), never handed a
/// confident wrong number. Confidence is the fraction of segment energy the
/// fundamental and its harmonics explain — high for a tone or a saw, low for
/// noise, and gated below `PitchOptions::min_confidence`. The estimator also
/// recovers the true fundamental when a HARMONIC is the loudest partial (a
/// rolled-off pulse whose 2nd, 3rd, … or higher partial dominates, or a faint
/// fundamental beneath a loud harmonic): it ranks the coarse peak against its
/// subharmonics (down to coarse/8) and adopts the lowest that both explains the
/// segment and carries real energy at its own fundamental — the tooth measured
/// against a leakage-aware floor, so a short analysis window's spectral leakage is
/// not mistaken for a subharmonic partial.
///
/// Two honest limitations:
///  • A genuine MISSING-FUNDAMENTAL signal — energy only at 2·f0 and 3·f0 with
///    nothing at f0 — has no tone at f0 to lock onto, so the estimate stays on the
///    loudest present partial (2·f0), an octave above the perceived pitch. This is
///    reported as the frequency actually present, not a fabricated f0.
///  • The subharmonic descent stops at coarse/8, so a signal whose loudest partial
///    is the 9th-or-higher harmonic while the fundamental is faint (rare — most
///    oscillators roll off monotonically) can report a lower harmonic rather than
///    the true f0. Both cases return a frequency genuinely present in the signal,
///    never a fabricated one.
///
/// Determinism: pure arithmetic over the supplied buffer (the FFT peak plus a
/// deterministic golden-section search). Identical input yields identical output
/// on a given platform. Test/tool tier only — never linked into a runtime build.

#include "audio_metrics.hpp"  // kSilenceFloorDb, FrequencyEstimate
#include "audio_spectrum.hpp" // Window, kDefaultKaiserBeta, fit_tone

#include <pulp/audio/buffer.hpp>

#include <span>
#include <vector>

namespace pulp::test::audio {

/// Musical interval between two frequencies in cents: `1200·log2(hz / reference)`.
/// Positive when `hz` is above `reference`. Returns `kSilenceFloorDb`-style
/// sentinel behavior is NOT used here — both arguments must be > 0 (throws
/// otherwise), because a cents value is only meaningful between two real pitches.
double cents_between(double hz, double reference_hz);

/// A single fundamental-frequency estimate.
struct PitchEstimate {
    /// Estimated fundamental in Hz. 0.0 when `voiced` is false.
    double hz = 0.0;
    /// Fraction of the analyzed segment's energy explained by the fundamental
    /// and its harmonics, clamped to [0, 1]. ~1 for a clean tone or saw, low for
    /// noise. This is the number gated against `PitchOptions::min_confidence`.
    double confidence = 0.0;
    /// False when the estimator refused: the segment was below the silence floor
    /// or its confidence fell under the gate. A refused estimate never carries a
    /// frequency — treat it as "no fundamental here", not as a measurement.
    bool voiced = false;
    /// Diagnostics — the coarse FFT-peak estimate before projection refinement,
    /// and the raw (unclamped) harmonic-energy fraction. Exposed so a caller can
    /// see how far the refinement moved the peak and how tonal the segment was.
    double coarse_hz = 0.0;
    double harmonic_energy_ratio = 0.0;
};

/// Options for `estimate_pitch`.
struct PitchOptions {
    /// FFT length for the coarse peak (power of two). Automatically reduced to
    /// the largest power of two that fits the analysis segment when the segment
    /// is shorter — a short per-frame window does not need a huge transform, and
    /// the projection refinement (not the FFT) sets the final accuracy.
    int fft_length = 16384;
    /// Search band. `max_hz = 0` means `0.45 · sample_rate` (safely below
    /// Nyquist, where `fit_tone` degenerates). The coarse peak is taken only from
    /// bins inside [min_hz, max_hz].
    double min_hz = 20.0;
    double max_hz = 0.0;
    /// Window for the coarse peak. Blackman by default: its −57 dB side lobes
    /// keep a strong harmonic from masking the fundamental peak on a dense
    /// signal, while its wide main lobe is irrelevant here because sub-bin
    /// accuracy comes from the projection stage, not the FFT.
    Window window = Window::blackman;
    double kaiser_beta = kDefaultKaiserBeta;
    /// Voicing gate: `confidence` below this refuses the estimate. 0.5 accepts
    /// tones and saws (harmonic ratio ~1) while rejecting noise.
    double min_confidence = 0.5;
    /// Harmonics (including the fundamental) summed for the confidence measure.
    /// `0` (the default) means "every harmonic below Nyquist" — required so a
    /// bright comb (BLIT/buzz, narrow pulse) whose energy is spread across
    /// hundreds of partials is not wrongly refused. A positive value caps the
    /// sum (and its cost) for a signal known to be spectrally compact.
    int harmonic_count = 0;
    /// RMS below this (linear, ~−80 dBFS) is treated as silence and refused
    /// before any spectral work — a dead processor must not measure a pitch.
    double silence_rms = 1.0e-4;
    /// Channel analyzed, and an offset/length window into it. `analysis_length =
    /// 0` means "from `analysis_offset` to the end of the buffer".
    int channel = 0;
    int analysis_offset = 0;
    int analysis_length = 0;
};

/// Estimate the fundamental of one channel of an already-rendered buffer.
///
/// Accurate for harmonically-dense signals (saw, pulse, additive stacks) where
/// `estimate_frequency`'s zero-crossing method fails. Fails closed: throws
/// `std::invalid_argument` for a non-positive sample rate or an analysis window
/// that does not lie within the buffer; returns `voiced = false` (never a
/// fabricated frequency) for silence or non-tonal content.
PitchEstimate estimate_pitch(const pulp::audio::BufferView<const float>& signal,
                             double sample_rate,
                             const PitchOptions& options = {});

/// Span overload — analyzes a single already-extracted channel.
PitchEstimate estimate_pitch(std::span<const float> samples, double sample_rate,
                             const PitchOptions& options = {});

/// One point on an f0(t) trajectory: the frame's center time and its estimate.
struct PitchTrackPoint {
    double time_s = 0.0;      ///< Center of the analysis window, in seconds.
    double hz = 0.0;          ///< Fundamental (0 when unvoiced).
    double confidence = 0.0;  ///< Per-frame confidence (see PitchEstimate).
    bool voiced = false;      ///< False = this frame was refused.
};

/// Options for `track_pitch`.
struct PitchTrackOptions {
    /// Samples per analysis frame. Sets the time–frequency trade-off: shorter
    /// frames follow a fast glide/vibrato but resolve pitch less precisely.
    int window_length = 8192;
    /// Samples between successive frame starts (the hop). Smaller = denser
    /// trajectory. Must be > 0.
    int hop_length = 2048;
    /// Per-frame estimator options. `fft_length` is auto-fitted to the window,
    /// so it can be left at its default.
    PitchOptions pitch{};
    int channel = 0;
};

/// An f0(t) trajectory extracted from a render — the demodulator output the
/// drift/jitter statistics consume.
struct PitchTrack {
    double sample_rate = 0.0;
    int window_length = 0;
    int hop_length = 0;
    std::vector<PitchTrackPoint> points;

    /// Center times of the voiced frames only, in order.
    std::vector<double> voiced_times_s() const;
    /// Fundamentals of the voiced frames only, index-aligned to
    /// `voiced_times_s()`. This is the f0(t) series handed to the Quality Lab
    /// (Allan deviation / Theil-Sen slope live there, not in C++).
    std::vector<double> voiced_hz() const;
};

/// Slide a window across `samples`, estimating the fundamental at each hop.
///
/// Throws `std::invalid_argument` for a non-positive rate, window, or hop, or a
/// window longer than the signal. Unvoiced frames are kept in `points` (with
/// `voiced = false`) so the caller sees where the estimator refused; the
/// `voiced_*` accessors filter them out for the statistics stage.
PitchTrack track_pitch(std::span<const float> samples, double sample_rate,
                       const PitchTrackOptions& options = {});

} // namespace pulp::test::audio
