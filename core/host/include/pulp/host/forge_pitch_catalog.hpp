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

#include <pulp/host/signal_graph.hpp>

#include <pulp/signal/pitch_shifter.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

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
inline signal::PedalMode decode_pedal_mode(float v) {
    const int i = std::clamp(static_cast<int>(std::lround(v)), 0, 3);
    switch (i) {
        case 1: return signal::PedalMode::harmony;
        case 2: return signal::PedalMode::detune;
        case 3: return signal::PedalMode::dive;
        default: return signal::PedalMode::whammy;
    }
}

/// Worst-case linear gain for the Forge registry (series law 8).
///
/// Cited from the invariant the DSP suite already asserts, not re-derived here.
/// That suite's peak-gain case sweeps the mix and asserts
/// `worst <= kWorstCaseGain * dc_blocker_peak_gain()` and
/// `worst > 0.99 * kWorstCaseGain` — so the bound is both an upper bound AND
/// tight, and this is that same expression.
///
/// The two factors are different kinds of thing, which is why neither can be
/// dropped:
///
///   * `kWorstCaseGain = √2` is the topology. There is no feedback, both legs
///     are bounded by the input peak, and the equal-power dry/wet mix sums them
///     at worst at the 50/50 point.
///   * `dc_blocker_peak_gain()` is `2/(1+p)` at Nyquist — the one place the wet
///     leg can exceed the crossfade's convex-combination bound of 1.0. It is
///     SAMPLE-RATE DEPENDENT (the pole comes from a 5 Hz corner) and grows as
///     the rate falls: 1.000327 at 48 kHz, 1.000356 at 44.1 kHz. Small, but
///     rounding it to 1.0 would make the registry number a slightly wrong
///     bound rather than a right one.
///
/// Control-path only: this constructs and prepares a probe shifter to read the
/// shipped accessor rather than duplicating its formula, so it allocates. Call
/// it when registering, never from `process`.
inline float whammy_worst_case_gain(double sample_rate) {
    Shifter probe;
    probe.prepare(sample_rate);
    return static_cast<float>(Shifter::kWorstCaseGain * probe.dc_blocker_peak_gain());
}

/// The node's reported latency at a given window and sample rate, in samples.
///
/// Exposed because `CustomNodeType` cannot declare latency and the graph's PDC
/// therefore sees zero (see the gap note at the top of this file). A host that
/// needs to align a whammy branch against a dry one has to read this and
/// compensate itself. Computed by the shipped block rather than by repeating
/// its expression here.
inline int whammy_latency_samples(double sample_rate,
                                  double window_ms = Shifter::kWindowMsDefault) {
    Shifter probe;
    probe.prepare(sample_rate);
    probe.set_window_ms(window_ms);
    return probe.latency_samples();
}

/// The conventional dry/wet default for a mode, for the PRESET layer.
/// Deliberately not applied when the `pedal_mode` param moves — see the note at
/// the top of this file.
inline float whammy_default_mix_for_mode(signal::PedalMode mode) {
    return static_cast<float>(Shifter::default_mix_for(mode));
}

/// The expression-pedal pitch shifter as a lowerable custom node.
///
/// `window_ms` is a construction-time argument, not a param: it IS this
/// member's `latency_samples()`, and it sets the warble rate. The DSP sizes its
/// line for the whole declared range at `prepare()`, so any value in range is
/// allocation-free — it is frozen here for the host's sake.
inline CustomNodeType make_whammy_node(double window_ms = Shifter::kWindowMsDefault) {
    CustomNodeType t;
    t.type_id = kTypeId;
    t.version = 1;
    t.num_input_ports = 1;
    t.num_output_ports = 1;
    t.default_name = "Whammy";
    t.lowerable = true;

    t.create = []() -> void* { return new Instance{}; };
    t.destroy = [](void* p) { delete static_cast<Instance*>(p); };
    t.prepare = [window_ms](void* p, double sr, int /*max_block*/) {
        auto* s = static_cast<Instance*>(p);
        s->shifter.prepare(sr);
        // After `prepare()`, which is what sized the line for the whole window
        // range. Setting it here rather than before also means the value
        // survives a re-prepare at a new sample rate, which is where a
        // construction-time realization would otherwise quietly revert.
        s->shifter.set_window_ms(window_ms);
    };
    t.reset = [](void* p) { static_cast<Instance*>(p)->shifter.reset(); };

    // Ranges and defaults are the module's canonical contract, in REAL units.
    // Each row is a design-parameter declaration per the series contract; the
    // `set_*` comments in the DSP header mirror these same numbers for
    // readability at the call site and are not a second declaration.
    using S = Shifter;
    t.baked_params.push_back({kPedal, 0.0f, 1.0f, 0.0f});
    t.baked_params.push_back({kPedalMode, kModeWhammy, kModeDive, kModeWhammy});
    t.baked_params.push_back({kShiftSource, kSourcePedal, kSourceDirect, kSourcePedal});
    t.baked_params.push_back({kShiftSemitones, static_cast<float>(S::kShiftSemisMin),
                              static_cast<float>(S::kShiftSemisMax), 0.0f});
    t.baked_params.push_back({kHeelSemis, static_cast<float>(S::kShiftSemisMin),
                              static_cast<float>(S::kShiftSemisMax),
                              static_cast<float>(S::kHeelSemisDefault)});
    t.baked_params.push_back({kToeSemis, static_cast<float>(S::kShiftSemisMin),
                              static_cast<float>(S::kShiftSemisMax),
                              static_cast<float>(S::kToeSemisDefault)});
    // Harmony intervals clamp to ±24 in the DSP rather than to the shift range;
    // the node's row mirrors the DSP's clamp so a host's knob cannot travel
    // somewhere the block will silently pull it back from.
    t.baked_params.push_back({kIntervalASemis, -24.0f, 24.0f,
                              static_cast<float>(S::kIntervalASemisDefault)});
    t.baked_params.push_back({kIntervalBSemis, -24.0f, 24.0f,
                              static_cast<float>(S::kIntervalBSemisDefault)});
    t.baked_params.push_back({kDetuneCents, -static_cast<float>(S::kDetuneCentsMax),
                              static_cast<float>(S::kDetuneCentsMax),
                              static_cast<float>(S::kDetuneCentsDefault)});
    t.baked_params.push_back({kDiveFloorSemis, static_cast<float>(S::kDiveFloorSemisMin),
                              static_cast<float>(S::kDiveFloorSemisMax),
                              static_cast<float>(S::kDiveFloorSemisDefault)});
    t.baked_params.push_back({kGlideUpMs, static_cast<float>(S::kGlideMsMin),
                              static_cast<float>(S::kGlideMsMax),
                              static_cast<float>(S::kGlideMsDefault)});
    t.baked_params.push_back({kGlideDownMs, static_cast<float>(S::kGlideMsMin),
                              static_cast<float>(S::kGlideMsMax),
                              static_cast<float>(S::kGlideMsDefault)});
    // The whammy mode's conventional default (dry muted). Other modes' defaults
    // are a preset concern, not a live consequence of the mode param.
    t.baked_params.push_back({kMix, 0.0f, 1.0f,
                              whammy_default_mix_for_mode(signal::PedalMode::whammy)});
    t.baked_params.push_back({kDetents, 0.0f, 1.0f, 0.0f});
    t.baked_params.push_back({kInterp, kInterpLinear, kInterpCubic, kInterpLinear});
    t.baked_params.push_back({kDriftDepth, 0.0f, 1.0f, 0.0f});

    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& in, int n,
                                        const BakedParamView& params) {
        auto* s = static_cast<Instance*>(p);
        const float* input = in.channel_ptr(0);
        float* output = out.channel_ptr(0);

        // Sample at a time so every control is sample-accurate — a pedal sweep
        // is the point of this node, and a block-rate pedal would stair-step
        // the one gesture it exists to perform. The setters are a clamp and a
        // store apiece; the slew and the ratio are recomputed inside `process`
        // regardless, so this costs a handful of inlined calls per sample
        // rather than any re-preparation.
        //
        // NOT set here: `set_window_ms`, which is the registration-time
        // realization above. Adding it to this loop would silently make the
        // node's latency automatable.
        for (int k = 0; k < n; ++k) {
            const auto offset = static_cast<std::int32_t>(k);
            s->shifter.set_shift_source(
                params.value_at(kShiftSource, offset) >= 0.5f
                    ? signal::ShiftSource::direct
                    : signal::ShiftSource::pedal);
            s->shifter.set_pedal_mode(decode_pedal_mode(params.value_at(kPedalMode, offset)));
            s->shifter.set_pedal(params.value_at(kPedal, offset));
            s->shifter.set_shift_semitones(params.value_at(kShiftSemitones, offset));
            s->shifter.set_targets(params.value_at(kHeelSemis, offset),
                                   params.value_at(kToeSemis, offset));
            s->shifter.set_harmony(params.value_at(kIntervalASemis, offset),
                                   params.value_at(kIntervalBSemis, offset));
            s->shifter.set_detune_cents(params.value_at(kDetuneCents, offset));
            s->shifter.set_dive_floor_semis(params.value_at(kDiveFloorSemis, offset));
            s->shifter.set_glide_ms(params.value_at(kGlideUpMs, offset),
                                    params.value_at(kGlideDownMs, offset));
            s->shifter.set_mix(params.value_at(kMix, offset));
            s->shifter.set_detents(params.value_at(kDetents, offset) >= 0.5f);
            s->shifter.set_interp(params.value_at(kInterp, offset) >= 0.5f
                                      ? signal::PitchInterp::cubic
                                      : signal::PitchInterp::linear);
            s->shifter.set_drift_depth(params.value_at(kDriftDepth, offset));

            output[static_cast<std::size_t>(k)] =
                s->shifter.process(input[static_cast<std::size_t>(k)]);
        }
    };
    return t;
}

}  // namespace whammy

// ── The diatonic harmony engine lands here (M16, in flight) ───────────────
//
// Its node goes in a `harmony` namespace beside `whammy` above, with its own
// `kTypeId` and its own param ids restarting at 1 — the framework namespaces
// param ids per node, so the numbers may repeat without colliding. It will
// want the same realization test applied to whatever its analysis window
// costs: if its pitch detector carries latency, that latency is frozen at
// registration for the reason given at the top of this file.

}  // namespace pulp::host::pitch
