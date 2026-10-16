#include <pulp/host/forge_modulation_catalog.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace pulp::host::forge_modulation {

signal::Lfo::Wave mod_lfo_wave_from_id(float id) noexcept {
    if (!std::isfinite(id))
        id = 0.0f;
    const int index = static_cast<int>(std::lround(std::clamp(id, 0.0f, 6.0f)));
    return static_cast<signal::Lfo::Wave>(index);
}

CustomNodeType make_mod_lfo_node() {
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
    t.baked_params.push_back({kModLfoRandomSegments, 1.0f,
                              static_cast<float>(signal::Lfo::kMaxRandomSegments),
                              static_cast<float>(signal::Lfo::kDefaultRandomSegments)});
    t.baked_params.push_back({kModLfoPhaseDegrees, 0.0f, 360.0f, 0.0f});
    t.baked_params.push_back({kModLfoFadeOutMs, 0.0f, 5000.0f, 0.0f});
    t.baked_params.push_back({kModLfoFadeQuadratic, 0.0f, 1.0f, 0.0f});
    t.baked_params.push_back(
        {kModLfoRepeatCount, 0.0f, static_cast<float>(signal::Lfo::kMaxRepeatCount), 0.0f});
    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& /*in*/, int n,
                                        const BakedParamView& params) {
        auto* s = static_cast<ModLfoInstance*>(p);
        float* o = out.channel_ptr(0);

        for (int k = 0; k < n; ++k) {
            const auto off = static_cast<std::int32_t>(k);
            const auto delay_ms = static_cast<double>(params.value_at(kModLfoDelayMs, off));
            if (delay_ms != s->delay_ms) {
                s->lfo.set_delay_ms(delay_ms);
                s->delay_ms = delay_ms;
            }
            const auto fade_ms = static_cast<double>(params.value_at(kModLfoFadeInMs, off));
            if (fade_ms != s->fade_in_ms) {
                s->lfo.set_fade_in_ms(fade_ms);
                s->fade_in_ms = fade_ms;
            }
            const auto fade_out_ms = static_cast<double>(params.value_at(kModLfoFadeOutMs, off));
            if (fade_out_ms != s->fade_out_ms) {
                s->lfo.set_fade_out_ms(fade_out_ms, fade_out_ms > 0.0);
                s->fade_out_ms = fade_out_ms;
            }
            const int fade_curve = params.value_at(kModLfoFadeQuadratic, off) >= 0.5f ? 1 : 0;
            if (fade_curve != s->fade_curve) {
                s->lfo.set_fade_curve(fade_curve != 0 ? signal::Lfo::FadeCurve::quadratic
                                                      : signal::Lfo::FadeCurve::linear);
                s->fade_curve = fade_curve;
            }
            const int repeat_count =
                static_cast<int>(std::lround(params.value_at(kModLfoRepeatCount, off)));
            if (repeat_count != s->repeat_count) {
                s->lfo.set_repeat_count(repeat_count);
                s->repeat_count = repeat_count;
            }
            s->lfo.set_rate_hz(static_cast<double>(params.value_at(kModLfoRateHz, off)));
            s->lfo.set_wave(mod_lfo_wave_from_id(params.value_at(kModLfoWave, off)));
            s->lfo.set_shape_morph(params.value_at(kModLfoShapeMorph, off));
            s->lfo.set_shape_morph_enabled(params.value_at(kModLfoMorphEnabled, off) >= 0.5f);
            s->lfo.set_pulse_width(params.value_at(kModLfoPulseWidth, off));
            s->lfo.set_tri_bias(params.value_at(kModLfoTriangleBias, off));
            s->lfo.set_random_blend(params.value_at(kModLfoRandomBlend, off));
            s->lfo.set_random_segments(
                static_cast<int>(std::lround(params.value_at(kModLfoRandomSegments, off))));
            s->lfo.set_phase_offset(params.value_at(kModLfoPhaseDegrees, off) / 360.0f);
            const float depth = params.value_at(kModLfoDepth, off);
            o[static_cast<std::size_t>(k)] =
                std::clamp(0.5f + 0.5f * depth * s->lfo.next(), 0.0f, 1.0f);
        }
    };
    return t;
}

CustomNodeType make_mod_lpg_node() {
    CustomNodeType t;
    t.type_id = kModLpgTypeId;
    t.version = 1;
    t.num_input_ports = 2; // 0 = signal, 1 = control CV
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
        {kModLpgRiseMs, 0.05f, 100.0f, static_cast<float>(signal::Lpg::kRiseMs)});
    t.baked_params.push_back(
        {kModLpgDarknessHz, signal::Lpg::kMinFcHz, 500.0f, signal::Lpg::kDefaultFcMinHz});
    t.baked_params.push_back({kModLpgStrikeThreshold, 0.01f, 0.99f, kDefaultModLpgStrikeThreshold});
    t.baked_params.push_back({kModLpgRefractoryMs, 0.0f, 100.0f, 2.0f});
    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& in, int n,
                                        const BakedParamView& params) {
        auto* s = static_cast<ModLpgInstance*>(p);
        const float* sig = in.channel_ptr(0);
        const float* cv = in.channel_ptr(1);
        float* o = out.channel_ptr(0);

        for (int k = 0; k < n; ++k) {
            const auto off = static_cast<std::int32_t>(k);
            const float darkness = params.value_at(kModLpgDarknessHz, off);
            const float brightness = params.value_at(kModLpgBrightnessHz, off);
            if (darkness != s->last_darkness_hz || brightness != s->last_brightness_hz) {
                s->lpg.set_range_hz(darkness, brightness);
                s->last_darkness_hz = darkness;
                s->last_brightness_hz = brightness;
            }
            const float threshold = params.value_at(kModLpgStrikeThreshold, off);
            s->edge.set_threshold(threshold);
            const float refractory_ms = params.value_at(kModLpgRefractoryMs, off);
            if (refractory_ms != s->last_refractory_ms) {
                s->edge.set_refractory_ms(refractory_ms);
                s->last_refractory_ms = refractory_ms;
            }
            const float decay_ms = params.value_at(kModLpgDecayMs, off);
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

CustomNodeType make_mod_slew_node() {
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
    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& in, int n,
                                        const BakedParamView& params) {
        auto* s = static_cast<ModSlewInstance*>(p);
        const float* cv = in.channel_ptr(0);
        float* o = out.channel_ptr(0);
        for (int k = 0; k < n; ++k) {
            const auto off = static_cast<std::int32_t>(k);
            const float rise_ms = params.value_at(kModSlewRiseMs, off);
            const float fall_ms = params.value_at(kModSlewFallMs, off);
            if (rise_ms != s->last_rise_ms || fall_ms != s->last_fall_ms) {
                s->slew.set_times_ms(rise_ms, fall_ms);
                s->last_rise_ms = rise_ms;
                s->last_fall_ms = fall_ms;
            }
            const int curved = params.value_at(kModSlewCurved, off) >= 0.5f ? 1 : 0;
            if (curved != s->last_curved) {
                s->slew.set_mode(curved != 0 ? signal::SlewLimiter::Mode::exponential
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

CustomNodeType make_mod_transient_node() {
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
    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& in, int n,
                                        const BakedParamView& params) {
        auto* s = static_cast<ModTransientInstance*>(p);
        const float* sig = in.channel_ptr(0);
        float* o = out.channel_ptr(0);

        for (int k = 0; k < n; ++k) {
            const auto off = static_cast<std::int32_t>(k);
            const float fast_ms = params.value_at(kModTransientFastMs, off);
            if (fast_ms != s->last_fast_ms) {
                s->detector.set_fast_ms(fast_ms);
                s->last_fast_ms = fast_ms;
            }
            const float slow_ms = params.value_at(kModTransientSlowMs, off);
            if (slow_ms != s->last_slow_ms) {
                s->detector.set_slow_ms(slow_ms);
                s->last_slow_ms = slow_ms;
            }
            const float sensitivity = params.value_at(kModTransientSensitivity, off);
            const float detected = std::clamp(
                s->detector.process(sig[static_cast<std::size_t>(k)]) * sensitivity, 0.0f, 1.0f);
            o[static_cast<std::size_t>(k)] =
                params.value_at(kModTransientInvert, off) >= 0.5f ? 1.0f - detected : detected;
        }
    };
    return t;
}

CustomNodeType make_mod_env_node() {
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
    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& in, int n,
                                        const BakedParamView& params) {
        auto* s = static_cast<ModEnvInstance*>(p);
        const float* cv = in.channel_ptr(0);
        float* o = out.channel_ptr(0);

        for (int k = 0; k < n; ++k) {
            const auto off = static_cast<std::int32_t>(k);
            const float delay_ms = params.value_at(kModEnvDelayMs, off);
            if (delay_ms != s->last_delay_ms) {
                s->envelope.set_delay_ms(static_cast<double>(delay_ms));
                s->last_delay_ms = delay_ms;
            }
            const bool loop = params.value_at(kModEnvLoop, off) >= 0.5f;
            const int loop_count =
                static_cast<int>(std::lround(params.value_at(kModEnvLoopCount, off)));
            if (loop != s->last_loop || loop_count != s->last_loop_count) {
                s->envelope.set_loop(loop, loop_count);
                s->last_loop = loop;
                s->last_loop_count = loop_count;
            }
            const float refractory_ms = params.value_at(kModEnvRefractoryMs, off);
            if (refractory_ms != s->last_refractory_ms) {
                s->edge.set_refractory_ms(refractory_ms);
                s->last_refractory_ms = refractory_ms;
            }
            const float attack_ms = params.value_at(kModEnvAttackMs, off);
            if (attack_ms != s->last_attack_ms) {
                s->envelope.set_attack_ms(static_cast<double>(attack_ms));
                s->last_attack_ms = attack_ms;
            }
            const float hold_ms = params.value_at(kModEnvHoldMs, off);
            if (hold_ms != s->last_hold_ms) {
                s->envelope.set_hold_ms(static_cast<double>(hold_ms));
                s->last_hold_ms = hold_ms;
            }
            const float decay_ms = params.value_at(kModEnvDecayMs, off);
            if (decay_ms != s->last_decay_ms) {
                s->envelope.set_decay_ms(static_cast<double>(decay_ms));
                s->last_decay_ms = decay_ms;
            }
            const float curve = params.value_at(kModEnvCurve, off);
            const bool independent_curves = params.value_at(kModEnvIndependentCurves, off) >= 0.5f;
            s->envelope.set_attack_curve(
                independent_curves ? params.value_at(kModEnvAttackCurve, off) : curve);
            s->envelope.set_decay_curve(independent_curves ? params.value_at(kModEnvDecayCurve, off)
                                                           : curve);
            s->edge.set_threshold(params.value_at(kModEnvThreshold, off));

            if (s->edge.process_signal(cv[static_cast<std::size_t>(k)])) {
                const float velocity = params.value_at(kModEnvVelocitySensitive, off) >= 0.5f
                                           ? std::clamp(cv[static_cast<std::size_t>(k)], 0.0f, 1.0f)
                                           : 1.0f;
                s->envelope.trigger(velocity);
            }
            const float depth = params.value_at(kModEnvDepth, off);
            o[static_cast<std::size_t>(k)] = std::clamp(depth * s->envelope.next(), 0.0f, 1.0f);
        }
    };
    return t;
}

ForgeNodeDescriptor mod_lfo_descriptor() {
    return {
        "mod_lfo",
        "Modulation LFO",
        "A unipolar modulation oscillator with shaped, random, delayed, faded, and repeating "
        "cycles.",
        {},
        {{"default", kModLfoTypeId}},
        {
            {"rate_hz", kModLfoRateHz, "Rate", "Hz", "Sets the oscillator rate.",
             ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
            {"depth", kModLfoDepth, "Depth", "%", "Scales the modulation output.",
             ForgeParamKind::continuous, ForgeParamCurve::linear},
            {"wave",
             kModLfoWave,
             "Wave",
             "",
             "Selects the base waveform.",
             ForgeParamKind::stepped,
             ForgeParamCurve::linear,
             {{"sine", "Sine", 0},
              {"triangle", "Triangle", 1},
              {"saw_up", "Saw Up", 2},
              {"saw_down", "Saw Down", 3},
              {"square", "Square", 4},
              {"sample_hold", "Sample and Hold", 5},
              {"smooth_random", "Smooth Random", 6}}},
            {"pulse_width", kModLfoPulseWidth, "Pulse Width", "%", "Sets square-wave duty.",
             ForgeParamKind::continuous, ForgeParamCurve::linear},
            {"random_blend", kModLfoRandomBlend, "Random Blend", "%",
             "Blends random motion into the cycle.", ForgeParamKind::continuous,
             ForgeParamCurve::linear},
            {"delay_ms", kModLfoDelayMs, "Delay", "ms", "Delays modulation after trigger.",
             ForgeParamKind::continuous, ForgeParamCurve::linear},
            {"fade_in_ms", kModLfoFadeInMs, "Fade In", "ms", "Fades modulation in.",
             ForgeParamKind::continuous, ForgeParamCurve::linear},
            {"shape_morph", kModLfoShapeMorph, "Shape Morph", "",
             "Moves continuously through adjacent waveform shapes.", ForgeParamKind::continuous,
             ForgeParamCurve::linear},
            {"morph_enabled",
             kModLfoMorphEnabled,
             "Morph",
             "",
             "Enables continuous shape morphing.",
             ForgeParamKind::stepped,
             ForgeParamCurve::linear,
             {{"off", "Off", 0}, {"on", "On", 1}}},
            {"triangle_bias", kModLfoTriangleBias, "Triangle Bias", "",
             "Skews the triangle rise and fall.", ForgeParamKind::continuous,
             ForgeParamCurve::linear},
            {"random_segments", kModLfoRandomSegments, "Random Segments", "",
             "Sets the random waveform segment count.", ForgeParamKind::continuous,
             ForgeParamCurve::linear},
            {"phase_degrees", kModLfoPhaseDegrees, "Phase", "deg", "Sets the cycle start phase.",
             ForgeParamKind::continuous, ForgeParamCurve::linear},
            {"fade_out_ms", kModLfoFadeOutMs, "Fade Out", "ms",
             "Fades modulation out after its repeat lifecycle.", ForgeParamKind::continuous,
             ForgeParamCurve::linear},
            {"fade_quadratic",
             kModLfoFadeQuadratic,
             "Fade Shape",
             "",
             "Selects linear or quadratic fades.",
             ForgeParamKind::stepped,
             ForgeParamCurve::linear,
             {{"linear", "Linear", 0}, {"quadratic", "Quadratic", 1}}},
            {"repeat_count", kModLfoRepeatCount, "Repeat Count", "",
             "Limits the number of cycles; zero repeats indefinitely.", ForgeParamKind::continuous,
             ForgeParamCurve::linear},
        }};
}

ForgeNodeDescriptor mod_lpg_descriptor() {
    return {
        "mod_lpg",
        "Low-Pass Gate",
        "A vactrol-style gate coupling loudness, brightness, and natural decay.",
        {},
        {{"default", kModLpgTypeId}},
        {
            {"decay_ms", kModLpgDecayMs, "Decay", "ms", "Sets the vactrol release time.",
             ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
            {"colour", kModLpgColour, "Colour", "", "Balances amplitude and filtering character.",
             ForgeParamKind::continuous, ForgeParamCurve::linear},
            {"droop", kModLpgDroop, "Droop", "", "Adds vactrol sag to the response.",
             ForgeParamKind::continuous, ForgeParamCurve::linear},
            {"brightness_hz", kModLpgBrightnessHz, "Brightness", "Hz", "Sets the open-gate cutoff.",
             ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
            {"struck",
             kModLpgStruck,
             "Struck",
             "",
             "Selects struck rather than continuously driven behavior.",
             ForgeParamKind::stepped,
             ForgeParamCurve::linear,
             {{"off", "Off", 0}, {"on", "On", 1}}},
            {"rise_ms", kModLpgRiseMs, "Rise", "ms", "Sets the vactrol opening time.",
             ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
            {"darkness_hz", kModLpgDarknessHz, "Darkness", "Hz", "Sets the closed-gate cutoff.",
             ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
            {"strike_threshold", kModLpgStrikeThreshold, "Strike Threshold", "",
             "Sets the trigger threshold for struck mode.", ForgeParamKind::continuous,
             ForgeParamCurve::linear},
            {"refractory_ms", kModLpgRefractoryMs, "Refractory", "ms",
             "Sets the trigger re-arm time.", ForgeParamKind::continuous, ForgeParamCurve::linear},
        }};
}

ForgeNodeDescriptor mod_slew_descriptor() {
    return {"mod_slew",
            "Slew Limiter",
            "Smooths a control signal with independent rising and falling times.",
            {},
            {{"default", kModSlewTypeId}},
            {{"rise_ms", kModSlewRiseMs, "Rise", "ms", "Sets upward slew time.",
              ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
             {"fall_ms", kModSlewFallMs, "Fall", "ms", "Sets downward slew time.",
              ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
             {"curved",
              kModSlewCurved,
              "Curve",
              "",
              "Selects linear or curved slewing.",
              ForgeParamKind::stepped,
              ForgeParamCurve::linear,
              {{"linear", "Linear", 0}, {"curved", "Curved", 1}}}}};
}

ForgeNodeDescriptor mod_transient_descriptor() {
    return {
        "mod_transient",
        "Transient Detector",
        "Produces a level-independent control signal from attacks in audio.",
        {},
        {{"default", kModTransientTypeId}},
        {{"fast_ms", kModTransientFastMs, "Fast Time", "ms", "Sets the attack follower time.",
          ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
         {"slow_ms", kModTransientSlowMs, "Slow Time", "ms", "Sets the reference follower time.",
          ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
         {"sensitivity", kModTransientSensitivity, "Sensitivity", "", "Scales transient contrast.",
          ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
         {"invert",
          kModTransientInvert,
          "Invert",
          "",
          "Inverts the transient control output.",
          ForgeParamKind::stepped,
          ForgeParamCurve::linear,
          {{"off", "Off", 0}, {"on", "On", 1}}}}};
}

ForgeNodeDescriptor mod_env_descriptor() {
    return {
        "mod_env",
        "Modulation Envelope",
        "A triggerable delay, attack, hold, and decay envelope with looping and velocity response.",
        {},
        {{"default", kModEnvTypeId}},
        {{"attack_ms", kModEnvAttackMs, "Attack", "ms", "Sets attack time.",
          ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
         {"hold_ms", kModEnvHoldMs, "Hold", "ms", "Sets peak hold time.",
          ForgeParamKind::continuous, ForgeParamCurve::linear},
         {"decay_ms", kModEnvDecayMs, "Decay", "ms", "Sets decay time.", ForgeParamKind::continuous,
          ForgeParamCurve::logarithmic},
         {"curve", kModEnvCurve, "Curve", "", "Shapes attack and decay together.",
          ForgeParamKind::continuous, ForgeParamCurve::linear},
         {"threshold", kModEnvThreshold, "Threshold", "", "Sets the trigger threshold.",
          ForgeParamKind::continuous, ForgeParamCurve::linear},
         {"delay_ms", kModEnvDelayMs, "Delay", "ms", "Sets pre-attack delay.",
          ForgeParamKind::continuous, ForgeParamCurve::linear},
         {"depth", kModEnvDepth, "Depth", "%", "Scales the envelope output.",
          ForgeParamKind::continuous, ForgeParamCurve::linear},
         {"loop",
          kModEnvLoop,
          "Loop",
          "",
          "Enables envelope looping.",
          ForgeParamKind::stepped,
          ForgeParamCurve::linear,
          {{"off", "Off", 0}, {"on", "On", 1}}},
         {"loop_count", kModEnvLoopCount, "Loop Count", "",
          "Limits loop repetitions; zero loops indefinitely.", ForgeParamKind::continuous,
          ForgeParamCurve::linear},
         {"refractory_ms", kModEnvRefractoryMs, "Refractory", "ms", "Sets trigger re-arm time.",
          ForgeParamKind::continuous, ForgeParamCurve::linear},
         {"velocity_sensitive",
          kModEnvVelocitySensitive,
          "Velocity",
          "",
          "Scales the peak from trigger strength.",
          ForgeParamKind::stepped,
          ForgeParamCurve::linear,
          {{"off", "Off", 0}, {"on", "On", 1}}},
         {"independent_curves",
          kModEnvIndependentCurves,
          "Independent Curves",
          "",
          "Uses separate attack and decay shapes.",
          ForgeParamKind::stepped,
          ForgeParamCurve::linear,
          {{"off", "Off", 0}, {"on", "On", 1}}},
         {"attack_curve", kModEnvAttackCurve, "Attack Curve", "",
          "Shapes attack when independent curves are enabled.", ForgeParamKind::continuous,
          ForgeParamCurve::linear},
         {"decay_curve", kModEnvDecayCurve, "Decay Curve", "",
          "Shapes decay when independent curves are enabled.", ForgeParamKind::continuous,
          ForgeParamCurve::linear}}};
}

} // namespace pulp::host::forge_modulation
