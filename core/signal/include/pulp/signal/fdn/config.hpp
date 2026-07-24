#pragma once

// Multirate FDN reverb — structural constants, tuning constants, and mode table.
//
// Everything the engine treats as *design* (topology, matrix size, the decay
// law) is a hard constant here; everything it treats as *voicing* is a named
// tuning constant with a documented default, so a change of taste is a one-line
// data edit rather than a hunt through the DSP. The mode table is the same idea
// one level up: a mode stamps parameter DEFAULTS, it never selects a code path,
// so all five catalog reverbs run byte-identical DSP.

#include <array>
#include <cstddef>

namespace pulp::signal::fdn {

// ── Structural constants (topology, not taste) ───────────────────────────
inline constexpr int kNumChannels = 16;        // FDN lines; Hadamard needs 2^k
inline constexpr int kMaxDiffusionStages = 8;  // per cascade, pre and in-loop
inline constexpr int kNumEqBands = 10;         // 1 low shelf + 8 peaks + 1 high shelf
inline constexpr int kNumShimmerGrains = 4;    // grains per channel pitch shifter
inline constexpr int kNumEnsembleLfos = 6;     // output ensemble chorus

// Per-line prime offsets. Added (never multiplied) to the base lengths so the
// lines stay pairwise distinct even when the requested range is narrow — see
// the delay-length note in fdn/tank.hpp.
inline constexpr std::array<int, kNumChannels> kChannelPrimes = {
    2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47, 53};

// ── The eight tank rates ─────────────────────────────────────────────────
// Discrete by design: a continuously variable tank rate would pitch-shift the
// running tail and explode the resampler configuration space. Eight pinned
// rates keep switches well defined and renders reproducible.
inline constexpr std::array<double, 8> kTankRates = {
    16000.0, 20000.0, 24000.0, 32000.0, 44100.0, 48000.0, 64000.0, 96000.0};
inline constexpr int kNumTankRates = static_cast<int>(kTankRates.size());
inline constexpr double kMaxTankRate = 96000.0;

// Rate-step makeup gain, in dB, applied to the wet signal after everything.
// Derived by the procedure in the acceptance suite (broadband noise, hall
// defaults, 48 kHz host, steady-state wet RMS per rate against the `direct`
// no-resample reference, inverted); `fdn_reverb_rate_makeup` re-derives and
// re-verifies these, so an engine change that shifts a rate's level fails a
// test instead of silently re-voicing the mode.
inline constexpr std::array<double, 8> kRateMakeupDb = {
    2.01, 1.42, 0.92, 0.35, 0.06, 0.00, 0.21, 0.26};

// ── Tuning constants ─────────────────────────────────────────────────────
// Delay lengths, in SECONDS, at size 0.5 — converted at the tank rate, so a
// tank-rate change alters texture, never the room.
inline constexpr double kDelayMinSeconds = 0.010;
inline constexpr double kDelayMaxSeconds = 0.090;
// `size` scales both ends over this span (size 0 → 0.5x, size 1 → 1.667x),
// keeping the realized lengths inside the 5–150 ms design envelope.
inline constexpr double kSizeScaleMin = 0.5;
inline constexpr double kSizeScaleMax = 5.0 / 3.0;
// Clusters lengths toward the short end (early density) while keeping a few
// long lines (low mode spacing). 1 = linear spread, 3 = heavily clustered.
inline constexpr double kDelayExponent = 2.0;

// Diffusion. The loop wants LESS diffusion than the input: a high coefficient
// re-applied every pass turns metallic.
inline constexpr int kPreDiffusionStages = 4;
inline constexpr int kLoopDiffusionStages = 4;
inline constexpr double kPreDiffusionG = 0.70;
inline constexpr double kLoopDiffusionG = 0.45;
inline constexpr double kPreDiffusionBaseMs = 12.0;
inline constexpr double kLoopDiffusionBaseMs = 7.0;
inline constexpr double kDiffusionStageShrink = 0.70710678118654752;  // 1/sqrt(2)
// Flutter modulates the in-loop allpass delays. Off by default: the integer
// path is bit-exact and cheaper, and the wobble is a deliberate effect.
inline constexpr double kFlutterDepth = 0.02;      // fraction of the stage delay
inline constexpr double kFlutterRateMinHz = 0.13;
inline constexpr double kFlutterRateMaxHz = 0.71;

// Delay modulation (§ dual-source: sine + mean-reverting walk).
inline constexpr double kModSineMix = 0.6;
inline constexpr double kModWalkMix = 0.4;
inline constexpr double kModRateMinHz = 0.10;
inline constexpr double kModRateMaxHz = 1.50;
// Peak excursion as a fraction of the line length at mod = 1 and short decay.
inline constexpr double kModMaxFraction = 0.0035;
// Modulation depth SHRINKS as decay lengthens: constant-depth modulation on a
// very long tail accumulates audible pitch drift, while short tails want the
// lushness. Full depth at or below kModShrinkFullSeconds, kModShrinkFloor of it
// at or above kModShrinkMinSeconds.
inline constexpr double kModShrinkFullSeconds = 2.0;
inline constexpr double kModShrinkMinSeconds = 20.0;
inline constexpr double kModShrinkFloor = 0.25;
// Mean-reverting (Ornstein-Uhlenbeck-style) walk: how fast it returns to zero
// and how far it strays. Stepped at the control rate.
inline constexpr double kWalkReversion = 0.004;
inline constexpr double kWalkStep = 0.09;

// Damping. kDampingScale blends textbook-correct Jot-proportional damping
// (1.0) against uniform damping (0.0) — uniform is "wrong" but sometimes
// sounds better on small bright rooms, so the choice is exposed as data.
inline constexpr double kDampingScale = 1.0;
inline constexpr double kDampHiMinHz = 1200.0;    // damp_hi = 1 → in-loop LP here
// damp_hi = 0 must be TRANSPARENT, not merely gentle: a one-pole whose cutoff
// still sits below a few times Nyquist leaves a fraction of a dB of loss per
// pass, and a fraction of a dB per pass is several percent of the realized T60
// at long decays. 192 kHz is two octaves above the highest tank Nyquist, which
// drives the pole coefficient to ~1e-5 at 96 kHz and to nothing below it.
inline constexpr double kDampHiMaxHz = 192000.0;
inline constexpr double kDampLoMinHz = 10.0;      // damp_lo = 0 → DC blocker only
inline constexpr double kDampLoMaxHz = 500.0;     // damp_lo = 1 → in-loop HP here
// Skews both damping knobs so the audible action lands in the upper half
// rather than being squashed against the transparent end of the range.
inline constexpr double kDampCurve = 1.6;

// Flux — the wandering in-loop peak. It is ABSORPTIVE ONLY (gain in
// [-kFluxMaxDb, 0]; see fdn/loop_eq.hpp), so the motion can never make a pass
// louder than flat and the Jot decay law stays exact at every decay setting.
inline constexpr double kFluxMaxDb = 3.0;
// Default depth. Every dB of in-loop absorption is a real per-pass loss that
// shortens the broadband decay, so the default is set from the acceptance
// suite's T60 budget: deep enough to hear the colour move, shallow enough that
// the realized T60 stays inside the Jot law's stated tolerance.
inline constexpr double kFluxDefaultDb = 0.5;
inline constexpr double kFluxBaseMinHz = 300.0;
inline constexpr double kFluxBaseMaxHz = 6000.0;
inline constexpr double kFluxSpreadOctaves = 0.7;  // re-target range around base
inline constexpr double kFluxIntervalMinMs = 120.0;
inline constexpr double kFluxIntervalMaxMs = 250.0;
inline constexpr double kFluxGlideMs = 90.0;
// A wide dip takes a broadband bite out of every pass, and a broadband bite is
// a decay change. This Q keeps the absorption narrow enough that the realized
// T60 stays within the Jot law's tolerance while the colour still moves.
inline constexpr double kFluxQ = 8.0;

// Shimmer. Grain size varies per channel across this span so the 16 shifters'
// grain-boundary artifacts decorrelate instead of stacking.
inline constexpr double kShimmerGrainMinMs = 40.0;
inline constexpr double kShimmerGrainMaxMs = 60.0;
inline constexpr double kShimmerToneHz = 8000.0;
// Injection weight ceiling before the 1/sqrt(active_n) energy normalization.
inline constexpr double kShimmerMaxWeight = 0.6;

// Stability. The realized per-pass gain is provably <= kGainCeil for every
// parameter combination; Bloom lifts toward that ceiling but never past it.
inline constexpr double kGainCeil = 0.999;
// Bloom's authority ramps in over this decay span: at short decays Bloom only
// fattens the tail, at long decays it reaches the ceiling (the freeze regime).
inline constexpr double kBloomAuthorityMinSeconds = 1.0;
inline constexpr double kBloomAuthorityMaxSeconds = 20.0;

// Transient ducking of the INPUT (attacks excite the reverb less).
inline constexpr double kDuckFastMs = 3.0;
inline constexpr double kDuckSlowMs = 40.0;
inline constexpr double kDuckAmount = 0.3;
inline constexpr double kDuckKnee = 1.5;  // transient ratio that saturates the duck

// Output stage.
inline constexpr double kEnsembleBaseMs = 5.0;
inline constexpr double kEnsembleDepthMs = 1.6;
inline constexpr double kEnsembleRateMinHz = 0.11;
inline constexpr double kEnsembleRateMaxHz = 0.83;
inline constexpr double kEnsembleWetMix = 0.35;
inline constexpr double kWetLimiterHeadroom = 2.0;  // soft-tanh knee (~ +6 dB)

// Resampling / anti-alias.
// Butterworth 4-pole Q pair, cut at 0.9 x Nyquist of the bottleneck rate.
inline constexpr double kButterworthQ0 = 0.5412;
inline constexpr double kButterworthQ1 = 1.3065;
inline constexpr double kAaCutoffFraction = 0.45;  // 0.9 x Nyquist
inline constexpr int kHermiteGuard = 8;
// Below this tank Nyquist the in-tank input lowpass engages — the deliberate
// vintage-hardware darkening at the low rates.
inline constexpr double kTankInputLpThresholdHz = 18000.0;
inline constexpr double kTankInputLpFraction = 0.42;

// Sanitization (defense in depth). A 16-line recursive structure needs more
// than one guard at the output: a single corrupted channel is invisible at the
// sum until it has already poisoned every line through the matrix, so the
// non-finite kill and the magnitude ceiling are applied both after the matrix
// and at the delay-line write.
inline constexpr double kSanityCeil = 10.0;

// Parameter smoothing and control-rate cadence.
inline constexpr double kSmoothingMs = 5.0;
inline constexpr int kControlRateSamples = 32;

// ── Parameters ───────────────────────────────────────────────────────────
// Index into the engine's parameter array. The catalog node maps its baked
// param ids onto these, so the DSP never learns about host param ids.
enum class Param {
    decay = 0,
    size,
    predelay,
    damp_hi,
    damp_lo,
    diffusion,
    mod,
    shimmer,
    drive,
    bloom,
    width,
    tank_rate,
    count
};
inline constexpr int kNumParams = static_cast<int>(Param::count);

struct ParamSpec {
    const char* id;
    double min;
    double max;
    double default_value;
    bool stepped;
};

// The canonical parameter contract. Forge's registry mirrors the ranges; the
// contract test asserts the two agree, so this stays the single source.
inline constexpr std::array<ParamSpec, kNumParams> kParamSpecs = {{
    {"decay", 0.1, 60.0, 2.5, false},
    {"size", 0.0, 1.0, 0.5, false},
    {"predelay", 0.0, 250.0, 10.0, false},
    {"damp_hi", 0.0, 1.0, 0.4, false},
    {"damp_lo", 0.0, 1.0, 0.1, false},
    {"diffusion", 0.0, 1.0, 0.7, false},
    {"mod", 0.0, 1.0, 0.35, false},
    {"shimmer", 0.0, 1.0, 0.0, false},
    {"drive", 0.0, 1.0, 0.0, false},
    {"bloom", 0.0, 1.0, 0.0, false},
    {"width", 0.0, 1.0, 1.0, false},
    {"tank_rate", 0.0, 7.0, 1.0, true},
}};

// ── Modes ────────────────────────────────────────────────────────────────
// A mode is a parameter-stamping table, never a code branch: every mode runs
// the same DSP and differs only in the defaults it stamps and the output
// lowpass it asks for.
enum class Mode { room = 0, hall, galaxy, shimmer, lofi, count };
inline constexpr int kNumModes = static_cast<int>(Mode::count);

struct ModeConfig {
    const char* name;
    double decay;
    double size;
    int tank_rate_index;
    double diffusion;
    double mod;
    double shimmer;
    double bloom;
    double damp_hi;
    double damp_lo;
    double drive;
    double output_lp_hz;  // per-mode wet lowpass, at the HOST rate
};

inline constexpr std::array<ModeConfig, kNumModes> kModeConfigs = {{
    //  name       decay size  rate  diff  mod  shim bloom dhi   dlo  drive  outLP
    {"room", 0.8, 0.35, 5, 0.75, 0.30, 0.00, 0.00, 0.50, 0.15, 0.00, 16000.0},
    {"hall", 2.8, 0.65, 4, 0.70, 0.35, 0.00, 0.20, 0.40, 0.10, 0.00, 14000.0},
    {"galaxy", 12.0, 0.90, 3, 0.60, 0.50, 0.15, 0.70, 0.25, 0.05, 0.10, 12000.0},
    {"shimmer", 6.0, 0.70, 4, 0.65, 0.40, 0.60, 0.40, 0.30, 0.10, 0.00, 15000.0},
    {"lofi", 1.6, 0.45, 0, 0.55, 0.25, 0.00, 0.00, 0.70, 0.20, 0.35, 7000.0},
}};

inline constexpr const ModeConfig& mode_config(Mode m) {
    return kModeConfigs[static_cast<std::size_t>(m)];
}

// The value a mode stamps into one parameter, or that parameter's own default
// when the mode does not speak to it (predelay and width are performance
// choices, not part of a mode's voicing). This is the single mapping from the
// mode table onto the parameter list: the engine's set_mode() and the catalog
// node's declared defaults both read it, so a re-voiced mode can never mean one
// thing to the DSP and another to the host's parameter defaults.
inline constexpr double mode_default(Mode mode, Param param) {
    const ModeConfig& m = mode_config(mode);
    switch (param) {
        case Param::decay: return m.decay;
        case Param::size: return m.size;
        case Param::tank_rate: return static_cast<double>(m.tank_rate_index);
        case Param::diffusion: return m.diffusion;
        case Param::mod: return m.mod;
        case Param::shimmer: return m.shimmer;
        case Param::bloom: return m.bloom;
        case Param::damp_hi: return m.damp_hi;
        case Param::damp_lo: return m.damp_lo;
        case Param::drive: return m.drive;
        case Param::predelay:
        case Param::width: break;
        case Param::count: return 0.0;  // not a parameter; never indexes the table
    }
    return kParamSpecs[static_cast<std::size_t>(param)].default_value;
}

}  // namespace pulp::signal::fdn
