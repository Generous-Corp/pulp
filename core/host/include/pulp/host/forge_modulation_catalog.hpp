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
// Header-only, like its companion: every wrapped primitive is a header-only
// template, so including this adds no link dependency to pulp-host.
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
#include <pulp/host/signal_graph.hpp>

#include <pulp/signal/envelope.hpp>
#include <pulp/signal/lfo.hpp>
#include <pulp/signal/lpg.hpp>
#include <pulp/signal/mod_tools.hpp>
#include <pulp/signal/trigger.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

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
inline signal::Lfo::Wave mod_lfo_wave_from_id(float id) noexcept {
    if (!std::isfinite(id)) id = 0.0f;
    const int index = static_cast<int>(std::lround(std::clamp(id, 0.0f, 6.0f)));
    return static_cast<signal::Lfo::Wave>(index);
}

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

inline CustomNodeType make_mod_lfo_node() {
    CustomNodeType t;
    t.type_id = kModLfoTypeId;
    t.version = 1;
    t.num_input_ports = 0;
    t.num_output_ports = 1;
    t.default_name = "Mod LFO";
    t.lowerable = true;
    t.create = []() -> void* { return new ModLfoInstance{}; };
    t.destroy = [](void* p) { delete static_cast<ModLfoInstance*>(p); };
    t.prepare = [](void* p, double sr, int /*max_block*/) {
        auto* s = static_cast<ModLfoInstance*>(p);
        s->lfo.prepare(sr);
        s->lfo.set_random_segments(signal::Lfo::kDefaultRandomSegments);
        s->delay_ms = -1.0;
        s->fade_in_ms = -1.0;
        s->fade_out_ms = -1.0;
        s->fade_curve = -1;
        s->repeat_count = -1;
    };
    t.reset = [](void* p) {
        auto* s = static_cast<ModLfoInstance*>(p);
        s->lfo.reset();
        s->delay_ms = -1.0;
        s->fade_in_ms = -1.0;
        s->fade_out_ms = -1.0;
        s->fade_curve = -1;
        s->repeat_count = -1;
    };
    t.baked_params.push_back({kModLfoRateHz, 0.001f, 2000.0f, 2.0f});
    t.baked_params.push_back({kModLfoDepth, 0.0f, 1.0f, 1.0f});
    t.baked_params.push_back({kModLfoWave, 0.0f, 6.0f, 0.0f});
    t.baked_params.push_back({kModLfoPulseWidth, 0.05f, 0.95f, 0.5f});
    t.baked_params.push_back({kModLfoRandomBlend, 0.0f, 1.0f, 0.0f});
    t.baked_params.push_back({kModLfoDelayMs, 0.0f, 5000.0f, 0.0f});
    t.baked_params.push_back({kModLfoFadeInMs, 0.0f, 5000.0f, 0.0f});
    t.baked_params.push_back({kModLfoShapeMorph, 0.0f, 3.0f, 0.0f});
    t.baked_params.push_back({kModLfoMorphEnabled, 0.0f, 1.0f, 0.0f});
    t.baked_params.push_back({kModLfoTriangleBias, -1.0f, 1.0f, 0.0f});
    t.baked_params.push_back(
        {kModLfoRandomSegments, 1.0f,
         static_cast<float>(signal::Lfo::kMaxRandomSegments),
         static_cast<float>(signal::Lfo::kDefaultRandomSegments)});
    t.baked_params.push_back({kModLfoPhaseDegrees, 0.0f, 360.0f, 0.0f});
    t.baked_params.push_back({kModLfoFadeOutMs, 0.0f, 5000.0f, 0.0f});
    t.baked_params.push_back({kModLfoFadeQuadratic, 0.0f, 1.0f, 0.0f});
    t.baked_params.push_back(
        {kModLfoRepeatCount, 0.0f,
         static_cast<float>(signal::Lfo::kMaxRepeatCount), 0.0f});
    t.process_instance_baked_param =
        [](void* p, audio::BufferView<float>& out,
           const audio::BufferView<const float>& /*in*/, int n,
           const BakedParamView& params) {
            auto* s = static_cast<ModLfoInstance*>(p);
            float* o = out.channel_ptr(0);

            for (int k = 0; k < n; ++k) {
                const auto off = static_cast<std::int32_t>(k);
                const auto delay_ms =
                    static_cast<double>(params.value_at(kModLfoDelayMs, off));
                if (delay_ms != s->delay_ms) {
                    s->lfo.set_delay_ms(delay_ms);
                    s->delay_ms = delay_ms;
                }
                const auto fade_ms =
                    static_cast<double>(params.value_at(kModLfoFadeInMs, off));
                if (fade_ms != s->fade_in_ms) {
                    s->lfo.set_fade_in_ms(fade_ms);
                    s->fade_in_ms = fade_ms;
                }
                const auto fade_out_ms =
                    static_cast<double>(params.value_at(kModLfoFadeOutMs, off));
                if (fade_out_ms != s->fade_out_ms) {
                    s->lfo.set_fade_out_ms(fade_out_ms, fade_out_ms > 0.0);
                    s->fade_out_ms = fade_out_ms;
                }
                const int fade_curve =
                    params.value_at(kModLfoFadeQuadratic, off) >= 0.5f ? 1 : 0;
                if (fade_curve != s->fade_curve) {
                    s->lfo.set_fade_curve(
                        fade_curve != 0 ? signal::Lfo::FadeCurve::quadratic
                                        : signal::Lfo::FadeCurve::linear);
                    s->fade_curve = fade_curve;
                }
                const int repeat_count = static_cast<int>(
                    std::lround(params.value_at(kModLfoRepeatCount, off)));
                if (repeat_count != s->repeat_count) {
                    s->lfo.set_repeat_count(repeat_count);
                    s->repeat_count = repeat_count;
                }
                s->lfo.set_rate_hz(
                    static_cast<double>(params.value_at(kModLfoRateHz, off)));
                s->lfo.set_wave(
                    mod_lfo_wave_from_id(params.value_at(kModLfoWave, off)));
                s->lfo.set_shape_morph(
                    params.value_at(kModLfoShapeMorph, off));
                s->lfo.set_shape_morph_enabled(
                    params.value_at(kModLfoMorphEnabled, off) >= 0.5f);
                s->lfo.set_pulse_width(
                    params.value_at(kModLfoPulseWidth, off));
                s->lfo.set_tri_bias(
                    params.value_at(kModLfoTriangleBias, off));
                s->lfo.set_random_blend(
                    params.value_at(kModLfoRandomBlend, off));
                s->lfo.set_random_segments(static_cast<int>(std::lround(
                    params.value_at(kModLfoRandomSegments, off))));
                s->lfo.set_phase_offset(
                    params.value_at(kModLfoPhaseDegrees, off) / 360.0f);
                const float depth = params.value_at(kModLfoDepth, off);
                o[static_cast<std::size_t>(k)] =
                    std::clamp(0.5f + 0.5f * depth * s->lfo.next(), 0.0f, 1.0f);
            }
        };
    return t;
}

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

inline CustomNodeType make_mod_lpg_node() {
    CustomNodeType t;
    t.type_id = kModLpgTypeId;
    t.version = 1;
    t.num_input_ports = 2;   // 0 = signal, 1 = control CV
    t.num_output_ports = 1;
    t.default_name = "Low-Pass Gate";
    t.lowerable = true;
    t.create = []() -> void* { return new ModLpgInstance{}; };
    t.destroy = [](void* p) { delete static_cast<ModLpgInstance*>(p); };
    t.prepare = [](void* p, double sr, int /*max_block*/) {
        auto* s = static_cast<ModLpgInstance*>(p);
        s->lpg.prepare(static_cast<float>(sr));
        s->edge.prepare(sr);
        s->edge.set_refractory_ms(2.0);
        s->edge.set_threshold(kDefaultModLpgStrikeThreshold);
        s->last_decay_ms = -1.0f;
        s->last_rise_ms = -1.0f;
        s->last_darkness_hz = -1.0f;
        s->last_brightness_hz = -1.0f;
        s->last_refractory_ms = -1.0f;
        s->strike_peak = 0.0f;
        s->striking = false;
    };
    t.reset = [](void* p) {
        auto* s = static_cast<ModLpgInstance*>(p);
        s->lpg.reset();
        s->edge.reset();
        s->strike_peak = 0.0f;
        s->striking = false;
    };
    t.baked_params.push_back({kModLpgDecayMs, 20.0f, 2000.0f, 150.0f});
    t.baked_params.push_back({kModLpgColour, 0.0f, 1.0f, 0.5f});
    t.baked_params.push_back({kModLpgDroop, 0.0f, 0.95f, 0.5f});
    t.baked_params.push_back({kModLpgBrightnessHz, 500.0f, 12000.0f, 12000.0f});
    t.baked_params.push_back({kModLpgStruck, 0.0f, 1.0f, 0.0f});
    t.baked_params.push_back(
        {kModLpgRiseMs, 0.05f, 100.0f,
         static_cast<float>(signal::Lpg::kRiseMs)});
    t.baked_params.push_back(
        {kModLpgDarknessHz, signal::Lpg::kMinFcHz, 500.0f,
         signal::Lpg::kDefaultFcMinHz});
    t.baked_params.push_back(
        {kModLpgStrikeThreshold, 0.01f, 0.99f,
         kDefaultModLpgStrikeThreshold});
    t.baked_params.push_back({kModLpgRefractoryMs, 0.0f, 100.0f, 2.0f});
    t.process_instance_baked_param =
        [](void* p, audio::BufferView<float>& out,
           const audio::BufferView<const float>& in, int n,
           const BakedParamView& params) {
            auto* s = static_cast<ModLpgInstance*>(p);
            const float* sig = in.channel_ptr(0);
            const float* cv = in.channel_ptr(1);
            float* o = out.channel_ptr(0);

            for (int k = 0; k < n; ++k) {
                const auto off = static_cast<std::int32_t>(k);
                const float darkness =
                    params.value_at(kModLpgDarknessHz, off);
                const float brightness =
                    params.value_at(kModLpgBrightnessHz, off);
                if (darkness != s->last_darkness_hz
                    || brightness != s->last_brightness_hz) {
                    s->lpg.set_range_hz(darkness, brightness);
                    s->last_darkness_hz = darkness;
                    s->last_brightness_hz = brightness;
                }
                const float threshold =
                    params.value_at(kModLpgStrikeThreshold, off);
                s->edge.set_threshold(threshold);
                const float refractory_ms =
                    params.value_at(kModLpgRefractoryMs, off);
                if (refractory_ms != s->last_refractory_ms) {
                    s->edge.set_refractory_ms(refractory_ms);
                    s->last_refractory_ms = refractory_ms;
                }
                const float decay_ms =
                    params.value_at(kModLpgDecayMs, off);
                if (decay_ms != s->last_decay_ms) {
                    s->lpg.set_decay_ms(static_cast<double>(decay_ms));
                    s->last_decay_ms = decay_ms;
                }
                const float rise_ms = params.value_at(kModLpgRiseMs, off);
                if (rise_ms != s->last_rise_ms) {
                    s->lpg.set_rise_ms(static_cast<double>(rise_ms));
                    s->last_rise_ms = rise_ms;
                }
                s->lpg.set_colour(params.value_at(kModLpgColour, off));
                s->lpg.set_droop(params.value_at(kModLpgDroop, off));

                const float control = std::clamp(cv[static_cast<std::size_t>(k)], 0.0f, 1.0f);
                const bool struck =
                    params.value_at(kModLpgStruck, off) >= 0.5f;
                if (struck) {
                    s->lpg.set_gate(0.0f);
                    if (s->edge.process_signal(control)) {
                        s->striking = true;
                        s->strike_peak = control;
                        s->lpg.strike(signal::Lpg::velocity_to_strike(control));
                    } else if (s->striking) {
                        if (control > s->strike_peak) {
                            // Still rising: re-drive toward the higher level.
                            // strike() restarts the pulse, so the cell keeps
                            // charging instead of coasting from the threshold.
                            s->strike_peak = control;
                            s->lpg.strike(signal::Lpg::velocity_to_strike(control));
                        } else if (control < threshold) {
                            s->striking = false;
                        }
                    }
                } else {
                    s->lpg.set_gate(control);
                    s->striking = false;
                }
                o[static_cast<std::size_t>(k)] = s->lpg.process(sig[static_cast<std::size_t>(k)]);
            }
        };
    return t;
}

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

inline CustomNodeType make_mod_slew_node() {
    CustomNodeType t;
    t.type_id = kModSlewTypeId;
    t.version = 1;
    t.num_input_ports = 1;
    t.num_output_ports = 1;
    t.default_name = "Slew";
    t.lowerable = true;
    t.create = []() -> void* { return new ModSlewInstance{}; };
    t.destroy = [](void* p) { delete static_cast<ModSlewInstance*>(p); };
    t.prepare = [](void* p, double sr, int /*max_block*/) {
        auto* s = static_cast<ModSlewInstance*>(p);
        s->slew.prepare(static_cast<float>(sr));
        s->last_rise_ms = -1.0f;
        s->last_fall_ms = -1.0f;
        s->last_curved = -1;
    };
    t.reset = [](void* p) {
        auto* s = static_cast<ModSlewInstance*>(p);
        // Reset to the CV convention's NEUTRAL, not to zero. A control signal is
        // unipolar [0, 1] with 0.5 as its resting value, so resetting to 0 makes
        // every render open with a rise-time ramp from silence up to whatever
        // the incoming control actually is — a start-of-playback modulation
        // transient in any `... -> slew -> CV port` chain.
        s->slew.reset(0.5f);
        s->primed = false;
        s->last_rise_ms = -1.0f;
        s->last_fall_ms = -1.0f;
        s->last_curved = -1;
    };
    t.baked_params.push_back({kModSlewRiseMs, 0.0f, 2000.0f, 20.0f});
    t.baked_params.push_back({kModSlewFallMs, 0.0f, 2000.0f, 20.0f});
    t.baked_params.push_back({kModSlewCurved, 0.0f, 1.0f, 0.0f});
    t.process_instance_baked_param =
        [](void* p, audio::BufferView<float>& out,
           const audio::BufferView<const float>& in, int n,
           const BakedParamView& params) {
            auto* s = static_cast<ModSlewInstance*>(p);
            const float* cv = in.channel_ptr(0);
            float* o = out.channel_ptr(0);
            for (int k = 0; k < n; ++k) {
                const auto off = static_cast<std::int32_t>(k);
                const float rise_ms = params.value_at(kModSlewRiseMs, off);
                const float fall_ms = params.value_at(kModSlewFallMs, off);
                if (rise_ms != s->last_rise_ms
                    || fall_ms != s->last_fall_ms) {
                    s->slew.set_times_ms(rise_ms, fall_ms);
                    s->last_rise_ms = rise_ms;
                    s->last_fall_ms = fall_ms;
                }
                const int curved =
                    params.value_at(kModSlewCurved, off) >= 0.5f ? 1 : 0;
                if (curved != s->last_curved) {
                    s->slew.set_mode(
                        curved != 0 ? signal::SlewLimiter::Mode::exponential
                                    : signal::SlewLimiter::Mode::linear);
                    s->last_curved = curved;
                }
                const float control = cv[static_cast<std::size_t>(k)];
                if (!s->primed) {
                    s->slew.reset(control);
                    s->primed = true;
                }
                o[static_cast<std::size_t>(k)] = s->slew.process(control);
            }
        };
    return t;
}

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

inline CustomNodeType make_mod_transient_node() {
    CustomNodeType t;
    t.type_id = kModTransientTypeId;
    t.version = 1;
    t.num_input_ports = 1;
    t.num_output_ports = 1;
    t.default_name = "Transient";
    t.lowerable = true;
    t.create = []() -> void* { return new ModTransientInstance{}; };
    t.destroy = [](void* p) { delete static_cast<ModTransientInstance*>(p); };
    t.prepare = [](void* p, double sr, int /*max_block*/) {
        auto* s = static_cast<ModTransientInstance*>(p);
        s->detector.prepare(static_cast<float>(sr));
        s->last_fast_ms = -1.0f;
        s->last_slow_ms = -1.0f;
    };
    t.reset = [](void* p) { static_cast<ModTransientInstance*>(p)->detector.reset(); };
    t.baked_params.push_back({kModTransientFastMs, 0.5f, 20.0f, 2.0f});
    t.baked_params.push_back({kModTransientSlowMs, 10.0f, 500.0f, 40.0f});
    t.baked_params.push_back({kModTransientSensitivity, 0.1f, 8.0f, 1.0f});
    t.baked_params.push_back({kModTransientInvert, 0.0f, 1.0f, 0.0f});
    t.process_instance_baked_param =
        [](void* p, audio::BufferView<float>& out,
           const audio::BufferView<const float>& in, int n,
           const BakedParamView& params) {
            auto* s = static_cast<ModTransientInstance*>(p);
            const float* sig = in.channel_ptr(0);
            float* o = out.channel_ptr(0);

            for (int k = 0; k < n; ++k) {
                const auto off = static_cast<std::int32_t>(k);
                const float fast_ms =
                    params.value_at(kModTransientFastMs, off);
                if (fast_ms != s->last_fast_ms) {
                    s->detector.set_fast_ms(fast_ms);
                    s->last_fast_ms = fast_ms;
                }
                const float slow_ms =
                    params.value_at(kModTransientSlowMs, off);
                if (slow_ms != s->last_slow_ms) {
                    s->detector.set_slow_ms(slow_ms);
                    s->last_slow_ms = slow_ms;
                }
                const float sensitivity =
                    params.value_at(kModTransientSensitivity, off);
                const float detected =
                    std::clamp(s->detector.process(sig[static_cast<std::size_t>(k)]) * sensitivity,
                               0.0f, 1.0f);
                o[static_cast<std::size_t>(k)] =
                    params.value_at(kModTransientInvert, off) >= 0.5f
                        ? 1.0f - detected
                        : detected;
            }
        };
    return t;
}

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

inline CustomNodeType make_mod_env_node() {
    CustomNodeType t;
    t.type_id = kModEnvTypeId;
    t.version = 1;
    t.num_input_ports = 1;
    t.num_output_ports = 1;
    t.default_name = "Mod Envelope";
    t.lowerable = true;
    t.create = []() -> void* { return new ModEnvInstance{}; };
    t.destroy = [](void* p) { delete static_cast<ModEnvInstance*>(p); };
    t.prepare = [](void* p, double sr, int /*max_block*/) {
        auto* s = static_cast<ModEnvInstance*>(p);
        s->envelope.prepare(sr);
        s->edge.prepare(sr);
        s->edge.set_refractory_ms(2.0);
        s->last_delay_ms = -1.0f;
        s->last_attack_ms = -1.0f;
        s->last_hold_ms = -1.0f;
        s->last_decay_ms = -1.0f;
        s->last_refractory_ms = -1.0f;
        s->last_loop = false;
        s->last_loop_count = -1;
    };
    t.reset = [](void* p) {
        auto* s = static_cast<ModEnvInstance*>(p);
        s->envelope.reset();
        s->edge.reset();
    };
    t.baked_params.push_back({kModEnvAttackMs, 0.1f, 500.0f, 2.0f});
    t.baked_params.push_back({kModEnvHoldMs, 0.0f, 500.0f, 0.0f});
    t.baked_params.push_back({kModEnvDecayMs, 1.0f, 2000.0f, 150.0f});
    t.baked_params.push_back({kModEnvCurve, -1.0f, 1.0f, 0.0f});
    t.baked_params.push_back({kModEnvThreshold, 0.05f, 0.95f, 0.3f});
    t.baked_params.push_back({kModEnvDelayMs, 0.0f, 5000.0f, 0.0f});
    t.baked_params.push_back({kModEnvDepth, 0.0f, 1.0f, 1.0f});
    t.baked_params.push_back({kModEnvLoop, 0.0f, 1.0f, 0.0f});
    t.baked_params.push_back({kModEnvLoopCount, 0.0f, 128.0f, 0.0f});
    t.baked_params.push_back({kModEnvRefractoryMs, 0.0f, 100.0f, 2.0f});
    t.baked_params.push_back({kModEnvVelocitySensitive, 0.0f, 1.0f, 0.0f});
    t.baked_params.push_back({kModEnvIndependentCurves, 0.0f, 1.0f, 0.0f});
    t.baked_params.push_back({kModEnvAttackCurve, -1.0f, 1.0f, 0.0f});
    t.baked_params.push_back({kModEnvDecayCurve, -1.0f, 1.0f, 0.0f});
    t.process_instance_baked_param =
        [](void* p, audio::BufferView<float>& out,
           const audio::BufferView<const float>& in, int n,
           const BakedParamView& params) {
            auto* s = static_cast<ModEnvInstance*>(p);
            const float* cv = in.channel_ptr(0);
            float* o = out.channel_ptr(0);

            for (int k = 0; k < n; ++k) {
                const auto off = static_cast<std::int32_t>(k);
                const float delay_ms =
                    params.value_at(kModEnvDelayMs, off);
                if (delay_ms != s->last_delay_ms) {
                    s->envelope.set_delay_ms(static_cast<double>(delay_ms));
                    s->last_delay_ms = delay_ms;
                }
                const bool loop = params.value_at(kModEnvLoop, off) >= 0.5f;
                const int loop_count = static_cast<int>(
                    std::lround(params.value_at(kModEnvLoopCount, off)));
                if (loop != s->last_loop
                    || loop_count != s->last_loop_count) {
                    s->envelope.set_loop(loop, loop_count);
                    s->last_loop = loop;
                    s->last_loop_count = loop_count;
                }
                const float refractory_ms =
                    params.value_at(kModEnvRefractoryMs, off);
                if (refractory_ms != s->last_refractory_ms) {
                    s->edge.set_refractory_ms(refractory_ms);
                    s->last_refractory_ms = refractory_ms;
                }
                const float attack_ms =
                    params.value_at(kModEnvAttackMs, off);
                if (attack_ms != s->last_attack_ms) {
                    s->envelope.set_attack_ms(
                        static_cast<double>(attack_ms));
                    s->last_attack_ms = attack_ms;
                }
                const float hold_ms = params.value_at(kModEnvHoldMs, off);
                if (hold_ms != s->last_hold_ms) {
                    s->envelope.set_hold_ms(static_cast<double>(hold_ms));
                    s->last_hold_ms = hold_ms;
                }
                const float decay_ms =
                    params.value_at(kModEnvDecayMs, off);
                if (decay_ms != s->last_decay_ms) {
                    s->envelope.set_decay_ms(
                        static_cast<double>(decay_ms));
                    s->last_decay_ms = decay_ms;
                }
                const float curve = params.value_at(kModEnvCurve, off);
                const bool independent_curves =
                    params.value_at(kModEnvIndependentCurves, off) >= 0.5f;
                s->envelope.set_attack_curve(
                    independent_curves
                        ? params.value_at(kModEnvAttackCurve, off)
                        : curve);
                s->envelope.set_decay_curve(
                    independent_curves
                        ? params.value_at(kModEnvDecayCurve, off)
                        : curve);
                s->edge.set_threshold(
                    params.value_at(kModEnvThreshold, off));

                if (s->edge.process_signal(cv[static_cast<std::size_t>(k)])) {
                    const float velocity =
                        params.value_at(kModEnvVelocitySensitive, off) >= 0.5f
                            ? std::clamp(cv[static_cast<std::size_t>(k)], 0.0f, 1.0f)
                            : 1.0f;
                    s->envelope.trigger(velocity);
                }
                const float depth = params.value_at(kModEnvDepth, off);
                o[static_cast<std::size_t>(k)] =
                    std::clamp(depth * s->envelope.next(), 0.0f, 1.0f);
            }
        };
    return t;
}

inline ForgeNodeDescriptor mod_lfo_descriptor() {
    return {
        "mod_lfo", "Modulation LFO",
        "A unipolar modulation oscillator with shaped, random, delayed, faded, and repeating cycles.",
        {}, {{"default", kModLfoTypeId}},
        {
            {"rate_hz", kModLfoRateHz, "Rate", "Hz", "Sets the oscillator rate.",
             ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
            {"depth", kModLfoDepth, "Depth", "%", "Scales the modulation output.",
             ForgeParamKind::continuous, ForgeParamCurve::linear},
            {"wave", kModLfoWave, "Wave", "", "Selects the base waveform.",
             ForgeParamKind::stepped, ForgeParamCurve::linear,
             {{"sine", "Sine", 0}, {"triangle", "Triangle", 1}, {"saw_up", "Saw Up", 2},
              {"saw_down", "Saw Down", 3}, {"square", "Square", 4},
              {"sample_hold", "Sample and Hold", 5}, {"smooth_random", "Smooth Random", 6}}},
            {"pulse_width", kModLfoPulseWidth, "Pulse Width", "%", "Sets square-wave duty.",
             ForgeParamKind::continuous, ForgeParamCurve::linear},
            {"random_blend", kModLfoRandomBlend, "Random Blend", "%", "Blends random motion into the cycle.",
             ForgeParamKind::continuous, ForgeParamCurve::linear},
            {"delay_ms", kModLfoDelayMs, "Delay", "ms", "Delays modulation after trigger.",
             ForgeParamKind::continuous, ForgeParamCurve::linear},
            {"fade_in_ms", kModLfoFadeInMs, "Fade In", "ms", "Fades modulation in.",
             ForgeParamKind::continuous, ForgeParamCurve::linear},
            {"shape_morph", kModLfoShapeMorph, "Shape Morph", "", "Moves continuously through adjacent waveform shapes.",
             ForgeParamKind::continuous, ForgeParamCurve::linear},
            {"morph_enabled", kModLfoMorphEnabled, "Morph", "", "Enables continuous shape morphing.",
             ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0}, {"on", "On", 1}}},
            {"triangle_bias", kModLfoTriangleBias, "Triangle Bias", "", "Skews the triangle rise and fall.",
             ForgeParamKind::continuous, ForgeParamCurve::linear},
            {"random_segments", kModLfoRandomSegments, "Random Segments", "", "Sets the random waveform segment count.",
             ForgeParamKind::continuous, ForgeParamCurve::linear},
            {"phase_degrees", kModLfoPhaseDegrees, "Phase", "deg", "Sets the cycle start phase.",
             ForgeParamKind::continuous, ForgeParamCurve::linear},
            {"fade_out_ms", kModLfoFadeOutMs, "Fade Out", "ms", "Fades modulation out after its repeat lifecycle.",
             ForgeParamKind::continuous, ForgeParamCurve::linear},
            {"fade_quadratic", kModLfoFadeQuadratic, "Fade Shape", "", "Selects linear or quadratic fades.",
             ForgeParamKind::stepped, ForgeParamCurve::linear, {{"linear", "Linear", 0}, {"quadratic", "Quadratic", 1}}},
            {"repeat_count", kModLfoRepeatCount, "Repeat Count", "", "Limits the number of cycles; zero repeats indefinitely.",
             ForgeParamKind::continuous, ForgeParamCurve::linear},
        }};
}

inline ForgeNodeDescriptor mod_lpg_descriptor() {
    return {"mod_lpg", "Low-Pass Gate", "A vactrol-style gate coupling loudness, brightness, and natural decay.",
            {}, {{"default", kModLpgTypeId}},
            {
                {"decay_ms", kModLpgDecayMs, "Decay", "ms", "Sets the vactrol release time.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
                {"colour", kModLpgColour, "Colour", "", "Balances amplitude and filtering character.", ForgeParamKind::continuous, ForgeParamCurve::linear},
                {"droop", kModLpgDroop, "Droop", "", "Adds vactrol sag to the response.", ForgeParamKind::continuous, ForgeParamCurve::linear},
                {"brightness_hz", kModLpgBrightnessHz, "Brightness", "Hz", "Sets the open-gate cutoff.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
                {"struck", kModLpgStruck, "Struck", "", "Selects struck rather than continuously driven behavior.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0}, {"on", "On", 1}}},
                {"rise_ms", kModLpgRiseMs, "Rise", "ms", "Sets the vactrol opening time.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
                {"darkness_hz", kModLpgDarknessHz, "Darkness", "Hz", "Sets the closed-gate cutoff.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
                {"strike_threshold", kModLpgStrikeThreshold, "Strike Threshold", "", "Sets the trigger threshold for struck mode.", ForgeParamKind::continuous, ForgeParamCurve::linear},
                {"refractory_ms", kModLpgRefractoryMs, "Refractory", "ms", "Sets the trigger re-arm time.", ForgeParamKind::continuous, ForgeParamCurve::linear},
            }};
}

inline ForgeNodeDescriptor mod_slew_descriptor() {
    return {"mod_slew", "Slew Limiter", "Smooths a control signal with independent rising and falling times.",
            {}, {{"default", kModSlewTypeId}},
            {{"rise_ms", kModSlewRiseMs, "Rise", "ms", "Sets upward slew time.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
             {"fall_ms", kModSlewFallMs, "Fall", "ms", "Sets downward slew time.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
             {"curved", kModSlewCurved, "Curve", "", "Selects linear or curved slewing.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"linear", "Linear", 0}, {"curved", "Curved", 1}}}}};
}

inline ForgeNodeDescriptor mod_transient_descriptor() {
    return {"mod_transient", "Transient Detector", "Produces a level-independent control signal from attacks in audio.",
            {}, {{"default", kModTransientTypeId}},
            {{"fast_ms", kModTransientFastMs, "Fast Time", "ms", "Sets the attack follower time.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
             {"slow_ms", kModTransientSlowMs, "Slow Time", "ms", "Sets the reference follower time.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
             {"sensitivity", kModTransientSensitivity, "Sensitivity", "", "Scales transient contrast.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
             {"invert", kModTransientInvert, "Invert", "", "Inverts the transient control output.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0}, {"on", "On", 1}}}}};
}

inline ForgeNodeDescriptor mod_env_descriptor() {
    return {"mod_env", "Modulation Envelope", "A triggerable delay, attack, hold, and decay envelope with looping and velocity response.",
            {}, {{"default", kModEnvTypeId}},
            {{"attack_ms", kModEnvAttackMs, "Attack", "ms", "Sets attack time.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
             {"hold_ms", kModEnvHoldMs, "Hold", "ms", "Sets peak hold time.", ForgeParamKind::continuous, ForgeParamCurve::linear},
             {"decay_ms", kModEnvDecayMs, "Decay", "ms", "Sets decay time.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
             {"curve", kModEnvCurve, "Curve", "", "Shapes attack and decay together.", ForgeParamKind::continuous, ForgeParamCurve::linear},
             {"threshold", kModEnvThreshold, "Threshold", "", "Sets the trigger threshold.", ForgeParamKind::continuous, ForgeParamCurve::linear},
             {"delay_ms", kModEnvDelayMs, "Delay", "ms", "Sets pre-attack delay.", ForgeParamKind::continuous, ForgeParamCurve::linear},
             {"depth", kModEnvDepth, "Depth", "%", "Scales the envelope output.", ForgeParamKind::continuous, ForgeParamCurve::linear},
             {"loop", kModEnvLoop, "Loop", "", "Enables envelope looping.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0}, {"on", "On", 1}}},
             {"loop_count", kModEnvLoopCount, "Loop Count", "", "Limits loop repetitions; zero loops indefinitely.", ForgeParamKind::continuous, ForgeParamCurve::linear},
             {"refractory_ms", kModEnvRefractoryMs, "Refractory", "ms", "Sets trigger re-arm time.", ForgeParamKind::continuous, ForgeParamCurve::linear},
             {"velocity_sensitive", kModEnvVelocitySensitive, "Velocity", "", "Scales the peak from trigger strength.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0}, {"on", "On", 1}}},
             {"independent_curves", kModEnvIndependentCurves, "Independent Curves", "", "Uses separate attack and decay shapes.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0}, {"on", "On", 1}}},
             {"attack_curve", kModEnvAttackCurve, "Attack Curve", "", "Shapes attack when independent curves are enabled.", ForgeParamKind::continuous, ForgeParamCurve::linear},
             {"decay_curve", kModEnvDecayCurve, "Decay Curve", "", "Shapes decay when independent curves are enabled.", ForgeParamKind::continuous, ForgeParamCurve::linear}}};
}

} // namespace pulp::host::forge_modulation
