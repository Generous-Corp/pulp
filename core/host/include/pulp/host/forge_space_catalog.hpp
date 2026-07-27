#pragma once

// Space — bake-layer catalog nodes.
//
// The home for the reverb / room family. Two members today, a third landing
// next:
//
//   * `convolution` — the zero-latency non-uniform partitioned convolver. A
//     MEASURED space: you hand it an impulse response and it reproduces that
//     room, exactly, with no I/O delay.
//   * `nonlin_ambience` — the velvet-noise tap cloud. A DESIGNED space: the
//     envelope is drawn rather than decayed, which is how it makes the four
//     shapes (Ambience, Gated, Reverse, NonLin2) a recirculating tank cannot.
//   * **speaker / cabinet emulation is being built now and joins this file.**
//     It is the convolver's physics sibling: the sampled-IR path is
//     `convolution` here, the physics path is that module's. Its DSP header
//     already names `ZeroLatencyConvolverT` + `IrNormalizeMode::peak` as the
//     loader a cabinet IR should cite, so when it lands it belongs beside them
//     rather than in a file of its own. Room is left below; nothing here needs
//     to move to make space for it.
//
// The two members share a family and share nothing else — one is a frequency-
// domain partition schedule over measured taps, the other a sparse FIR over
// designed ones — so what this file standardises is the CONTRACT, not the DSP.
//
// BOTH MEMBERS ARE TRUE STEREO: two ports in, two out, as ONE logical wire.
// That is not the family default being pattern-matched; each earns it. The
// convolver's `width` is a mid/side operation across the pair and its
// true-stereo realization routes a four-cell IR as a 2x2 matrix, so its
// channels are genuinely coupled. The ambience mono-SUMS its input into one
// diffused field and then reads it through two decorrelated tap tables, which
// means a dual-mono instancing would produce two independent fields from two
// independent inputs — a different effect, not the same one twice.
//
// ── REALIZATION vs INJECTABLE PARAM, applied per member ─────────────────────
//
// The standing rule first: anything that moves `latency_samples()` is a
// registration-time realization, because a node whose reported latency changes
// under the audio thread breaks the host's delay compensation.
//
// **Neither member has such a parameter.** Both report a literal, constant
// zero — the convolver because its head is a direct FIR over samples already
// present in the block, the ambience because its pre-delay is the effect rather
// than lookahead. So nothing here is frozen for latency's sake, and the
// realizations below are each frozen for a different, stated reason:
//
//   1. **The IR is a realization** (convolver). `load_impulse_response()`
//      allocates — resampling, tail analysis, partition spectra — and is
//      documented as worker/prepare context. There is no way to make it a
//      param, and pretending otherwise would put an FFT plan on the audio
//      thread.
//   2. **The load-time IR policy is a realization** (convolver:
//      `normalize_mode`, `tail_trim_db`, `tail_fade_ms`,
//      `resample_taps_per_phase`). Its DSP says these "take effect on the NEXT
//      `load_impulse_response()`" — so declaring them as baked params would
//      produce knobs that accept every value, report success, and do nothing
//      until a reload that never comes on the audio thread. A silently inert
//      param is worse than an absent one.
//   3. **`true_stereo` is a realization** (convolver). It re-maps which IR
//      cells feed which output channel — a 2x2 routing matrix rather than two
//      independent paths — and requires a four-channel IR to mean anything. A
//      genuine topology change, in the diode-bridge member's sense.
//   4. **`seed` is a realization** (ambience), and this one is a series law
//      rather than a judgement call: law 2 requires seeds to be fixed at reset
//      and never automated or macro-exposed. The ambience's own RT contract
//      says the same. Exposing it as a baked param would contradict both.
//   5. **`max_length_ms` is a realization** (ambience). It sizes the tap ring
//      and the tap tables at `prepare()`, exactly as the VCA compressor's
//      lookahead ceiling sizes its ring — it is the largest field the node can
//      ever be asked for, not a control.
//
// ── PARAM RATE: two tiers, and why the split is real ────────────────────────
//
// Not every injectable param here is sample-accurate, and the ones that are not
// say so rather than implying a resolution they do not have.
//
//   * **Sample-rate (ambience continuous params).** `diffusion`, `tone`,
//     `hf_damp_hz`, `width_pct`, `converter_amount`, `output_gain_db`,
//     `mix_pct` are read per sample. Each is a gain or a filter coefficient the
//     DSP smooths or applies state-continuously, so per-sample injection is
//     both cheap and meaningful.
//   * **Block-rate (ambience topology params).** `program`, `length_ms`,
//     `predelay_ms`, `density_pct`, `density_growth`, `gate_hold_pct`,
//     `attack_pct` are read once per block, at offset 0. Each of them
//     REGENERATES the tap table — up to 8000 taps per channel — and starts the
//     module's 20 ms equal-power crossfade into the new one. Reading them per
//     sample would invite one full rebuild per sample, which at the maximum
//     field length is roughly five orders of magnitude past real time, to
//     produce sub-block resolution on a change that takes 20 ms to fade in
//     anyway. The DSP already no-ops an unchanged value, so a static setting
//     costs nothing at either rate.
//   * **Block-rate (every convolver param).** This one is not a choice, it is
//     an observation about the DSP: `mix_chunk` hoists `wet_gain`, `dry_gain`,
//     `width` and `ir_gain` OUT of its sample loop, so a value injected at
//     sample 64 of a block could not take effect until the next chunk boundary
//     no matter how often this node set it. Declaring these sample-accurate
//     would be claiming a resolution the engine does not implement. They are
//     applied at offset 0 and the suite asserts that contract rather than
//     assuming it.
//
// ── `worst_case_gain` (series law 8) ────────────────────────────────────────
//
// Each member's number is the bound its OWN DSP suite already asserts, composed
// up to this node's parameter ceilings — never a fresh estimate. Both are
// reported at the ceilings rather than at the defaults, following the FET
// member, because a baked param can be automated anywhere in its declared
// range.
//
// WET/DRY: both members own a dry/wet control, unlike the dynamics family. A
// reverb's dry path is part of the effect — "80 % wet" is a thing a user asks
// a reverb for and not a thing they ask a compressor for — so both nodes carry
// it and both bounds include the dry sum.

#include <pulp/host/signal_graph.hpp>

#include <pulp/signal/nonlin_ambience.hpp>
#include <pulp/signal/speaker_cabinet.hpp>
#include <pulp/signal/zero_latency_convolver.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace pulp::host::space {

// ── The convolution reverb (measured spaces) ──────────────────────────────
//
// One node type. The IR and the whole load-time policy are registration
// arguments, for the reasons in the file note; everything left is a gain, a
// filter corner, or a delay, and all of it injects.
namespace convolution {

inline constexpr const char* kTypeId = "space.convolution_reverb";

inline constexpr state::ParamID kIrGainDb = 1;     // dB   — trim on the convolved path
inline constexpr state::ParamID kPredelayMs = 2;   // ms   — wet-path pre-delay, NOT latency
inline constexpr state::ParamID kWetPercent = 3;   // %
inline constexpr state::ParamID kDryPercent = 4;   // %
inline constexpr state::ParamID kWidthPercent = 5; // %    — mid/side on the wet return
inline constexpr state::ParamID kLowcutHz = 6;     // Hz   — send high-pass; min == bypass
inline constexpr state::ParamID kHighcutHz = 7;    // Hz   — send low-pass; max == bypass

using Engine = signal::ZeroLatencyConvolver;

/// An impulse response, owned by the registered type.
///
/// Held by `shared_ptr` because `CustomNodeType` is copied into the graph's
/// registry and every instance's `prepare` re-ingests it at the session rate —
/// copying a multi-second four-channel IR per instance would be pure waste, and
/// the buffer is immutable once registered.
struct ImpulseResponse {
    std::vector<std::vector<float>> channels;  // 1, 2, or 4 channels
    double sample_rate = 48000.0;
};

inline bool valid_impulse_response(const ImpulseResponse& ir) {
    if (ir.channels.size() != 1u && ir.channels.size() != 2u && ir.channels.size() != 4u)
        return false;
    if (!std::isfinite(ir.sample_rate) || ir.sample_rate <= 0.0 || ir.channels[0].empty())
        return false;
    const std::size_t length = ir.channels[0].size();
    if (length > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        !Engine::valid_resample_geometry(static_cast<int>(length), ir.sample_rate,
                                         48000.0, Engine::kResampTapsPerPhaseDefault))
        return false;
    for (const auto& channel : ir.channels) {
        if (channel.size() != length) return false;
        for (float sample : channel)
            if (!std::isfinite(sample)) return false;
    }
    return true;
}

/// The load-time policy, frozen at registration. See the file note, item 2:
/// each of these is documented by the DSP as taking effect on the next load, so
/// none of them can be a param.
struct IrPolicy {
    signal::IrNormalizeMode normalize = signal::IrNormalizeMode::energy;
    double tail_trim_db = Engine::kTailTrimDbDefault;
    double tail_fade_ms = Engine::kTailFadeMsDefault;
    int resample_taps_per_phase = Engine::kResampTapsPerPhaseDefault;
    /// 2x2 cell routing. Requires a four-channel IR to mean anything; with
    /// fewer channels the DSP falls back to its diagonal routing.
    bool true_stereo = false;
};

struct Instance {
    Engine engine;
};

/// Worst-case linear gain for the Forge registry (series law 8).
///
/// The wet bound is not computed here: the DSP measures `ir_gain * ||h||_1` AT
/// LOAD — the exact ℓ∞ operator norm of the wet path, approached by the
/// sign-matched input — and its own suite asserts it. This composes that
/// measured number up to the node's ceilings:
///
///     dry_max + wet_max * width_max * (ir_gain_max * ||h||_1)
///
/// The `width_max` factor is easy to miss and is real: the DSP's mid/side stage
/// computes `side * width` before the wet gain, so at 200 % a fully
/// out-of-phase wet pair (`mid = 0`) comes out at twice the amplitude.
///
/// IR-DEPENDENT BY CONSTRUCTION. `||h||_1` is a property of the impulse
/// response, so a registry row for this type carries the formula and the IR
/// reference; the number is only meaningful once an IR is named.
inline float convolution_reverb_worst_case_gain(const ImpulseResponse& ir,
                                                const IrPolicy& policy,
                                                double sample_rate,
                                                int max_block) {
    Engine probe;
    probe.prepare(sample_rate, max_block, 2);
    probe.set_normalize_mode(policy.normalize);
    probe.set_tail_trim_db(policy.tail_trim_db);
    probe.set_tail_fade_ms(policy.tail_fade_ms);
    probe.set_resample_taps_per_phase(policy.resample_taps_per_phase);
    probe.set_true_stereo(policy.true_stereo);

    std::vector<const float*> ptrs(ir.channels.size());
    for (std::size_t c = 0; c < ir.channels.size(); ++c) ptrs[c] = ir.channels[c].data();
    if (ir.channels.empty() ||
        !probe.load_impulse_response(ptrs.data(), static_cast<int>(ir.channels.size()),
                                     static_cast<int>(ir.channels[0].size()),
                                     ir.sample_rate))
        return 0.0f;

    probe.set_ir_gain_db(Engine::kIrGainDbMax);
    const double dry_max = 1.0;   // kDryPercent ceiling, as a linear gain
    const double wet_max = 1.0;   // kWetPercent ceiling
    const double width_max = Engine::kWidthPercentMax / 100.0;
    return static_cast<float>(dry_max + wet_max * width_max * probe.worst_case_gain());
}

/// The measured-space reverb as a lowerable custom node.
///
/// `ir` and `policy` are registration arguments; see the file note. `ir` is
/// copied once into a shared buffer, and every instance re-ingests it in
/// `prepare()` because the ingest resamples to the SESSION rate — which is not
/// known until then.
inline CustomNodeType make_convolution_reverb_node(ImpulseResponse ir,
                                                   IrPolicy policy = {}) {
    if (!valid_impulse_response(ir))
        throw std::invalid_argument("convolution IR must have finite, representable rate/length geometry and 1, 2, or 4 equal-length channels");
    auto shared = std::make_shared<ImpulseResponse>(std::move(ir));

    CustomNodeType t;
    t.type_id = kTypeId;
    t.version = 1;
    t.num_input_ports = 2;  // 0 = left, 1 = right (ONE logical stereo wire)
    t.num_output_ports = 2;
    t.default_name = "Convolution Reverb";
    t.lowerable = true;

    t.create = []() -> void* { return new Instance{}; };
    t.destroy = [](void* p) { delete static_cast<Instance*>(p); };
    t.prepare = [shared, policy](void* p, double sr, int max_block) {
        auto* s = static_cast<Instance*>(p);
        s->engine.prepare(sr, max_block, 2);
        // Policy before load: every one of these is read BY the load.
        s->engine.set_normalize_mode(policy.normalize);
        s->engine.set_tail_trim_db(policy.tail_trim_db);
        s->engine.set_tail_fade_ms(policy.tail_fade_ms);
        s->engine.set_resample_taps_per_phase(policy.resample_taps_per_phase);
        s->engine.set_true_stereo(policy.true_stereo);
        if (!shared->channels.empty()) {
            std::vector<const float*> ptrs(shared->channels.size());
            for (std::size_t c = 0; c < shared->channels.size(); ++c)
                ptrs[c] = shared->channels[c].data();
            const bool loaded = s->engine.load_impulse_response(
                ptrs.data(), static_cast<int>(shared->channels.size()),
                static_cast<int>(shared->channels[0].size()), shared->sample_rate);
            if (!loaded) throw std::runtime_error("validated convolution IR failed to load");
        }
    };
    t.reset = [](void* p) { static_cast<Instance*>(p)->engine.reset(); };

    t.baked_params.push_back({kIrGainDb, static_cast<float>(Engine::kIrGainDbMin),
                              static_cast<float>(Engine::kIrGainDbMax),
                              static_cast<float>(Engine::kIrGainDbDefault)});
    t.baked_params.push_back({kPredelayMs, static_cast<float>(Engine::kPredelayMsMin),
                              static_cast<float>(Engine::kPredelayMsMax),
                              static_cast<float>(Engine::kPredelayMsDefault)});
    t.baked_params.push_back({kWetPercent, 0.0f, 100.0f,
                              static_cast<float>(Engine::kWetPercentDefault)});
    t.baked_params.push_back({kDryPercent, 0.0f, 100.0f,
                              static_cast<float>(Engine::kDryPercentDefault)});
    t.baked_params.push_back({kWidthPercent, static_cast<float>(Engine::kWidthPercentMin),
                              static_cast<float>(Engine::kWidthPercentMax),
                              static_cast<float>(Engine::kWidthPercentDefault)});
    t.baked_params.push_back({kLowcutHz, static_cast<float>(Engine::kLowcutHzMin),
                              static_cast<float>(Engine::kLowcutHzMax),
                              static_cast<float>(Engine::kLowcutHzDefault)});
    t.baked_params.push_back({kHighcutHz, static_cast<float>(Engine::kHighcutHzMin),
                              static_cast<float>(Engine::kHighcutHzMax),
                              static_cast<float>(Engine::kHighcutHzDefault)});

    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& in, int n,
                                        const BakedParamView& params) {
        auto* s = static_cast<Instance*>(p);

        // Block rate, at offset 0. Not a shortcut — the engine hoists all four
        // mix gains out of its own sample loop, so a mid-block value could not
        // reach the audio even if this read it. See the file note.
        s->engine.set_ir_gain_db(params.value_at(kIrGainDb, 0));
        s->engine.set_predelay_ms(params.value_at(kPredelayMs, 0));
        s->engine.set_wet_percent(params.value_at(kWetPercent, 0));
        s->engine.set_dry_percent(params.value_at(kDryPercent, 0));
        s->engine.set_width_percent(params.value_at(kWidthPercent, 0));
        s->engine.set_lowcut_hz(params.value_at(kLowcutHz, 0));
        s->engine.set_highcut_hz(params.value_at(kHighcutHz, 0));

        const float* in_ptrs[2] = {in.channel_ptr(0), in.channel_ptr(1)};
        float* out_ptrs[2] = {out.channel_ptr(0), out.channel_ptr(1)};
        s->engine.process(in_ptrs, out_ptrs, n);
    };
    return t;
}

}  // namespace convolution

// ── The nonlin / gated ambience (designed spaces) ──────────────────────────
//
// One node type, four programs. The program is a PARAM and not a realization,
// which is the one classification here most worth arguing rather than
// asserting:
//
//   * It does not move `latency_samples()` — that is a constant 0 for every
//     program.
//   * The signal path is unchanged. All four programs run the same velvet tap
//     cloud through the same segment filters; what differs is the sequence of
//     tap GAINS, which is a coefficient set, not a topology. (The Reverse
//     program additionally mirrors the segment mapping, which is one flag on
//     the same filters — not a second filter bank.)
//   * The DSP was built for the switch to happen live. It regenerates into a
//     back tap bank and crossfades in over `kSwapFadeMs` precisely so a program
//     change is click-free and allocation-free at run time, and its suite
//     asserts that the swap produces no step larger than the signal already
//     contains.
//   * And it is the front panel. The four names ARE the machine; freezing the
//     program at registration would ship four nodes that a user cannot switch
//     between, for a lineage whose identity is that you turn that one knob.
//
// That is the same reading the FET member's ratio switch gets — a stepped
// front-panel control over an unchanged path with invariant latency — and the
// opposite of the diode bridge's feedback switch, which re-maps a measured
// static curve into a different function.
namespace nonlin_ambience {

inline constexpr const char* kTypeId = "space.nonlin_ambience";

// Topology — read once per block (see the file note on param rate).
inline constexpr state::ParamID kProgram = 1;        // stepped 0..3
inline constexpr state::ParamID kLengthMs = 2;       // ms
inline constexpr state::ParamID kPredelayMs = 3;     // ms
inline constexpr state::ParamID kDensityPct = 4;     // %
inline constexpr state::ParamID kDensityGrowth = 5;  // gamma, dimensionless
inline constexpr state::ParamID kGateHoldPct = 6;    // % — Gated only
inline constexpr state::ParamID kAttackPct = 7;      // % — Reverse / NonLin2

// Continuous — read per sample.
inline constexpr state::ParamID kDiffusion = 8;         // 0..0.85
inline constexpr state::ParamID kTone = 9;              // -1..+1
inline constexpr state::ParamID kHfDampHz = 10;         // Hz
inline constexpr state::ParamID kWidthPct = 11;         // %
inline constexpr state::ParamID kConverterAmount = 12;  // 0..1
inline constexpr state::ParamID kOutputGainDb = 13;     // dB
inline constexpr state::ParamID kMixPct = 14;           // %

using Engine = signal::NonlinAmbience;
namespace cal = signal::nonlin_ambience;

/// The program selector's step count. Four documented programs, injected as
/// 0..3 and rounded — the same shape as the FET member's ratio switch.
inline constexpr float kProgramSteps = 3.0f;

/// Node-level ceilings for the ranges the DSP does not publish as constants.
/// Each mirrors the module's baked-params table.
/// [design parameter] defaults as shown; ranges are the DSP's own.
inline constexpr float kLengthMsMin = static_cast<float>(cal::kMinLengthMs);
inline constexpr float kLengthMsDefault = 350.0f;
inline constexpr float kPredelayMsMax = 200.0f;
inline constexpr float kDensityPctMin = static_cast<float>(cal::kMinDensityPct);
inline constexpr float kGateHoldPctMin = 10.0f;
inline constexpr float kGateHoldPctMax = 95.0f;
inline constexpr float kAttackPctMin = 5.0f;
inline constexpr float kAttackPctMax = 98.0f;
inline constexpr float kOutputGainDbMax = 24.0f;

/// Forwards `value` to `apply` only when it differs from what was last
/// forwarded.
///
/// This is not a micro-optimisation, it is a CORRECTNESS guard, and it is here
/// rather than in the DSP because the trap is in a shared primitive that this
/// file must not edit. `SmoothedValue::set_target()` has no unchanged-value
/// guard: every call recomputes `increment = (target - current) / ramp_samples`
/// and restarts the ramp. So a node that calls a SmoothedValue-backed setter
/// once per sample with a HELD value converts the documented 20 ms LINEAR ramp
/// into an exponential approach with a 20 ms TIME CONSTANT — measured, it
/// reaches 0.3673 of its journey at the ramp length instead of 1.0, and never
/// reaches the target exactly at all. Four of the ambience's continuous setters
/// are SmoothedValue-backed (`width`, `converter_amount`, `output_gain`,
/// `mix`), so without this guard "mix = 0 %" never becomes the dry wire and
/// "width = 0 %" never becomes exactly mono.
///
/// `last` starts as NaN so the first sample always forwards (`x == NaN` is
/// false for every x, including NaN).
template <typename Fn>
inline void forward_if_changed(float& last, float value, Fn&& apply) {
    if (value == last) return;
    last = value;
    apply(value);
}

struct Instance {
    Engine engine;
    /// Last value forwarded for each continuous param. See
    /// `forward_if_changed`.
    float last_diffusion = std::numeric_limits<float>::quiet_NaN();
    float last_tone = std::numeric_limits<float>::quiet_NaN();
    float last_hf_damp = std::numeric_limits<float>::quiet_NaN();
    float last_width = std::numeric_limits<float>::quiet_NaN();
    float last_converter = std::numeric_limits<float>::quiet_NaN();
    float last_output_gain = std::numeric_limits<float>::quiet_NaN();
    float last_mix = std::numeric_limits<float>::quiet_NaN();
};

/// Worst-case linear gain for the Forge registry (series law 8).
///
/// The DSP ships the bound as a closed form of its own shipped constants —
/// `Pi_i(1 + 2*g_i) * G_L1`, an upper bound on the rendered response's
/// `sum|h[n]|` by Young's convolution inequality — and its suite renders the
/// impulse response and asserts the measured sum stays under it. This composes
/// that same closed form at THIS node's ceilings:
///
///   * `diffusion` at its maximum, not its default: 0.85 gives `(1+1.7)^2`
///     rather than the default's `(1+1.4)^2`.
///   * the converter stage engaged, which adds its DC blocker's L1 gain of
///     exactly 2.
///   * `output_gain_db` at +24, the post trim's ceiling.
///
/// `width_pct` and `mix_pct` contribute nothing: this module's width law is a
/// convex mid/side blend capped at 100 % (unlike the convolver's, which reaches
/// 200 %), and the dry/wet law is `(1-m, m)`.
inline float nonlin_ambience_worst_case_gain() {
    const double core = cal::worst_case_gain(cal::kDiffusionMax, /*converter_on=*/true);
    return static_cast<float>(core * std::pow(10.0, kOutputGainDbMax / 20.0));
}

/// The designed-space ambience as a lowerable custom node.
///
/// `seed` and `max_length_ms` are registration arguments; see the file note,
/// items 4 and 5. The seed selects which velvet realization this node IS — two
/// nodes differing only in seed are two different rooms — and law 2 forbids
/// automating it.
inline CustomNodeType make_nonlin_ambience_node(std::uint32_t seed = cal::kDefaultSeed,
                                                double max_length_ms = cal::kMaxLengthMs) {
    const double normalized_max_length_ms = std::isfinite(max_length_ms)
                                                ? std::max(cal::kMinLengthMs, max_length_ms)
                                                : cal::kMaxLengthMs;
    CustomNodeType t;
    t.type_id = kTypeId;
    t.version = 1;
    t.num_input_ports = 2;  // 0 = left, 1 = right (ONE logical stereo wire)
    t.num_output_ports = 2;
    t.default_name = "Nonlin Ambience";
    t.lowerable = true;

    t.create = []() -> void* { return new Instance{}; };
    t.destroy = [](void* p) { delete static_cast<Instance*>(p); };
    t.prepare = [seed, normalized_max_length_ms](void* p, double sr, int /*max_block*/) {
        auto* s = static_cast<Instance*>(p);
        // Seed before prepare: prepare builds the first tap table, and building
        // it twice to honour a seed set afterwards would be waste.
        s->engine.set_seed(seed);
        s->engine.prepare(sr, normalized_max_length_ms);
    };
    t.reset = [](void* p) { static_cast<Instance*>(p)->engine.reset(); };

    t.baked_params.push_back({kProgram, 0.0f, kProgramSteps, 0.0f});
    t.baked_params.push_back({kLengthMs, kLengthMsMin,
                              static_cast<float>(normalized_max_length_ms),
                              std::min(kLengthMsDefault,
                                       static_cast<float>(normalized_max_length_ms))});
    t.baked_params.push_back({kPredelayMs, 0.0f, kPredelayMsMax, 0.0f});
    t.baked_params.push_back({kDensityPct, kDensityPctMin, 100.0f,
                              static_cast<float>(cal::kDensityRefPct)});
    t.baked_params.push_back({kDensityGrowth, 0.0f, 2.0f,
                              static_cast<float>(cal::kGammaDefault)});
    t.baked_params.push_back({kGateHoldPct, kGateHoldPctMin, kGateHoldPctMax,
                              static_cast<float>(cal::kGateHold * 100.0)});
    t.baked_params.push_back({kAttackPct, kAttackPctMin, kAttackPctMax,
                              static_cast<float>(cal::kRevRise * 100.0)});
    t.baked_params.push_back({kDiffusion, 0.0f, static_cast<float>(cal::kDiffusionMax),
                              static_cast<float>(cal::kDiffusionDefault)});
    t.baked_params.push_back({kTone, -1.0f, 1.0f, 0.0f});
    t.baked_params.push_back({kHfDampHz, 1000.0f, 18000.0f,
                              static_cast<float>(cal::kFcDark)});
    t.baked_params.push_back({kWidthPct, 0.0f, 100.0f, 100.0f});
    t.baked_params.push_back({kConverterAmount, 0.0f, 1.0f, 0.0f});
    t.baked_params.push_back({kOutputGainDb, -kOutputGainDbMax, kOutputGainDbMax, 0.0f});
    t.baked_params.push_back({kMixPct, 0.0f, 100.0f, 100.0f});  // send-style default

    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& in, int n,
                                        const BakedParamView& params) {
        auto* s = static_cast<Instance*>(p);

        // Topology is one atomic snapshot per block. Several simultaneous
        // events must regenerate the inactive tap bank once, not once per
        // parameter (seven full 8000-tap/channel walks in one callback).
        const float program_value = params.value_at(kProgram, 0);
        const int program = std::isfinite(program_value)
                                ? std::clamp(static_cast<int>(std::lround(program_value)), 0,
                                             static_cast<int>(kProgramSteps))
                                : static_cast<int>(s->engine.program());
        s->engine.request_topology(
            static_cast<signal::NonlinProgram>(program), params.value_at(kLengthMs, 0),
            params.value_at(kPredelayMs, 0), params.value_at(kDensityPct, 0),
            params.value_at(kDensityGrowth, 0), params.value_at(kGateHoldPct, 0),
            params.value_at(kAttackPct, 0));

        const float* in_left = in.channel_ptr(0);
        const float* in_right = in.channel_ptr(1);
        float* out_left = out.channel_ptr(0);
        float* out_right = out.channel_ptr(1);

        // Continuous: per sample, but only forwarded on CHANGE — see
        // `forward_if_changed`, which is load-bearing rather than thrifty. Each
        // setter is otherwise a clamp, a store, and at most a handful of `exp`
        // calls for the segment corners.
        for (int k = 0; k < n; ++k) {
            const auto offset = static_cast<std::int32_t>(k);
            forward_if_changed(s->last_diffusion, params.value_at(kDiffusion, offset),
                               [s](float v) { s->engine.set_diffusion(v); });
            forward_if_changed(s->last_tone, params.value_at(kTone, offset),
                               [s](float v) { s->engine.set_tone(v); });
            forward_if_changed(s->last_hf_damp, params.value_at(kHfDampHz, offset),
                               [s](float v) { s->engine.set_hf_damp_hz(v); });
            forward_if_changed(s->last_width, params.value_at(kWidthPct, offset),
                               [s](float v) { s->engine.set_width_pct(v); });
            forward_if_changed(s->last_converter, params.value_at(kConverterAmount, offset),
                               [s](float v) { s->engine.set_converter_amount(v); });
            forward_if_changed(s->last_output_gain, params.value_at(kOutputGainDb, offset),
                               [s](float v) { s->engine.set_output_gain_db(v); });
            forward_if_changed(s->last_mix, params.value_at(kMixPct, offset),
                               [s](float v) { s->engine.set_mix_pct(v); });

            float left = in_left[static_cast<std::size_t>(k)];
            float right = in_right[static_cast<std::size_t>(k)];
            s->engine.process_sample(left, right);
            out_left[static_cast<std::size_t>(k)] = left;
            out_right[static_cast<std::size_t>(k)] = right;
        }
    };
    return t;
}

}  // namespace nonlin_ambience

namespace cabinet {

inline constexpr const char* kTypeId = "space.speaker_cabinet";
inline constexpr state::ParamID kDriver = 1;
inline constexpr state::ParamID kBox = 2;
inline constexpr state::ParamID kVolumeL = 3;
inline constexpr state::ParamID kResonanceTrimSt = 4;
inline constexpr state::ParamID kQ = 5;
inline constexpr state::ParamID kBreakupPct = 6;
inline constexpr state::ParamID kTrebleHz = 7;
inline constexpr state::ParamID kDriveDb = 8;
inline constexpr state::ParamID kCompressionPct = 9;
inline constexpr state::ParamID kMicDistanceCm = 10;
inline constexpr state::ParamID kMicPositionPct = 11;
inline constexpr state::ParamID kMicAxisDeg = 12;
inline constexpr state::ParamID kDiffractionPct = 13;
inline constexpr state::ParamID kOutputTrimDb = 14;

using Engine = signal::SpeakerModel;
struct Instance {
    Engine engine;
    std::array<float, 14> last_params = [] {
        std::array<float, 14> values{};
        values.fill(std::numeric_limits<float>::quiet_NaN());
        return values;
    }();
};

inline float speaker_cabinet_worst_case_gain() {
    return static_cast<float>(Engine{}.worst_case_gain() *
                              signal::units::db_to_linear(Engine::kOutputTrimDbMax));
}

inline CustomNodeType make_speaker_cabinet_node() {
    CustomNodeType t;
    t.type_id = kTypeId;
    t.version = 1;
    t.num_input_ports = 1;
    t.num_output_ports = 1;
    t.default_name = "Speaker Cabinet";
    t.lowerable = true;
    t.create = []() -> void* { return new Instance{}; };
    t.destroy = [](void* p) { delete static_cast<Instance*>(p); };
    t.prepare = [](void* p, double sr, int) { static_cast<Instance*>(p)->engine.prepare(sr); };
    t.reset = [](void* p) { static_cast<Instance*>(p)->engine.reset(); };
    t.baked_params.push_back(
        {kDriver, 0.0f, static_cast<float>(Engine::kArchetypeCount - 1), 0.0f});
    t.baked_params.push_back({kBox, 0.0f, 1.0f, 0.0f});
    t.baked_params.push_back({kVolumeL, static_cast<float>(Engine::kBoxVolumeLMin),
                              static_cast<float>(Engine::kBoxVolumeLMax),
                              static_cast<float>(Engine::kBoxVolumeLDefault)});
    t.baked_params.push_back({kResonanceTrimSt,
                              static_cast<float>(Engine::kResonanceTrimSemitonesMin),
                              static_cast<float>(Engine::kResonanceTrimSemitonesMax), 0.0f});
    t.baked_params.push_back({kQ, 0.0f, static_cast<float>(Engine::kQResonanceMax), 0.0f});
    t.baked_params.push_back(
        {kBreakupPct, 0.0f, 100.0f, static_cast<float>(Engine::kConeBreakupAmountDefault)});
    t.baked_params.push_back({kTrebleHz, static_cast<float>(Engine::kTrebleRolloffHzMin),
                              static_cast<float>(Engine::kTrebleRolloffHzMax),
                              static_cast<float>(Engine::kTrebleRolloffHzDefault)});
    t.baked_params.push_back({kDriveDb, static_cast<float>(Engine::kDriveDbMin),
                              static_cast<float>(Engine::kDriveDbMax),
                              static_cast<float>(Engine::kDriveDbDefault)});
    t.baked_params.push_back(
        {kCompressionPct, 0.0f, 100.0f, static_cast<float>(Engine::kCompressionAmountDefault)});
    t.baked_params.push_back({kMicDistanceCm, static_cast<float>(Engine::kMicDistanceCmMin),
                              static_cast<float>(Engine::kMicDistanceCmMax),
                              static_cast<float>(Engine::kMicDistanceCmDefault)});
    t.baked_params.push_back(
        {kMicPositionPct, 0.0f, 100.0f, static_cast<float>(Engine::kMicPositionPctDefault)});
    t.baked_params.push_back({kMicAxisDeg, static_cast<float>(Engine::kMicAxisDegMin),
                              static_cast<float>(Engine::kMicAxisDegMax), 0.0f});
    t.baked_params.push_back(
        {kDiffractionPct, 0.0f, 100.0f, static_cast<float>(Engine::kDiffractionAmountDefault)});
    t.baked_params.push_back({kOutputTrimDb, static_cast<float>(Engine::kOutputTrimDbMin),
                              static_cast<float>(Engine::kOutputTrimDbMax), 0.0f});
    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& in, int n,
                                        const BakedParamView& params) {
        auto& e = static_cast<Instance*>(p)->engine;
        auto& last = static_cast<Instance*>(p)->last_params;
        for (int i = 0; i < n; ++i) {
            const auto o = static_cast<std::int32_t>(i);
            auto forward = [&](std::size_t slot, state::ParamID id, auto&& setter) {
                const float value = params.value_at(id, o);
                if (std::isfinite(value) && value != last[slot]) {
                    last[slot] = value;
                    setter(value);
                }
            };
            forward(0, kDriver,
                    [&](float v) { e.set_driver_archetype(static_cast<int>(std::lround(v))); });
            forward(1, kBox, [&](float v) {
                e.set_box_type(v >= 0.5f ? signal::SpeakerBoxType::open_back
                                         : signal::SpeakerBoxType::sealed);
            });
            forward(2, kVolumeL, [&](float v) { e.set_box_volume_l(v); });
            forward(3, kResonanceTrimSt, [&](float v) { e.set_resonance_trim_semitones(v); });
            forward(4, kQ, [&](float v) { e.set_q_resonance(v); });
            forward(5, kBreakupPct, [&](float v) { e.set_cone_breakup_amount(v); });
            forward(6, kTrebleHz, [&](float v) { e.set_treble_rolloff_hz(v); });
            forward(7, kDriveDb, [&](float v) { e.set_drive_db(v); });
            forward(8, kCompressionPct, [&](float v) { e.set_compression_amount(v); });
            forward(9, kMicDistanceCm, [&](float v) { e.set_mic_distance_cm(v); });
            forward(10, kMicPositionPct, [&](float v) { e.set_mic_position_pct(v); });
            forward(11, kMicAxisDeg, [&](float v) { e.set_mic_axis_deg(v); });
            forward(12, kDiffractionPct, [&](float v) { e.set_diffraction_amount(v); });
            forward(13, kOutputTrimDb, [&](float v) { e.set_output_trim_db(v); });
            out.channel_ptr(0)[i] = e.process(in.channel_ptr(0)[i]);
        }
    };
    return t;
}

/// Compatibility spelling retained for the original public design contract.
/// Both names return the same stable node type and parameter surface.
inline CustomNodeType make_speaker_emulation_node() {
    return make_speaker_cabinet_node();
}

} // namespace cabinet

}  // namespace pulp::host::space
