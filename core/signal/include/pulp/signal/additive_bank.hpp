#pragma once

/// @file additive_bank.hpp
/// The sinusoidal bank: a tone built by summing sinusoids, one phase
/// accumulator per partial, up to 128 of them.
///
/// This is the most physically direct synthesis method there is — a
/// spectrogram made audible. It earns its place in the catalog by covering two
/// things subtractive and wavetable engines cannot:
///
///   - **Sustained harmonic tone with independent partial control** — drawbar
///     organs, evolving pads, formant timbres where each harmonic breathes on
///     its own curve.
///   - **Inharmonic struck bodies** — bells, gongs, tuned percussion, electric
///     piano tines, where partials sit at non-integer ratios and each decays at
///     its own rate. A single global fade cannot make a bell; a bell is a
///     spectrum whose top dies in half a second while its hum rings for eight.
///
/// ## What this file is NOT: the analysis half of SMS
///
/// Spectral Modeling Synthesis (Serra & Smith 1990) is an analysis/resynthesis
/// system: it estimates partial tracks and a residual noise model FROM A
/// RECORDING, then replays them. This engine ships the SYNTHESIS half only — it
/// plays partial tables supplied by an author or a preset.
///
/// The reason is not squeamishness about difficulty. Analysis — peak picking,
/// partial tracking across frames, birth/death heuristics, residual
/// modelling — is an offline, non-real-time, framework-scale subsystem with its
/// own latency and its own tuning surface. Putting it behind a real-time node's
/// `process()` would mean either blocking the audio thread or inventing a
/// second, hidden threading contract. It belongs in a tooling pipeline that
/// emits voice tables this engine then plays. SMS is cited here as the lineage
/// that explains WHY per-partial trajectories matter, not as something
/// half-implemented.
///
/// The "plus stochastic" band of SMS — filtered noise for breath and attack
/// chiff — is likewise absent. This engine is purely tonal. Compose an external
/// noise source and `LpgT` for now.
///
/// Inverse-FFT synthesis (Rodet & Depalle 1992) is also out, and that one is a
/// measured tradeoff rather than a scoping decision: it costs one window/hop of
/// latency and per-frame phase-continuity bookkeeping, and only out-earns a
/// direct oscillator bank above several hundred partials. At this engine's
/// 128-partial ceiling the direct bank wins on both latency (0 versus a frame)
/// and control granularity (sample-accurate frequency trajectories versus frame
/// quantisation). See `latency_samples()`.
///
/// ## The four series laws, and where each one lands
///
/// **Law 1 (gain-carrying nonlinearity) — N/A, provably.** The signal path is a
/// pure linear sum of sinusoids. There is no waveshaper, no filter, no
/// feedback path, and therefore no loop gain to bound. The only gain question a
/// sine bank raises is the CREST FACTOR of a coherent sum, and that is bounded
/// by construction rather than by a limiter — see the normaliser below.
///
/// **Law 4 (oversampling) — N/A, and for a reason worth stating rather than
/// waving at.** A single sinusoid has no harmonics. There is no waveshape
/// nonlinearity anywhere in this engine to fold anything, so there is nothing
/// for oversampling to catch. The ONLY way this engine can alias is a partial
/// whose own frequency crosses Nyquist — a low fundamental with many harmonics,
/// or a glide or an inharmonicity sweep that walks a partial upward. That is
/// handled at the source by the per-partial Nyquist guard, which fades a
/// partial out before its frequency reaches `fs/2`. Oversampling would not help
/// with it: a partial above Nyquist is above Nyquist at any oversampling factor,
/// it just folds somewhere else.
///
/// **Law 5 (latency) — 0, tested.** Direct time-domain oscillators. Nothing is
/// buffered, so with the attack at zero an impulse-phase render is non-zero at
/// sample 0.
///
/// **Law 7 (scale invariance) — the interesting one, stated precisely because
/// the usual phrasing is wrong.** Partial amplitudes are sampled from a
/// dimensionless spectral-envelope SHAPE rather than stored as per-pitch
/// amplitude tables. That is the law's real content: never interpolate
/// independently-fitted per-partial amplitude sets across pitch, because
/// near-identical curves fit into different parameter basins and the
/// interpolation walks between them through timbres neither endpoint contains.
///
/// But "the spectral envelope makes the timbre pitch-independent" is only true
/// under a condition, and this header states it rather than repeating the
/// slogan. See `SpectralDomain` below.
///
/// ## The crest bound — the engine's entire gain safety
///
/// With every partial in phase — which is not a statistical worry but a
/// CONSTRUCTIBLE state, reached by storing φ̃ₚ = 0.25 for all p and
/// retriggering — the instantaneous sum reaches Σ|aₚ|. There is no feedback and
/// no limiter, so the bound has to come from a static normaliser applied at
/// control rate:
///
/// ```
///     g_norm = 1 / max(1, Σ_p |a_p| · E_p · tilt_p · g_nyq_p)
/// ```
///
/// where the sum runs over the current, post-envelope, post-morph, post-guard
/// amplitudes. Then by the triangle inequality
///
/// ```
///     |y| = |Σ_p g_p · env_p · sin(θ_p)|  ≤  Σ_p |g_p|  ≤  1
/// ```
///
/// since every per-partial envelope is in [0, 1]. This is an equality when the
/// phases align, so the bound is TIGHT, not conservative — and the suite
/// asserts both halves.
///
/// Two details that make the bound survive real use:
///
///   - **It holds during a parameter ramp.** Gains are interpolated linearly
///     between control blocks, and Σ|(1−α)·gᵒˡᵈ + α·gⁿᵉʷ| ≤ (1−α)Σ|gᵒˡᵈ| +
///     αΣ|gⁿᵉʷ| ≤ 1 by convexity. A normaliser applied only at block edges
///     would otherwise leave the crossfade unbounded.
///   - **`master_gain_db` sits OUTSIDE it.** The trim is a user control whose
///     legal range reaches +6 dB, so the honest worst case over the whole legal
///     parameter space is `10^(6/20) ≈ 1.995`, not 1.0. `worst_case_gain()`
///     reports that, and reports it as a measured invariant rather than an
///     estimate. The −6 dB default leaves headroom for the musically ordinary
///     case, where phases partially cancel and the sum sits well below its
///     bound.
///
/// *Phase is a timbre control, not just a determinism detail.* Coherent phases
/// give a percussive click transient (high crest); scattered phases give a
/// softer, airier attack at identical partial amplitudes. That is why the
/// retrigger policy is an enum on the public surface.
///
/// ## Why a `double` phase accumulator per partial
///
/// `osc::PhaseAccumulator`, one instance per partial, composed rather than
/// reimplemented. Its own doc block explains the arithmetic; the reason it
/// matters HERE is that a bank holds notes for a long time. At 20 Hz and 96 kHz
/// the increment is ~2e-4, where `float` carries ~1e-7 of relative error per
/// accumulation — which integrates into audible pitch drift over a held pad,
/// and into partials sliding out of their harmonic relationship with each
/// other, which is worse than either drifting alone.
///
/// ## The sine
///
/// The default is `std::sin(2π·φ)` evaluated in `double` — the catalog sine,
/// not a substitute for it. It is bit-identical to `osc::VaOscillator` at the
/// same phase, which the suite asserts. Float banks may explicitly select
/// `FastTrigProfile::realtime_precise`: on supported Apple arm64/Clang targets
/// four bounded-cycle degree-9 evaluations run together while phase
/// accumulation, wrap events, gain/frequency ramps, and envelope state remain
/// in double. The profile has its own two-float-LSB bank-output and −100 dB THD
/// gates. Other targets retain the same scalar approximation semantics but do
/// not claim an acceleration; query `trig_profile_has_vector_path()` before
/// presenting the choice as a performance feature. `AdditiveBank64` always
/// retains reference sine semantics.
///
/// ## Calibration table — every design parameter in one place
///
/// | Name | Default | Range | Why |
/// |---|---|---|---|
/// | `kMaxPartials` | 128 | 1 .. 256 | hard array bound; `prepare()` never exceeds it |
/// | `kPartialCountDefault` | 64 | 1 .. 128 | covers both shipped voices with headroom |
/// | `kFundamentalDefaultHz` | 220 | 20 .. 8000 | base pitch |
/// | `kInharmonicityDefault` | 0.0 | 0 .. 0.05 | 0 = pure harmonic; see the B note below |
/// | `kSpectralTiltDefaultDbOct` | −6 | −24 .. +6 | global brightness |
/// | `kMasterGainDefaultDb` | −6 | −60 .. +6 | trim; the default leaves crest headroom |
/// | `kAttackDefaultMs` | 5 | 0 .. 2000 | shared onset |
/// | `kReleaseDefaultMs` | 400 | 1 .. 20000 | key-off fall |
/// | `kDetuneDefaultCents` | 0 | 0 .. 50 | doublet spread; 0 disables the pair |
/// | `kAmpUpdateCadenceSamples` | 32 | 8 .. 128 | control-rate cadence, ramped between |
/// | `kNyquistGuardBandHz` | 1000 | 200 .. 4000 | taper width below `fs/2` |
/// | `kDoubletJitterSpread` | 0.5 | 0 .. 1 | per-mode variation in doublet beat rate |
/// | `kEnvelopeCurve` | 0.5 | 0 .. 1 | shared onset/release curvature |
/// | `SpectralEnvelope::kMaxBreakpoints` | 64 | 2 .. 64 | 16 is the documented working size |
///
/// **The inharmonicity coefficient B.** The LAW is published — Fletcher,
/// Blackham & Stratton 1962 — and so is the measured band across the piano
/// compass, roughly B ≈ 8e-5 to 2e-2, minimum in the tenor and rising toward
/// both extremes. The shipped reference points (piano tenor 0.0002, piano mid
/// 0.0008, upper treble 0.004, electric-piano tine 0.02) are design parameters
/// chosen INSIDE that published band. The parameter's range extends to 0.05,
/// which is an **honest gap**: no citable literature was found supporting
/// B > 0.02, and that headroom exists only for deliberately hyper-metallic
/// timbres beyond any measured real string or bar. No shipped voice uses it.
///
/// ## Citations
///
/// - Fletcher, H., Blackham, E. D., Stratton, R. (1962). "Quality of Piano
///   Tones." *JASA* 34(6), 749–761 — the stiff-string law `fₙ = n·f₀·√(1+Bn²)`
///   and the measured B band.
/// - Fletcher, N. H., Rossing, T. D. (1998). *The Physics of Musical
///   Instruments*, 2nd ed., Springer — stiff-string dispersion; bell modes.
/// - Serra, X., Smith, J. O. III (1990). "Spectral Modeling Synthesis."
///   *Computer Music Journal* 14(4), 12–24 — SMS lineage; analysis out of scope.
/// - Rodet, X., Depalle, Ph. (1992). "Spectral Envelopes and Inverse FFT
///   Synthesis." Proc. 93rd AES Convention — the spectral-envelope concept, and
///   the IFFT method this engine deliberately does not use.
/// - McAulay, R. J., Quatieri, T. F. (1986). "Speech Analysis/Synthesis Based on
///   a Sinusoidal Representation." *IEEE TASSP* 34(4), 744–754 — the lineage for
///   inter-frame amplitude/frequency interpolation. Cited as lineage: this
///   engine synthesises author-supplied trajectories, so linear-amplitude
///   ramping per control block is sufficient and cheaper than their cubic phase.
/// - Perrin, R., Charnley, T., de Pont, J. (1983). "Normal Modes of the Modern
///   English Church Bell." *JSV* 90(1), 29–49 — the bell's named modal ratios.
/// - Hammond, L. (1934). "Electrical Musical Instrument," US Patent 1,956,350 —
///   the drawbar-footage-to-harmonic mapping, cited as documented concept only.
/// - Freed, A., Rodet, X., Depalle, Ph. (1993). "Synthesis and Control of
///   Hundreds of Sinusoidal Partials on a Desktop Computer without Custom
///   Hardware." Proc. ICMC — partial-count feasibility.

#include <pulp/signal/denormal.hpp>
#include <pulp/signal/additive_spectral_envelope.hpp>
#include <pulp/signal/envelope.hpp>
#include <pulp/signal/fast_math.hpp>
#include <pulp/signal/osc/phase.hpp>
#include <pulp/signal/rng.hpp>
#include <pulp/signal/units.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <type_traits>
#include <vector>

namespace pulp::signal {

/// Which abscissa a `SpectralEnvelope`'s break-points are measured along.
/// One row of a voice table.
struct VoicePartial {
    double ratio = 1.0;      ///< Frequency as a multiple of `f0`.
    double amp = 1.0;        ///< Unit-referenced amplitude, pre-normaliser.
    double phase01 = 0.0;    ///< Stored initial phase in cycles, `[0, 1)`.
    double decay_ms = 0.0;   ///< Exponential τ. **≤ 0 means sustained** — the
                             ///< table notation for the organ's "∞" column.
};

/// A partial set. Fixed capacity; copying one is a memcpy, never an allocation.
struct VoiceTable {
    static constexpr int kMaxPartials = 256;

    /// Whether `inharmonicity_B` applies. A harmonic-typed voice (organ, piano,
    /// tine) gets the stiff-string stretch layered on top of its ratios; a
    /// modal voice (bell) does not, because its ratios are already absolute
    /// measurements of a body's modes and stretching them would be applying a
    /// string's physics to a casting.
    bool harmonic = true;
    int count = 0;
    std::array<VoicePartial, kMaxPartials> partials{};

    void clear() { count = 0; }
    bool add(const VoicePartial& p) {
        if (count >= kMaxPartials) return false;
        partials[static_cast<std::size_t>(count++)] = p;
        return true;
    }
};

/// The drawbar organ voice (§10a): nine tonewheel footages over a −12 dB/oct
/// harmonic tail.
///
/// The footage→harmonic mapping is documented behaviour (Hammond, US Patent
/// 1,956,350): 16′→0.5, 5⅓′→1.5, 8′→1, 4′→2, 2⅔′→3, 2′→4, 1⅗′→5, 1⅓′→6, 1′→8.
/// The drawbar LEVELS shipped here are a "full principal" registration and are
/// design parameters — a registration is a performance choice, not a published
/// constant. Partials past the ninth fill in as a −12 dB/oct tail so the voice
/// has body instead of stopping at a synthetic cliff.
///
/// Every partial is sustained (`decay_ms ≤ 0`): a tonewheel does not decay, it
/// is switched. All stored phases are 0 — documented tonewheel voices are
/// phase-coherent.
inline VoiceTable make_organ_voice(int count = 64) {
    static constexpr double kDrawbarRatio[9] = {0.5, 1.0, 1.5, 2.0, 3.0,
                                                4.0, 5.0, 6.0, 8.0};
    static constexpr double kDrawbarAmp[9] = {0.50, 1.00, 0.35, 0.70, 0.45,
                                              0.55, 0.25, 0.20, 0.30};
    /// Level of the harmonic tail at ratio 1, before its −12 dB/oct rolloff.
    /// [design parameter] default 0.30, range 0.0 .. 1.0.
    constexpr double kTailLevel = 0.30;

    VoiceTable v;
    v.harmonic = true;
    const int n = std::clamp(count, 1, VoiceTable::kMaxPartials);
    for (int p = 0; p < n; ++p) {
        VoicePartial row;
        if (p < 9) {
            row.ratio = kDrawbarRatio[p];
            row.amp = kDrawbarAmp[p];
        } else {
            // −12 dB/oct is amplitude ∝ 1/ratio², which is what the exponent 2
            // here means; writing it as a power of the ratio keeps the tail
            // scale-invariant rather than tied to a partial index.
            row.ratio = static_cast<double>(p + 1);
            row.amp = kTailLevel / (row.ratio * row.ratio);
        }
        row.phase01 = 0.0;
        row.decay_ms = 0.0;   // sustained
        v.add(row);
    }
    return v;
}

/// The bell voice (§10b): a modal body whose partials each decay at their own
/// rate.
///
/// The five named modes — hum 0.5, prime 1.0, tierce ≈1.2, quint 1.5, nominal
/// 2.0 — are the documented tuned partials of the modern English church bell
/// (Perrin, Charnley & de Pont 1983; Fletcher & Rossing). Ratios above the
/// nominal are design parameters: the cited work establishes that higher modes
/// exist, but the specific fill used here (2.50, 2.66, 3.00, 4.00, 5.00, then a
/// geometric 5.0·1.14^(p−9) cluster) is not individually sourced from it. All
/// amplitudes and decay times throughout are design parameters.
///
/// The decay SPREAD is the point of this voice and the reason `per_partial_decay`
/// exists: the nominal rings for 2.5 s while the p=10 cluster mode dies in about
/// 526 ms, a ~4.75:1 ratio, heard as the bell brightening downward into its hum.
inline VoiceTable make_bell_voice(int count = 64) {
    static constexpr double kModeRatio[10] = {0.50, 1.00, 1.19, 1.50, 2.00,
                                              2.50, 2.66, 3.00, 4.00, 5.00};
    static constexpr double kModeAmp[10] = {0.60, 1.00, 0.70, 0.55, 0.80,
                                            0.35, 0.30, 0.40, 0.30, 0.20};
    static constexpr double kModeDecayMs[10] = {8000, 6000, 4000, 3000, 2500,
                                                1500, 1400, 1200, 800,  500};
    /// Geometric spacing of the upper-mode cluster above the tenth partial.
    /// [design parameter] default 1.14, range 1.05 .. 1.30.
    constexpr double kClusterRatioStep = 1.14;
    /// Upper-cluster amplitude and decay laws.
    /// [design parameter] default level 0.20 with exponent 1.5; decay scale
    /// 3000 ms divided by the ratio.
    constexpr double kClusterLevel = 0.20;
    constexpr double kClusterAmpExponent = 1.5;
    constexpr double kClusterDecayScaleMs = 3000.0;

    VoiceTable v;
    v.harmonic = false;   // modal: the ratios are the measurement
    const int n = std::clamp(count, 1, VoiceTable::kMaxPartials);
    for (int p = 0; p < n; ++p) {
        VoicePartial row;
        if (p < 10) {
            row.ratio = kModeRatio[p];
            row.amp = kModeAmp[p];
            row.decay_ms = kModeDecayMs[p];
        } else {
            row.ratio = 5.0 * std::pow(kClusterRatioStep,
                                       static_cast<double>(p - 9));
            row.amp = kClusterLevel * std::pow(1.0 / row.ratio,
                                               kClusterAmpExponent);
            row.decay_ms = kClusterDecayScaleMs / row.ratio;
        }
        row.phase01 = 0.0;
        v.add(row);
    }
    return v;
}

/// A sinusoidal-bank additive synthesizer.
template <typename SampleType = float>
class AdditiveBankT {
public:
    /// Phase treatment on `retrigger()`.
    enum class RetrigPhase : std::uint8_t {
        reset_stored,    ///< Every phase returns to its table `phase01`. The
                         ///< default, because it makes the attack bit-identical
                         ///< every strike (series law 2).
        free_run,        ///< Phases keep running. A softer, already-sounding
                         ///< re-entry; still deterministic given trigger times.
        seeded_random,   ///< Phases drawn from the `Xorshift32` seeded at
                         ///< `reset()`. Diffuse attack with no crest alignment,
                         ///< identical across render/reset/re-render.
    };

    /// How a partial's amplitude evolves.
    enum class EnvelopeMode : std::uint8_t {
        shared_ar,           ///< One onset/release shape for every partial.
                             ///< The organ/pad case, where partials move
                             ///< together.
        per_partial_decay,   ///< The shared onset times each partial's own
                             ///< `exp(−t/τ_p)`. The physical mode: higher
                             ///< partials die first, which is what makes a
                             ///< struck body read as an object.
    };

    // ── Calibration constants ─────────────────────────────────────────────

    static constexpr int kMaxPartialsCeiling = VoiceTable::kMaxPartials;
    static constexpr int kMaxPartialsDefault = 128;
    static constexpr int kPartialCountDefault = 64;

    static constexpr double kFundamentalMinHz = 20.0;
    static constexpr double kFundamentalMaxHz = 8000.0;
    static constexpr double kFundamentalDefaultHz = 220.0;

    static constexpr double kInharmonicityDefault = 0.0;
    /// Ceiling of the Fletcher/Blackham/Stratton measured band.
    static constexpr double kInharmonicityCitedMax = 0.02;
    /// Parameter ceiling. Beyond `kInharmonicityCitedMax` this is an uncited
    /// extension — see the honest-gap note in the file doc block.
    static constexpr double kInharmonicityMax = 0.05;
    /// Reference points, all inside the cited band.
    static constexpr double kBPianoTenor = 0.0002;
    static constexpr double kBPianoMid = 0.0008;
    static constexpr double kBUpperTreble = 0.004;
    static constexpr double kBElectricPianoTine = 0.02;

    static constexpr double kSpectralTiltMinDbOct = -24.0;
    static constexpr double kSpectralTiltMaxDbOct = 6.0;
    static constexpr double kSpectralTiltDefaultDbOct = -6.0;

    static constexpr double kMasterGainMinDb = -60.0;
    static constexpr double kMasterGainMaxDb = 6.0;
    static constexpr double kMasterGainDefaultDb = -6.0;

    static constexpr double kAttackDefaultMs = 5.0;
    static constexpr double kAttackMinMs = 0.1;
    static constexpr double kAttackMaxMs = 2000.0;
    static constexpr double kReleaseDefaultMs = 400.0;
    static constexpr double kReleaseMinMs = 1.0;
    static constexpr double kReleaseMaxMs = 20000.0;

    static constexpr double kDetuneDefaultCents = 0.0;
    static constexpr double kDetuneMaxCents = 50.0;
    /// Per-mode variation in the doublet's detune, as a fraction of the
    /// nominal. Real bells do not beat in lockstep; without this every mode
    /// beats at the same rate and the result reads as a chorus effect rather
    /// than a casting asymmetry.
    /// [design parameter] default 0.5, range 0 .. 1.
    static constexpr double kDoubletJitterSpread = 0.5;

    /// Control-rate cadence for frequency/gain recomputation, with linear
    /// ramping between updates so partial gains never step.
    /// [design parameter] default 32 samples, range 8 .. 128.
    static constexpr int kAmpUpdateCadenceSamples = 32;

    /// Width of the raised-cosine taper below Nyquist.
    /// [design parameter] default 1000 Hz, range 200 .. 4000 Hz.
    static constexpr double kNyquistGuardBandHz = 1000.0;

    /// Curvature of the shared onset and release segments.
    /// [design parameter] default 0.5, range 0 .. 1.
    static constexpr double kEnvelopeCurve = 0.5;

    static constexpr double kTwoPi = 6.283185307179586476925286766559;

    // ── Static laws (shipped, so tests compute expectations from them) ────

    /// The published stiff-string law: `f = f0 · ratio · √(1 + B·ratio²)`.
    ///
    /// Note the mode number used for the stretch is the RATIO, not the array
    /// index. For a pure harmonic table those are the same thing, but for a
    /// table like the organ's — whose second row is the 16′ sub at ratio
    /// 0.5 — they are not, and the physics depends on which mode of the string
    /// is vibrating, not on where the author happened to put the row.
    static double partial_frequency_hz(double f0_hz, double ratio,
                                       double inharmonicity_b) {
        return f0_hz * ratio *
               std::sqrt(1.0 + inharmonicity_b * ratio * ratio);
    }

    /// The Nyquist guard: unity below `fs/2 − kNyquistGuardBandHz`, a raised
    /// cosine to exactly zero at `fs/2`, and zero above it. Smooth at both
    /// ends, so a partial gliding through the guard fades rather than clicking.
    static double nyquist_guard_gain(double f_hz, double sample_rate) {
        const double nyquist = 0.5 * sample_rate;
        const double taper_start = nyquist - kNyquistGuardBandHz;
        if (f_hz <= taper_start) return 1.0;
        if (f_hz >= nyquist) return 0.0;
        const double t = (f_hz - taper_start) / kNyquistGuardBandHz;
        return 0.5 * (1.0 + std::cos(kPi() * t));
    }

    /// The worst-case output magnitude over the entire legal parameter space.
    /// The normaliser bounds the partial sum to 1 by construction, so this is
    /// the master trim's legal maximum and nothing else. An equality, attained
    /// with aligned phases — not a safety estimate.
    static double worst_case_gain() {
        return units::db_to_linear(kMasterGainMaxDb);
    }

    /// Zero. Direct time-domain oscillators; nothing is buffered.
    static constexpr int latency_samples() { return 0; }

    // ── Lifecycle ─────────────────────────────────────────────────────────

    AdditiveBankT() { prepare(44100.0, kMaxPartialsDefault); }

    /// Sizes the per-partial arrays and returns the bank to a fresh state.
    /// **The only method here that may allocate**, and only when `max_partials`
    /// grows past what is already reserved.
    void prepare(double sample_rate, int max_partials = kMaxPartialsDefault) {
        if (std::isfinite(sample_rate) && sample_rate > 0.0) sample_rate_ = sample_rate;
        max_partials_ = std::clamp(max_partials, 1, kMaxPartialsCeiling);
        partial_count_ = std::min(partial_count_, max_partials_);

        // Two slots per partial: a doublet is a genuine pair of oscillators,
        // not a modulation trick, so the storage is sized for it up front and
        // `prepare` stays the only allocation point whether or not the user
        // later engages the detune.
        const auto slots = static_cast<std::size_t>(2 * max_partials_);
        phase_.resize(slots);
        gain_.assign(slots, 0.0);
        gain_step_.assign(slots, 0.0);
        increment_.assign(slots, 0.0);
        increment_step_.assign(slots, 0.0);
        decay_env_.assign(slots, 1.0);
        decay_coef_.assign(slots, 1.0);
        slot_detune_.assign(slots, 1.0);
        render_slots_.assign(slots, 0);

        onset_.prepare(sample_rate_);
        onset_.set_curve(kEnvelopeCurve);
        refresh_onset();
        reset();
    }

    /// Clears every phase, envelope, and ramp, and rewinds the generator, so a
    /// render from here is bit-identical every time.
    void reset() {
        rng_.reset();
        onset_.reset();
        for (auto& p : phase_) p.reset(0.0);
        std::fill(decay_env_.begin(), decay_env_.end(), 1.0);
        std::fill(gain_.begin(), gain_.end(), 0.0);
        std::fill(gain_step_.begin(), gain_step_.end(), 0.0);
        std::fill(increment_step_.begin(), increment_step_.end(), 0.0);
        draw_doublet_jitter();
        apply_stored_phases();
        cadence_countdown_ = 0;
        update_control_block(true);
    }

    // ── Global voice ──────────────────────────────────────────────────────

    void set_fundamental_hz(double f0) {
        if (!std::isfinite(f0)) return;
        fundamental_hz_ = std::clamp(f0, kFundamentalMinHz, kFundamentalMaxHz);
    }
    double fundamental_hz() const { return fundamental_hz_; }

    void set_partial_count(int n) {
        // `std::max(1, ...)` is not defensive padding: an empty voice table
        // would otherwise hand `std::clamp` a hi below its lo, which is UB.
        partial_count_ =
            std::clamp(n, 1, std::max(1, std::min(max_partials_, voice_.count)));
    }
    int partial_count() const { return partial_count_; }
    int max_partials() const { return max_partials_; }

    /// Stiff-string coefficient. Applies only to harmonic-typed voices; a modal
    /// voice ignores it (see `VoiceTable::harmonic`).
    void set_inharmonicity_b(double b) {
        if (!std::isfinite(b)) return;
        inharmonicity_b_ = std::clamp(b, 0.0, kInharmonicityMax);
    }
    double inharmonicity_b() const { return inharmonicity_b_; }

    void set_spectral_tilt_db_oct(double s) {
        if (!std::isfinite(s)) return;
        tilt_db_oct_ = std::clamp(s, kSpectralTiltMinDbOct, kSpectralTiltMaxDbOct);
    }
    double spectral_tilt_db_oct() const { return tilt_db_oct_; }

    void set_master_gain_db(double g) {
        if (!std::isfinite(g)) return;
        master_gain_db_ = std::clamp(g, kMasterGainMinDb, kMasterGainMaxDb);
        master_linear_ = units::db_to_linear(master_gain_db_);
    }
    double master_gain_db() const { return master_gain_db_; }

    /// Selects the bounded-cycle sine implementation used by the float bank.
    /// The reference profile remains the compatibility default. The precise
    /// profile is opt-in; unsupported targets retain scalar profile semantics.
    bool set_trig_profile(FastTrigProfile profile) noexcept {
        if (profile != FastTrigProfile::reference &&
            profile != FastTrigProfile::realtime_precise)
            return false;
        if constexpr (!std::is_same_v<SampleType, float>) {
            if (profile != FastTrigProfile::reference) return false;
        }
        trig_profile_ = profile;
        return true;
    }
    FastTrigProfile trig_profile() const noexcept { return trig_profile_; }

    static constexpr bool trig_profile_has_vector_path(FastTrigProfile profile) {
#if defined(__APPLE__) && defined(__clang__) && defined(__aarch64__)
        return std::is_same_v<SampleType, float> &&
               profile == FastTrigProfile::realtime_precise;
#else
        (void)profile;
        return false;
#endif
    }

    // ── The partial table ─────────────────────────────────────────────────

    void load_voice(const VoiceTable& v) {
        if (v.count < 0 || v.count > VoiceTable::kMaxPartials) return;
        for (int p = 0; p < v.count; ++p) {
            const auto& row = v.partials[static_cast<std::size_t>(p)];
            if (!std::isfinite(row.ratio) || !std::isfinite(row.amp) ||
                !std::isfinite(row.phase01) || !std::isfinite(row.decay_ms))
                return;
        }
        voice_ = v;
        partial_count_ = std::clamp(partial_count_, 1,
                                    std::max(1, std::min(max_partials_, voice_.count)));
    }
    const VoiceTable& voice() const { return voice_; }

    /// Sets one row of the active table. Control-rate: the change reaches the
    /// audio through the next control block's ramp, not as a step.
    void set_partial(int p, double ratio, double amp_lin, double phase01,
                     double decay_ms) {
        if (p < 0 || p >= VoiceTable::kMaxPartials) return;
        if (!std::isfinite(ratio) || !std::isfinite(amp_lin) ||
            !std::isfinite(phase01) || !std::isfinite(decay_ms))
            return;
        voice_.partials[static_cast<std::size_t>(p)] = {ratio, amp_lin, phase01,
                                                        decay_ms};
        voice_.count = std::max(voice_.count, p + 1);
    }

    // ── Spectral envelope and morph ───────────────────────────────────────

    void set_envelope_a(const SpectralEnvelope& e) { envelope_a_ = e; }
    void set_envelope_b(const SpectralEnvelope& e) { envelope_b_ = e; }

    /// Crossfade A↔B. The blend is in **dB**, on the shared abscissa grid —
    /// never a crossfade of linear partial amplitudes, and never an
    /// interpolation between independently-fitted per-pitch amplitude tables
    /// (series law 7).
    void set_morph(float m) {
        if (!std::isfinite(static_cast<double>(m))) return;
        morph_ = std::clamp(static_cast<double>(m), 0.0, 1.0);
    }
    double morph() const { return morph_; }

    void set_spectral_domain(SpectralDomain d) { domain_ = d; }
    SpectralDomain spectral_domain() const { return domain_; }

    /// The live morphed envelope in dB at a partial frequency — the exact
    /// quantity the render applies, exposed so a test asserts the shipped path
    /// rather than a reimplementation of it.
    double envelope_db_at(double f_hz) const {
        const double abscissa = domain_ == SpectralDomain::absolute_hz
                                    ? f_hz
                                    : f_hz / fundamental_hz_;
        const double a = envelope_a_.gain_db_at(abscissa);
        const double b = envelope_b_.gain_db_at(abscissa);
        return (1.0 - morph_) * a + morph_ * b +
               tilt_db_oct_ * std::log2(f_hz / fundamental_hz_);
    }

    // ── Envelope and trajectory ───────────────────────────────────────────

    void set_envelope_mode(EnvelopeMode mode) { envelope_mode_ = mode; }
    EnvelopeMode envelope_mode() const { return envelope_mode_; }

    void set_attack_ms(double ms) {
        if (!std::isfinite(ms)) return;
        // Zero is the explicit instantaneous-envelope mode used by the raw
        // oscillator/test surface. Non-zero values obey the published catalog
        // range, including its reachable 0.1 ms lower endpoint.
        attack_ms_ = ms == 0.0 ? 0.0 : std::clamp(ms, kAttackMinMs, kAttackMaxMs);
        refresh_onset();
    }
    void set_release_ms(double ms) {
        if (!std::isfinite(ms)) return;
        // As above, exact zero is a deliberate instantaneous internal mode;
        // all non-zero requests obey the declared 1 ms minimum.
        release_ms_ = ms == 0.0 ? 0.0 : std::clamp(ms, kReleaseMinMs, kReleaseMaxMs);
        refresh_onset();
    }

    /// Doublet spread in cents. 0 disables the pair outright — the second
    /// oscillator is not rendered, so a mono voice costs half of a detuned one.
    void set_detune_cents(double cents) {
        if (!std::isfinite(cents)) return;
        detune_cents_ = std::clamp(cents, 0.0, kDetuneMaxCents);
    }
    double detune_cents() const { return detune_cents_; }
    bool doublet_active() const { return detune_cents_ > 0.0; }

    /// A shared initial pitch glide: every partial is scaled by
    /// `2^(cents(t)/1200)`, starting at `start_cents` and reaching 0 over
    /// `time_ms`. The bell's strike chiff and a drum's pitch drop.
    void set_pitch_glide(double start_cents, double time_ms) {
        if (!std::isfinite(start_cents) || !std::isfinite(time_ms)) return;
        glide_start_cents_ = start_cents;
        glide_time_ms_ = std::max(time_ms, 0.0);
    }

    void set_retrig_phase(RetrigPhase p) { retrig_phase_ = p; }
    RetrigPhase retrig_phase() const { return retrig_phase_; }

    /// Seeds the initial-phase and doublet-jitter generator. Construction or
    /// preset choice only — never an automatable parameter (series law 2).
    void set_seed(std::uint32_t seed) {
        rng_.set_seed(seed);
        draw_doublet_jitter();
    }

    /// Note-on. Applies the phase policy, restarts the per-partial decays, and
    /// opens the shared onset.
    void retrigger() {
        switch (retrig_phase_) {
            case RetrigPhase::reset_stored:
                apply_stored_phases();
                break;
            case RetrigPhase::free_run:
                break;
            case RetrigPhase::seeded_random:
                for (int p = 0; p < partial_count_; ++p)
                    for (int k = 0; k < 2; ++k)
                        phase_[slot(p, k)].reset(rng_.next_unit<double>());
                break;
        }
        std::fill(decay_env_.begin(), decay_env_.end(), 1.0);
        glide_position_ = 0.0;
        onset_.gate_on();
        update_control_block(true);
    }

    /// Note-off. The shared onset falls over `release_ms`. Named separately
    /// from `retrigger()` because `release_ms` is a parameter and a parameter
    /// with no way to reach it is a parameter that does not exist.
    void release() { onset_.gate_off(); }

    bool active() const { return onset_.active(); }

    /// The current swept frequency of one partial's primary oscillator, in Hz —
    /// the quantity the inharmonicity and guard laws take as input. Exposed
    /// because it is what a host draws, and what the trajectory tests measure
    /// directly rather than recovering from audio.
    double partial_frequency(int p) const {
        if (p < 0 || p >= partial_count_) return 0.0;
        return increment_[slot(p, 0)] * sample_rate_;
    }

    // ── Render ────────────────────────────────────────────────────────────

    SampleType next() {
        SampleType out{};
        process(&out, 1);
        return out;
    }

    void process(SampleType* out, int n) {
        switch (trig_profile_) {
            case FastTrigProfile::realtime_precise:
                process_with_trig<FastTrigProfile::realtime_precise>(out, n);
                return;
            case FastTrigProfile::reference:
            default:
                process_with_trig<FastTrigProfile::reference>(out, n);
                return;
        }
    }

private:
    template <FastTrigProfile Profile>
    void process_with_trig(SampleType* out, int n) {
        for (int i = 0; i < n; ++i) {
            if (cadence_countdown_ <= 0) update_control_block(false);
            --cadence_countdown_;

            double sum = 0.0;
#if defined(__APPLE__) && defined(__clang__) && defined(__aarch64__)
            if constexpr (std::is_same_v<SampleType, float> &&
                          Profile == FastTrigProfile::realtime_precise)
                sum = render_precise_simd_sample();
#endif
            constexpr bool rendered_simd =
#if defined(__APPLE__) && defined(__clang__) && defined(__aarch64__)
                std::is_same_v<SampleType, float> &&
                Profile == FastTrigProfile::realtime_precise;
#else
                false;
#endif
            for (int r = rendered_simd ? render_count_ : 0; r < render_count_; ++r) {
                const auto s = static_cast<std::size_t>(render_slots_[
                    static_cast<std::size_t>(r)]);
                // Evaluate at the CURRENT phase, then advance — the catalog's
                // oscillator convention (`osc::VaOscillator::next` reads its
                // entry phase). Advancing first would put the first sample one
                // increment past the stored phase, so a partial stored at 0.25
                // would start at 0.9983 rather than at 1, and a bank would not
                // agree sample-for-sample with a catalog sine osc set to the
                // same phase.
                const double sine = [&] {
                    if constexpr (Profile == FastTrigProfile::reference)
                        return std::sin(kTwoPi * phase_[s].phase());
                    else
                        return static_cast<double>(
                            FastMath::sin_cycles<Profile>(
                                static_cast<float>(phase_[s].phase())));
                }();
                sum += gain_[s] * decay_env_[s] * sine;
                phase_[s].advance(increment_[s]);
                gain_[s] += gain_step_[s];
                increment_[s] += increment_step_[s];
                decay_env_[s] *= decay_coef_[s];
            }

            const double env = static_cast<double>(onset_.next());
            // The accumulator is the one recursive-looking quantity here; the
            // per-partial decays are geometric and would otherwise drizzle
            // subnormals into the sum for minutes after a long tail.
            out[i] = static_cast<SampleType>(
                snap_to_zero(sum * env * master_linear_));
        }
    }
    static constexpr double kPi() { return 3.14159265358979323846; }

    /// Slot layout: partial `p`'s primary oscillator is `2p`, its doublet
    /// partner `2p+1`. Fixed rather than compacted, so engaging the detune
    /// mid-note does not renumber a running partial's phase.
    static constexpr std::size_t slot(int p, int k) {
        return static_cast<std::size_t>(2 * p + k);
    }

#if defined(__APPLE__) && defined(__clang__) && defined(__aarch64__)
    /// Evaluates four float outputs at once while retaining double phase state.
    double render_precise_simd_sample() {
        simd_float4 accumulated = simd_make_float4(0.0f);
        int r = 0;
        for (; r + 3 < render_count_; r += 4) {
            const auto s0 = static_cast<std::size_t>(render_slots_[r]);
            const auto s1 = static_cast<std::size_t>(render_slots_[r + 1]);
            const auto s2 = static_cast<std::size_t>(render_slots_[r + 2]);
            const auto s3 = static_cast<std::size_t>(render_slots_[r + 3]);
            const simd_float4 phase = simd_make_float4(
                static_cast<float>(phase_[s0].phase()),
                static_cast<float>(phase_[s1].phase()),
                static_cast<float>(phase_[s2].phase()),
                static_cast<float>(phase_[s3].phase()));
            const simd_float4 sine = FastMath::sin_cycles_precise(phase);
            const simd_float4 amplitude = simd_make_float4(
                static_cast<float>(gain_[s0] * decay_env_[s0]),
                static_cast<float>(gain_[s1] * decay_env_[s1]),
                static_cast<float>(gain_[s2] * decay_env_[s2]),
                static_cast<float>(gain_[s3] * decay_env_[s3]));
            accumulated += amplitude * sine;

            const std::array<std::size_t, 4> slots{s0, s1, s2, s3};
            for (const auto s : slots) {
                phase_[s].advance(increment_[s]);
                gain_[s] += gain_step_[s];
                increment_[s] += increment_step_[s];
                decay_env_[s] *= decay_coef_[s];
            }
        }
        double sum = static_cast<double>(simd_reduce_add(accumulated));
        for (; r < render_count_; ++r) {
            const auto s = static_cast<std::size_t>(render_slots_[r]);
            sum += gain_[s] * decay_env_[s] *
                   static_cast<double>(FastMath::sin_cycles<
                                       FastTrigProfile::realtime_precise>(
                       static_cast<float>(phase_[s].phase())));
            phase_[s].advance(increment_[s]);
            gain_[s] += gain_step_[s];
            increment_[s] += increment_step_[s];
            decay_env_[s] *= decay_coef_[s];
        }
        return sum;
    }
#endif

    /// The shared onset is an attack that HOLDS at unity while gated, then
    /// releases — a shape none of the named envelope types provides directly.
    /// `ArT` is the right core: attack, no decay, no hold.
    ///
    /// `ArT` owns the hold-at-unity invariant; this bank configures only the two
    /// time constants its onset actually exposes.
    void refresh_onset() {
        onset_.set_attack_ms(attack_ms_);
        onset_.set_release_ms(release_ms_);
    }

    void apply_stored_phases() {
        for (int p = 0; p < std::max(partial_count_, 1); ++p) {
            const double stored =
                voice_.partials[static_cast<std::size_t>(p)].phase01;
            for (int k = 0; k < 2; ++k) {
                const auto s = slot(p, k);
                if (s < phase_.size()) phase_[s].reset(stored);
            }
        }
    }

    /// One seeded factor per partial, so each mode's pair beats at its own
    /// rate. Drawn at `reset()`/`set_seed()` and never per sample, which keeps
    /// the render allocation-free and the beat pattern reproducible.
    void draw_doublet_jitter() {
        for (std::size_t s = 0; s < slot_detune_.size(); ++s)
            slot_detune_[s] = 1.0 + kDoubletJitterSpread *
                                        rng_.next_bipolar<double>();
    }

    /// Recomputes every partial's frequency and gain, rebuilds the render list,
    /// and sets the linear ramps that carry the change across the next cadence
    /// block. `immediate` snaps instead of ramping — used at `reset()` and
    /// `retrigger()`, where there is no previous value worth gliding from.
    void update_control_block(bool immediate) {
        cadence_countdown_ = kAmpUpdateCadenceSamples;

        const double glide = glide_ratio();
        advance_glide();

        const bool pair = doublet_active();
        const int sub_slots = pair ? 2 : 1;
        const double pair_scale = pair ? 0.5 : 1.0;

        // Pass one: frequencies, guard gains, envelope gains, and the sum the
        // normaliser needs. The normaliser has to see every factor that reaches
        // the output, or the bound it computes is a bound on a different signal.
        double magnitude_sum = 0.0;
        render_count_ = 0;
        for (int p = 0; p < partial_count_; ++p) {
            const auto& row = voice_.partials[static_cast<std::size_t>(p)];
            const double base_hz =
                voice_.harmonic
                    ? partial_frequency_hz(fundamental_hz_, row.ratio,
                                           inharmonicity_b_)
                    : fundamental_hz_ * row.ratio;

            for (int k = 0; k < sub_slots; ++k) {
                const auto s = slot(p, k);
                // The jitter is indexed by PARTIAL, not by slot, so the pair
                // straddles its nominal symmetrically. Drawing it per slot
                // would detune the two members by different amounts, which
                // shifts the mode's centre frequency instead of splitting it —
                // a mistuned bell rather than a beating one.
                const double cents =
                    pair ? (k == 0 ? -0.5 : 0.5) * detune_cents_ *
                               slot_detune_[slot(p, 0)]
                         : 0.0;
                const double f_hz =
                    base_hz * glide * units::cents_to_ratio(cents);

                const double guard = nyquist_guard_gain(f_hz, sample_rate_);
                const double env_db = f_hz > 0.0 ? envelope_db_at(f_hz) : 0.0;
                const double linear = units::db_to_linear(env_db);
                const double signed_gain =
                    row.amp * pair_scale * linear * guard;

                target_gain_[s] = signed_gain;
                target_increment_[s] = f_hz / sample_rate_;
                magnitude_sum += std::abs(signed_gain);

                // `shared_ar` means exactly that: the onset is the whole
                // amplitude story and the table's per-partial τ is ignored, so
                // every partial holds while gated. Reading `decay_ms` in this
                // mode would make the organ table's sustained rows and the
                // bell's 500 ms rows behave differently under a control the
                // user thinks selects between them.
                decay_coef_[s] =
                    envelope_mode_ == EnvelopeMode::per_partial_decay
                        ? decay_coefficient(row.decay_ms)
                        : 1.0;
                render_slots_[static_cast<std::size_t>(render_count_++)] =
                    static_cast<int>(s);
            }
        }

        // Pass two: the normaliser, then the ramps. Applying `g_norm` to the
        // per-partial gains rather than to the summed output is what makes the
        // bound survive the ramp — a post-sum normaliser would be a different
        // number on each side of a control-block edge.
        normaliser_ = 1.0 / std::max(1.0, magnitude_sum);
        const double step = 1.0 / static_cast<double>(kAmpUpdateCadenceSamples);
        for (int r = 0; r < render_count_; ++r) {
            const auto s = static_cast<std::size_t>(
                render_slots_[static_cast<std::size_t>(r)]);
            const double g = target_gain_[s] * normaliser_;
            if (immediate) {
                gain_[s] = g;
                increment_[s] = target_increment_[s];
                gain_step_[s] = 0.0;
                increment_step_[s] = 0.0;
            } else {
                gain_step_[s] = (g - gain_[s]) * step;
                increment_step_[s] = (target_increment_[s] - increment_[s]) * step;
            }
        }
    }

    /// `exp(−1/(τ·fs))` per sample, or exactly 1 for a sustained partial. The
    /// per-sample form is one multiply; evaluating `exp(−t/τ)` per partial per
    /// sample would be 128 transcendentals a sample for the same curve.
    double decay_coefficient(double decay_ms) const {
        if (!(decay_ms > 0.0)) return 1.0;   // sustained — the table's "∞"
        const double tau_samples = decay_ms * 0.001 * sample_rate_;
        return tau_samples > 0.0 ? std::exp(-1.0 / tau_samples) : 0.0;
    }

    double glide_ratio() const {
        if (glide_time_ms_ <= 0.0 || glide_start_cents_ == 0.0) return 1.0;
        const double t = std::min(glide_position_, 1.0);
        return units::cents_to_ratio(glide_start_cents_ * (1.0 - t));
    }

    void advance_glide() {
        if (glide_time_ms_ <= 0.0) {
            glide_position_ = 1.0;
            return;
        }
        const double block_ms = 1000.0 *
                                static_cast<double>(kAmpUpdateCadenceSamples) /
                                sample_rate_;
        glide_position_ =
            std::min(1.0, glide_position_ + block_ms / glide_time_ms_);
    }

    double sample_rate_ = 44100.0;
    int max_partials_ = kMaxPartialsDefault;
    int partial_count_ = kPartialCountDefault;

    double fundamental_hz_ = kFundamentalDefaultHz;
    double inharmonicity_b_ = kInharmonicityDefault;
    double tilt_db_oct_ = kSpectralTiltDefaultDbOct;
    double master_gain_db_ = kMasterGainDefaultDb;
    double master_linear_ = units::db_to_linear(kMasterGainDefaultDb);
    double morph_ = 0.0;
    double detune_cents_ = kDetuneDefaultCents;
    double attack_ms_ = kAttackDefaultMs;
    double release_ms_ = kReleaseDefaultMs;
    double glide_start_cents_ = 0.0;
    double glide_time_ms_ = 0.0;
    double glide_position_ = 1.0;
    double normaliser_ = 1.0;

    SpectralDomain domain_ = SpectralDomain::absolute_hz;
    EnvelopeMode envelope_mode_ = EnvelopeMode::per_partial_decay;
    RetrigPhase retrig_phase_ = RetrigPhase::reset_stored;
    FastTrigProfile trig_profile_ = FastTrigProfile::reference;

    VoiceTable voice_ = make_organ_voice(kPartialCountDefault);
    SpectralEnvelope envelope_a_{};
    SpectralEnvelope envelope_b_{};

    ArT<double> onset_{};
    Xorshift32 rng_{0x9E3779B9u};

    std::vector<osc::PhaseAccumulator> phase_{};
    std::vector<double> gain_{}, gain_step_{};
    std::vector<double> increment_{}, increment_step_{};
    std::vector<double> decay_env_{}, decay_coef_{};
    std::vector<double> slot_detune_{};
    std::vector<int> render_slots_{};
    std::array<double, 2 * VoiceTable::kMaxPartials> target_gain_{};
    std::array<double, 2 * VoiceTable::kMaxPartials> target_increment_{};

    int render_count_ = 0;
    int cadence_countdown_ = 0;
};

using AdditiveBank = AdditiveBankT<float>;
using AdditiveBank64 = AdditiveBankT<double>;

}  // namespace pulp::signal
