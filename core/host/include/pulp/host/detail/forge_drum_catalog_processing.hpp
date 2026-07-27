#pragma once

#include <pulp/host/detail/forge_drum_catalog_impl.hpp>

namespace pulp::host::forge_drum::detail {

inline void apply_common(DrumInstance& instance, const BakedParamView& params,
                         std::int32_t offset) {
    signal::drum::VelocityResponse response;
    response.level_db = params.value_at(kVelocityLevelDb, offset);
    response.bend_octaves = params.value_at(kVelocityBendOctaves, offset);
    response.brightness_octaves = params.value_at(kVelocityBrightnessOctaves, offset);
    response.noise_balance = params.value_at(kVelocityNoiseBalance, offset);
    const bool level_changed = changed(instance, kVelocityLevelDb, response.level_db);
    const bool bend_changed = changed(instance, kVelocityBendOctaves, response.bend_octaves);
    const bool brightness_changed =
        changed(instance, kVelocityBrightnessOctaves, response.brightness_octaves);
    const bool noise_balance_changed =
        changed(instance, kVelocityNoiseBalance, response.noise_balance);
    const bool response_changed =
        level_changed || bend_changed || brightness_changed || noise_balance_changed;
    if (response_changed)
        instance.voice->set_velocity_response(response);
    auto& stage = *output_stage(instance);
    const float drive = params.value_at(kOutputDrive, offset);
    if (changed(instance, kOutputDrive, drive))
        stage.set_drive(drive);
    const float fold = params.value_at(kOutputFold, offset);
    if (changed(instance, kOutputFold, fold))
        stage.set_fold(fold);
    const float level = params.value_at(kOutputLevel, offset);
    if (changed(instance, kOutputLevel, level))
        stage.set_level(level);
    const float attack = params.value_at(kOutputAttackMs, offset);
    const float hold = params.value_at(kOutputHoldMs, offset);
    const float decay = params.value_at(kOutputDecayMs, offset);
    const bool attack_changed = changed(instance, kOutputAttackMs, attack);
    const bool hold_changed = changed(instance, kOutputHoldMs, hold);
    const bool decay_changed = changed(instance, kOutputDecayMs, decay);
    const bool ahd_times_changed = attack_changed || hold_changed || decay_changed;
    if (ahd_times_changed)
        stage.set_ahd_ms(attack, hold, decay);
    const float ahd_enabled = params.value_at(kOutputAhdEnabled, offset);
    const bool ahd_enabled_changed = changed(instance, kOutputAhdEnabled, ahd_enabled);
    if (ahd_times_changed || ahd_enabled_changed)
        stage.set_ahd_enabled(ahd_enabled >= 0.5f);
    const float bits = params.value_at(kOutputBits, offset);
    if (changed(instance, kOutputBits, bits))
        stage.lofi().set_bits(bits);
    const float hold_rate = params.value_at(kOutputHoldRateHz, offset);
    if (changed(instance, kOutputHoldRateHz, hold_rate))
        stage.lofi().set_hold_rate_hz(hold_rate);
    const float jitter = params.value_at(kOutputJitter, offset);
    if (changed(instance, kOutputJitter, jitter))
        stage.lofi().set_jitter(jitter);
    const float smoothing = params.value_at(kOutputSmoothing, offset);
    if (changed(instance, kOutputSmoothing, smoothing))
        stage.lofi().set_smoothing(smoothing);
    const float dead_zone = params.value_at(kOutputDeadZone, offset);
    if (changed(instance, kOutputDeadZone, dead_zone))
        stage.lofi().set_dead_zone(dead_zone);
}

inline int discrete(const BakedParamView& params, state::ParamID id, std::int32_t offset) {
    return static_cast<int>(std::lround(params.value_at(id, offset)));
}

template <typename Gate>
inline void apply_gate(DrumInstance& instance, Gate& gate, const BakedParamView& p,
                       std::int32_t s) {
    const float rise = p.value_at(kGateRiseMs, s);
    if (changed(instance, kGateRiseMs, rise))
        gate.set_rise_ms(rise);
    const float fall = p.value_at(kGateFallMs, s);
    if (changed(instance, kGateFallMs, fall))
        gate.set_fall_ms(fall);
    const float colour = p.value_at(kGateColour, s);
    if (changed(instance, kGateColour, colour))
        gate.set_colour(colour);
    const float closed = p.value_at(kGateClosedHz, s);
    if (changed(instance, kGateClosedHz, closed))
        gate.set_closed_cutoff_hz(closed);
    const float open = p.value_at(kGateOpenHz, s);
    if (changed(instance, kGateOpenHz, open))
        gate.set_open_cutoff_hz(open);
    const float exponent = p.value_at(kGateGainExponent, s);
    if (changed(instance, kGateGainExponent, exponent))
        gate.set_gain_exponent(exponent);
}

template <typename Lofi>
inline void apply_lofi(DrumInstance& instance, Lofi& lofi, state::ParamID base,
                       const BakedParamView& p, std::int32_t s) {
    for (state::ParamID id = base; id < base + 5; ++id) {
        const float value = p.value_at(id, s);
        if (!changed(instance, id, value))
            continue;
        switch (id - base) {
        case 0:
            lofi.set_bits(value);
            break;
        case 1:
            lofi.set_hold_rate_hz(value);
            break;
        case 2:
            lofi.set_jitter(value);
            break;
        case 3:
            lofi.set_smoothing(value);
            break;
        case 4:
            lofi.set_dead_zone(value);
            break;
        }
    }
}

template <typename VoiceType>
inline void apply_noise_color(DrumInstance& instance, VoiceType& voice, const BakedParamView& p,
                              std::int32_t s) {
    const float value = p.value_at(kNoiseColor, s);
    if (changed(instance, kNoiseColor, value))
        voice.set_noise_color(static_cast<signal::NoiseColor>(std::lround(value)));
}

inline void apply_voice(DrumInstance& instance, const BakedParamView& p, std::int32_t s) {
    using namespace signal::drum;
    switch (instance.id) {
    case EngineId::kick_oscillator:
    case EngineId::kick_resonant:
    case EngineId::kick_circuit: {
        auto& v = static_cast<KickVoice&>(*instance.voice);
        v.set_tune_hz(p.value_at(kTuneHz, s));
        v.set_body_decay_ms(p.value_at(kDecay, s));
        v.set_pitch_sweep_octaves(p.value_at(kPitchSweepOctaves, s));
        v.set_pitch_sweep_ms(p.value_at(kPitchSweepMs, s));
        apply_noise_color(instance, v, p, s);
        v.set_click_level(p.value_at(kControl0, s));
        v.set_click_tone_hz(p.value_at(kControl1, s));
        v.set_click_decay_ms(p.value_at(kControl2, s));
        v.set_noise_level(p.value_at(kControl3, s));
        v.set_noise_decay_ms(p.value_at(kControl4, s));
        v.set_sub_level(p.value_at(kControl5, s));
        if (instance.id == EngineId::kick_oscillator) {
            v.set_oscillator_triangle(p.value_at(kControl6, s) >= 0.5f);
            v.set_fm_amount(p.value_at(kControl7, s));
            v.set_fm_ratio(p.value_at(kControl8, s));
        } else if (instance.id == EngineId::kick_circuit) {
            v.set_circuit_feedback(p.value_at(kControl6, s));
            v.set_circuit_drive(p.value_at(kControl7, s));
            v.set_circuit_attack_ms(p.value_at(kControl8, s));
            v.set_circuit_pulse_ms(p.value_at(kControl9, s));
            v.set_circuit_sigh(p.value_at(kControl10, s) >= 0.5f);
            v.set_circuit_hit_life(static_cast<HitLifeMode>(discrete(p, kControl11, s)));
            signal::BridgedTComponents components;
            components.c41 = std::max<double>(p.value_at(kCircuitC41, s), 1.0e-10);
            components.c42 = std::max<double>(p.value_at(kCircuitC42, s), 1.0e-10);
            components.r161 = std::max<double>(p.value_at(kCircuitR161, s), 100.0);
            components.r165 = std::max<double>(p.value_at(kCircuitR165, s), 100.0);
            components.r166 = std::max<double>(p.value_at(kCircuitR166, s), 100.0);
            components.r167 = std::max<double>(p.value_at(kCircuitR167, s), 100.0);
            components.r170 = std::max<double>(p.value_at(kCircuitR170, s), 100.0);
            v.set_circuit_components(components);
        }
        break;
    }
    case EngineId::snare: {
        auto& v = static_cast<SnareVoice&>(*instance.voice);
        v.set_tune_hz(p.value_at(kTuneHz, s));
        v.set_tone_decay_ms(p.value_at(kDecay, s));
        v.set_pitch_sweep_octaves(p.value_at(kPitchSweepOctaves, s));
        v.set_pitch_sweep_ms(p.value_at(kPitchSweepMs, s));
        apply_noise_color(instance, v, p, s);
        v.set_tone_ratio(p.value_at(kControl0, s));
        v.set_tone_level(p.value_at(kControl1, s));
        v.set_fm_amount(p.value_at(kControl2, s));
        v.set_ring(p.value_at(kControl3, s));
        v.set_noise_level(p.value_at(kControl4, s));
        v.set_noise_decay_ms(p.value_at(kControl5, s));
        v.set_noise_cutoff_hz(p.value_at(kControl6, s));
        v.set_noise_resonance(p.value_at(kControl7, s));
        v.set_noise_sweep_octaves(p.value_at(kControl8, s));
        v.set_rattle(p.value_at(kControl9, s));
        v.set_rattle_hz(p.value_at(kControl10, s));
        v.set_snap_level(p.value_at(kControl11, s));
        v.set_snap_cutoff_hz(p.value_at(kControl12, s));
        v.set_snap_decay_ms(p.value_at(kControl13, s));
        v.set_shell_level(p.value_at(kControl14, s));
        v.set_shell_resonance(p.value_at(kControl15, s));
        apply_lofi(instance, v.tone_lofi(), kToneLofiBits, p, s);
        apply_lofi(instance, v.noise_lofi(), kNoiseLofiBits, p, s);
        break;
    }
    case EngineId::hat: {
        auto& v = static_cast<HatVoice&>(*instance.voice);
        v.set_tune_hz(p.value_at(kTuneHz, s));
        v.set_decay_ms(p.value_at(kDecay, s));
        apply_noise_color(instance, v, p, s);
        v.set_spread(p.value_at(kControl0, s));
        v.set_metal(p.value_at(kControl1, s));
        v.set_grit(p.value_at(kControl2, s));
        v.set_grit_ratio(p.value_at(kControl3, s));
        v.set_cutoff_hz(p.value_at(kControl4, s));
        v.set_resonance(p.value_at(kControl5, s));
        v.set_bandpass(p.value_at(kControl6, s) >= 0.5f);
        break;
    }
    case EngineId::clap: {
        auto& v = static_cast<ClapVoice&>(*instance.voice);
        apply_noise_color(instance, v, p, s);
        v.set_burst_count(discrete(p, kControl0, s));
        v.set_burst_spacing_ms(p.value_at(kControl1, s));
        v.set_burst_decay_ms(p.value_at(kControl2, s));
        v.set_burst_falloff(p.value_at(kControl3, s));
        v.set_gap_jitter(p.value_at(kControl4, s));
        v.set_alternate_polarity(p.value_at(kControl5, s) >= 0.5f);
        v.set_stereo_width(p.value_at(kControl6, s));
        v.set_tail_level(p.value_at(kControl7, s));
        v.set_tail_decay_ms(p.value_at(kControl8, s));
        v.set_cutoff_hz(p.value_at(kControl9, s));
        v.set_resonance(p.value_at(kControl10, s));
        v.set_body_level(p.value_at(kControl11, s));
        v.set_body_hz(p.value_at(kControl12, s));
        break;
    }
    case EngineId::tom_generic:
    case EngineId::tom_simmons: {
        auto& v = static_cast<TomVoice&>(*instance.voice);
        v.set_tune_hz(p.value_at(kTuneHz, s));
        v.set_decay_ms(p.value_at(kDecay, s));
        v.set_bend_octaves(p.value_at(kPitchSweepOctaves, s));
        v.set_bend_ms(p.value_at(kPitchSweepMs, s));
        apply_noise_color(instance, v, p, s);
        v.set_wave(static_cast<TomVoice::Wave>(discrete(p, kControl0, s)));
        v.set_noise_balance(p.value_at(kControl1, s));
        v.set_noise_cutoff_hz(p.value_at(kControl2, s));
        v.set_noise_resonance(p.value_at(kControl3, s));
        v.set_click_level(p.value_at(kControl4, s));
        v.set_click_cutoff_hz(p.value_at(kControl5, s));
        v.set_click_decay_ms(p.value_at(kControl6, s));
        break;
    }
    case EngineId::membrane_modal: {
        auto& v = static_cast<MembraneVoice&>(*instance.voice);
        v.set_tune_hz(p.value_at(kTuneHz, s));
        v.set_decay_ms(p.value_at(kDecay, s));
        v.set_structure(p.value_at(kControl0, s));
        v.set_stretch(p.value_at(kControl1, s));
        v.set_damping(p.value_at(kControl2, s));
        v.set_brightness(p.value_at(kControl3, s));
        v.set_position(p.value_at(kControl4, s));
        v.set_spread(p.value_at(kControl5, s));
        v.set_exciter_ms(p.value_at(kControl6, s));
        v.set_exciter_cutoff_hz(p.value_at(kControl7, s));
        v.set_exciter(static_cast<MembraneExciter>(discrete(p, kControl8, s)));
        v.set_sub_level(p.value_at(kControl9, s));
        v.set_air_level(p.value_at(kControl10, s));
        v.set_air_decay_ms(p.value_at(kControl11, s));
        v.set_click_level(p.value_at(kControl12, s));
        v.set_click_decay_ms(p.value_at(kControl13, s));
        apply_gate(instance, v.gate(), p, s);
        break;
    }
    case EngineId::cymbal_comb: {
        auto& v = static_cast<CymbalVoice&>(*instance.voice);
        v.set_tune_hz(p.value_at(kTuneHz, s));
        v.set_decay_ms(p.value_at(kDecay, s));
        apply_noise_color(instance, v, p, s);
        v.set_decay_tilt(p.value_at(kControl0, s));
        v.set_high_mode_emphasis_db(p.value_at(kControl1, s));
        v.set_velocity_feedback(p.value_at(kControl2, s));
        v.set_velocity_high_mode_db(p.value_at(kControl3, s));
        v.set_upper_highpass_hz(p.value_at(kControl4, s));
        v.set_spread(p.value_at(kControl5, s));
        v.set_inharmonicity(p.value_at(kControl6, s));
        v.set_shift_hz(p.value_at(kControl7, s));
        v.set_noise_level(p.value_at(kControl8, s));
        v.set_strike_level(p.value_at(kControl9, s));
        v.set_strike_ms(p.value_at(kControl10, s));
        v.set_tone_hz(p.value_at(kControl11, s));
        const int life = discrete(p, kControl14, s);
        if (life != instance.cymbal_hit_life) {
            v.set_hit_life(static_cast<HitLifeMode>(life));
            instance.cymbal_hit_life = life;
        }
        v.set_low_cut_hz(p.value_at(kControl12, s));
        break;
    }
    case EngineId::string_karplus_strong: {
        auto& v = static_cast<StringVoice&>(*instance.voice);
        v.set_tune_hz(p.value_at(kTuneHz, s));
        v.set_decay_seconds(p.value_at(kDecay, s));
        apply_noise_color(instance, v, p, s);
        v.set_damping(p.value_at(kControl0, s));
        v.set_stiffness(p.value_at(kControl1, s));
        v.set_pluck_position(p.value_at(kControl2, s));
        v.set_exciter_ms(p.value_at(kControl3, s));
        v.set_brightness_hz(p.value_at(kControl4, s));
        v.set_pick_direction(p.value_at(kControl5, s));
        v.set_restart_on_hit(p.value_at(kControl6, s) >= 0.5f);
        v.set_modulation(static_cast<StringModulation>(discrete(p, kControl7, s)));
        v.set_modulation_mix(p.value_at(kControl8, s));
        v.set_modulation_ratio(p.value_at(kControl9, s));
        v.set_fm_depth_octaves(p.value_at(kControl10, s));
        v.set_lpg_amount(p.value_at(kControl11, s));
        apply_gate(instance, v.gate(), p, s);
        break;
    }
    case EngineId::zap_cz: {
        auto& v = static_cast<ZapVoice&>(*instance.voice);
        v.set_tune_hz(p.value_at(kTuneHz, s));
        v.set_decay_ms(p.value_at(kDecay, s));
        v.set_pitch_sweep_octaves(p.value_at(kPitchSweepOctaves, s));
        v.set_pitch_sweep_ms(p.value_at(kPitchSweepMs, s));
        v.set_shape(static_cast<signal::PhaseDistortionShape>(discrete(p, kControl0, s)));
        v.set_distortion(p.value_at(kControl1, s));
        v.set_distortion_ms(p.value_at(kControl2, s));
        v.set_resonant_depth(p.value_at(kControl3, s));
        v.set_detune_cents(p.value_at(kControl4, s));
        v.set_ring(p.value_at(kControl5, s));
        v.set_ring_ratio(p.value_at(kControl6, s));
        apply_gate(instance, v.gate(), p, s);
        break;
    }
    case EngineId::fm2: {
        auto& v = static_cast<FmDrumVoice&>(*instance.voice);
        v.set_tune_hz(p.value_at(kTuneHz, s));
        v.set_decay_ms(p.value_at(kDecay, s));
        v.set_pitch_sweep_octaves(p.value_at(kPitchSweepOctaves, s));
        v.set_pitch_sweep_ms(p.value_at(kPitchSweepMs, s));
        v.set_ratio(p.value_at(kControl0, s));
        v.set_index(p.value_at(kControl1, s));
        v.set_index_ms(p.value_at(kControl2, s));
        v.set_feedback(p.value_at(kControl3, s));
        v.set_carrier_wave(discrete(p, kControl4, s));
        v.set_modulator_wave(discrete(p, kControl5, s));
        v.set_carrier_warp(p.value_at(kControl6, s));
        v.set_modulator_warp(p.value_at(kControl7, s));
        v.set_carrier_warp_ms(p.value_at(kControl8, s));
        v.set_modulator_warp_ms(p.value_at(kControl9, s));
        v.set_lfo_rate_hz(p.value_at(kControl10, s));
        v.set_lfo_depth_octaves(p.value_at(kControl11, s));
        v.set_lfo_delay_ms(p.value_at(kControl12, s));
        v.set_lfo_fade_ms(p.value_at(kControl13, s));
        v.set_hard_sync(p.value_at(kControl14, s) >= 0.5f);
        v.set_cutoff_hz(p.value_at(kControl18, s));
        v.set_resonance(p.value_at(kControl19, s));
        v.set_bandpass(p.value_at(kControl20, s) >= 0.5f);
        const int transient = discrete(p, kControl15, s);
        const bool transient_changed = changed(instance, kControl15, static_cast<float>(transient));
        if (transient >= 0) {
            if (transient_changed)
                v.set_transient(transient);
        } else {
            if (transient_changed)
                instance.applied_params[static_cast<std::size_t>(kNoiseColor)] =
                    std::numeric_limits<float>::quiet_NaN();
            apply_noise_color(instance, v, p, s);
            v.set_noise_level(p.value_at(kControl16, s));
            v.set_noise_decay_ms(p.value_at(kControl17, s));
            v.set_click_level(p.value_at(kControl21, s));
            v.set_click_cutoff_hz(p.value_at(kControl22, s));
        }
        instance.fm_transient = transient;
        break;
    }
    case EngineId::fm6: {
        auto& v = static_cast<Fm6DrumVoice&>(*instance.voice);
        v.set_tune_hz(p.value_at(kTuneHz, s));
        v.set_algorithm(discrete(p, kControl0, s));
        v.set_depth(p.value_at(kControl1, s));
        v.set_formant_hz(p.value_at(kControl2, s));
        v.set_formant_q(p.value_at(kControl3, s));
        v.set_feedback(p.value_at(kControl4, s));
        v.set_pitch_sweep_octaves(p.value_at(kPitchSweepOctaves, s));
        v.set_pitch_sweep_ms(p.value_at(kPitchSweepMs, s));
        for (int op = 0; op < 6; ++op) {
            v.set_operator_ratio(op, p.value_at(kOperatorRatioBase + op, s));
            v.set_operator_level(op, p.value_at(kOperatorLevelBase + op, s));
            v.set_operator_decay_ms(op, p.value_at(kOperatorDecayBase + op, s));
        }
        break;
    }
    case EngineId::fm8: {
        auto& v = static_cast<Fm8DrumVoice&>(*instance.voice);
        v.set_tune_hz(p.value_at(kTuneHz, s));
        v.set_algorithm(discrete(p, kControl0, s));
        v.set_depth(p.value_at(kControl1, s));
        v.set_formant_hz(p.value_at(kControl2, s));
        v.set_formant_q(p.value_at(kControl3, s));
        const int transient = discrete(p, kControl4, s);
        const bool transient_changed = changed(instance, kControl4, static_cast<float>(transient));
        if (transient >= 0) {
            if (transient_changed)
                v.set_transient(transient);
        } else {
            if (transient_changed)
                instance.applied_params[static_cast<std::size_t>(kNoiseColor)] =
                    std::numeric_limits<float>::quiet_NaN();
            apply_noise_color(instance, v, p, s);
            v.set_noise_level(p.value_at(kControl5, s));
            v.set_noise_decay_ms(p.value_at(kControl6, s));
            v.set_click_level(p.value_at(kControl7, s));
        }
        instance.fm_transient = transient;
        for (int op = 0; op < 8; ++op) {
            v.set_operator_ratio(op, p.value_at(kOperatorRatioBase + op, s));
            v.set_operator_level(op, p.value_at(kOperatorLevelBase + op, s));
            v.set_operator_decay_ms(op, p.value_at(kOperatorDecayBase + op, s));
            v.set_operator_feedback(op, p.value_at(kOperatorFeedbackBase + op, s));
            v.set_operator_wave(op, discrete(p, kOperatorWaveBase + op, s));
        }
        break;
    }
    case EngineId::dx7_msfa:
        break;
    }
}

} // namespace pulp::host::forge_drum::detail
