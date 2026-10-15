#pragma once

// Forge modulation catalog — the `pulp::signal` modulation toolkit exposed as
// bake-layer custom nodes.
//
// Companion to forge_lofi_catalog.hpp, which carries the lo-fi effect nodes and
// the first CV pack (lfo / vca / env_follower / filter_cv / delay_cv). This
// header adds the capabilities that pack cannot express, each one a thin wrapper
// over a `pulp::signal` primitive rather than catalog-local DSP:
//
//   * mod_lfo   — LfoT: seven waveforms including sample-and-hold and smooth
//                 random, shape morphing, triangle/pulse shaping, random
//                 segmentation, phase, and the repeat/fade lifecycle. The
//                 existing `lfo` node is four fixed shapes with no lifecycle
//                 and no random layer.
//   * mod_lpg   — LpgT: the vactrol low-pass gate. Loudness and brightness move
//                 together, which no combination of the existing nodes produces.
//   * mod_slew  — SlewLimiterT: an independent-rise/fall rate limiter on a
//                 control signal. What makes a stepped CV musical.
//   * mod_transient — TransientDetectorT: level-INDEPENDENT attack detection.
//                 Distinct from `env_follower`, which tracks level and therefore
//                 needs re-tuning whenever the source's level changes.
//   * mod_env   — ModEnvT: a delay/attack/hold/decay envelope fired by a rising
//                 edge on its control input, with depth, velocity response,
//                 looping, and configurable trigger re-arming.
//
// The CV convention is the one the first pack established: a control signal is a
// UNIPOLAR [0, 1] signal on an ordinary audio port. Modulation is therefore
// ordinary graph topology that the existing bake and routing already handle —
// there are no modulation edges, and nothing here needs a new graph concept.
//
// The public declarations stay small; implementations live in the host
// library's dedicated translation unit so consumers do not compile every
// factory and descriptor into each including target.
//
// KNOWN DUPLICATION, for the migration that follows: `forge_lofi::make_lfo_node`
// carries its own four-shape oscillator (`forge_lofi_osc`), and the private LFOs
// inside `signal::chorus` and `signal::phaser` are two more. All three predate
// `signal::LfoT` and should migrate onto it. They are deliberately left alone
// here: `forge_lofi::kLfoTypeId`'s triangle runs in the opposite phase to
// `LfoT`'s, so swapping the implementation would change every artifact already
// baked against it. That is a migration with its own compatibility story, not a
// side effect of adding nodes.

#include <pulp/host/forge_param_descriptor.hpp>
#include <pulp/host/custom_node_type.hpp>

#include <pulp/signal/envelope.hpp>
#include <pulp/signal/lfo.hpp>
#include <pulp/signal/lpg.hpp>
#include <pulp/signal/mod_tools.hpp>
#include <pulp/signal/trigger.hpp>

namespace pulp::host::forge_modulation {

// ── Stable type ids ──────────────────────────────────────────────────────
inline constexpr const char* kModLfoTypeId       = "forge_mod_lfo";
inline constexpr const char* kModLpgTypeId       = "forge_mod_lpg";
inline constexpr const char* kModSlewTypeId      = "forge_mod_slew";
inline constexpr const char* kModTransientTypeId = "forge_mod_transient";
inline constexpr const char* kModEnvTypeId       = "forge_mod_env";

// ── Injectable macro params (node-local ids, as in the lo-fi catalog) ────
inline constexpr state::ParamID kModLfoRateHz      = 1;
inline constexpr state::ParamID kModLfoDepth       = 2;
inline constexpr state::ParamID kModLfoWave        = 3;
inline constexpr state::ParamID kModLfoPulseWidth  = 4;
inline constexpr state::ParamID kModLfoRandomBlend = 5;
inline constexpr state::ParamID kModLfoDelayMs     = 6;
inline constexpr state::ParamID kModLfoFadeInMs    = 7;
inline constexpr state::ParamID kModLfoShapeMorph      = 8;
inline constexpr state::ParamID kModLfoMorphEnabled    = 9;
inline constexpr state::ParamID kModLfoTriangleBias    = 10;
inline constexpr state::ParamID kModLfoRandomSegments  = 11;
inline constexpr state::ParamID kModLfoPhaseDegrees    = 12;
inline constexpr state::ParamID kModLfoFadeOutMs       = 13;
inline constexpr state::ParamID kModLfoFadeQuadratic   = 14;
inline constexpr state::ParamID kModLfoRepeatCount     = 15;

inline constexpr state::ParamID kModLpgDecayMs      = 1;
inline constexpr state::ParamID kModLpgColour       = 2;
inline constexpr state::ParamID kModLpgDroop        = 3;
inline constexpr state::ParamID kModLpgBrightnessHz = 4;
inline constexpr state::ParamID kModLpgStruck       = 5;
inline constexpr state::ParamID kModLpgRiseMs          = 6;
inline constexpr state::ParamID kModLpgDarknessHz      = 7;
inline constexpr state::ParamID kModLpgStrikeThreshold = 8;
inline constexpr state::ParamID kModLpgRefractoryMs    = 9;

inline constexpr state::ParamID kModSlewRiseMs = 1;
inline constexpr state::ParamID kModSlewFallMs = 2;
inline constexpr state::ParamID kModSlewCurved = 3;

inline constexpr state::ParamID kModTransientFastMs     = 1;
inline constexpr state::ParamID kModTransientSlowMs     = 2;
inline constexpr state::ParamID kModTransientSensitivity = 3;
inline constexpr state::ParamID kModTransientInvert     = 4;

inline constexpr state::ParamID kModEnvAttackMs = 1;
inline constexpr state::ParamID kModEnvHoldMs   = 2;
inline constexpr state::ParamID kModEnvDecayMs  = 3;
inline constexpr state::ParamID kModEnvCurve    = 4;
inline constexpr state::ParamID kModEnvThreshold = 5;
inline constexpr state::ParamID kModEnvDelayMs           = 6;
inline constexpr state::ParamID kModEnvDepth             = 7;
inline constexpr state::ParamID kModEnvLoop              = 8;
inline constexpr state::ParamID kModEnvLoopCount         = 9;
inline constexpr state::ParamID kModEnvRefractoryMs      = 10;
inline constexpr state::ParamID kModEnvVelocitySensitive = 11;
inline constexpr state::ParamID kModEnvIndependentCurves = 12;
inline constexpr state::ParamID kModEnvAttackCurve       = 13;
inline constexpr state::ParamID kModEnvDecayCurve        = 14;

/// Waveform selector for `mod_lfo`, in the order `signal::LfoT::Wave` declares
/// them. The id is what the agent writes and what a baked artifact stores, so
/// this order is locked exactly as the enum's is.
signal::Lfo::Wave mod_lfo_wave_from_id(float id) noexcept;

// ── mod_lfo — the full LFO as a control source ───────────────────────────
// Zero audio inputs, one output carrying a UNIPOLAR control signal in [0, 1].
// `cv = clamp(0.5 + 0.5 * depth * lfo, 0, 1)`, the same convention the first CV
// pack uses, so it drops into any CV input port that already accepts an `lfo`.
//
// Every runtime control is read at the current sample offset. Controls that
// recompute lifecycle state are cached and applied only when their value
// changes, preserving sample-accurate automation without paying their setup
// cost for static parameters.
//
// The random waveforms are seeded deterministically at reset, so a baked render
// reproduces exactly.
struct ModLfoInstance {
    signal::Lfo lfo;
    double delay_ms = -1.0;
    double fade_in_ms = -1.0;
    double fade_out_ms = -1.0;
    int fade_curve = -1;
    int repeat_count = -1;
};

CustomNodeType make_mod_lfo_node();

// ── mod_lpg — vactrol low-pass gate ──────────────────────────────────────
// Port 0 = signal, port 1 = control CV in [0, 1]; one output. The cell drives
// amplitude and cutoff together, so a falling control darkens as it quietens —
// which is what makes a pinged LPG read as a struck object rather than a synth
// patch.
//
// `struck` chooses how the CV is read. Gated (0) hands the CV straight to the
// cell, so an envelope or a fader opens it as a swell. Struck (1) pings the cell
// on each rising edge of the CV, which is the percussion patch: a re-strike
// during the decay starts from the cell's current state, so a fast roll
// crescendos the way a real one does.
//
// A struck hit keeps driving the cell for as long as the control is still
// rising, rather than sampling the control once at the instant it crosses the
// threshold. Sampling once looks right and is wrong for every source with a
// finite rise: a slewed or transient-derived control would strike at roughly
// the threshold level no matter how loud the hit actually was, so velocity
// would vanish the moment anything smoothed the CV.
// Re-driving with the running maximum keeps the attack's timing (the first
// strike still lands on the edge) while the cell charges toward the control's
// real peak, which is also what a vactrol does while its LED is still
// brightening.
//
// Worst-case gain is unity: the amplitude law is control^1.5 with control in
// [0, 1], and the cell caps its commanded cutoff at sample_rate / 4, below
// which the one-pole's peak output provably cannot exceed its peak input. (An
// uncapped trapezoidal one-pole is NOT attenuate-only — commanded above
// sample_rate / 4 its step response overshoots, so `brightness` settings past
// the cap saturate there at low sample rates.)
struct ModLpgInstance {
    signal::Lpg lpg;
    signal::TriggerDetect edge;
    float last_decay_ms = -1.0f;
    float last_rise_ms = -1.0f;
    float last_darkness_hz = -1.0f;
    float last_brightness_hz = -1.0f;
    float last_refractory_ms = -1.0f;
    /// Running maximum of the control since the current rising edge, and
    /// whether that edge is still open. See the strike note below.
    float strike_peak = 0.0f;
    bool striking = false;
};

/// Control level a rising edge must cross to ping the cell, and the level it
/// must fall back below before the next rise counts as a new hit.
inline constexpr float kDefaultModLpgStrikeThreshold = 0.25f;

CustomNodeType make_mod_lpg_node();

// ── mod_slew — control-signal rate limiter ───────────────────────────────
// One control input, one control output. Independent rise and fall times, and a
// choice of law: linear travels at a fixed slope so the time is exactly the time
// a full 0 -> 1 move takes; curved is a one-pole, where the time is a constant
// and the approach is asymptotic.
//
// This is what turns a stepped control musical — a sample-and-hold LFO through a
// short slew burbles instead of clicking, and a square LFO through a 2 ms slew
// is a trance gate instead of a hazard.
struct ModSlewInstance {
    signal::SlewLimiter slew;
    float last_rise_ms = -1.0f;
    float last_fall_ms = -1.0f;
    int last_curved = -1;
    /// The first sample after a reset is adopted outright rather than slewed to,
    /// so a render starts wherever its control already sits.
    bool primed = false;
};

CustomNodeType make_mod_slew_node();

// ── mod_transient — level-independent attack detector (audio -> CV) ──────
// One audio input, one control output in [0, 1]. It emits the normalized excess
// of a fast envelope over a slow one, which is a ratio and therefore does not
// change when the input's level does: the same gesture at -6 dBFS and at
// -30 dBFS produces the same CV.
//
// That is the difference from `env_follower`, which tracks level and whose
// threshold has to be re-tuned for every source. Use this one when the question
// is "did something get hit", and `env_follower` when it is "how loud is it".
struct ModTransientInstance {
    signal::TransientDetector detector;
    float last_fast_ms = -1.0f;
    float last_slow_ms = -1.0f;
};

CustomNodeType make_mod_transient_node();

// ── mod_env — per-hit modulation envelope (CV -> CV) ─────────────────────
// One control input, one control output in [0, 1]. A rising edge on the input
// past `threshold` fires a delay/attack/hold/decay shape with a settable curve
// and depth. It ignores gate duration: the input is an event source, not a note.
//
// The chain it exists for is `mod_transient -> mod_env -> filter_cv(1)` — a
// filter sweep whose depth and contour belong to the hit rather than to the
// input's envelope. Doing that with `env_follower` gives the shape of the sound;
// this gives the shape you asked for, fired by the sound.
struct ModEnvInstance {
    signal::ModEnv envelope;
    signal::TriggerDetect edge;
    float last_delay_ms = -1.0f;
    float last_attack_ms = -1.0f;
    float last_hold_ms = -1.0f;
    float last_decay_ms = -1.0f;
    float last_refractory_ms = -1.0f;
    bool last_loop = false;
    int last_loop_count = -1;
};

CustomNodeType make_mod_env_node();

ForgeNodeDescriptor mod_lfo_descriptor();

ForgeNodeDescriptor mod_lpg_descriptor();

ForgeNodeDescriptor mod_slew_descriptor();

ForgeNodeDescriptor mod_transient_descriptor();

ForgeNodeDescriptor mod_env_descriptor();

} // namespace pulp::host::forge_modulation
