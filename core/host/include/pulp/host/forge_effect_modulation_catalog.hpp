#pragma once

// Modulation — bake-layer catalog nodes.
//
// The home for the modulation family: effects whose character comes from
// something moving underneath the signal rather than from a nonlinearity. It
// opens with the single-sideband frequency shifter and is named for the family
// rather than for that one member, because the chorus, phaser and vibrato
// lineages that follow share this file's conventions — a moving delay or phase
// relationship, a rate or offset in real units, a wet path that a mix control
// blends against dry — and differ only in what is moving and how far.
//
// TRUE STEREO, two ports in and two out as ONE logical wire. The shifter is not
// a per-rail memoryless function: `stereo_split` mode drives the left channel
// up and the right down from a SHARED carrier, so the two rails must be
// processed in the same call to stay phase-locked. Instancing it dual-mono
// would silently reduce that mode to two identical up-shifts — which sounds
// fine right up until someone reaches for the stereo control and nothing
// happens.
//
// EVERY CHOICE ON THE SHIFTER IS AN INJECTABLE PARAM, none is a
// registration-time realization, and that is worth stating because it is the
// opposite of the saturator's arrangement. The rule is that anything which
// changes reported latency must be frozen at registration, because a node whose
// latency moves under the audio thread breaks the host's delay compensation.
// The shifter reports zero latency in every mode: the Hilbert network is IIR
// allpass with no bulk delay, the mode switch only changes the sign and scale
// applied to the quadrature term, and the feedback delay lives inside a loop
// rather than in the signal's path to the output. Nothing there can move
// latency, so nothing there has to be baked in.
//
// The three lineages that follow are NOT all like that, and the differences are
// the interesting part of this file:
//
//   - **Chorus** freezes its VOICING, its Juno mode and its BBD colour stage at
//     registration. Latency is zero for all of them, so the latency rule is not
//     what forces it — topology is. The four voicings differ in VOICE COUNT and
//     in the mix matrix that combines them, and `set_voicing` rewinds every LFO
//     phase and overwrites the rate. See `chorus::make_chorus_node`.
//   - **Phaser** freezes its STAGE COUNT, for the same reason the diode-bridge
//     compressor freezes its detector position: the module's own published notch
//     law takes the stage count as an argument, so the measured response is a
//     different function per count, not a different value of one function.
//   - **Vibrato** ships THREE nodes rather than one with a mode knob, because
//     the three engines are different signal-theoretic objects — one modulates a
//     delay (real, frequency-independent pitch modulation) and two modulate
//     allpass corners (frequency-DEPENDENT apparent pitch, plus moving notches).
//     A mode knob would imply they are points on a continuum.
//
// And the delay vibrato is the one member in this file where the LATENCY rule
// bites: its reported latency is `ceil(base_delay + amplitude)` and the
// amplitude scales as `depth / rate`, so it moves by a factor of ~280 across the
// DSP's declared ranges. Its rate FLOOR is therefore a registration-time
// argument, exactly like the VCA compressor's lookahead — see
// `vibrato::make_delay_vibrato_node`.
//
// TWO SETTERS IN THIS FAMILY MUST NOT BE CALLED PER SAMPLE, and both would fail
// silently rather than loudly. `ChorusEnsembleT::set_voicing` /
// `set_juno_mode` rewind every LFO phase, so injecting either every sample
// freezes the modulation at phase zero — a chorus that does not chorus.
// `DelayVibratoT::set_delay_ms` / `set_fade_in_ms` re-arm the onset envelope and
// zero its depth, so injecting them every sample pins the depth envelope at zero
// — a vibrato that does not vibrate. The first pair is handled by making them
// realizations; the second by applying them only when the value actually
// changes. Both are the kind of bug that renders as "the effect is subtle".
//
// The one thing that IS fixed at registration is the feedback line's capacity,
// and it is fixed by the DSP header's `kMaxLoopMs` rather than by a node-local
// ceiling: `prepare()` sizes for it, so the `fb_delay_ms` knob's top is the
// largest delay the node can ever hold without allocating.
//
// WET AND DRY BOTH: unlike a compressor, whose output IS the processed signal,
// a frequency shifter is routinely run as a blend — the classic barberpole
// setting is a wet spiral sitting under a dry source. The DSP block owns the
// mix so that `mix < 100 %` blends dry against the whole wet chain including
// the recirculated tail, which is what makes turning the control down attenuate
// the barberpole rather than disable it.

#include <pulp/host/custom_node_type.hpp>

#include <vector>

#include <pulp/signal/chorus_family.hpp>
#include <pulp/signal/frequency_shifter_ssb.hpp>
#include <pulp/signal/phaser_stages.hpp>
#include <pulp/signal/vibrato.hpp>

#include <cmath>
#include <string>

namespace pulp::host::modulation {

// ── Stable type ids ───────────────────────────────────────────────────────
inline constexpr const char* kSsbFrequencyShifterTypeId = "modulation.frequency_shifter_ssb";

// ── Injectable param ids ──────────────────────────────────────────────────
// Node-local; the framework namespaces per node so two nodes never collide.
inline constexpr state::ParamID kShiftHz = 1;       // Hz, signed: + up, − down
inline constexpr state::ParamID kFeedback = 2;      // 0..kMaxFeedback
inline constexpr state::ParamID kFeedbackDelayMs = 3;  // ms
inline constexpr state::ParamID kMix = 4;           // %
inline constexpr state::ParamID kShiftMode = 5;     // stepped 0..3, see below
inline constexpr state::ParamID kStereoSpread = 6;  // %

/// The `mode` param's stepped encoding. Ordered so 0 is the default and the
/// two mono-equivalent modes sit next to each other.
inline constexpr float kModeUp = 0.0f;
inline constexpr float kModeDown = 1.0f;
inline constexpr float kModeDualMono = 2.0f;
inline constexpr float kModeStereoSplit = 3.0f;

/// Worst-case linear gain for the Forge registry (series law 8).
///
/// The topology HAS a feedback loop, so unlike the feedforward compressor
/// there is a bound to argue rather than a ceiling to read off. The argument is
/// in the DSP header: every element in the loop is allpass, a convex
/// interpolation, or the combine (whose retained-sideband gain is at most 1),
/// leaving the DC blocker's `2/(1 + p)` as the only element above unity. The
/// module budgets `kGshiftBudget` for it, so per-pass loop gain is at most
/// `kMaxFeedback · kGshiftBudget` and the steady-state envelope is
/// `1/(1 − that)`. Both factors and the resulting peak are measured by the DSP
/// suite; this function only restates the arithmetic they confirm.
float ssb_frequency_shifter_worst_case_gain();

/// Decodes the stepped `mode` param. Rounds rather than truncates, so a host
/// that ramps to a stepped value lands on the nearest step instead of the one
/// below it for the whole ramp.
inline signal::FrequencyShiftMode mode_from_param(float value) {
    const int step = static_cast<int>(std::lround(value));
    switch (step) {
        case 1: return signal::FrequencyShiftMode::down;
        case 2: return signal::FrequencyShiftMode::dual_mono;
        case 3: return signal::FrequencyShiftMode::stereo_split;
        default: return signal::FrequencyShiftMode::up;
    }
}

/// The Bode/Moog single-sideband frequency shifter as a lowerable custom node.
CustomNodeType make_frequency_shifter_node();

// ═══════════════════════════════════════════════════════════════════════════
//  Chorus — four voicings of one ensemble engine
// ═══════════════════════════════════════════════════════════════════════════

namespace chorus {

using Engine = signal::ChorusEnsemble;
using Voicing = Engine::Voicing;
using JunoMode = Engine::JunoMode;

// ── Injectable param ids ──────────────────────────────────────────────────
inline constexpr state::ParamID kRateHz = 1;       // Hz
inline constexpr state::ParamID kDepth = 2;        // %
inline constexpr state::ParamID kMix = 3;          // %
inline constexpr state::ParamID kStereoWidth = 4;  // %

/// The registered rate span, in Hz. The DSP clamps to the same window; quoting
/// it here rather than restating numbers keeps the two from drifting.
inline constexpr double kRateMinHz = 0.05;
inline constexpr double kRateMaxHz = 10.0;

/// The type id for one realization.
///
/// Built from the arguments rather than picked from a fixed list, because the
/// realization axes multiply: four voicings, three Juno positions on one of
/// them, and the colour stage on any. A registry that gave two differently
/// behaving nodes the same id would make a session unreloadable in the
/// direction that is hardest to notice — it loads, and sounds wrong.
std::string chorus_type_id(Voicing voicing, JunoMode juno_mode, bool bbd_color);

std::string chorus_display_name(Voicing voicing, JunoMode juno_mode, bool bbd_color);

struct Instance {
    Engine engine;
};

/// Worst-case linear gain for the Forge registry (series law 8).
///
/// Delegates to the DSP's own closed-form L1 bound rather than restating it,
/// because two of its terms are counterintuitive and a hand-written number
/// would get them wrong: each modulated tap carries the 4-point Lagrange
/// kernel's L1 norm of 1.25 rather than unity, and the Dimension D's cross-feed
/// high-pass carries nearly 2 rather than the unity its passband gain suggests.
/// The DSP suite recomputes both directly from the shipped kernel.
///
/// Reported at the node's PARAMETER CEILING — full stereo width — because a
/// baked param can be automated anywhere in its declared range, and width
/// multiplies two of the terms.
float chorus_worst_case_gain(Voicing voicing, double sample_rate);

/// One chorus voicing as a lowerable custom node.
///
/// `voicing`, `juno_mode` and `bbd_color` are REGISTRATION-TIME REALIZATIONS,
/// and none of them is frozen for the usual latency reason — this engine reports
/// zero latency in every configuration. Three separate arguments force it:
///
///   - The voicings differ in VOICE COUNT (one tap for the CE-2, two for the
///     Juno and Dimension D, three for the TriChorus) and in the matrix that
///     recombines them. That is topology, not coefficients.
///   - `set_voicing` and `set_juno_mode` REWIND EVERY LFO PHASE and overwrite
///     the rate with the voicing's own table value. Injected per sample they
///     would hold every modulator at phase zero, and the node would render a
///     static comb that a reviewer would describe as "the chorus sounds weak".
///   - `mode_I_plus_II` engages BOTH of the Juno's LFOs rather than selecting a
///     third one, so even within the Juno the mode is a voice-count change.
///   - `bbd_color` routes every tap through a companded, clock-jittered BBD
///     device model. It is a fidelity-and-CPU policy for the artifact's life,
///     which is the same argument that freezes the saturator's antialiasing.
///
/// What is left — rate, depth, mix and width — are pure coefficient stores, and
/// they inject.
CustomNodeType make_chorus_node(Voicing voicing = Voicing::ce2,
                                JunoMode juno_mode = JunoMode::mode_I, bool bbd_color = false);

}  // namespace chorus

// ═══════════════════════════════════════════════════════════════════════════
//  Phaser — a swept allpass cascade
// ═══════════════════════════════════════════════════════════════════════════

namespace phaser {

using Engine = signal::PhaserStages;

// ── Injectable param ids ──────────────────────────────────────────────────
inline constexpr state::ParamID kRateHz = 1;         // Hz
inline constexpr state::ParamID kDepth = 2;          // %
inline constexpr state::ParamID kCenterHz = 3;       // Hz
inline constexpr state::ParamID kFeedback = 4;       // −kFeedbackMax .. +kFeedbackMax
inline constexpr state::ParamID kMix = 5;            // %
inline constexpr state::ParamID kStereoSpread = 6;   // cycles, 0 .. 0.5
inline constexpr state::ParamID kStaggerRatio = 7;   // ratio
inline constexpr state::ParamID kWave = 8;           // stepped 0..6

/// The registered rate span, in Hz — the DSP's documented single-pot range.
inline constexpr double kRateMinHz = 0.02;
inline constexpr double kRateMaxHz = 10.0;
/// The registered centre-frequency span, in Hz.
inline constexpr double kCenterMinHz = 20.0;
inline constexpr double kCenterMaxHz = 5000.0;

/// The `wave` param's stepped encoding, in `LfoWave`'s own declaration order so
/// the two cannot drift apart.
inline constexpr float kWaveSine = 0.0f;
inline constexpr float kWaveTriangle = 1.0f;
inline constexpr float kWaveSawUp = 2.0f;
inline constexpr float kWaveSawDown = 3.0f;
inline constexpr float kWaveSquare = 4.0f;
inline constexpr float kWaveSampleHold = 5.0f;
inline constexpr float kWaveSmoothRandom = 6.0f;

/// Decodes the stepped `wave` param. Rounds rather than truncates, so a host
/// ramping to a stepped value lands on the nearest step instead of sitting on
/// the one below it for the whole ramp.
inline signal::LfoWave wave_from_param(float value) {
    const int step = static_cast<int>(std::lround(value));
    switch (step) {
        case 1: return signal::LfoWave::triangle;
        case 2: return signal::LfoWave::saw_up;
        case 3: return signal::LfoWave::saw_down;
        case 4: return signal::LfoWave::square;
        case 5: return signal::LfoWave::sample_hold;
        case 6: return signal::LfoWave::smooth_random;
        default: return signal::LfoWave::sine;
    }
}

struct Instance {
    Engine engine;
};

std::string phaser_type_id(int stages);

/// Worst-case linear gain for the Forge registry (series law 8).
///
/// The topology has a feedback path, so this is a loop bound rather than a
/// constructive sum: `1/(1 − kFeedbackMax)`. The DSP publishes it as a
/// `constexpr` and its suite asserts realised gain stays under it, so this
/// reports rather than re-derives — and it is independent of stage count,
/// because the cascade inside the loop is allpass.
float phaser_worst_case_gain();

/// A swept allpass cascade as a lowerable custom node.
///
/// `stages` is a REGISTRATION-TIME REALIZATION. Latency is zero at every count,
/// so — as with the chorus — the latency rule is not what forces it. The notch
/// law is: the module publishes `notch_frequency_hz(k, stages, fc, fs)` and
/// `notch_count(stages)`, both taking the stage count as an ARGUMENT, which is
/// the same shape as the diode-bridge compressor's separate
/// `static_curve_feedback_db()` — the measured response is a different function
/// per count, not a different value of one function. Automating it would move
/// the number of notches under a user who is listening to them.
///
/// The count is also rounded down to even inside the DSP, so half the values a
/// param would accept are not distinct settings.
///
/// `wave` DOES inject, including the two stochastic shapes: it selects which
/// shape one unchanged LFO evaluates, changes no state and no latency, and is a
/// front-panel switch on several of the units this models.
CustomNodeType make_phaser_node(int stages = Engine::kStageCountDefault);

}  // namespace phaser

// ═══════════════════════════════════════════════════════════════════════════
//  Vibrato — three lineages, three nodes
// ═══════════════════════════════════════════════════════════════════════════

namespace vibrato {

// Public for compatibility with the modulation catalog's response tests and
// downstream characterization tools. The implementation remains in the
// catalog translation unit so the public header carries no DSP helper body.
double allpass_cascade_l1(const std::vector<double>& corners_hz, double sample_rate,
                          int length = 200000);

// ── Delay vibrato — the only one that really shifts pitch ─────────────────

namespace delay_line {

using Engine = signal::DelayVibrato;

// Injectable parameter ids.
inline constexpr state::ParamID kRateHz = 1;
inline constexpr state::ParamID kDepthCents = 2;
inline constexpr state::ParamID kDelayMs = 3;
inline constexpr state::ParamID kFadeInMs = 4;

struct Instance {
    Engine engine;
    // Last values applied to the onset setters. Those setters re-arm the
    // lifecycle envelope, so the process callback updates them only when the
    // injected value changes.
    float applied_delay_ms = -1.0f;
    float applied_fade_in_ms = -1.0f;
};

// The floor is part of the realization identity because it changes the
// injectable rate range.
std::string delay_vibrato_type_id(float min_rate_hz);

/// Worst-case linear gain at the registered interpolator ceiling.
float delay_vibrato_worst_case_gain();

/// True pitch vibrato as a lowerable mono custom node.
CustomNodeType make_delay_vibrato_node(float min_rate_hz = 4.0f);

}  // namespace delay_line

// ── Phase vibrato — the Magnatone varistor lineage ────────────────────────

namespace phase {

using Engine = signal::PhaseVibrato;

// Injectable parameter ids.
inline constexpr state::ParamID kRateHz = 1;
inline constexpr state::ParamID kDepth = 2;
inline constexpr state::ParamID kCenterHz = 3;
inline constexpr state::ParamID kMix = 4;

struct Instance {
    Engine engine;
};

std::string phase_vibrato_type_id(int stages);

/// Worst-case linear gain at the lowest registered allpass corner.
float phase_vibrato_worst_case_gain(int stages, double sample_rate);

/// Phase vibrato as a lowerable mono custom node.
CustomNodeType make_phase_vibrato_node(int stages = Engine::kDefaultStageCount);

}  // namespace phase

// ── Univibe — the lamp-and-photocell lineage ──────────────────────────────

namespace univibe {

using Engine = signal::UniVibe;
using Mode = Engine::Mode;

inline constexpr state::ParamID kRateHz = 1;
inline constexpr state::ParamID kDepth = 2;
inline constexpr state::ParamID kMode = 3;
inline constexpr float kModeVibrato = 0.0f;
inline constexpr float kModeChorus = 1.0f;
inline constexpr const char* kTypeId = "modulation.vibrato.univibe";

struct Instance {
    Engine engine;
};

/// Decodes the stepped mode parameter.
Mode mode_from_param(float value);

/// Worst-case linear gain at the lowest registered allpass corner.
float univibe_worst_case_gain(double sample_rate);

/// Univibe as a lowerable stereo-in/stereo-out custom node.
CustomNodeType make_univibe_node();

}  // namespace univibe

}  // namespace vibrato

}  // namespace pulp::host::modulation

#include <pulp/host/detail/forge_effect_modulation_catalog_descriptor.hpp>
#include <pulp/host/detail/forge_effect_modulation_extended_catalog.hpp>
