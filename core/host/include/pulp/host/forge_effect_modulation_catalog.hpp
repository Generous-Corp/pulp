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

#include <pulp/host/signal_graph.hpp>

#include <pulp/signal/chorus_family.hpp>
#include <pulp/signal/frequency_shifter_ssb.hpp>
#include <pulp/signal/phaser_stages.hpp>
#include <pulp/signal/tpt_filter.hpp>
#include <pulp/signal/vibrato.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
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
inline std::string chorus_type_id(Voicing voicing, JunoMode juno_mode, bool bbd_color) {
    std::string id = "modulation.chorus.";
    switch (voicing) {
        case Voicing::ce2: id += "ce2"; break;
        case Voicing::juno_ensemble:
            id += "juno_";
            id += juno_mode == JunoMode::mode_I    ? "i"
                  : juno_mode == JunoMode::mode_II ? "ii"
                                                   : "i_plus_ii";
            break;
        case Voicing::dimension_d: id += "dimension_d"; break;
        case Voicing::tri_chorus: id += "tri_chorus"; break;
    }
    if (bbd_color) id += ".bbd";
    return id;
}

inline std::string chorus_display_name(Voicing voicing, JunoMode juno_mode, bool bbd_color) {
    std::string name;
    switch (voicing) {
        case Voicing::ce2: name = "Chorus (CE-2)"; break;
        case Voicing::juno_ensemble:
            name = "Chorus (Juno ";
            name += juno_mode == JunoMode::mode_I    ? "I"
                    : juno_mode == JunoMode::mode_II ? "II"
                                                     : "I+II";
            name += ")";
            break;
        case Voicing::dimension_d: name = "Chorus (Dimension D)"; break;
        case Voicing::tri_chorus: name = "Chorus (Tri)"; break;
    }
    if (bbd_color) name += " BBD";
    return name;
}

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
inline float chorus_worst_case_gain(Voicing voicing, double sample_rate) {
    Engine engine;
    engine.prepare(sample_rate);
    engine.set_voicing(voicing);
    engine.set_stereo_width(1.0f);
    return static_cast<float>(engine.worst_case_gain());
}

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
inline CustomNodeType make_chorus_node(Voicing voicing = Voicing::ce2,
                                       JunoMode juno_mode = JunoMode::mode_I,
                                       bool bbd_color = false) {
    CustomNodeType t;
    t.type_id = chorus_type_id(voicing, juno_mode, bbd_color);
    t.version = 1;
    t.num_input_ports = 2;  // ONE logical stereo wire: the mix matrix crosses
    t.num_output_ports = 2; // the channels, so instancing dual-mono loses it
    t.default_name = chorus_display_name(voicing, juno_mode, bbd_color);
    t.lowerable = true;

    t.create = []() -> void* { return new Instance{}; };
    t.destroy = [](void* p) { delete static_cast<Instance*>(p); };
    t.prepare = [voicing, juno_mode, bbd_color](void* p, double sr, int /*max_block*/) {
        auto* s = static_cast<Instance*>(p);
        s->engine.prepare(sr);
        // Order matters and the DSP says so: `set_voicing` adopts the voicing's
        // shipped rate, so the mode must follow it, and any rate the params
        // carry is applied per sample afterwards.
        s->engine.set_juno_mode(juno_mode);
        s->engine.set_voicing(voicing);
        s->engine.set_bbd_color(bbd_color);
    };
    t.reset = [](void* p) { static_cast<Instance*>(p)->engine.reset(); };

    t.baked_params.push_back({kRateHz, static_cast<float>(kRateMinHz),
                              static_cast<float>(kRateMaxHz),
                              static_cast<float>(Engine::calibration(voicing).rate_hz)});
    t.baked_params.push_back({kDepth, 0.0f, 100.0f, 100.0f});
    const auto mix_default =
        static_cast<float>(Engine::calibration(voicing).mix_default * 100.0);
    t.baked_params.push_back({kMix, 0.0f, 100.0f, mix_default});
    t.baked_params.push_back({kStereoWidth, 0.0f, 100.0f, 100.0f});

    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& in, int n,
                                        const BakedParamView& params) {
        auto* s = static_cast<Instance*>(p);
        const float* in_left = in.channel_ptr(0);
        const float* in_right = in.channel_ptr(1);
        float* out_left = out.channel_ptr(0);
        float* out_right = out.channel_ptr(1);

        for (int k = 0; k < n; ++k) {
            const auto offset = static_cast<std::int32_t>(k);
            s->engine.set_rate_hz(params.value_at(kRateHz, offset));
            s->engine.set_depth(params.value_at(kDepth, offset) * 0.01f);
            s->engine.set_mix(params.value_at(kMix, offset) * 0.01f);
            s->engine.set_stereo_width(params.value_at(kStereoWidth, offset) * 0.01f);

            // The engine processes in place, so the frame is staged in locals
            // rather than written to the output first — which also keeps this
            // correct when a caller aliases the input and output views.
            float left = in_left[static_cast<std::size_t>(k)];
            float right = in_right[static_cast<std::size_t>(k)];
            s->engine.process(&left, &right, 1);
            out_left[static_cast<std::size_t>(k)] = left;
            out_right[static_cast<std::size_t>(k)] = right;
        }
    };
    return t;
}

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

inline std::string phaser_type_id(int stages) {
    return "modulation.phaser." + std::to_string(stages);
}

/// Worst-case linear gain for the Forge registry (series law 8).
///
/// The topology has a feedback path, so this is a loop bound rather than a
/// constructive sum: `1/(1 − kFeedbackMax)`. The DSP publishes it as a
/// `constexpr` and its suite asserts realised gain stays under it, so this
/// reports rather than re-derives — and it is independent of stage count,
/// because the cascade inside the loop is allpass.
inline float phaser_worst_case_gain() {
    return static_cast<float>(Engine::worst_case_gain());
}

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
inline CustomNodeType make_phaser_node(int stages = Engine::kStageCountDefault) {
    // Normalised the same way the DSP normalises it, so the registered id and
    // the running engine can never disagree about what was asked for.
    const int clamped = std::clamp(stages, Engine::kMinStages, Engine::kMaxStages);
    const int even_stages = clamped - (clamped % 2);

    CustomNodeType t;
    t.type_id = phaser_type_id(even_stages);
    t.version = 1;
    t.num_input_ports = 2;  // ONE logical stereo wire: the stereo spread puts
    t.num_output_ports = 2; // the two channels' LFOs in quadrature
    t.default_name = "Phaser (" + std::to_string(even_stages) + "-stage)";
    t.lowerable = true;

    t.create = []() -> void* { return new Instance{}; };
    t.destroy = [](void* p) { delete static_cast<Instance*>(p); };
    t.prepare = [even_stages](void* p, double sr, int /*max_block*/) {
        auto* s = static_cast<Instance*>(p);
        s->engine.prepare(sr);
        s->engine.set_stage_count(even_stages);
    };
    t.reset = [](void* p) { static_cast<Instance*>(p)->engine.reset(); };

    t.baked_params.push_back({kRateHz, static_cast<float>(kRateMinHz),
                              static_cast<float>(kRateMaxHz),
                              static_cast<float>(Engine::kRateDefaultHz)});
    t.baked_params.push_back({kDepth, 0.0f, 100.0f, 100.0f});
    t.baked_params.push_back({kCenterHz, static_cast<float>(kCenterMinHz),
                              static_cast<float>(kCenterMaxHz), 400.0f});
    t.baked_params.push_back({kFeedback, -static_cast<float>(Engine::kFeedbackMax),
                              static_cast<float>(Engine::kFeedbackMax),
                              static_cast<float>(Engine::kColorOffFeedback)});
    t.baked_params.push_back({kMix, 0.0f, 100.0f,
                              static_cast<float>(Engine::kMixDefault * 100.0)});
    t.baked_params.push_back({kStereoSpread, 0.0f, 0.5f, 0.25f});
    t.baked_params.push_back({kStaggerRatio, static_cast<float>(Engine::kStaggerMin),
                              static_cast<float>(Engine::kStaggerMax),
                              static_cast<float>(Engine::kStaggerDefault)});
    t.baked_params.push_back({kWave, kWaveSine, kWaveSmoothRandom, kWaveTriangle});

    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& in, int n,
                                        const BakedParamView& params) {
        auto* s = static_cast<Instance*>(p);
        const float* in_left = in.channel_ptr(0);
        const float* in_right = in.channel_ptr(1);
        float* out_left = out.channel_ptr(0);
        float* out_right = out.channel_ptr(1);

        for (int k = 0; k < n; ++k) {
            const auto offset = static_cast<std::int32_t>(k);
            s->engine.set_rate_hz(params.value_at(kRateHz, offset));
            s->engine.set_depth(params.value_at(kDepth, offset) * 0.01f);
            s->engine.set_center_hz(params.value_at(kCenterHz, offset));
            s->engine.set_feedback(params.value_at(kFeedback, offset));
            s->engine.set_mix(params.value_at(kMix, offset) * 0.01f);
            s->engine.set_stereo_spread(params.value_at(kStereoSpread, offset));
            s->engine.set_stagger_ratio(params.value_at(kStaggerRatio, offset));
            s->engine.set_wave(wave_from_param(params.value_at(kWave, offset)));

            const float left = in_left[static_cast<std::size_t>(k)];
            const float right = in_right[static_cast<std::size_t>(k)];
            s->engine.process(&left, &right, &out_left[static_cast<std::size_t>(k)],
                              &out_right[static_cast<std::size_t>(k)], 1);
        }
    };
    return t;
}

}  // namespace phaser

// ═══════════════════════════════════════════════════════════════════════════
//  Vibrato — three lineages, three nodes
// ═══════════════════════════════════════════════════════════════════════════

namespace vibrato {

/// L1 norm of a static allpass cascade's impulse response, at fixed corners.
///
/// This is the honest worst-case SAMPLE gain of the two phase engines, and it
/// is not 1. Unity magnitude response bounds steady-state sinusoids only; an
/// allpass's impulse response changes sign, so a sign-matched bounded input
/// accumulates, and both engines measure above a factor of two in practice. The
/// DSP suite computes this same quantity the same way and asserts the realised
/// peak stays under it — reporting the sinusoidal bound of 1 instead would put a
/// number in the registry that the DSP's own suite disproves.
///
/// Registration-time only. It runs a long impulse response and is never called
/// from the audio thread.
inline double allpass_cascade_l1(const std::vector<double>& corners_hz, double sample_rate,
                                 int length = 200000) {
    std::vector<signal::TptFilter64> stages(corners_hz.size());
    for (std::size_t i = 0; i < corners_hz.size(); ++i) {
        stages[i].prepare(sample_rate);
        stages[i].set_cutoff(corners_hz[i]);
    }
    double l1 = 0.0;
    for (int i = 0; i < length; ++i) {
        double x = (i == 0) ? 1.0 : 0.0;
        for (auto& stage : stages) x = stage.process_allpass(x);
        l1 += std::abs(x);
    }
    return l1;
}

// ── Delay vibrato — the only one that really shifts pitch ─────────────────

namespace delay_line {

using Engine = signal::DelayVibrato;

// ── Injectable param ids ──────────────────────────────────────────────────
inline constexpr state::ParamID kRateHz = 1;      // Hz
inline constexpr state::ParamID kDepthCents = 2;  // cents
inline constexpr state::ParamID kDelayMs = 3;     // ms, onset
inline constexpr state::ParamID kFadeInMs = 4;    // ms, onset

struct Instance {
    Engine engine;
    // Last values actually pushed into the onset setters. See the process
    // callback for why these two cannot be written unconditionally.
    float applied_delay_ms = -1.0f;
    float applied_fade_in_ms = -1.0f;
};

inline std::string delay_vibrato_type_id(float min_rate_hz) {
    // The floor changes the injectable rate range, so it remains part of the
    // stable realization identity even though vibrato declares no PDC latency.
    return "modulation.vibrato.delay." +
           std::to_string(static_cast<int>(std::lround(min_rate_hz * 10.0f)));
}

/// Worst-case linear gain for the Forge registry (series law 8).
///
/// One unit-gain tap, and still not 0 dB: the tap is read through the 4-point
/// Lagrange kernel, whose L1 norm peaks at 1.25 at a half-sample offset. The
/// DSP suite scans the whole fractional range and asserts that constant is the
/// maximum, and separately that the engine's realised peak stays under it.
inline float delay_vibrato_worst_case_gain() {
    return static_cast<float>(Engine::kInterpolatorPeakGain);
}

/// True pitch vibrato as a lowerable custom node. Mono: the engine has no
/// stereo coupling, so a stereo source is two instances rather than a pretend
/// stereo image.
///
/// The delay tap moves with the injectable rate and depth controls, so it is an
/// intentional modulation delay rather than a fixed processing latency. The
/// node therefore leaves CustomNodeType::latency_samples empty: publishing a
/// worst-case bound would make PDC delay parallel dry paths by the wrong amount
/// for every setting except that single extreme.
inline CustomNodeType make_delay_vibrato_node(float min_rate_hz = 4.0f) {
    const float floor_hz = std::clamp(min_rate_hz, static_cast<float>(Engine::kMinRateHz),
                                      static_cast<float>(Engine::kMaxRateHz));

    CustomNodeType t;
    t.type_id = delay_vibrato_type_id(floor_hz);
    t.version = 1;
    t.num_input_ports = 1;
    t.num_output_ports = 1;
    t.default_name = "Vibrato (Delay)";
    t.lowerable = true;

    t.create = []() -> void* { return new Instance{}; };
    t.destroy = [](void* p) { delete static_cast<Instance*>(p); };
    t.prepare = [](void* p, double sr, int /*max_block*/) {
        static_cast<Instance*>(p)->engine.prepare(sr);
    };
    t.reset = [](void* p) {
        auto* s = static_cast<Instance*>(p);
        s->engine.reset();
        s->applied_delay_ms = -1.0f;
        s->applied_fade_in_ms = -1.0f;
    };

    t.baked_params.push_back({kRateHz, floor_hz, static_cast<float>(Engine::kMaxRateHz),
                              std::max(floor_hz, static_cast<float>(Engine::kDefaultRateHz))});
    t.baked_params.push_back({kDepthCents, 0.0f, static_cast<float>(Engine::kMaxDepthCents),
                              static_cast<float>(Engine::kDefaultDepthCents)});
    t.baked_params.push_back({kDelayMs, 0.0f, static_cast<float>(Engine::kMaxLifecycleMs), 0.0f});
    t.baked_params.push_back({kFadeInMs, 0.0f, static_cast<float>(Engine::kMaxLifecycleMs), 0.0f});

    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& in, int n,
                                        const BakedParamView& params) {
        auto* s = static_cast<Instance*>(p);
        const float* input = in.channel_ptr(0);
        float* output = out.channel_ptr(0);

        for (int k = 0; k < n; ++k) {
            const auto offset = static_cast<std::int32_t>(k);
            s->engine.set_rate_hz(params.value_at(kRateHz, offset));
            s->engine.set_depth_cents(params.value_at(kDepthCents, offset));

            // The two onset controls are applied ONLY WHEN THEY CHANGE, and
            // that is load-bearing rather than an optimisation. Both setters
            // re-arm the lifecycle envelope, which zeroes its depth scale — so
            // writing them every sample would hold the envelope at zero forever
            // and the node would pass audio through with no vibrato at all,
            // while every parameter read back exactly the value that was set.
            const float delay_ms = params.value_at(kDelayMs, offset);
            if (delay_ms != s->applied_delay_ms) {
                s->engine.set_delay_ms(delay_ms);
                s->applied_delay_ms = delay_ms;
            }
            const float fade_ms = params.value_at(kFadeInMs, offset);
            if (fade_ms != s->applied_fade_in_ms) {
                s->engine.set_fade_in_ms(fade_ms);
                s->applied_fade_in_ms = fade_ms;
            }

            output[static_cast<std::size_t>(k)] =
                s->engine.process(input[static_cast<std::size_t>(k)]);
        }
    };
    return t;
}

}  // namespace delay_line

// ── Phase vibrato — the Magnatone varistor lineage ────────────────────────

namespace phase {

using Engine = signal::PhaseVibrato;

inline constexpr state::ParamID kRateHz = 1;    // Hz
inline constexpr state::ParamID kDepth = 2;     // %
inline constexpr state::ParamID kCenterHz = 3;  // Hz
inline constexpr state::ParamID kMix = 4;       // %

struct Instance {
    Engine engine;
};

inline std::string phase_vibrato_type_id(int stages) {
    return "modulation.vibrato.phase." + std::to_string(stages);
}

/// Worst-case linear gain for the Forge registry (series law 8).
///
/// The cascade's impulse-response L1 at the LOWEST corner the registered sweep
/// can reach with every stage active — which is what the DSP suite asserts the
/// realised peak stays under, and which is well above 1. See
/// `allpass_cascade_l1` for why the unity-magnitude property does not bound
/// this.
inline float phase_vibrato_worst_case_gain(int stages, double sample_rate) {
    const double lowest_corner = Engine::kMinCenterHz * std::exp2(-Engine::kSweepOctaves);
    return static_cast<float>(allpass_cascade_l1(
        std::vector<double>(static_cast<std::size_t>(stages), lowest_corner), sample_rate));
}

/// Phase vibrato as a lowerable custom node. Mono, for the same reason as the
/// delay engine.
///
/// `stages` is a REGISTRATION-TIME REALIZATION: it is the length of the allpass
/// cascade, so it sets how many notches the summed path has and how much phase
/// the sweep can accumulate. Latency is zero at every count, so this is the
/// topology rule rather than the latency rule. The DSP also resets stages as
/// they become active, which is a state discontinuity a user would hear as a
/// click if it happened under automation.
inline CustomNodeType make_phase_vibrato_node(int stages = Engine::kDefaultStageCount) {
    const int clamped = std::clamp(stages, 1, Engine::kMaxStages);

    CustomNodeType t;
    t.type_id = phase_vibrato_type_id(clamped);
    t.version = 1;
    t.num_input_ports = 1;
    t.num_output_ports = 1;
    t.default_name = "Vibrato (Phase, " + std::to_string(clamped) + "-stage)";
    t.lowerable = true;

    t.create = []() -> void* { return new Instance{}; };
    t.destroy = [](void* p) { delete static_cast<Instance*>(p); };
    t.prepare = [clamped](void* p, double sr, int /*max_block*/) {
        auto* s = static_cast<Instance*>(p);
        s->engine.prepare(sr);
        s->engine.set_stage_count(clamped);
    };
    t.reset = [](void* p) { static_cast<Instance*>(p)->engine.reset(); };

    t.baked_params.push_back({kRateHz, static_cast<float>(Engine::kMinRateHz),
                              static_cast<float>(Engine::kMaxRateHz),
                              static_cast<float>(Engine::kDefaultRateHz)});
    t.baked_params.push_back({kDepth, 0.0f, 100.0f,
                              static_cast<float>(Engine::kDefaultDepth * 100.0)});
    t.baked_params.push_back({kCenterHz, static_cast<float>(Engine::kMinCenterHz),
                              static_cast<float>(Engine::kMaxCenterHz),
                              static_cast<float>(Engine::kDefaultCenterHz)});
    t.baked_params.push_back({kMix, 0.0f, 100.0f,
                              static_cast<float>(Engine::kDefaultMix * 100.0)});

    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& in, int n,
                                        const BakedParamView& params) {
        auto* s = static_cast<Instance*>(p);
        const float* input = in.channel_ptr(0);
        float* output = out.channel_ptr(0);
        for (int k = 0; k < n; ++k) {
            const auto offset = static_cast<std::int32_t>(k);
            s->engine.set_rate_hz(params.value_at(kRateHz, offset));
            s->engine.set_depth(params.value_at(kDepth, offset) * 0.01);
            s->engine.set_center_hz(params.value_at(kCenterHz, offset));
            s->engine.set_mix(params.value_at(kMix, offset) * 0.01);
            output[static_cast<std::size_t>(k)] =
                s->engine.process(input[static_cast<std::size_t>(k)]);
        }
    };
    return t;
}

}  // namespace phase

// ── Univibe — the lamp-and-photocell lineage ──────────────────────────────

namespace univibe {

using Engine = signal::UniVibe;
using Mode = Engine::Mode;

inline constexpr state::ParamID kRateHz = 1;  // Hz
inline constexpr state::ParamID kDepth = 2;   // %
inline constexpr state::ParamID kMode = 3;    // stepped 0 = vibrato, 1 = chorus

inline constexpr float kModeVibrato = 0.0f;
inline constexpr float kModeChorus = 1.0f;

inline constexpr const char* kTypeId = "modulation.vibrato.univibe";

struct Instance {
    Engine engine;
};

/// Decodes the stepped `mode` param, rounding to the nearest step.
inline Mode mode_from_param(float value) {
    return std::lround(value) >= 1 ? Mode::chorus : Mode::vibrato;
}

/// Worst-case linear gain for the Forge registry (series law 8).
///
/// As the phase engine: the four staggered stages' impulse-response L1 at the
/// bottom of the sweep, which is the ceiling the DSP suite asserts against.
inline float univibe_worst_case_gain(double sample_rate) {
    const double scale = Engine::corner_scale(0.0, 1.0);
    std::vector<double> corners(static_cast<std::size_t>(Engine::kStageCount));
    for (int i = 0; i < Engine::kStageCount; ++i)
        corners[static_cast<std::size_t>(i)] =
            Engine::kStageBaseHz[static_cast<std::size_t>(i)] * scale;
    return static_cast<float>(allpass_cascade_l1(corners, sample_rate));
}

/// The Univibe as a lowerable custom node.
///
/// TWO PORTS IN, and the engine takes one — the input is folded to mono at the
/// node boundary, deliberately. The circuit is a mono instrument pedal whose
/// chorus position SPLITS one input into an untouched path and a phase-shifted
/// one, so it is genuinely mono-in/stereo-out. Registering it that way would
/// make it the only asymmetric node in the family and would not survive a
/// stereo insert; folding at the boundary keeps the stereo image the mode
/// creates while accepting a stereo source.
///
/// NO REALIZATION AXIS. The stage count is fixed at four by the circuit and is
/// not settable, and `mode` is a coefficient change over one unchanged path —
/// it re-weights the direct and shifted paths, touches no state, and leaves
/// latency at zero — so it injects, exactly like the FET compressor's ratio
/// buttons.
inline CustomNodeType make_univibe_node() {
    CustomNodeType t;
    t.type_id = kTypeId;
    t.version = 1;
    t.num_input_ports = 2;
    t.num_output_ports = 2;
    t.default_name = "Univibe";
    t.lowerable = true;

    t.create = []() -> void* { return new Instance{}; };
    t.destroy = [](void* p) { delete static_cast<Instance*>(p); };
    t.prepare = [](void* p, double sr, int /*max_block*/) {
        static_cast<Instance*>(p)->engine.prepare(sr);
    };
    t.reset = [](void* p) { static_cast<Instance*>(p)->engine.reset(); };

    t.baked_params.push_back({kRateHz, static_cast<float>(Engine::kMinRateHz),
                              static_cast<float>(Engine::kMaxRateHz),
                              static_cast<float>(Engine::kDefaultRateHz)});
    t.baked_params.push_back({kDepth, 0.0f, 100.0f,
                              static_cast<float>(Engine::kDefaultDepth * 100.0)});
    t.baked_params.push_back({kMode, kModeVibrato, kModeChorus, kModeVibrato});

    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& in, int n,
                                        const BakedParamView& params) {
        auto* s = static_cast<Instance*>(p);
        const float* in_left = in.channel_ptr(0);
        const float* in_right = in.channel_ptr(1);
        float* out_left = out.channel_ptr(0);
        float* out_right = out.channel_ptr(1);

        for (int k = 0; k < n; ++k) {
            const auto offset = static_cast<std::int32_t>(k);
            s->engine.set_rate_hz(params.value_at(kRateHz, offset));
            s->engine.set_depth(params.value_at(kDepth, offset) * 0.01);
            s->engine.set_mode(mode_from_param(params.value_at(kMode, offset)));

            const float mono = 0.5f * (in_left[static_cast<std::size_t>(k)] +
                                       in_right[static_cast<std::size_t>(k)]);
            s->engine.process(mono, out_left[static_cast<std::size_t>(k)],
                              out_right[static_cast<std::size_t>(k)]);
        }
    };
    return t;
}

}  // namespace univibe

}  // namespace vibrato

}  // namespace pulp::host::modulation

#include <pulp/host/detail/forge_effect_modulation_extended_catalog.hpp>

#include <pulp/host/detail/forge_effect_modulation_descriptors.hpp>
