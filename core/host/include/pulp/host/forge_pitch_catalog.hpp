#pragma once

// Pitch — bake-layer catalog nodes.
//
// The home for the pitch family: nodes that move a signal's pitch rather than
// its level, its spectrum's position, or its time base. It opens with the
// time-domain WHAMMY shifter and is named for the family rather than for that
// one member, because the DIATONIC HARMONY ENGINE is being built alongside it
// and will land here as a second member. Its node belongs in this file, in its
// own `harmony` namespace beside `whammy` below — it shares this file's
// conventions (a shift expressed in real musical units, a pedal or interval
// control, a wet leg a mix blends against dry) and differs in how the target
// interval is CHOSEN: fixed by a pedal law here, derived from a detected pitch
// and a key/scale there. Splitting those across two headers named after their
// algorithms would scatter one set of decisions across two places.
//
// MONO, one port in and one out. `PitchShifterT` is a per-rail block: it has no
// cross-channel coupling of any kind — no shared detector, no shared carrier,
// no stereo mode — so a stereo instrument is exactly two independent copies,
// which is what instancing two nodes already gives. Giving it two ports would
// claim a coupling that does not exist in the code. Contrast the modulation
// family's shifter, which is genuinely true-stereo because `stereo_split`
// drives both rails from ONE carrier.
//
// ── REALIZATION vs INJECTABLE PARAM ───────────────────────────────────────
//
// The standing rule: anything that moves `latency_samples()` is frozen at
// registration, because a node whose reported latency changes under the audio
// thread breaks the host's delay compensation. A genuine topology change is a
// realization too. Coefficients are params.
//
// For this node exactly ONE control is caught by that rule, and it is the whole
// reason this member takes a construction argument at all:
//
//   `window_ms` IS the latency. `latency_samples() = round(window_ms·fs/2000)` —
//   the crossfade window's centre, which the DSP header reports honestly rather
//   than claiming zero. At the default 40 ms window that is 960 samples at
//   48 kHz. Exposing it as an injectable param would let automation move the
//   node's reported latency mid-render, which is the exact failure the rule
//   exists to prevent. So it is a construction argument, like the VCA
//   compressor's lookahead, and for the same reason: the DSP sizes its delay
//   line for the whole declared range at `prepare()`, so any value in range is
//   allocation-free — it is frozen here for the HOST's sake, not the
//   allocator's.
//
//   The DSP header agrees on its own terms, calling `window_ms` "a VOICING
//   control, not a performance one". It is also the warble rate (Eq. 3.5), so
//   freezing it freezes the effect's character, which is a preset-level
//   decision rather than something a pedal sweeps.
//
// Everything else injects, and the two that look like they might not are worth
// saying out loud:
//
//   - `pedal_mode` (whammy / harmony / detune / dive) changes the LAW mapping
//     pedal position to semitones — it does not change the signal path, the
//     port count, or the latency. Same reasoning as the FET compressor's five
//     ratio buttons: a front-panel switch over one unchanged topology.
//   - `interp` (linear / 4-point Lagrange) changes the read STENCIL, not the
//     structure. `latency_samples()` does not depend on it, and cannot: it
//     reports the window centre. The two kernels do differ in group delay by
//     about one sample, which is a sub-0.1 % change on a 960-sample window and
//     is below what this block's honest-throughput latency figure claims to
//     resolve in the first place. If a future interpolant ever carried a
//     latency worth reporting, it would have to move to registration.
//
// ── A GAP WORTH KNOWING ABOUT ─────────────────────────────────────────────
//
// `CustomNodeType` has no way to declare latency. `SignalGraph::node_latency_
// samples()` reads the PDC chain, which accumulates from plugin nodes; a custom
// node contributes zero. So this node's ~960 samples are invisible to the
// graph's delay compensation, and a host that mixes a whammy-bearing branch
// against a dry branch will hear them time-misaligned.
//
// That is a framework gap, not something this file can fix, and it is the
// second reason `window_ms` is frozen at registration: a host CAN be told the
// figure out of band — `whammy_latency_samples()` below computes it from the
// same expression the DSP uses — but only if the value is stable, which it is
// only if it cannot be automated.
//
// ── WET AND DRY BOTH ──────────────────────────────────────────────────────
//
// Unlike a compressor, whose output IS the processed signal, a pitch shifter is
// routinely blended: a harmony voice sits under the dry source, a detune
// thickens it, and only a whammy dive is conventionally heard alone. The DSP
// block owns the equal-power mix so the node exposes it as one control.
//
// The per-mode conventional defaults (`default_mix_for`: 1.0 whammy and dive,
// 0.5 harmony, 0.4 detune) are DELIBERATELY NOT APPLIED when `pedal_mode`
// changes. The DSP header is explicit that a mode change mid-performance must
// not jump the level under the player's foot, and since `pedal_mode` injects,
// applying them live would do exactly that. They belong to the preset layer:
// `whammy_default_mix_for_mode()` exposes them so a preset can apply one at
// load time, and the registry rows below quote them per mode.

#include <pulp/host/forge_param_descriptor.hpp>
#include <pulp/host/custom_node_type.hpp>
#include <pulp/signal/pitch_shifter.hpp>
#include <pulp/signal/harmony_engine.hpp>

namespace pulp::host::pitch {

// ── The whammy / expression-pedal shifter ─────────────────────────────────
//
// One node type. Nothing here changes topology and nothing changes latency
// except the crossfade window, which is therefore taken at construction.
namespace whammy {

inline constexpr const char* kTypeId = "pitch.whammy";

// ── Injectable param ids ──────────────────────────────────────────────────
// Node-local; the framework namespaces per node so two nodes never collide.
inline constexpr state::ParamID kPedal = 1;           // 0 = heel .. 1 = toe
inline constexpr state::ParamID kPedalMode = 2;       // stepped 0..3, see below
inline constexpr state::ParamID kShiftSource = 3;     // stepped 0 = pedal, 1 = direct
inline constexpr state::ParamID kShiftSemitones = 4;  // semitones, direct target
inline constexpr state::ParamID kHeelSemis = 5;       // semitones
inline constexpr state::ParamID kToeSemis = 6;        // semitones
inline constexpr state::ParamID kIntervalASemis = 7;  // semitones, harmony heel
inline constexpr state::ParamID kIntervalBSemis = 8;  // semitones, harmony toe
inline constexpr state::ParamID kDetuneCents = 9;     // cents
inline constexpr state::ParamID kDiveFloorSemis = 10; // semitones, negative
inline constexpr state::ParamID kGlideUpMs = 11;      // ms
inline constexpr state::ParamID kGlideDownMs = 12;    // ms
inline constexpr state::ParamID kMix = 13;            // 0..1, equal-power
inline constexpr state::ParamID kDetents = 14;        // stepped 0/1
inline constexpr state::ParamID kInterp = 15;         // stepped 0 = linear, 1 = cubic
inline constexpr state::ParamID kDriftDepth = 16;     // 0..1

/// The `pedal_mode` param's stepped encoding. Ordered so 0 is the default and
/// the two continuous-bend modes sit at the ends.
inline constexpr float kModeWhammy = 0.0f;
inline constexpr float kModeHarmony = 1.0f;
inline constexpr float kModeDetune = 2.0f;
inline constexpr float kModeDive = 3.0f;

/// The `shift_source` param's stepped encoding.
inline constexpr float kSourcePedal = 0.0f;
inline constexpr float kSourceDirect = 1.0f;

/// The `interp` param's stepped encoding.
inline constexpr float kInterpLinear = 0.0f;
inline constexpr float kInterpCubic = 1.0f;

using Shifter = signal::PitchShifter;

struct Instance {
    Shifter shifter;
};

/// Decodes the stepped `pedal_mode` value. Rounds rather than truncates so a
/// host that sends 0.9999 for "harmony" gets harmony instead of whammy.
signal::PedalMode decode_pedal_mode(float v);

/// Worst-case linear gain for the Forge registry (series law 8).
///
/// Cited from the invariant the DSP suite asserts, not re-derived here.
///
/// This used to be `kWorstCaseGain * dc_blocker_peak_gain()` with
/// `kWorstCaseGain = √2` and the DC blocker contributing `2/(1+p)` at Nyquist —
/// 1.000327 at 48 kHz, so the product was 1.4147. Measured, the node reaches
/// 2.2117 on sustained near-DC content: the registry was understating the
/// headroom a downstream stage plans against by 3.9 dB.
///
/// The error was using the DC blocker's MAGNITUDE-response peak as a bound on
/// its worst single SAMPLE. Those are different quantities; the sample bound is
/// the impulse response's L1 norm, which is exactly 2 at every pole position.
/// `Shifter::kWorstCaseGain` now folds that in — an equal-power mix maximises
/// `cos θ · 1 + sin θ · 2` at `√5` — so it is the whole bound and multiplying
/// by the blocker again would double-count it.
///
/// No longer sample-rate dependent, and that is a consequence rather than a
/// simplification: the L1 norm of `y[n] = x[n] − x[n−1] + p·y[n−1]` is 2 for
/// every `p`, so the pole's position (and hence the rate) drops out.
float whammy_worst_case_gain(double sample_rate);

/// The node's reported latency at a given window and sample rate, in samples.
///
/// Exposed because `CustomNodeType` cannot declare latency and the graph's PDC
/// therefore sees zero (see the gap note at the top of this file). A host that
/// needs to align a whammy branch against a dry one has to read this and
/// compensate itself. Computed by the shipped block rather than by repeating
/// its expression here.
int whammy_latency_samples(double sample_rate, double window_ms = Shifter::kWindowMsDefault);

/// The conventional dry/wet default for a mode, for the PRESET layer.
/// Deliberately not applied when the `pedal_mode` param moves — see the note at
/// the top of this file.
float whammy_default_mix_for_mode(signal::PedalMode mode);

/// The expression-pedal pitch shifter as a lowerable custom node.
///
/// `window_ms` is a construction-time argument, not a param: it IS this
/// member's `latency_samples()`, and it sets the warble rate. The DSP sizes its
/// line for the whole declared range at `prepare()`, so any value in range is
/// allocation-free — it is frozen here for the host's sake.
CustomNodeType make_whammy_node(double window_ms = Shifter::kWindowMsDefault);

ForgeNodeDescriptor descriptor();

}  // namespace whammy

namespace harmony {
inline constexpr const char* kTypeId = "pitch.harmony_engine";

enum : state::ParamID {
    kKey = 1,
    kScale,
    kV1Interval,
    kV1Detune,
    kV1Level,
    kV2Enable,
    kV2Interval,
    kV2Detune,
    kV2Level,
    kGlideMs,
    kDryLevel,
    kHumanize,
};

using Engine = signal::HarmonyEngine;

struct Instance {
    Engine engine;
};

CustomNodeType make_harmony_engine_node();

float worst_case_gain();

int latency_samples(double sample_rate);

ForgeNodeDescriptor descriptor();

}  // namespace harmony

}  // namespace pulp::host::pitch
