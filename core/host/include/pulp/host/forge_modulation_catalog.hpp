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
//                 random, a pulse width, a random blend, and the delay/fade-in
//                 lifecycle. The existing `lfo` node is four fixed shapes with
//                 no lifecycle and no random layer, so delayed vibrato and every
//                 random-step patch are out of its reach.
//   * mod_lpg   — LpgT: the vactrol low-pass gate. Loudness and brightness move
//                 together, which no combination of the existing nodes produces.
//   * mod_slew  — SlewLimiterT: an independent-rise/fall rate limiter on a
//                 control signal. What makes a stepped CV musical.
//   * mod_transient — TransientDetectorT: level-INDEPENDENT attack detection.
//                 Distinct from `env_follower`, which tracks level and therefore
//                 needs re-tuning whenever the source's level changes.
//   * mod_env   — ModEnvT: a delay/attack/hold/decay envelope fired by a rising
//                 edge on its control input. Per-hit modulation shaping.
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

inline constexpr state::ParamID kModLpgDecayMs      = 1;
inline constexpr state::ParamID kModLpgColour       = 2;
inline constexpr state::ParamID kModLpgDroop        = 3;
inline constexpr state::ParamID kModLpgBrightnessHz = 4;
inline constexpr state::ParamID kModLpgStruck       = 5;

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

/// Waveform selector for `mod_lfo`, in the order `signal::LfoT::Wave` declares
/// them. The id is what the agent writes and what a baked artifact stores, so
/// this order is locked exactly as the enum's is.
inline signal::Lfo::Wave mod_lfo_wave_from_id(float id) noexcept {
    const int index = static_cast<int>(std::lround(std::clamp(id, 0.0f, 6.0f)));
    return static_cast<signal::Lfo::Wave>(index);
}

// ── mod_lfo — the full LFO as a control source ───────────────────────────
// Zero audio inputs, one output carrying a UNIPOLAR control signal in [0, 1].
// `cv = clamp(0.5 + 0.5 * depth * lfo, 0, 1)`, the same convention the first CV
// pack uses, so it drops into any CV input port that already accepts an `lfo`.
//
// Rate, depth, wave, pulse width, and random blend are read per sample. The
// delay and fade-in times are read once per block and applied only when they
// change: they recompute stage lengths rather than scale a value, and a
// lifecycle whose boundaries moved every sample would not have boundaries. That
// is the same honest block-rate tradeoff the dry/wet mixer makes.
//
// The random waveforms are seeded deterministically at reset, so a baked render
// reproduces exactly.
struct ModLfoInstance {
    signal::Lfo lfo;
    double delay_ms = -1.0;
    double fade_in_ms = -1.0;
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
    };
    t.reset = [](void* p) {
        auto* s = static_cast<ModLfoInstance*>(p);
        s->lfo.reset();
        s->delay_ms = -1.0;
        s->fade_in_ms = -1.0;
    };
    t.baked_params.push_back({kModLfoRateHz, 0.01f, 40.0f, 2.0f});
    t.baked_params.push_back({kModLfoDepth, 0.0f, 1.0f, 1.0f});
    t.baked_params.push_back({kModLfoWave, 0.0f, 6.0f, 0.0f});
    t.baked_params.push_back({kModLfoPulseWidth, 0.05f, 0.95f, 0.5f});
    t.baked_params.push_back({kModLfoRandomBlend, 0.0f, 1.0f, 0.0f});
    t.baked_params.push_back({kModLfoDelayMs, 0.0f, 5000.0f, 0.0f});
    t.baked_params.push_back({kModLfoFadeInMs, 0.0f, 5000.0f, 0.0f});
    t.process_instance_baked_param =
        [](void* p, audio::BufferView<float>& out,
           const audio::BufferView<const float>& /*in*/, int n,
           const BakedParamView& params) {
            auto* s = static_cast<ModLfoInstance*>(p);
            float* o = out.channel_ptr(0);

            const auto delay_ms =
                static_cast<double>(std::clamp(params.value_at(kModLfoDelayMs, 0), 0.0f, 5000.0f));
            if (delay_ms != s->delay_ms) {
                s->lfo.set_delay_ms(delay_ms);
                s->delay_ms = delay_ms;
            }
            const auto fade_ms =
                static_cast<double>(std::clamp(params.value_at(kModLfoFadeInMs, 0), 0.0f, 5000.0f));
            if (fade_ms != s->fade_in_ms) {
                s->lfo.set_fade_in_ms(fade_ms);
                s->fade_in_ms = fade_ms;
            }

            for (int k = 0; k < n; ++k) {
                const auto off = static_cast<std::int32_t>(k);
                s->lfo.set_rate_hz(
                    static_cast<double>(std::clamp(params.value_at(kModLfoRateHz, off), 0.01f, 40.0f)));
                s->lfo.set_wave(mod_lfo_wave_from_id(params.value_at(kModLfoWave, off)));
                s->lfo.set_pulse_width(params.value_at(kModLfoPulseWidth, off));
                s->lfo.set_random_blend(params.value_at(kModLfoRandomBlend, off));
                const float depth = std::clamp(params.value_at(kModLfoDepth, off), 0.0f, 1.0f);
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
// would vanish the moment anything smoothed the CV — including the
// `transient -> lpg` and `slew -> lpg` patches this node is meant for.
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
    float last_brightness_hz = -1.0f;
    /// Running maximum of the control since the current rising edge, and
    /// whether that edge is still open. See the strike note below.
    float strike_peak = 0.0f;
    bool striking = false;
};

/// Control level a rising edge must cross to ping the cell, and the level it
/// must fall back below before the next rise counts as a new hit.
inline constexpr float kModLpgStrikeThreshold = 0.25f;

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
        s->edge.set_threshold(kModLpgStrikeThreshold);
        s->last_brightness_hz = -1.0f;
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
    t.baked_params.push_back({kModLpgBrightnessHz, 500.0f, 18000.0f, 12000.0f});
    t.baked_params.push_back({kModLpgStruck, 0.0f, 1.0f, 0.0f});
    t.process_instance_baked_param =
        [](void* p, audio::BufferView<float>& out,
           const audio::BufferView<const float>& in, int n,
           const BakedParamView& params) {
            auto* s = static_cast<ModLpgInstance*>(p);
            const float* sig = in.channel_ptr(0);
            const float* cv = in.channel_ptr(1);
            float* o = out.channel_ptr(0);

            // The cutoff ceiling reconfigures the filter's range rather than
            // scaling a value, so it is applied per block on change.
            const float brightness =
                std::clamp(params.value_at(kModLpgBrightnessHz, 0), 500.0f, 18000.0f);
            if (brightness != s->last_brightness_hz) {
                s->lpg.set_range_hz(signal::Lpg::kDefaultFcMinHz, brightness);
                s->last_brightness_hz = brightness;
            }

            for (int k = 0; k < n; ++k) {
                const auto off = static_cast<std::int32_t>(k);
                s->lpg.set_decay_ms(
                    static_cast<double>(std::clamp(params.value_at(kModLpgDecayMs, off), 20.0f, 2000.0f)));
                s->lpg.set_colour(params.value_at(kModLpgColour, off));
                s->lpg.set_droop(params.value_at(kModLpgDroop, off));

                const float control = std::clamp(cv[static_cast<std::size_t>(k)], 0.0f, 1.0f);
                const bool struck = params.value_at(kModLpgStruck, off) >= 0.5f;
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
                        } else if (control < kModLpgStrikeThreshold) {
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
        static_cast<ModSlewInstance*>(p)->slew.prepare(static_cast<float>(sr));
    };
    t.reset = [](void* p) { static_cast<ModSlewInstance*>(p)->slew.reset(0.0f); };
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
                s->slew.set_times_ms(std::clamp(params.value_at(kModSlewRiseMs, off), 0.0f, 2000.0f),
                                     std::clamp(params.value_at(kModSlewFallMs, off), 0.0f, 2000.0f));
                s->slew.set_mode(params.value_at(kModSlewCurved, off) >= 0.5f
                                     ? signal::SlewLimiter::Mode::exponential
                                     : signal::SlewLimiter::Mode::linear);
                o[static_cast<std::size_t>(k)] = s->slew.process(cv[static_cast<std::size_t>(k)]);
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

            // The ballistics times recompute filter coefficients, so they are
            // block-rate on change — the same honest tradeoff the lo-fi
            // envelope follower makes.
            const float fast_ms = std::clamp(params.value_at(kModTransientFastMs, 0), 0.5f, 20.0f);
            if (fast_ms != s->last_fast_ms) {
                s->detector.set_fast_ms(fast_ms);
                s->last_fast_ms = fast_ms;
            }
            const float slow_ms = std::clamp(params.value_at(kModTransientSlowMs, 0), 10.0f, 500.0f);
            if (slow_ms != s->last_slow_ms) {
                s->detector.set_slow_ms(slow_ms);
                s->last_slow_ms = slow_ms;
            }

            for (int k = 0; k < n; ++k) {
                const auto off = static_cast<std::int32_t>(k);
                const float sensitivity =
                    std::clamp(params.value_at(kModTransientSensitivity, off), 0.1f, 8.0f);
                const float detected =
                    std::clamp(s->detector.process(sig[static_cast<std::size_t>(k)]) * sensitivity,
                               0.0f, 1.0f);
                o[static_cast<std::size_t>(k)] =
                    params.value_at(kModTransientInvert, off) >= 0.5f ? 1.0f - detected : detected;
            }
        };
    return t;
}

// ── mod_env — per-hit modulation envelope (CV -> CV) ─────────────────────
// One control input, one control output in [0, 1]. A rising edge on the input
// past `threshold` fires a delay-free attack/hold/decay shape with a settable
// curve. It ignores gates entirely: the input is an event source, not a note.
//
// The chain it exists for is `mod_transient -> mod_env -> filter_cv(1)` — a
// filter sweep whose depth and contour belong to the hit rather than to the
// input's envelope. Doing that with `env_follower` gives the shape of the sound;
// this gives the shape you asked for, fired by the sound.
struct ModEnvInstance {
    signal::ModEnv envelope;
    signal::TriggerDetect edge;
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
    t.process_instance_baked_param =
        [](void* p, audio::BufferView<float>& out,
           const audio::BufferView<const float>& in, int n,
           const BakedParamView& params) {
            auto* s = static_cast<ModEnvInstance*>(p);
            const float* cv = in.channel_ptr(0);
            float* o = out.channel_ptr(0);
            for (int k = 0; k < n; ++k) {
                const auto off = static_cast<std::int32_t>(k);
                s->envelope.set_attack_ms(
                    static_cast<double>(std::clamp(params.value_at(kModEnvAttackMs, off), 0.1f, 500.0f)));
                s->envelope.set_hold_ms(
                    static_cast<double>(std::clamp(params.value_at(kModEnvHoldMs, off), 0.0f, 500.0f)));
                s->envelope.set_decay_ms(
                    static_cast<double>(std::clamp(params.value_at(kModEnvDecayMs, off), 1.0f, 2000.0f)));
                const float curve = std::clamp(params.value_at(kModEnvCurve, off), -1.0f, 1.0f);
                s->envelope.set_attack_curve(curve);
                s->envelope.set_decay_curve(curve);
                s->edge.set_threshold(
                    std::clamp(params.value_at(kModEnvThreshold, off), 0.05f, 0.95f));

                if (s->edge.process_signal(cv[static_cast<std::size_t>(k)])) s->envelope.trigger();
                o[static_cast<std::size_t>(k)] =
                    std::clamp(s->envelope.next(), 0.0f, 1.0f);
            }
        };
    return t;
}

} // namespace pulp::host::forge_modulation
