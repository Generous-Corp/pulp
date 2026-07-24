#pragma once

// Calibration tables for the multi-character delay.
//
// This file is the module's tuning surface. Every value that was CHOSEN rather
// than derived sits here as a named constant on a knot table, so retuning the
// module by ear — or replacing a table wholesale with values measured from a
// real machine — is a data edit that touches no DSP logic. Nothing in this
// file is transcribed from another framework's source or from unpublished
// measurements; the mechanisms are from the open literature (see the module
// header for the concept-to-citation map) and the numbers are our own starting
// points.
//
// Values that are NOT design parameters — published physical or mathematical
// constants — are marked as such inline and are not knot tables: they are facts
// (chip stage counts, the 50 µs emphasis time constant, Butterworth pole Qs,
// γ-Fe2O3 material constants, Dattorro's published diffuser network) and moving
// them would make the model wrong rather than differently tuned.

#include <array>
#include <cstddef>
#include <cstdint>

namespace pulp::signal::chardelay {

// ── Engine-wide ───────────────────────────────────────────────────────────

/// Longest addressable delay. Sizes every buffer at set_sample_rate().
inline constexpr double kMaxDelayMs = 2000.0;

/// Coefficient recomputation and stochastic-walk stepping cadence, in samples.
/// 32 at 48 kHz is 1.5 kHz of control bandwidth — far above any modulation in
/// this module and far below the cost of doing it per sample.
inline constexpr int kControlInterval = 32;

/// De-zipper glide for scalar params (feedback, crossfeed, duck, freeze, and
/// the character table's per-sample outputs), in seconds.
inline constexpr double kParamGlideS = 0.005;

/// Feedback ceiling for the characters with NO in-loop saturator. Without a
/// bounding nonlinearity, unity feedback is a marginally stable integrator, so
/// the clamp — not a NaN sanitizer — is what guarantees decay.
inline constexpr double kUnsaturatedFeedbackMax = 0.98;

/// Feedback ceiling for the saturating characters. Above unity is deliberate:
/// driving a saturating loop past 1.0 is the classic self-oscillating dub
/// behavior, bounded by the loop saturator rather than by a clamp.
inline constexpr double kSaturatedFeedbackMax = 1.1;

/// Generic modulation LFO: log rate map and maximum depth as a fraction of
/// delay time. [design parameters; depth range 0.02–0.1]
inline constexpr double kModRateMinHz = 0.05;
inline constexpr double kModRateMaxHz = 10.0;
inline constexpr double kModMaxDepth = 0.05;

/// Right-channel depth scaler for every shared-LFO modulation. One oscillator
/// with slightly unequal depths reads as natural width without the slow phase
/// drift two free-running oscillators produce. [design parameter, 0.9–0.99]
inline constexpr double kStereoDecorr = 0.95;

/// PRNG seed. [design parameter — the requirement is determinism, not the
/// value.] Streams that must not correlate derive their own seeds from it.
inline constexpr std::uint32_t kPrngSeed = 74207u;

// ── Delay-time slew, per character ────────────────────────────────────────
// The character's transport inertia, in milliseconds. Every character reads
// through a slewed continuous time, so a time change produces the physically
// correct pitch glide; there is deliberately no crossfade-jump path anywhere.
// [all design parameters]
inline constexpr double kTimeSlewCleanMs = 15.0;      // 5–30, de-zipper only
inline constexpr double kTimeSlewVintageMs = 50.0;    // 30–80, clock glide
inline constexpr double kTimeSlewTapeMs = 250.0;      // 150–400, motor/capstan lag
inline constexpr double kTimeSlewBbdMs = 80.0;        // 50–150, clock glide
inline constexpr double kTimeSlewDiffusionMs = 15.0;  // 5–30, de-zipper only

// ── Clean Modern ──────────────────────────────────────────────────────────
// [all design parameters]
inline constexpr std::array<double, 3> kCleanAxis = {0.0, 0.5, 1.0};
inline constexpr std::array<double, 3> kCleanLoopLpHz = {20000.0, 12000.0, 6000.0};
inline constexpr std::array<double, 3> kCleanLoopHpHz = {20.0, 20.0, 120.0};

/// A loop lowpass at or above this cutoff is BYPASSED rather than run. That
/// makes the character_amount = 0 configuration an exact null-test baseline:
/// one pass through the wet path then differs from a textbook Lagrange delay
/// by the 20 Hz DC-removal highpass and nothing else, which is the property
/// the clean character is specified to have.
inline constexpr double kLoopLpBypassHz = 20000.0;

// ── Vintage Digital ───────────────────────────────────────────────────────
// No open paper documents the early-80s rack units this character evokes; the
// MECHANISMS are the published general ones (band-limited clocked conversion,
// pre/de-emphasis around a quantizer, low-bit PCM artifacts — Välimäki et al.
// 2008), but every value below is a design parameter.
inline constexpr std::array<double, 4> kVintageAxis = {0.0, 0.33, 0.67, 1.0};
inline constexpr std::array<double, 4> kVintageInternalRateHz = {32000.0, 24000.0, 16000.0,
                                                                 8000.0};
inline constexpr std::array<double, 4> kVintageBits = {14.0, 12.0, 12.0, 10.0};

/// Anti-alias / reconstruction cutoff as a fraction of the internal rate.
inline constexpr double kVintageAntiAliasFraction = 0.45;

/// 3183 Hz is the 50 µs time constant — the published CD/FM-era emphasis
/// standard (IEC 60908), not a tuned value. The shelf AMOUNT is a design
/// parameter [6–12 dB].
inline constexpr double kVintageEmphasisHz = 3183.0;
inline constexpr double kVintageEmphasisDb = 9.0;

// ── Tape (standard tier) ──────────────────────────────────────────────────
// [all design parameters unless noted]
inline constexpr std::array<double, 4> kTapeAxis = {0.0, 0.33, 0.67, 1.0};
inline constexpr std::array<double, 4> kTapeDrive = {1.5, 2.5, 4.0, 6.0};
inline constexpr std::array<double, 4> kTapeBias = {0.0, 0.10, 0.20, 0.30};
inline constexpr std::array<double, 4> kTapeWowDepthMs = {0.0, 0.15, 0.5, 1.2};
inline constexpr std::array<double, 4> kTapeFlutterDepthMs = {0.0, 0.05, 0.15, 0.4};
inline constexpr std::array<double, 4> kTapeLossLpHz = {12000.0, 9000.0, 6000.0, 3500.0};
inline constexpr std::array<double, 4> kTapeBumpDb = {1.5, 2.0, 3.0, 4.0};

/// Record/playback EQ bracketing the saturator: boost HF into saturation, cut
/// it after, so the saturator works the highs harder than the lows. The pair
/// is exactly inverse, so with the saturator bypassed it is transparent.
inline constexpr double kTapeEmphasisDb = 3.0;    // 0–6
inline constexpr double kTapeEmphasisHz = 1600.0; // 1–3 kHz

/// Loss stand-in head bump (the physical tier replaces this with a
/// speed-dependent center; see kHeadBumpIps/kHeadBumpHz).
inline constexpr double kTapeBumpHz = 60.0;
inline constexpr double kTapeBumpQ = 1.5;

/// In-loop DC blocker. [10–20 Hz, the published range for tape-machine
/// playback electronics]
inline constexpr double kTapeDcBlockHz = 15.0;

/// Wow: periodic + stochastic. The 0.5–6 Hz band is published (Chowdhury 2019
/// §3); the specific rate and the Ornstein-Uhlenbeck walk parameters are
/// design choices within it.
inline constexpr double kWowRateHz = 1.0;
inline constexpr double kWowOuTheta = 1.5;      // s^-1 mean-reversion
inline constexpr double kWowOuSigma = 0.6;      // innovation scale
inline constexpr double kWowOuNoiseLpHz = 10.0; // pre-filter on the innovation

/// Flutter: published band 5–100 Hz. Rotational components produce harmonic
/// structure, so a small harmonic stack; count, ratios, amplitudes and phases
/// are all design parameters.
inline constexpr double kFlutterBaseHz = 8.0;
inline constexpr std::array<double, 3> kFlutterAmplitudes = {1.0, 0.5, 0.25};
inline constexpr std::array<double, 3> kFlutterPhases = {0.0, 0.33, 0.71};  // cycles

/// Right-channel flutter phase offset, in cycles. Deterministic but
/// uncorrelated with the left channel.
inline constexpr double kFlutterPhaseRight = 0.41;

// ── BBD ───────────────────────────────────────────────────────────────────
inline constexpr std::array<double, 3> kBbdAxis = {0.0, 0.5, 1.0};

/// PUBLISHED chip stage counts (Panasonic MN3005 / MN3008 / MN3007
/// datasheets), not design parameters: they are what sets each device's
/// delay-per-clock and therefore its bandwidth at a given time.
inline constexpr std::array<double, 3> kBbdStages = {4096.0, 2048.0, 1024.0};

inline constexpr std::array<double, 3> kBbdDrive = {0.10, 0.30, 0.50};
inline constexpr std::array<double, 3> kBbdJitterAmount = {0.0, 0.3, 0.6};
inline constexpr std::array<double, 3> kBbdFlutterDepth = {0.0, 0.005, 0.015};

/// Largest stage count any table knot may request; sizes the bucket buffer.
inline constexpr std::size_t kBbdMaxStages = 4096;

/// Internal oversampling of the clocked core. Must comfortably exceed the
/// clock rate across the delay range. [8–24]
inline constexpr int kBbdOversample = 16;

/// Bandwidth law: the usable passband must sit well below the clock's Nyquist
/// (the published aliasing constraint in both BBD papers). The /3 margin keeps
/// a 2-pole transition band below f_clock/2. [limits are design parameters]
inline constexpr double kBbdBandwidthDivisor = 3.0;
inline constexpr double kBbdBandwidthMinHz = 300.0;
inline constexpr double kBbdBandwidthMaxHz = 10000.0;

/// Compander: hardware τ is set by an external capacitor, so real units vary.
/// [1–20 ms]. The floor is a divide-by-near-zero guard, not a tuned value.
inline constexpr double kBbdCompanderTauMs = 10.0;
inline constexpr double kBbdCompanderFloor = 0.01;

/// The compander holds loop level near the rail, so the waveshaper's drive is
/// backed off when it runs behind one. [0.2–0.5]
inline constexpr double kBbdCompanderDriveScale = 0.3;

/// Asymmetric soft clip: gain map and DC offset for even-harmonic content.
inline constexpr double kBbdDriveGainSpan = 9.0;  // drive 0..1 → gain 1..10
inline constexpr double kBbdShape = 0.25;         // 0–0.5

/// Clock jitter, as a fraction of the clock period. [1e-4–2e-3]
inline constexpr double kBbdJitterMax = 5e-4;
inline constexpr double kBbdJitterSmoothness = 0.5;

/// BBD flutter rate map, Hz.
inline constexpr double kBbdFlutterRateBaseHz = 0.3;
inline constexpr double kBbdFlutterRateSpanHz = 1.2;

// ── Diffusion ─────────────────────────────────────────────────────────────
// The base network is Dattorro's PUBLISHED input diffuser (Effect Design Part
// 1, Table 1): 142/107/379/277 samples at his 29.8 kHz reference rate, given
// here in milliseconds, with his published coefficients. Only the scalings are
// design parameters.
inline constexpr std::array<double, 4> kDiffusionStageMs = {4.7651, 3.5906, 12.7181,
                                                            9.2953};
inline constexpr std::array<double, 4> kDiffusionStageGain = {0.75, 0.75, 0.625, 0.625};

inline constexpr std::array<double, 3> kDiffusionAxis = {0.0, 0.5, 1.0};
inline constexpr std::array<double, 3> kDiffusionSizeScale = {0.5, 1.0, 1.7};
inline constexpr std::array<double, 3> kDiffusionGainScale = {0.7, 1.0, 1.12};

/// Stability / metallicity guard. The published useful range for a Schroeder
/// allpass coefficient tops out well below 1.
inline constexpr double kDiffusionGainMax = 0.85;

/// Anti-metallic modulation of stages 1 and 3 (Dattorro modulates his
/// diffusers). Depths in samples, rates in Hz. [design parameters]
inline constexpr double kDiffusionModDepthSamples = 2.0;
inline constexpr std::array<double, 2> kDiffusionModRatesHz = {0.40, 0.53};

/// Diffusion loop filters. [LP is a design parameter]
inline constexpr double kDiffusionLoopHpHz = 20.0;
inline constexpr double kDiffusionLoopLpHz = 9000.0;

// ── Reverse ───────────────────────────────────────────────────────────────
/// Raised-cosine splice fade at each end of a reversed segment, in samples.
/// [32–256]
inline constexpr int kReverseFadeSamples = 96;

// ── Ducking ───────────────────────────────────────────────────────────────
// [all design parameters]
inline constexpr double kDuckAttackS = 0.005;   // 1–20 ms
inline constexpr double kDuckReleaseS = 0.250;  // 50–1000 ms
inline constexpr double kDuckThreshold = 0.1;   // ≈ −20 dBFS

// ── Physical tape tier ────────────────────────────────────────────────────

/// PUBLISHED γ-Fe2O3 material constants (Jiles, Thoelke & Devine 1992,
/// reproduced in Chowdhury 2019). Units are A/m except the dimensionless c and
/// α. The user-facing drive/saturation/bias macros re-map onto a, M_s and c;
/// k and α are held at their published values.
inline constexpr double kJaSaturationMagnetization = 3.5e5;  // M_s
inline constexpr double kJaAnhystereticShape = 2.2e4;        // a
inline constexpr double kJaPinning = 2.7e4;                  // k
inline constexpr double kJaReversibility = 0.2;              // c, published 0.1–0.3
inline constexpr double kJaInterdomainCoupling = 1.6e-3;     // α

/// Newton-Raphson budget. The convergence threshold is referenced to the
/// 24-bit noise floor (≈ −144 dBFS) in the normalized magnetization units the
/// solver works in. [iteration cap is a design parameter, 4–8]
inline constexpr int kJaMaxIterations = 8;
inline constexpr double kJaConvergenceThreshold = 6e-8;

/// Langevin argument clamp — keeps coth() and its derivative in the range
/// where the rational approximation is accurate and the solver well-scaled.
inline constexpr double kJaLangevinClamp = 30.0;
/// Below this |H| and |dH/dt|, magnetization is snapped to 0 so a silent loop
/// cannot hold a frozen DC magnetization.
inline constexpr double kJaSilenceThreshold = 1e-7;

/// Hysteresis oversampling factor and half-band design. 4× is Chowdhury's
/// "good across the range" finding; ≥8× shows diminishing returns. 65 taps at
/// Kaiser β ≈ 8 is the house half-band design — the stopband figure below is
/// the β = 8 point of the standard Kaiser formula, not an independent choice.
inline constexpr std::size_t kHysteresisHalfBandTaps = 65;
inline constexpr double kHysteresisHalfBandStopbandDb = 81.3;  // → β ≈ 8

/// Makeup-gain tolerance around unity small-signal gain, in dB.
inline constexpr double kJaMakeupToleranceDb = 1.0;

/// Wallace playback loss (Wallace 1951; physical ranges from Bertram 1994) is
/// split across two realizations — see tape_loss.hpp for why.
///
/// The gap null goes to a minimum-phase FIR: order at 48 kHz, scaled ∝ fs.
/// [64–128]
inline constexpr std::size_t kLossFirOrder48k = 96;
/// The smooth spacing × thickness tilt goes to a fitted IIR cascade: how many
/// log-spaced points the least-squares fit is evaluated at, and how many
/// simplex iterations it gets. The fit runs on the control thread, once per age
/// knot per tape speed.
inline constexpr int kLossIirFitPoints = 64;

/// Sections in each fitted cascade.
///
/// The counts are set by MEASUREMENT, not by taste. The spacing term is
/// exp(-k·d), whose magnitude in dB is linear in FREQUENCY — on a
/// log-frequency axis a curve whose slope steepens without bound, while every
/// filter section contributes a bounded 20 dB/decade. Brute-forced worst-case
/// error against the analytic magnitude:
///
///     2 poles + 1 shelf   7.5 ips 2.2 dB   3.75 ips 5.3 dB   1.875 ips 5.3 dB
///     2 poles + 2 shelves         1.2               2.7                2.5
///     2 poles + 3 shelves         0.8               1.9                1.5
///     2 poles + 5 shelves         0.3               0.9                0.6
///
/// Five shelves is where +-1 dB is reached across the speed range. The
/// thickness term is far gentler — it falls asymptotically at 20 dB/decade, so
/// it is nearly one pole already — and needs only three sections.
inline constexpr std::size_t kSpacingPoleSections = 3;
inline constexpr std::size_t kSpacingShelfSections = 8;
inline constexpr std::size_t kThicknessPoleSections = 1;
inline constexpr std::size_t kThicknessShelfSections = 2;

inline constexpr std::size_t kLossPoleSections =
    kSpacingPoleSections + kThicknessPoleSections;
inline constexpr std::size_t kLossShelfSections =
    kSpacingShelfSections + kThicknessShelfSections;

/// Search dimension of ONE shape fit (the larger of the two).
inline constexpr std::size_t kLossSearchDimension =
    kSpacingPoleSections + 2u * kSpacingShelfSections;

/// THE FITTED SHAPES, as shipped.
///
/// These are constants of the PHYSICS, not of any machine or setting: the
/// spacing term depends on frequency only through f·d/v and the thickness term
/// only through f·δ/v, so one fit of each dimensionless curve is correct at
/// every tape speed, every head-tape spacing and every sample rate, with its
/// corners multiplied by a scalar.
///
/// They are shipped rather than fitted at load because the fit is a multi-start
/// simplex search that takes over a second — fine as an offline derivation,
/// unacceptable as a stall when a plugin is instantiated. The derivation is not
/// lost: fit_tape_loss_shapes() still performs it, and the acceptance suite
/// re-derives the fit and checks that these values reproduce the same response,
/// so regenerating the table is a supported operation rather than a rewrite.
///
/// Worst-case error against the analytic curves: spacing 0.36 dB, thickness
/// 0.12 dB, both equiripple (which is the signature of a minimax fit sitting at
/// its structural optimum for the section count).
inline constexpr std::array<double, kSpacingPoleSections> kSpacingShapePoleX = {
    0.1736328177, 0.5872051207, 0.7066518389};
inline constexpr std::array<double, kSpacingShelfSections> kSpacingShapeShelfX = {
    0.0379101878, 0.8617586156, 1.195357912,  0.546821599,
    1.874319242,  1.250559327,  1.349688739,  0.5618309026};
inline constexpr std::array<double, kSpacingShelfSections> kSpacingShapeShelfDb = {
    -4.870968905, -22.02414799, -32.23365298, -35.9700058,
    -20.85639305, -35.24359399, -23.08806288, -40.0};
inline constexpr std::array<double, kThicknessPoleSections> kThicknessShapePoleX = {
    0.2127135512};
inline constexpr std::array<double, kThicknessShelfSections> kThicknessShapeShelfX = {
    0.03067430198, 0.1724152232};
inline constexpr std::array<double, kThicknessShelfSections> kThicknessShapeShelfDb = {
    -1.705785229, -0.6809460063};

/// Normalized-frequency range each shape is fitted over, and how many
/// log-spaced points. The range covers every (frequency, speed, spacing)
/// combination the module can reach.
inline constexpr double kLossShapeMinX = 1e-3;
inline constexpr double kLossShapeMaxX = 4.0;

/// Simplex iterations per start, restarts per start, and how many starts.
///
/// The shapes are DIMENSIONLESS: they do not depend on the sample rate, the
/// tape speed or the spacing, only on the form of the physics. So they are
/// fitted exactly once per process (see tape_loss_shapes()) and every instance
/// shares the result — which is what lets this be as thorough as it is without
/// costing anything at plugin load.
inline constexpr int kLossIirFitIterations = 4000;
inline constexpr int kLossIirFitRestarts = 3;
inline constexpr int kLossIirFitStarts = 16;

/// Deepest cut any one shelf may ask for.
inline constexpr double kLossShelfMinDb = -40.0;
/// NOTE: there are no age knots and no interpolation along the age axis.
/// Both loss terms are exact frequency SCALINGS of a fixed dimensionless shape
/// — the spacing term depends on frequency only through f·d/v and the thickness
/// term only through f·δ/v — so a shape fitted once is correct at every spacing
/// and every speed with its corners multiplied by a scalar. That removes the
/// interpolation entirely, which matters: an earlier design that fitted the
/// combined response at nine age knots and interpolated the parameters between
/// them was inside 1 dB AT the knots and 3-4 dB BETWEEN them, because two fits
/// of nearly the same curve can sit in different parameter basins and blending
/// two descriptions describes neither.
/// Crossfade applied when the tape SPEED changes and both filters are
/// redesigned. Two complete instances run and their OUTPUTS are crossfaded.
/// [20–50 ms]
inline constexpr double kLossBankCrossfadeS = 0.030;

/// Fixed tape geometry, in metres. Coating thickness and head gap sit inside
/// Bertram's published physical ranges (thickness 1–20 µm, gap 0.5–10 µm);
/// the specific values are design parameters. Spacing is the age axis.
inline constexpr double kTapeCoatingThicknessM = 12e-6;
inline constexpr double kTapeHeadGapM = 3e-6;

/// The IEC/NAB standard tape-speed set, in inches per second (published).
inline constexpr std::array<double, 5> kTapeSpeedsIps = {1.875, 3.75, 7.5, 15.0, 30.0};

/// Head-bump center against speed. The qualitative behavior (bump rises with
/// speed) is published; the mapping is a design parameter.
inline constexpr std::array<double, 5> kHeadBumpIps = {1.875, 3.75, 7.5, 15.0, 30.0};
inline constexpr std::array<double, 5> kHeadBumpHz = {35.0, 45.0, 60.0, 80.0, 105.0};

/// Age macro. In the physical tier `character_amount` becomes the age axis;
/// wow and flutter depths reuse kTapeTable's columns. [all design parameters]
inline constexpr std::array<double, 3> kAgeAxis = {0.0, 0.5, 1.0};
inline constexpr std::array<double, 3> kAgeSpacingUm = {5.0, 12.0, 25.0};
inline constexpr std::array<double, 3> kAgeHissDbfs = {-120.0, -78.0, -60.0};
inline constexpr std::array<double, 3> kAgeChewDepth = {0.0, 0.2, 0.6};
inline constexpr std::array<double, 3> kAgeDegrade = {0.0, 0.2, 0.5};

/// Hiss: white noise through a 1-pole lowpass, injected AFTER the delay line
/// so it accumulates per repeat the way real tape self-noise does.
inline constexpr double kHissLpHz = 4000.0;

/// Chew — an intermittent dropout modeled as a two-state machine. Durations
/// are drawn as `scale × r^variance` from the shared PRNG.
inline constexpr double kChewCleanScaleS = 1.5;
inline constexpr double kChewDegradedScaleS = 0.4;
inline constexpr double kChewVariance = 1.5;
inline constexpr double kChewTransitionS = 0.008;
inline constexpr double kChewPower = 2.0;
inline constexpr double kChewLpHz = 3000.0;

/// Degrade — continuous wear. Bandwidth falls exponentially toward this floor,
/// with a small noise floor and gain dip at maximum.
inline constexpr double kDegradeMinLpHz = 500.0;
inline constexpr double kDegradeGainDipDb = -1.5;
inline constexpr int kDegradeUpdateSamples = 1024;

}  // namespace pulp::signal::chardelay
