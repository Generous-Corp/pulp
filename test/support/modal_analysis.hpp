#pragma once

/// @file modal_analysis.hpp
/// Offline modal analysis of a rendered impulse response or note: recover the
/// resonant modes (frequency, T60, amplitude), track partials and measure
/// inharmonicity, and resolve a single quasi-sinusoidal note's f0(t), decay
/// rate sigma(t) and Q(t) cycle by cycle.
///
/// This is the measurement side of physical modelling. A resonator is
/// specified as modes; this header reads the modes back out of what the
/// resonator actually emitted, so "did it do what I asked" is a number rather
/// than a spectrogram someone squints at.
///
/// Test/tool layer only — every entry point analyzes a buffer that has already
/// left the audio thread. Nothing here is realtime-safe.
///
/// Layering (test/support/README.md): this sits at the metrics layer. It takes
/// plain buffers and returns facts; it never renders and never asserts. It
/// depends only on `pulp::audio-analysis` (for the discovery FFT) and the
/// standard library — no scenario or contract knowledge.
///
/// ── Analyzer determinism contract ─────────────────────────────────────────
/// Every measurement below is deterministic for identical input on a given
/// platform: no randomness, no clocks, no threading, no platform math beyond
/// IEEE-754 double. Mode discovery runs one Hann-windowed FFT (via
/// `magnitude_spectrum_curve`); everything after it is an exact-frequency
/// Hann-windowed DFT probe plus least-squares fits, which are FFT-backend
/// independent. Discovery only nominates candidate frequencies — the reported
/// numbers all come from the refinement stage — so a backend that shifts a
/// peak by a fraction of a bin cannot move a result outside its tolerance.
///
/// ── Stated limits (read these before trusting a number) ───────────────────
///   * **Every result carries a `message`.** Read it. A `MeasuredMode` with
///     low `confidence` is a nomination, not a measurement.
///   * **Modes must be resolvable.** Two modes closer than roughly one main
///     lobe of the discovery window (`~4 * sample_rate / fft_length` for
///     Hann) merge into one peak and are reported as one mode at a pulled
///     frequency. `ModeAnalysisOptions::min_separation_hz` is the guard;
///     widen `fft_length` to separate closer pairs.
///   * **T60 is a slope fit over a dB window, extrapolated — never a
///     chase to −60 dB.** A detector that hunts for the −60 dB crossing
///     latches onto the noise floor (or onto a neighbouring mode's tail) and
///     reports a confident, wrong, and typically *flat* number. The fit
///     window (`fit_start_db`..`fit_end_db` below the mode's own onset level)
///     stays in the region where the mode dominates. A mode whose fit window
///     does not fit inside the buffer is reported with
///     `confidence == 0` and a message saying so, not silently truncated.
///   * **T60 assumes exponential decay.** A mode that beats against a close
///     neighbour, or one whose decay rate is time-varying (the TR-808 kick's
///     feedback loop is exactly this), does not have a single T60. The fit's
///     R^2 lands in `confidence`; a low value means "this is not one
///     exponential", not "the analyzer failed".
///   * **Amplitude is back-extrapolated to t = 0** from the windowed
///     magnitude using the fitted decay, so it is only as good as the fit.
///     It is calibrated to read the amplitude of the mode's contribution to
///     the impulse response at n = 0 — for `pulp::signal::ModalBankT` that is
///     exactly `ModalMode::gain`. The probe is a one-sided DFT, so a mode
///     whose frequency is within ~2 bins of DC leaks its own negative-
///     frequency image into its magnitude and reads high; widen
///     `envelope_window` for very low modes.
///   * **Attack Q is ill-defined in principle**, not merely hard: a window
///     shorter than one period of the attack frequency contains no cycle to
///     measure a decay over. `track_cycles` therefore never reports a sigma
///     for the attack cycle; it reports the cycle's frequency and flags it
///     `directional_only`. Treat that frequency as a direction, never a value.
///   * **sigma(t) has a runway.** The sliding fit needs `sigma_half_window`
///     cycles on each side, so the first and last few cycles carry
///     `sigma_valid == false`. On a fast glide most of the interesting motion
///     can be over before sigma becomes valid — check `f0_span_percent` over
///     the valid region before concluding anything about how sigma behaved
///     "through the sigh".
///   * **`track_cycles` assumes one dominant component.** It is a
///     zero-crossing tracker; a signal with two comparable partials will
///     produce crossings that belong to neither. Check
///     `CycleTrack::monocomponent_confidence` before believing it.
///
/// ── Calibration ───────────────────────────────────────────────────────────
/// The estimators here are calibrated against synthetic signals with known
/// ground truth in `test/test_modal_analysis.cpp`, including the control that
/// makes the sigma/Q results usable: a **constant-sigma signal carrying a
/// prescribed frequency glide must recover sigma dead flat**. An estimator
/// that invents decay variation from a frequency glide would manufacture
/// exactly the state-dependent-Q result one hopes to find. Any change to this
/// file must keep that test green.

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace pulp::test::audio {

// ── Modes ─────────────────────────────────────────────────────────────────

/// One resonant mode recovered from a rendered impulse response.
struct MeasuredMode {
    double freq_hz = 0.0;   ///< Refined mode frequency.
    double t60_s = 0.0;     ///< Time to decay 60 dB, from the fitted slope.
    double amplitude = 0.0; ///< Linear amplitude of this mode at t = 0.
    /// Fit quality in [0, 1]: the R^2 of the log-envelope slope fit, and zero
    /// when no fit was possible at all (see `note`). This is a goodness-of-fit
    /// score, NOT a probability. Above ~0.95 the mode is a clean single
    /// exponential; below ~0.7 the decay is not one exponential (beating, a
    /// close neighbour, or a genuinely time-varying decay rate) and only
    /// `freq_hz` should be trusted. A weak mode scores low on its own, because
    /// a noisy envelope does not fit a line — no separate weighting is applied.
    double confidence = 0.0;
    /// Peak height in dB relative to the strongest peak in the discovery
    /// spectrum. Only `analyze_modes` populates this; `measure_mode` runs no
    /// discovery and leaves it at 0, which is not a claim that the mode is the
    /// loudest one present.
    double prominence_db = 0.0;
    /// Number of envelope points the T60 slope fit consumed.
    int fit_points = 0;
    /// Empty when the mode fit cleanly; otherwise says what went wrong
    /// (fit window ran past the buffer, envelope non-monotonic, ...).
    std::string note;
};

/// Options for `analyze_modes`. Defaults suit a decaying instrument body IR
/// at 44.1/48 kHz; every threshold is named here rather than buried in the
/// implementation.
struct ModeAnalysisOptions {
    /// Discovery FFT length (power of two). Sets the bin resolution
    /// (`sample_rate / fft_length`) and therefore which modes can be
    /// separated at all — see the resolvability limit above.
    int fft_length = 32768;
    /// Discovery search band. Peaks outside it are ignored.
    double search_low_hz = 20.0;
    double search_high_hz = 20000.0;
    /// Reject peaks quieter than this (dB below the strongest peak).
    double peak_floor_db = -60.0;
    /// Two peaks closer than this are treated as one mode; the louder wins.
    double min_separation_hz = 8.0;
    /// Cap on reported modes (strongest first during discovery).
    int max_modes = 64;

    /// Half-width of the cents grid scanned around each discovery peak when
    /// refining its frequency, and the grid step. The refined frequency is a
    /// parabolic interpolation of the windowed-magnitude peak on this grid.
    double refine_span_cents = 60.0;
    double refine_step_cents = 2.0;
    /// Window (samples) used by the refinement DFT probe. Must be long enough
    /// to resolve the mode from its neighbours and short enough that the mode
    /// has not decayed into the noise floor across it.
    int refine_window = 8192;

    /// dB window, relative to the mode's own level at `fit_offset_s`, over
    /// which the log-envelope slope is fit. The T60 is the extrapolation of
    /// that slope to −60 dB. Do NOT widen `fit_end_db` toward −60 expecting a
    /// better answer: the point of the window is to stay above the floor.
    double fit_start_db = -3.0;
    double fit_end_db = -30.0;
    /// Skip this much of the head before fitting, so an excitation pulse or a
    /// processor's pre-ring is outside the fit region.
    double fit_offset_s = 0.005;
    /// Envelope tracking window/hop for the decay fit, in samples.
    int envelope_window = 2048;
    int envelope_hop = 512;
    /// A slope fit needs at least this many envelope points to be reported
    /// with non-zero confidence.
    int min_fit_points = 4;
    /// Channel index when a multi-channel view is reduced (unused by the
    /// span overloads; present so callers can record their choice).
    int channel = 0;
};

/// Modes recovered from an impulse response, strongest first.
struct ModeAnalysis {
    std::vector<MeasuredMode> modes;
    double sample_rate = 0.0;
    double bin_hz = 0.0;          ///< Discovery resolution, sample_rate/fft_length.
    /// Frequencies closer than roughly this cannot be separated by the
    /// discovery window at all (Hann main lobe = 4 bins).
    double resolution_limit_hz = 0.0;
    int discovered_peaks = 0;     ///< Candidates before the mode cap was applied.
    bool ok = false;              ///< False when the buffer was unusable.
    std::string message;          ///< Always populated; read it.
};

/// Recover the modes present in `ir`, a rendered impulse response (mono).
///
/// Discovery nominates spectral peaks; each is then refined on a cents grid
/// and given a T60 from a log-envelope slope fit over the stated dB window.
/// Returns modes sorted by descending amplitude. A mode that could not be fit
/// still appears — with `confidence == 0` and a `note` — because "there is a
/// peak at 830 Hz that does not decay exponentially" is a finding, not a
/// failure.
ModeAnalysis analyze_modes(std::span<const float> ir, double sample_rate,
                           const ModeAnalysisOptions& options = {});

/// Measure one mode whose frequency is already known approximately. Skips
/// discovery entirely — use this when a test prescribed the mode and wants to
/// read it back (the calibration path), or when a peak is known to be buried
/// under a louder neighbour that discovery would nominate instead.
MeasuredMode measure_mode(std::span<const float> ir, double sample_rate,
                          double guess_hz,
                          const ModeAnalysisOptions& options = {});

/// One-line human-readable summary of a mode analysis, for INFO() in a test.
std::string summarize(const ModeAnalysis& analysis);
std::string summarize(const MeasuredMode& mode);

// ── Partials and inharmonicity ────────────────────────────────────────────

/// One tracked partial of a (quasi-)harmonic sound.
struct MeasuredPartial {
    int index = 0;                 ///< Harmonic number, 1 = fundamental.
    double freq_hz = 0.0;          ///< Measured frequency.
    double harmonic_hz = 0.0;      ///< index * f0, the ideal-string prediction.
    double deviation_cents = 0.0;  ///< Measured vs `harmonic_hz`, in cents.
    double amplitude = 0.0;        ///< Linear amplitude at t = 0.
    bool found = false;            ///< False when no peak was located.
};

/// Options for `measure_inharmonicity`.
struct InharmonicityOptions {
    int num_partials = 12;
    /// How far around each predicted partial to search, in cents. Widen for a
    /// very stiff string (high partials drift far sharp); narrow to stop the
    /// search wandering onto a neighbouring partial.
    double search_span_cents = 120.0;
    /// Amplitude floor, relative to the loudest partial, below which a
    /// partial is reported `found = false` rather than as a noise peak.
    double partial_floor_db = -60.0;
    ModeAnalysisOptions mode_options{};
};

/// Inharmonicity of a (quasi-)harmonic tone.
///
/// The model is the stiff string: `f_n = n * f0 * sqrt(1 + B * n^2)`, with B
/// the inharmonicity coefficient (dimensionless, ~1e-4 for a piano bass
/// string, ~0 for an ideal string). B is fit by least squares over the found
/// partials; `rms_deviation_cents` is the fit residual and is the honest
/// answer for anything the stiff-string model does not describe (a drum, a
/// bar, a bell), where B is meaningless and the per-partial deviations are
/// the actual result.
struct InharmonicityResult {
    double f0_hz = 0.0;               ///< Refined fundamental.
    double b_coefficient = 0.0;       ///< Stiff-string B (least squares).
    double rms_deviation_cents = 0.0; ///< Residual of the stiff-string fit.
    /// RMS deviation from the pure harmonic series (B = 0). Compare against
    /// `rms_deviation_cents` to see whether the stiff-string model bought
    /// anything at all.
    double rms_harmonic_deviation_cents = 0.0;
    std::vector<MeasuredPartial> partials;
    int found_partials = 0;
    bool ok = false;
    std::string message;
};

/// Track the partials of `signal` starting from `f0_guess` and measure the
/// inharmonicity. The fundamental is refined first, then each partial is
/// searched around the stiff-string prediction using the B estimated from the
/// partials found so far, so a stiff string's high partials are still found
/// after they have drifted well sharp of `n * f0`.
InharmonicityResult measure_inharmonicity(std::span<const float> signal,
                                          double sample_rate, double f0_guess,
                                          const InharmonicityOptions& options = {});

std::string summarize(const InharmonicityResult& result);

// ── Time-resolved f0(t), sigma(t), Q(t) ───────────────────────────────────

/// One cycle of a decaying quasi-sinusoid.
///
/// Q convention throughout: the envelope is `e^{-sigma t}`, so
/// `Q = pi * f0 / sigma`. (Quoting a Q without its envelope convention is how
/// factor-of-two arguments start.)
struct CycleObservation {
    double time_s = 0.0;        ///< Centre of the cycle.
    double freq_hz = 0.0;       ///< 1 / (period between zero crossings).
    double envelope = 0.0;      ///< Peak |x| within the cycle.
    double sigma_np_s = 0.0;    ///< Local decay rate, Np/s. Valid iff `sigma_valid`.
    double q = 0.0;             ///< pi * freq_hz / sigma_np_s. Valid iff `sigma_valid`.
    /// False for cycles within `half_window` of either end of the track,
    /// where the sliding log-envelope fit has no full window. `sigma_np_s`
    /// and `q` are zero there.
    bool sigma_valid = false;
    /// True for the first tracked cycle — the attack cycle, whose `freq_hz` is
    /// a mixture of the attack-mode resonance and excitation feedthrough
    /// rather than a settled f0. Treat its frequency as a direction, never a
    /// value. It never carries a sigma: the sliding fit needs `half_window`
    /// cycles of runway on both sides, so `sigma_valid` is always false here.
    /// This is not a limitation to route around — an attack window shorter
    /// than one period of the attack frequency contains no cycle to measure a
    /// decay over, so no attack Q exists to be measured.
    bool directional_only = false;
};

/// Options for `track_cycles`.
struct CycleTrackOptions {
    /// |x| above this counts as the onset.
    double onset_threshold = 1.0e-3;
    /// Start tracking this long after the detected onset, to clear the
    /// excitation transient. The default clears a few ms of contact pulse.
    double start_offset_s = 0.021;
    /// Stop tracking once |x| stays below max(|x|) * this ratio.
    double tail_floor_ratio = 3.0e-4;
    /// Half-width, in cycles, of the sliding log-envelope fit that yields
    /// sigma. Wider = smoother but blurs genuine sigma structure; narrower =
    /// responsive but noisy. 3 (a 7-cycle fit) is the calibrated default.
    int sigma_half_window = 3;
    /// A track with fewer usable cycles than this is reported `ok = false`.
    int min_cycles = 4;
};

/// Time-resolved track of one note.
struct CycleTrack {
    std::vector<CycleObservation> cycles;
    double onset_s = 0.0;
    double sample_rate = 0.0;
    /// Fraction of cycle periods within 20% of the median period, in [0, 1].
    /// A clean single component sits near 1.0. Well below ~0.8 means the
    /// zero-crossing tracker is seeing more than one comparable partial and
    /// the whole track is suspect — the estimator has no way to know which
    /// component it is following.
    double monocomponent_confidence = 0.0;
    bool ok = false;
    std::string message;

    /// Mean over cycles with `sigma_valid`, in [from_s, to_s). Returns 0 when
    /// the window holds no valid cycle.
    double mean_f0(double from_s, double to_s) const;
    double mean_sigma(double from_s, double to_s) const;
    double mean_q(double from_s, double to_s) const;
    /// Peak-to-peak spread of sigma over valid cycles in the window, as a
    /// percentage of its mean. The calibration control asserts on this.
    double sigma_span_percent(double from_s, double to_s) const;
    double f0_span_percent(double from_s, double to_s) const;
    /// Pearson correlation of f0 and Q over valid cycles in the window.
    /// Returns 0 when either is constant or fewer than 3 cycles qualify.
    double f0_q_correlation(double from_s, double to_s) const;
};

/// Track f0(t), sigma(t) and Q(t) of a decaying quasi-sinusoidal note, cycle
/// by cycle.
///
/// Method: positive-going zero crossings with sub-sample linear interpolation
/// give per-cycle periods (f0) and per-cycle peak envelopes; sigma is the
/// negative slope of a sliding least-squares fit of the log envelope over
/// `2 * sigma_half_window + 1` cycles; `Q = pi * f0 / sigma`.
///
/// This resolves the question a single T60 cannot: whether a resonator's decay
/// rate co-varies with its own instantaneous state within one note. The
/// calibration test proves it does not invent that co-variation out of a
/// frequency glide.
CycleTrack track_cycles(std::span<const float> signal, double sample_rate,
                        const CycleTrackOptions& options = {});

/// One short-window AR(2) pole fit.
struct Ar2Observation {
    double time_s = 0.0;      ///< Centre of the window.
    double freq_hz = 0.0;     ///< From the pole angle.
    double sigma_np_s = 0.0;  ///< From the pole radius, -ln(r) * fs.
    double q = 0.0;           ///< pi * freq_hz / sigma_np_s.
    /// Residual energy over signal energy in the window, in [0, 1]. A
    /// two-pole model fits a single decaying sinusoid to near zero; a large
    /// value means the window is not one mode and the pole is meaningless.
    double residual_ratio = 0.0;
};

/// Options for `track_ar2`.
struct Ar2TrackOptions {
    double window_s = 0.080;
    double hop_s = 0.020;
    double onset_threshold = 1.0e-3;
    double start_offset_s = 0.021;
    /// Windows whose peak |x| is below this are dropped (end of the tail).
    double silence_threshold = 1.0e-7;
};

/// AR(2) pole track — an independent cross-check on `track_cycles`.
///
/// Least-squares fits `x[n] = a1 x[n-1] + a2 x[n-2]` over short windows and
/// reads f0/sigma off the complex pole pair. It shares no machinery with the
/// zero-crossing tracker (no crossings, no envelope, no log), so agreement
/// between the two is real corroboration rather than one estimator's bias
/// showing up twice. Windows whose fit yields real poles (no resonance) are
/// dropped.
struct Ar2Track {
    std::vector<Ar2Observation> windows;
    bool ok = false;
    std::string message;
};

Ar2Track track_ar2(std::span<const float> signal, double sample_rate,
                   const Ar2TrackOptions& options = {});

std::string summarize(const CycleTrack& track);
std::string summarize(const Ar2Track& track);

// ── Primitives ────────────────────────────────────────────────────────────

/// Hann-windowed DFT magnitude of `x[start .. start+length)` evaluated at
/// exactly `freq_hz` — the measurement primitive the refinement and envelope
/// stages are built on. Evaluating at an exact frequency rather than a bin
/// centre is what removes the FFT's scalloping error from every number this
/// header reports. `length` is clamped to the available samples.
double windowed_magnitude(std::span<const float> x, std::size_t start,
                          std::size_t length, double freq_hz,
                          double sample_rate);

} // namespace pulp::test::audio
