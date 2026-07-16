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
/// Target-boundary rule: heavy FFT/analyzer code lives ONLY
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
///   * take all lengths as powers of two (the radix-2 FFT requirement);
///   * require the analysis segment to lie entirely within the supplied
///     buffer(s) — throws `std::invalid_argument` otherwise. Zero-padding a
///     short capture would window a truncated signal, whose edge leaks like
///     any discontinuity and reads as distortion; shrink `fft_length` to fit
///     the capture instead.
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
///
/// **Choosing one is a dynamic-range decision, not a style preference.** A
/// window's first side lobe is the floor below which a small component sitting
/// near a loud one cannot be seen: the analyzer reports its OWN leakage skirt
/// instead of the signal. Measuring a −100 dB component next to a 0 dB
/// fundamental through Hann (−31 dB side lobes) measures Hann.
///
/// First-side-lobe figures below are MEASURED by
/// `test_audio_doctor.cpp`'s side-lobe test (48 kHz, 16384-point FFT,
/// half-bin-offset tone), not quoted from a table:
///
/// | Window      | 1st side lobe | Falloff    | Usable floor near a loud tone |
/// |-------------|---------------|------------|-------------------------------|
/// | rectangular | −14 dB        | −6 dB/oct  | bin-coherent tones only       |
/// | hann        | −31 dB        | −18 dB/oct | ~−60 dBc                      |
/// | hamming     | −41 dB        | −6 dB/oct  | ~−45 dBc                      |
/// | blackman    | −57 dB        | −18 dB/oct | ~−90 dBc                      |
/// | flat_top    | −93 dB        | ~flat      | ~−93 dBc (does NOT improve)   |
/// | kaiser      | tunable by β  | steep      | −124 dBc at β = 14            |
///
/// **Only `kaiser` reaches a −100 dBc gate.** That is a measured result, not a
/// preference: at 16 bins from a 0 dB fundamental a −100 dBc tone reads −82 dB
/// through hann, −91 dB through blackman, and −96 dB through flat_top — every
/// one of those is the window's own leakage, not the signal (the test proves it
/// by re-measuring with the small tone REMOVED and getting the same number).
///
/// Note the falloff column outranks the first-side-lobe column with distance:
/// hann's first side lobe is 62 dB worse than flat_top's, but hann falls at
/// −18 dB/octave while flat_top is flat, so 64 bins out hann's floor (−117 dB)
/// is already better than flat_top's (−101 dB). flat_top is for amplitude
/// accuracy on a tone it CAN resolve, never for floor depth.
///
/// Coefficient math is NOT duplicated here: every window delegates to
/// `pulp::signal::WindowFunction` (core/signal/windowing.hpp), the source of
/// truth. See `window_coefficients` for the periodic/normalization contract.
enum class Window {
    /// Rectangular (no taper). Zero scalloping/leakage ONLY when every tone
    /// of interest is bin-coherent (an integer number of cycles in the FFT
    /// length). Use for THD on a coherent tone; the harmonics then land on
    /// exact bins with no spectral spreading.
    rectangular,
    /// Periodic Hann (`0.5 - 0.5*cos(2π n / N)`). −31 dB side lobes, wide
    /// main lobe; tolerant of non-coherent tones at the cost of bin
    /// resolution. Use for broadband response curves where the stimulus is
    /// not guaranteed bin-coherent. NOT usable below roughly −60 dBc near a
    /// loud tone — reach for `blackman`/`flat_top`/`kaiser` there.
    hann,
    /// Periodic Hamming. −41 dB first side lobe but a slow −6 dB/octave
    /// falloff, so its far skirt stays HIGHER than Hann's despite the better
    /// first lobe. Rarely the right pick here; prefer `blackman` or `kaiser`
    /// when the floor is what matters.
    hamming,
    /// Periodic Blackman. −57 dB first side lobe with an −18 dB/octave
    /// falloff, so the skirt collapses quickly with distance from the tone.
    /// Good to roughly −90 dBc. NOT sufficient for a −100 dBc gate: at 16 bins
    /// from a loud tone its leakage is still ≈ −89 dB and a −100 dB component
    /// reads ≈ −91 dB. Reach for `kaiser` there.
    blackman,
    /// Periodic flat-top (5-term). A flat-topped main lobe makes a tone's
    /// measured amplitude accurate to ~0.01 dB regardless of where it falls
    /// between bins — that is what this window is for. Its side lobes are
    /// ≈ −93 dB with an essentially FLAT falloff, so unlike hann/blackman its
    /// floor barely improves with distance (still ≈ −101 dB 64 bins out, by
    /// which point hann is at −117 dB). Its ~10-bin main lobe also swallows
    /// anything closer than ~6 bins. Use for amplitude accuracy, never for
    /// floor depth — it is not adequate for a −100 dBc gate.
    flat_top,
    /// Periodic Kaiser, side-lobe level dialed by β (`kaiser_beta` on the
    /// options structs, default `kDefaultKaiserBeta`). The only window here
    /// that reaches a −100 dBc gate — see `kDefaultKaiserBeta`. Trades
    /// main-lobe width for floor depth as β rises.
    kaiser,
};

/// Default Kaiser β.
///
/// Chosen from measurement, not a formula. At 48 kHz / 16384 points against a
/// 0 dB half-bin-offset fundamental (`test_audio_doctor.cpp`), β = 14 measures
/// a leakage floor of −124 dB or lower at ≥ 12 bins from the tone and reads a
/// −100 dBc component to within 0.4 dB. That clears the harness's "detection
/// floor ≤ −110 dB" acceptance bar with ~14 dB of margin.
///
/// Why not lower: β = 12 measures only ≈ −102 dB immediately outside the main
/// lobe — right at a −100 dBc gate, with no margin. Why not core/signal's
/// generic β = 3 fallback: ≈ −36 dB, useless here. Raise β for a deeper floor
/// and a wider main lobe; lower it for sharper resolution of close-in
/// components you do not need −100 dB of range on.
///
/// That floor holds in the tone's NEIGHBORHOOD, not at the bottom of the
/// spectrum: removing the mean of a non-integer-cycle tone leaves a DC-removal
/// pedestal that no window touches — measured on the same fixture, bins 1–3
/// read ≈ −66 / −75 / −92 dB through kaiser β = 14. A component landing within
/// a few bins of DC is pedestal-limited through ANY window; measure it with
/// the projection path instead (`measure_aliasing` fits the constant jointly
/// and reads a −100 dBc component exactly even under a 0.5 DC offset).
inline constexpr double kDefaultKaiserBeta = 14.0;

/// Human-readable window name for artifacts/messages ("rectangular", "hann",
/// "hamming", "blackman", "flat_top", "kaiser").
std::string window_name(Window w);

/// Coherent gain (a.k.a. DC gain) of `w` over an `n`-sample segment: the mean
/// of its coefficients, i.e. the factor by which the window scales a tone's
/// peak-bin magnitude. This is the number `window_coefficients` divides out —
/// see its normalization contract.
///
/// For `rectangular` (1.0) and `hann` (0.5) this is the exact analytic constant
/// — an exact power of two, which is what preserves their bit-exact results.
/// Every other window measures the true mean of its length-`n` coefficients
/// (which is why this takes `n`), so the DC gain lands on exactly 1 regardless
/// of how core/signal rounds its coefficient literals.
double window_coherent_gain(Window w, int n,
                            double kaiser_beta = kDefaultKaiserBeta);

/// Coefficients of `w` for an `n`-sample analysis segment.
///
/// Two properties define this function, and both are load-bearing:
///
/// **Periodic (DFT-even), not symmetric.** Coefficient `i` uses denominator `n`,
/// not `n - 1`, so the window wraps exactly the way the FFT assumes the segment
/// does. `pulp::signal::WindowFunction::generate` is symmetric, so this
/// generates `n + 1` points and drops the last — that yields the periodic form
/// exactly, with no coefficient math duplicated here.
///
/// **Coherent-gain normalized.** Coefficients are divided by
/// `window_coherent_gain(w)`, so every window has a DC gain of exactly 1 and a
/// tone's measured magnitude is the same through any of them. Without this a
/// window swap would silently move the 0 dB reference (flat-top attenuates by
/// ~13 dB relative to rectangular), which would make a "−100 dB" claim
/// meaningless. For `rectangular` the divisor is 1.0 and for `hann` it is 0.5 —
/// both exact powers of two — so those two paths are bit-for-bit identical to
/// the un-normalized coefficients they replaced, and every dB/ratio the
/// analyzers report is unchanged.
std::vector<double> window_coefficients(Window w, int n,
                                        double kaiser_beta = kDefaultKaiserBeta);

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
    /// decayed within `fft_length`. Choose a tapered window only with a
    /// non-impulse stimulus whose response rings past the window — and pick it
    /// by side-lobe level (see `Window`), not habit.
    Window window = Window::rectangular;
    int channel = 0;            ///< Output channel analyzed.
    /// β for `Window::kaiser`; ignored by every other window.
    double kaiser_beta = kDefaultKaiserBeta;
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

// ── Phase / group delay ───────────────────────────────────────────────────

/// One point on a phase/group-delay curve.
///
/// `defined` is the honesty gate: phase carries no information where the
/// transfer magnitude is at the noise floor (a stopband), so every field
/// below except `hz` and `magnitude_db` is meaningless when it is false, and
/// `phase_rad` / `group_delay_samples` are NaN there rather than a plausible
/// number. Check `defined` before reading them.
struct PhasePoint {
    double hz = 0.0;
    double magnitude_db = 0.0;        ///< Transfer magnitude — the gate input.
    double phase_rad = 0.0;           ///< Unwrapped transfer phase; NaN if !defined.
    double group_delay_samples = 0.0; ///< -dφ/dω in samples; NaN if !defined.
    bool defined = false;             ///< False where magnitude is below the floor.
};

/// Phase / group-delay curve of a transfer function.
///
/// Determinism contract (echoed into the artifact):
///   * stimulus: a unit impulse (or caller-supplied reference) driven through
///     the processor, same as the magnitude response;
///   * window/length: stated `window` over an `fft_length`-sample segment
///     beginning at `analysis_offset`. `rectangular` is the default and the
///     only window for which the estimator below returns the segment's true
///     group delay (see `measure_group_delay`);
///   * reference: phase and group delay are those of the transfer function
///     H = output/input, so a delay common to both cancels and the absolute
///     FFT scale is irrelevant (both facts are ratios) — backend-stable;
///   * resolution: one bin every `sample_rate / fft_length` Hz; checkpoints
///     read the nearest bin (no interpolation).
///
/// **Sign convention**: group delay is positive for a causal delay — a filter
/// that delays by k samples reports +k, matching `-dφ/dω`.
struct PhaseCurve {
    std::string analyzer = "group_delay";
    std::string stimulus;
    Window window = Window::rectangular;
    int fft_length = 0;
    int analysis_offset = 0;
    double sample_rate = 0.0;
    double bin_hz = 0.0;
    /// Magnitude gate, in dB **relative to the curve's peak transfer
    /// magnitude**. Bins below it are reported `defined = false`.
    double magnitude_floor_db = 0.0;
    std::vector<PhasePoint> full;        ///< Bin 0..N/2, monotonic in hz.
    std::vector<PhasePoint> checkpoints; ///< Requested frequencies only.

    /// True when the bin nearest `hz` carries enough magnitude for its phase
    /// to mean anything. False for an empty curve.
    bool defined_at(double hz) const;
    /// Group delay in samples at the bin nearest `hz`. **NaN when
    /// `defined_at(hz)` is false** — the caller must not treat a stopband as a
    /// measurement. NaN propagates loudly through arithmetic and fails any
    /// comparison, which is the intended failure mode.
    double group_delay_samples_at(double hz) const;
    /// Group delay in seconds at `hz` (samples / sample_rate). NaN when undefined.
    double group_delay_seconds_at(double hz) const;
    /// Unwrapped transfer phase in radians at `hz`. NaN when undefined. See
    /// `measure_group_delay` for the ambiguity a stopband null leaves behind.
    double phase_radians_at(double hz) const;
    /// Transfer magnitude in dB at `hz` — always defined (it IS the gate input).
    /// Returns kSilenceFloorDb for an empty curve.
    double magnitude_db_at(double hz) const;
};

/// Options for the phase/group-delay analyzer. Lengths must be powers of two.
struct GroupDelayOptions {
    /// Power of two; also the impulse render length. Must exceed twice the
    /// largest group delay present — see `measure_group_delay`.
    int fft_length = 16384;
    int analysis_offset = 0; ///< Samples skipped before the analysis segment.
    /// Rectangular by default, for the same reason as the response analyzer
    /// (an impulse at frame 0 would be annihilated by a Hann window) and
    /// because the group-delay estimator is exact only for a rectangular
    /// window — a taper measures the group delay of the *windowed* signal.
    Window window = Window::rectangular;
    int channel = 0;
    /// Magnitude gate in dB below the curve's peak transfer magnitude. Bins
    /// quieter than this are reported undefined rather than given a number
    /// read out of the noise floor. −60 dB is the default: it keeps a normal
    /// passband and transition band while rejecting a stopband, and is far
    /// enough above the analyzer's own numerical floor that a `defined` bin is
    /// genuinely measured.
    double magnitude_floor_db = -60.0;
};

/// Phase and group delay of `output` relative to `input`, one channel.
///
/// ── Estimator ──────────────────────────────────────────────────────────────
/// Group delay is the derivative `τ(ω) = -dφ/dω`, and differentiating a noisy,
/// wrapped phase curve by finite differences is both fragile (it aliases
/// whenever the true phase advances more than π between bins) and noisy (a
/// difference amplifies the bin-to-bin error). This analyzer instead uses the
/// **Fourier/ramped-signal identity**, which computes the derivative
/// analytically rather than numerically:
///
///     τ(ω) = Re{ X_r(ω) · conj(X(ω)) } / |X(ω)|²   where X_r = FFT(n·x[n])
///
/// (from `dX/dω = -j·FFT(n·x[n])`). It is evaluated per bin on the reference
/// and the output separately and subtracted — `τ_H = τ_output − τ_input` —
/// because phase subtracts in a ratio and the derivative is linear. Three
/// properties follow, and they are why this estimator was chosen:
///   * **No unwrapping is involved.** Group delay is exact per bin and cannot
///     be corrupted by an unwrapping mistake elsewhere in the curve.
///   * **No frequency step to choose.** There is no finite-difference stride
///     trading resolution against noise; the value at each bin is the true
///     derivative of the segment's phase there.
///   * **Exact, not approximate**, for any signal that lies entirely inside
///     the rectangular analysis window. A truncated (still-ringing) IR is the
///     one real error source: the estimator faithfully reports the group delay
///     of the *truncated* signal. Budget `fft_length` so the response has
///     decayed into the noise floor within it.
/// Expected tolerance for a well-contained IR: within ~0.01 samples of truth
/// for a pure delay or FIR, and dominated by IR truncation otherwise. The
/// asserted tolerances in the tests state what was actually observed.
///
/// ── Phase unwrapping ───────────────────────────────────────────────────────
/// `phase_rad` is reported separately and IS unwrapped, by the standard
/// cumulative-offset method: walk the bins low to high and add the multiple of
/// 2π that keeps each successive raw `atan2` difference inside (−π, π]. This is
/// exact only while the true phase advances less than π per bin, i.e. while
/// `group_delay < fft_length / 2` samples (the phase slope is `−τ·2π/N` per
/// bin). Beyond that the phase aliases and unwrapping silently tracks the
/// wrong branch — budget `fft_length` accordingly.
///
/// Unwrapping also cannot recover the branch across a magnitude null, where
/// the true phase steps discontinuously: **`phase_rad` above a deep stopband
/// null carries an unresolved 2πk ambiguity**, and the undefined bins it walks
/// through contribute noise to the accumulated offset. `group_delay_samples`
/// does not inherit this — it never consults the unwrapped phase.
///
/// ── Stopband contract ──────────────────────────────────────────────────────
/// Where the transfer magnitude is at the noise floor there is no signal whose
/// phase could be measured, and `atan2` of numerical noise yields a plausible-
/// looking number that means nothing. Every bin whose magnitude is more than
/// `options.magnitude_floor_db` below the curve's peak is therefore reported
/// `defined = false` with NaN phase and group delay. A bin whose reference
/// (input) spectrum is itself negligible is undefined for the same reason.
/// The analyzer reports no group delay it did not measure.
///
/// Computed in double precision (`Fft64`) throughout: the estimator's numerator
/// is a difference of products that can cancel heavily near a null, and the
/// double path is also the backend-portable one.
PhaseCurve measure_group_delay(
    const pulp::audio::BufferView<const float>& input,
    const pulp::audio::BufferView<const float>& output, double sample_rate,
    std::span<const double> checkpoints_hz,
    const GroupDelayOptions& options = {});

// ── THD / THD+N ───────────────────────────────────────────────────────────

/// One harmonic's contribution to a distortion measurement.
struct Harmonic {
    int index = 0;          ///< 1 = fundamental, 2 = 2nd harmonic, ...
    double hz = 0.0;
    /// Linear bin magnitude, analyzer-relative scale (the FFT backend's
    /// absolute normalization is arbitrary — compare these to each other, not
    /// to an absolute reference). Coherent-gain normalized, so the value does
    /// not depend on which `window` the analyzer selected. `db_below_
    /// fundamental`, `thd`, and `thd_plus_n` are ratios of these and so are
    /// unaffected by the normalization.
    double magnitude = 0.0;
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
///     `analysis_offset` when coherent (no leakage); `non_coherent_window`
///     (default `hann`) otherwise. The chosen window is recorded in `window`.
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
    /// Window used when the tone is NOT bin-coherent (a coherent tone always
    /// uses `rectangular` — it leaks nothing, so no taper can improve on it).
    /// Defaults to `hann`, preserving the analyzer's long-standing
    /// non-coherent behavior. Raise it to `blackman`/`kaiser` when a
    /// non-coherent reading must resolve HARMONICS far below Hann's −31 dB
    /// side lobes.
    ///
    /// It will NOT rescue `thd_plus_n`, and reaching for it there is a trap.
    /// THD treats a single bin as the fundamental, so a non-coherent tone's
    /// main-lobe spread is counted as noise — THD+N is dominated by main-lobe
    /// WIDTH here, and a deeper window is a wider one, so it measures more
    /// apparent noise, not less (pinned by `test_audio_doctor.cpp`). The fix
    /// for a trustworthy THD+N is a bin-coherent tone, never a better window.
    Window non_coherent_window = Window::hann;
    /// β for `Window::kaiser`; ignored by every other window.
    double kaiser_beta = kDefaultKaiserBeta;
};

/// Measure THD / THD+N of `signal` (one channel of an already-rendered buffer)
/// at `fundamental_hz`. The caller is responsible for ensuring the signal is a
/// steady tone at that frequency over the analysis segment; `coherent` is
/// computed from `fundamental_hz`, `fft_length`, and `sample_rate` and recorded
/// for the reader to judge the tolerance.
///
/// Fails closed rather than fabricating a pass: throws `std::invalid_argument`
/// for a fundamental at or above Nyquist (it would clamp to the Nyquist bin
/// and read thd = 0), for a buffer that does not cover the analysis segment
/// (see the determinism contract above), and for a signal with no energy at
/// the fundamental bin — silence would otherwise measure thd = 0, letting a
/// dead processor pass a distortion gate.
ThdResult measure_thd(const pulp::audio::BufferView<const float>& signal,
                      double fundamental_hz, double sample_rate,
                      const ThdOptions& options = {});

// ── Tone projection ───────────────────────────────────────────────────────
//
// Everything below this line is FFT-free. A windowed FFT answers "what is in
// this signal?" and pays for the answer with leakage: a 0 dB tone smears a
// skirt across its neighbors that no window removes, only reshapes (see the
// `Window` table above — even Kaiser at β = 14 only pushes it to −124 dB).
// Least-squares tone projection answers the narrower question "how much of
// THIS exact frequency is present?" and has no skirt at all, because it never
// truncates anything into a window: it fits the basis over the whole segment
// and subtracts. That is what makes a −100 dBc claim measurable with margin.
//
// Use the FFT analyzers to DISCOVER content at unknown frequencies. Use these
// when the frequencies of interest are known in advance — which, for an
// oscillator's harmonics and their fold-back images, they are.

/// Least-squares fit of a single sinusoid at a known frequency.
///
/// The fit spans both `sin` and `cos`, so the tone's PHASE is solved for, not
/// assumed — `amplitude` is phase-independent and `residual` is unaffected by
/// where the caller's segment happens to start.
struct ToneFit {
    double cycles_per_sample = 0.0; ///< The frequency fitted (f / sample_rate).
    double sin_gain = 0.0;          ///< Coefficient of sin(2π·f·n).
    double cos_gain = 0.0;          ///< Coefficient of cos(2π·f·n).
    double amplitude = 0.0;         ///< hypot(sin_gain, cos_gain).
    double fitted_energy = 0.0;     ///< Σ of the fitted tone squared.
    double residual_energy = 0.0;   ///< Σ of (samples − fitted) squared.
};

/// Fit one sinusoid at `cycles_per_sample` (frequency ÷ sample rate; 0.5 is
/// Nyquist) to `samples` by least squares.
///
/// Limits, all load-bearing:
///   * **The frequency must be known.** This does not search. Feed it a
///     frequency that is not present and it returns the amplitude of whatever
///     the signal happens to correlate with there — near zero for unrelated
///     content, but NOT a detection.
///   * **Degenerate at DC and at exactly Nyquist.** At `cycles_per_sample` = 0
///     the sin basis vanishes; at exactly 0.5 it vanishes again (sin(π·n) ≡ 0)
///     and only the cos basis (±1 alternating) carries information, so
///     amplitude and phase cannot be separated. Throws for both.
///   * **Not a multi-tone separator.** Two tones closer than roughly
///     1 / segment-duration are not orthogonal over the segment, so fitting
///     one absorbs part of the other. Use `fit_tones` for a known set.
ToneFit fit_tone(std::span<const double> samples, double cycles_per_sample);

/// Energy of `samples` NOT explained by a tone at `cycles_per_sample`, in dB
/// relative to the fitted tone's own energy: `10·log10(residual / fitted)`.
///
/// This is the single-tone purity primitive. Because it subtracts the tone
/// rather than windowing around it, it has no leakage skirt to fight — it
/// resolves residuals below −100 dB on a signal whose tone is NOT bin-coherent
/// and whose length is not a power of two, neither of which any FFT analyzer
/// here can offer.
///
/// What it measures: EVERYTHING that is not the tone, lumped together —
/// harmonics, aliases, noise, and hum are one number. It answers "how pure is
/// this?", never "what is the impurity?". For that, `measure_aliasing` fits the
/// whole expected grid and reports the offenders separately.
double tone_residual_db(std::span<const double> samples,
                        double cycles_per_sample);

/// Gain of a tone at `cycles_per_sample` relative to `input_amplitude`, in dB:
/// `20·log10(fitted_amplitude / input_amplitude)`. The passband-flatness
/// counterpart to `tone_residual_db` — same fit, different readout. Throws if
/// `input_amplitude` is not > 0.
double tone_gain_db(std::span<const double> samples, double cycles_per_sample,
                    double input_amplitude);

// ── Alias / image analysis ────────────────────────────────────────────────

/// Where a component at `frequency_hz` LANDS once sampled at `sample_rate`.
///
/// Sampling maps the whole frequency axis onto [0, sample_rate/2] by wrapping
/// at `sample_rate` and mirroring about Nyquist. This folds **repeatedly**, not
/// once: at 48 kHz, 80 kHz (1.67·fs) wraps to 32 kHz and then mirrors to
/// 16 kHz. Sign is ignored (a real signal's spectrum is symmetric).
///
/// Exact-boundary landings (0 and Nyquist) are returned as-is; they are
/// degenerate to MEASURE (see `fit_tone`), not to compute.
double fold_frequency(double frequency_hz, double sample_rate);

/// How `measure_aliasing` classified one expected component of the ideal
/// harmonic series `h · f0`.
enum class ComponentClass {
    /// `h · f0` is below Nyquist: it lands on the f0 grid, where it belongs.
    /// A legitimate part of the waveform, however loud.
    harmonic,
    /// `h · f0` is above Nyquist: it CANNOT exist at its own frequency, so
    /// whatever energy sits at its fold-back site is aliasing — the artifact an
    /// anti-aliased oscillator exists to suppress.
    alias,
};

/// One expected component of the ideal series, and what was measured where it
/// landed.
struct AliasComponent {
    int index = 0;              ///< h — 1 is the fundamental.
    ComponentClass component_class = ComponentClass::harmonic;
    double source_hz = 0.0;     ///< h · f0: where it would sit at infinite fs.
    double hz = 0.0;            ///< Where it actually landed (== source_hz for
                                ///< a harmonic; the fold site for an alias).
    /// Linear amplitude from the joint fit, in the SAME units as the input
    /// samples — directly comparable to the fundamental's. Meaningless when
    /// `resolved` is false.
    double amplitude = 0.0;
    double db_below_fundamental = 0.0; ///< 20·log10(amplitude / fundamental).
    /// Inside the measurement band — i.e. `hz <= max_alias_frequency_hz`.
    /// Applies to aliases only; a harmonic is legitimate at any frequency and
    /// is always reported with `in_band = true`.
    bool in_band = true;
    /// False when this component's landing site could not be separated from
    /// another component's, from DC, or from Nyquist (see
    /// `AliasOptions::min_separation_bins`). `amplitude` and
    /// `db_below_fundamental` are NOT populated — a gate must treat an
    /// unresolved in-band alias as INCONCLUSIVE, never as a pass.
    bool resolved = true;
};

/// Options for `measure_aliasing`.
struct AliasOptions {
    /// ── THE MOST IMPORTANT FIELD IN THIS HEADER. READ THIS BEFORE GATING. ──
    ///
    /// Aliases landing ABOVE this frequency are measured and reported, but are
    /// excluded from `worst_alias_db` / `worst_alias_hz`.
    ///
    /// **A full-band "no alias above −100 dB" gate is physically impossible for
    /// any finite method, and a caller who removes this qualifier has written a
    /// gate that no correct implementation can pass.** Two independent reasons:
    ///
    ///   1. **Fold-back is continuous across Nyquist.** A component at
    ///      Nyquist + ε lands at Nyquist − ε. As ε → 0 the alias approaches the
    ///      band edge and no filter can act on it, because "reject at Nyquist,
    ///      pass at Nyquist" is not a specification.
    ///   2. **Every realizable kernel has a transition band.** An anti-alias
    ///      response cannot step from 0 dB to −100 dB at a point; it needs
    ///      finite width, and that width straddles Nyquist with near-zero
    ///      attenuation. At 48 kHz a 100 dB-stopband 64-tap Kaiser design costs
    ///      ≈4.8 kHz of transition, so content in ≈24–25 kHz folds to
    ///      23–24 kHz essentially unattenuated — tens of dB above −100.
    ///
    /// The physics is not a defect, it is why the default is 20 kHz: an alias
    /// that lands at 23.4 kHz is above hearing and unavoidable; one that lands
    /// at 7 kHz is audible and is the bug. Band-qualifying is what makes the
    /// gate measure the oscillator instead of the sample rate.
    ///
    /// Raise it only to characterize (`full_band_worst_alias_db` already
    /// reports the unqualified number for exactly that). Lower it for a
    /// content-specific audibility argument. Setting it at or above Nyquist
    /// makes the qualifier a no-op and is the misuse described above.
    double max_alias_frequency_hz = 20000.0;

    /// Harmonics of the ideal series to account for, from the fundamental up.
    /// Every one above Nyquist contributes a fold site. Too few and real
    /// aliases are missed (they fall through to `noise_db`); too many and the
    /// fit spends conditioning on sites with nothing at them. For a saw or
    /// pulse, go high enough that `num_harmonics · f0` is several times the
    /// sample rate.
    ///
    /// A series with NO harmonic at or above Nyquist is rejected outright
    /// (`measure_aliasing` throws): it models no fold site, so its report
    /// would read as clean on ANY signal, however aliased. Note the default
    /// hits that wall for a low fundamental — 64 harmonics of anything below
    /// 375 Hz at 48 kHz never reach Nyquist — so size this from the sample
    /// rate, not from habit.
    int num_harmonics = 64;

    int analysis_offset = 0;  ///< Samples skipped (settling / plugin latency).
    /// Samples analyzed; 0 means "to the end of the buffer". Longer is
    /// strictly better here — the detection floor improves as 1/√length, and
    /// closely-spaced components separate as 1/length.
    int analysis_length = 0;
    int channel = 0;

    /// Minimum separation between two fitted landing sites, in units of
    /// 1 / analysis-duration (the natural resolution of a fit this long).
    /// Sites closer than this — to each other, to DC, or to Nyquist — are not
    /// linearly independent over the segment, so they are marked `resolved =
    /// false` rather than fitted to an ill-conditioned answer.
    double min_separation_bins = 2.0;
};

/// Three-way classification of an oscillator's output into harmonic / alias /
/// noise, with the worst in-band alias called out.
///
/// The method is projection, not bin classification: the full set of expected
/// landing sites (in-band harmonics AND the fold-back sites of every harmonic
/// above Nyquist) is fitted JOINTLY by least squares, and whatever is left over
/// is noise. Fitting jointly rather than site-by-site buys exactness at the
/// resolution limit and — more importantly — a solve that collapses audibly
/// when two sites genuinely cannot be separated, which is what `resolved` and
/// `has_unresolved_in_band_alias` report.
///
/// Determinism contract: pure arithmetic over the supplied buffer — no window,
/// no FFT, no power-of-two length, no coherence requirement, and no dependence
/// on where `analysis_offset` starts. Amplitudes are in input units and every
/// dB is a ratio, so nothing here depends on an FFT backend's normalization.
struct AliasReport {
    std::string analyzer = "alias";
    double fundamental_hz = 0.0;
    double sample_rate = 0.0;
    double nyquist_hz = 0.0;
    double max_alias_frequency_hz = 0.0; ///< The band qualifier that was used.
    int num_harmonics = 0;
    int analysis_offset = 0;
    int analysis_length = 0;   ///< Samples actually fitted.
    double bin_hz = 0.0;       ///< Fit resolution: sample_rate / analysis_length.

    /// Fitted amplitude of the fundamental — the reference every dB below is
    /// relative to.
    double fundamental_amplitude = 0.0;
    /// Every expected component h = 1..num_harmonics, in order, each labeled
    /// `harmonic` or `alias` by where `h · f0` fell relative to Nyquist.
    std::vector<AliasComponent> components;

    /// Worst (loudest) alias landing at or below `max_alias_frequency_hz`, in
    /// dB relative to the fundamental. **This is the number to gate on.**
    /// `kSilenceFloorDb` when the series produces no resolved in-band alias.
    double worst_alias_db = kSilenceFloorDb;
    double worst_alias_hz = 0.0;      ///< Where it landed.
    int worst_alias_index = 0;        ///< Which h produced it (0 = none).

    /// The unqualified worst alias at ANY frequency up to Nyquist. Reported for
    /// characterization and honesty — do NOT gate on it (see
    /// `AliasOptions::max_alias_frequency_hz` for why no implementation can
    /// pass such a gate).
    double full_band_worst_alias_db = kSilenceFloorDb;
    double full_band_worst_alias_hz = 0.0;

    /// Everything the fit could not attribute to any expected component: RMS of
    /// the residual, in dB relative to the fundamental's RMS. Broadband noise,
    /// intermodulation, and any alias from a harmonic beyond `num_harmonics`
    /// all land here.
    double noise_db = kSilenceFloorDb;

    /// The quietest single component this measurement could have detected, in
    /// dB relative to the fundamental — a ≈2σ statistical bound derived from
    /// the residual and the fit length, ASSUMING the residual is white.
    ///
    /// **Never assert a gate on this number.** That would be the analyzer
    /// grading its own homework, and the white-residual assumption fails
    /// exactly when it matters: an unmodeled alias is a discrete TONE, and a
    /// tone-dominated residual makes this bound optimistic (`noise_db`
    /// sitting near `worst_alias_db` is the tell). The floor a gate leans on
    /// must be PROVEN by negative control instead — a fixture with zero alias
    /// content by construction must read collapsed, and injected components
    /// of known level must be recovered. `test_audio_doctor.cpp`'s
    /// clean-series and injected-alias tests are the pattern.
    ///
    /// Its one legitimate use is the inconclusiveness check: a gate is
    /// trustworthy only while `worst_alias_db` clears this floor. When it
    /// does not, the measurement could not distinguish its own residual from
    /// a component and the report is INCONCLUSIVE — fail closed rather than
    /// reading it as a pass.
    double detection_floor_db = kSilenceFloorDb;

    /// True when at least one ALIAS inside the band could not be resolved. A
    /// gate must fail closed (or report inconclusive) on this — `worst_alias_db`
    /// silently omits those components.
    bool has_unresolved_in_band_alias = false;
};

/// Classify `signal` (one channel of an already-rendered buffer) against the
/// ideal harmonic series of `fundamental_hz`, and report the worst in-band
/// alias.
///
/// The caller is responsible for the signal being a steady tone at
/// `fundamental_hz` over the analysis segment; use `analysis_offset` to skip
/// settling. Throws `std::invalid_argument` on a non-positive rate/frequency,
/// a fundamental at or above Nyquist, a band qualifier outside (0, Nyquist],
/// a series whose harmonics all sit below Nyquist (no alias site to measure —
/// see `AliasOptions::num_harmonics`), a segment too short to fit the
/// requested series, or a signal with no energy at the fundamental.
AliasReport measure_aliasing(const pulp::audio::BufferView<const float>& signal,
                             double fundamental_hz, double sample_rate,
                             const AliasOptions& options = {});

} // namespace pulp::test::audio
