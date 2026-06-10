#pragma once

/// @file audio_spectrum.hpp
/// Buffer-level offline spectrum analyzers: magnitude/frequency response and
/// harmonic distortion (THD / THD+N) over already-rendered audio buffers.
///
/// This is the FFT-bearing core of the offline "Audio Doctor". It takes plain
/// `BufferView`s — buffers that have already left the audio thread, whether
/// rendered through a `RenderScenario` (test side) or decoded from a WAV file
/// (the `pulp audio validate doctor` CLI) — and returns the curve/THD data
/// structures the artifact serializers (audio_doctor_artifacts) write to JSON.
///
/// Target-boundary rule (harness plan §9): heavy FFT/analyzer code lives ONLY
/// in test/tool targets. This lib (`pulp-audio-analysis`) depends on core/signal
/// for the FFT and is never linked into a runtime/plugin build — a CMake target
/// boundary makes a violation a link error. The scenario-driven entry points
/// that drive a `Processor` stay in `test/support/audio_doctor.hpp` (test-only);
/// they delegate to the buffer-level functions here so there is a single
/// analysis implementation.
///
/// ── Analyzer Determinism Contract (shared by both analyzers) ───────────────
/// Every spectral fact below is deterministic for identical input on a given
/// platform. The per-analyzer specifics (window, length, stimulus, coherence)
/// are stated on each entry point and echoed verbatim into the curve artifact.
/// Both analyzers:
///   * remove DC by subtracting the segment mean before windowing (the response
///     analyzer skips this for an impulse stimulus — see below);
///   * window a single contiguous analysis segment (no overlap-averaging);
///   * report dB relative to a stated reference, so the FFT backend's absolute
///     normalization (vDSP vs the radix-2 fallback differ by a constant scale)
///     cancels — results are backend-stable to the tolerance the tests state;
///   * take all lengths as powers of two (the radix-2 FFT requirement).
///
/// Test/tool layer only — analysis happens entirely off the audio thread.

#include "audio_metrics.hpp" // kSilenceFloorDb + buffer types

#include <pulp/audio/buffer.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace pulp::test::audio {

// ── Window functions ──────────────────────────────────────────────────────

/// Analysis window. The leakage policy each implies is part of the
/// Analyzer Determinism Contract and is recorded in the curve artifact.
enum class Window {
    /// Rectangular (no taper). Zero scalloping/leakage ONLY when every tone
    /// of interest is bin-coherent (an integer number of cycles in the FFT
    /// length). Use for THD on a coherent tone; the harmonics then land on
    /// exact bins with no spectral spreading.
    rectangular,
    /// Periodic Hann (`0.5 - 0.5*cos(2π n / N)`). −31 dB side lobes, wide
    /// main lobe; tolerant of non-coherent tones at the cost of bin
    /// resolution. Use for broadband response curves where the stimulus is
    /// not guaranteed bin-coherent.
    hann,
};

/// Human-readable window name for artifacts/messages ("rectangular"/"hann").
std::string window_name(Window w);

// ── Magnitude / frequency response ────────────────────────────────────────

/// One point on a response curve: frequency and magnitude in dB. For
/// `response_relative_to_input` the dB is output-over-input; for a bare
/// spectrum it is dB relative to the curve's own peak bin.
struct ResponsePoint {
    double hz = 0.0;
    double magnitude_db = 0.0;
};

/// Magnitude/frequency-response curve.
///
/// Determinism contract (echoed into the artifact):
///   * stimulus: a unit impulse (or caller-supplied stimulus) driven through
///     the processor;
///   * window/length: stated `window` over an `fft_length`-sample segment
///     beginning at `analysis_offset` (skip plugin latency / warm-up here);
///   * reference: each point is output-magnitude over input-magnitude in dB
///     (a true transfer response), so absolute FFT scale cancels;
///   * resolution: one full-resolution bin every `sample_rate / fft_length`
///     Hz; checkpoint values are read from the nearest bin (no interpolation),
///     and `bin_hz` records that resolution for the tolerance to budget.
struct ResponseCurve {
    std::string analyzer = "magnitude_response";
    std::string stimulus;   ///< e.g. "impulse" / "log_sweep 20..20000".
    Window window = Window::rectangular;
    int fft_length = 0;
    int analysis_offset = 0;
    double sample_rate = 0.0;
    double bin_hz = 0.0;    ///< Frequency resolution (sample_rate / fft_length).
    std::vector<ResponsePoint> full;        ///< Bin 0..N/2, monotonic in hz.
    std::vector<ResponsePoint> checkpoints; ///< Requested frequencies only.

    /// Magnitude (dB) at the bin nearest `hz` on the full curve. Returns
    /// kSilenceFloorDb if the curve is empty.
    double magnitude_db_at(double hz) const;
    /// Positive attenuation in dB at `hz` (0 dB = unity, larger = more cut).
    /// Convenience over `magnitude_db_at` for the "drops N dB" claim.
    double attenuation_db_at(double hz) const { return -magnitude_db_at(hz); }
};

/// Options for the response analyzer. Lengths must be powers of two.
struct ResponseOptions {
    int fft_length = 16384;     ///< Power of two; also the impulse render length.
    int analysis_offset = 0;    ///< Samples skipped before the analysis segment.
    /// Rectangular by default: the stimulus is an impulse at frame 0, so a
    /// Hann window (zero weight at n=0) would annihilate it. With rectangular,
    /// the impulse passes intact, its spectrum is flat, and the output/input
    /// ratio is the true transfer magnitude with no leakage for an IR that has
    /// decayed within `fft_length`. Choose `hann` only with a non-impulse
    /// stimulus whose response rings past the window.
    Window window = Window::rectangular;
    int channel = 0;            ///< Output channel analyzed.
};

/// Compute the magnitude response of `output` relative to `input`, one channel.
///
/// `input` and `output` are already-rendered buffers (e.g. an impulse and the
/// processor's impulse response, or a decoded reference and processed WAV). The
/// output spectrum is divided by the input spectrum bin-by-bin, giving a true
/// transfer magnitude in dB. Both segments use the same window so its shaping
/// cancels in the ratio. No DC removal (an impulse's flat spectrum is the
/// reference). `checkpoints_hz` are read from the nearest full-resolution bin.
ResponseCurve response_relative_to_input(
    const pulp::audio::BufferView<const float>& input,
    const pulp::audio::BufferView<const float>& output, double sample_rate,
    std::span<const double> checkpoints_hz, const ResponseOptions& options = {});

/// The signal's OWN magnitude spectrum, one channel, in dB **relative to its
/// own peak bin** (loudest frequency = 0 dB, everything else negative). Unlike
/// `response_relative_to_input` this is NOT a transfer response — there is no
/// reference stimulus — so it answers "what frequencies are present and how
/// loud relative to the strongest" for an arbitrary captured file. DC (bin 0)
/// is removed before analysis and excluded from the peak so a DC offset can't
/// dominate the normalization. Use a leakage-tolerant window (`Window::hann`)
/// for arbitrary, non-bin-coherent file content. Peak-normalization makes the
/// result FFT-backend-stable (a ratio). `analyzer` is set to
/// `"magnitude_spectrum"`.
ResponseCurve magnitude_spectrum_curve(
    const pulp::audio::BufferView<const float>& signal, double sample_rate,
    std::span<const double> checkpoints_hz, const ResponseOptions& options = {});

// ── THD / THD+N ───────────────────────────────────────────────────────────

/// One harmonic's contribution to a distortion measurement.
struct Harmonic {
    int index = 0;          ///< 1 = fundamental, 2 = 2nd harmonic, ...
    double hz = 0.0;
    double magnitude = 0.0; ///< Linear bin magnitude (analyzer-relative scale).
    double db_below_fundamental = 0.0; ///< 0 for the fundamental, negative below.
};

/// Total harmonic distortion result.
///
/// Determinism contract (echoed into the artifact):
///   * stimulus: a steady sine at `fundamental_hz`; for a trustworthy reading
///     the tone MUST be bin-coherent — `fundamental_hz = k * sample_rate /
///     fft_length` for integer k — so the fundamental and all harmonics land on
///     exact bins. `coherent` records whether that held; a non-coherent reading
///     is advisory only.
///   * window/length: `rectangular` over an `fft_length` segment after
///     `analysis_offset` when coherent (no leakage); `hann` otherwise.
///   * THD = sqrt(Σ harmonic²) / fundamental (ratio); THD+N =
///     sqrt(total energy − fundamental²) / fundamental, i.e. everything that
///     is not the fundamental bin (harmonics + noise + leakage).
///   * both ratios cancel absolute FFT scale, so they are backend-stable.
struct ThdResult {
    std::string analyzer = "thd";
    double fundamental_hz = 0.0;
    Window window = Window::rectangular;
    int fft_length = 0;
    int analysis_offset = 0;
    double sample_rate = 0.0;
    double bin_hz = 0.0;
    bool coherent = false;    ///< True when the tone is bin-coherent (see above).
    int num_harmonics = 0;    ///< Harmonics summed (beyond the fundamental).
    double thd = 0.0;         ///< Ratio (Σ harmonics / fundamental).
    double thd_plus_n = 0.0;  ///< Ratio (all non-fundamental / fundamental).
    std::vector<Harmonic> harmonics; ///< [0] = fundamental, then 2nd, 3rd, ...

    double thd_db() const;        ///< 20·log10(thd), kSilenceFloorDb at zero.
    double thd_plus_n_db() const; ///< 20·log10(thd_plus_n).
    double thd_percent() const { return thd * 100.0; }
};

/// Options for the THD analyzer. `fft_length` must be a power of two; pick
/// `fundamental_hz` bin-coherent with it for a required-gate reading.
struct ThdOptions {
    int fft_length = 16384;
    int analysis_offset = 0;
    int num_harmonics = 8;  ///< Harmonics above the fundamental to include.
    int channel = 0;
    /// Sine amplitude (linear) used when a *scenario*-driven analyzer
    /// synthesizes the stimulus tone (see test/support/audio_doctor.hpp).
    /// Ignored by the buffer-level `measure_thd` below, which analyzes an
    /// already-rendered signal — kept here so both entry points share one
    /// options struct.
    float amplitude = 0.5f;
};

/// Measure THD / THD+N of `signal` (one channel of an already-rendered buffer)
/// at `fundamental_hz`. The caller is responsible for ensuring the signal is a
/// steady tone at that frequency over the analysis segment; `coherent` is
/// computed from `fundamental_hz`, `fft_length`, and `sample_rate` and recorded
/// for the reader to judge the tolerance.
ThdResult measure_thd(const pulp::audio::BufferView<const float>& signal,
                      double fundamental_hz, double sample_rate,
                      const ThdOptions& options = {});

} // namespace pulp::test::audio
