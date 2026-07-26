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
// EVERY CHOICE HERE IS AN INJECTABLE PARAM, none is a registration-time
// realization, and that is worth stating because it is the opposite of the
// saturator's arrangement. The rule is that anything which changes reported
// latency must be frozen at registration, because a node whose latency moves
// under the audio thread breaks the host's delay compensation. This node
// reports zero latency in every mode: the Hilbert network is IIR allpass with
// no bulk delay, the mode switch only changes the sign and scale applied to
// the quadrature term, and the feedback delay lives inside a loop rather than
// in the signal's path to the output. Nothing here can move latency, so
// nothing here has to be baked in.
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

#include <pulp/host/signal_graph.hpp>

#include <pulp/signal/frequency_shifter_ssb.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>

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

struct SsbFrequencyShifterInstance {
    signal::SsbFrequencyShifter shifter;
};

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
inline float ssb_frequency_shifter_worst_case_gain() {
    return static_cast<float>(signal::SsbFrequencyShifter::worst_case_gain());
}

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
inline CustomNodeType make_frequency_shifter_node() {
    CustomNodeType t;
    t.type_id = kSsbFrequencyShifterTypeId;
    t.version = 1;
    t.num_input_ports = 2;  // 0 = left, 1 = right (ONE logical stereo wire)
    t.num_output_ports = 2;
    t.default_name = "Frequency Shifter";
    t.lowerable = true;

    t.create = []() -> void* { return new SsbFrequencyShifterInstance{}; };
    t.destroy = [](void* p) { delete static_cast<SsbFrequencyShifterInstance*>(p); };
    t.prepare = [](void* p, double sr, int /*max_block*/) {
        static_cast<SsbFrequencyShifterInstance*>(p)->shifter.prepare(sr);
    };
    t.reset = [](void* p) { static_cast<SsbFrequencyShifterInstance*>(p)->shifter.reset(); };

    // Ranges and defaults are the module's canonical contract, in REAL units.
    // Each row is a design-parameter declaration per the series contract; the
    // `set_*` comments in the DSP header mirror these same numbers for
    // readability at the call site and are not a second declaration.
    //
    // `shift_hz` is registered in HERTZ across its full signed range rather
    // than as a normalised knob, because Hz is what the parameter means and
    // what automation should record. The signed piecewise linear/log taper the
    // spec describes — fine resolution below `kLinZoneHz` where detune and
    // barberpole live, geometric above it — is a knob-to-Hz mapping and lives
    // on `SsbFrequencyShifterT::shift_hz_from_knob`, so a UI, a macro or a
    // preset importer can all apply the same one curve. Putting the taper in
    // the baked range instead would make every automation lane store knob
    // positions whose meaning changes if the curve is ever retuned.
    using Shifter = signal::SsbFrequencyShifter;
    t.baked_params.push_back({kShiftHz, static_cast<float>(-Shifter::kMaxShiftHz),
                              static_cast<float>(Shifter::kMaxShiftHz), 0.0f});
    t.baked_params.push_back({kFeedback, 0.0f, static_cast<float>(Shifter::kMaxFeedback), 0.0f});
    t.baked_params.push_back({kFeedbackDelayMs, static_cast<float>(Shifter::kMinDelayMs),
                              static_cast<float>(Shifter::kMaxLoopMs), 8.0f});
    t.baked_params.push_back({kMix, 0.0f, 100.0f, 100.0f});
    t.baked_params.push_back({kShiftMode, kModeUp, kModeStereoSplit, kModeUp});
    t.baked_params.push_back({kStereoSpread, 0.0f, 100.0f, 100.0f});

    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& in, int n,
                                        const BakedParamView& params) {
        auto* s = static_cast<SsbFrequencyShifterInstance*>(p);
        const float* in_left = in.channel_ptr(0);
        const float* in_right = in.channel_ptr(1);
        float* out_left = out.channel_ptr(0);
        float* out_right = out.channel_ptr(1);

        // Sample at a time so every knob is sample-accurate. The setters are a
        // clamp, a store and a smoother retarget — no filter is redesigned and
        // nothing is resized — so this costs a handful of inlined calls per
        // sample rather than a re-preparation.
        for (int k = 0; k < n; ++k) {
            const auto offset = static_cast<std::int32_t>(k);
            s->shifter.set_shift_hz(params.value_at(kShiftHz, offset));
            s->shifter.set_feedback(params.value_at(kFeedback, offset));
            s->shifter.set_feedback_delay_ms(params.value_at(kFeedbackDelayMs, offset));
            s->shifter.set_mix(params.value_at(kMix, offset) * 0.01f);
            s->shifter.set_stereo_spread(params.value_at(kStereoSpread, offset) * 0.01f);
            s->shifter.set_mode(mode_from_param(params.value_at(kShiftMode, offset)));

            float left = in_left[static_cast<std::size_t>(k)];
            float right = in_right[static_cast<std::size_t>(k)];
            s->shifter.process_stereo(left, right);
            out_left[static_cast<std::size_t>(k)] = left;
            out_right[static_cast<std::size_t>(k)] = right;
        }
    };
    return t;
}

}  // namespace pulp::host::modulation
