#pragma once

#include <pulp/host/detail/forge_drum_catalog_instance.hpp>

namespace pulp::host::forge_drum::detail {

inline void apply_common(DrumInstance& instance, const BakedParamView& params,
                         std::int32_t offset) {
    // Only axes this engine declared are read. An axis the voice ignores is
    // never registered, so it keeps whatever the engine chose for itself
    // instead of being flattened by a value nothing consumes.
    auto response = instance.voice->velocity_response();
    bool response_changed = false;
    const auto axis = [&](state::ParamID id, float& field) {
        if (instance.params->slot(id) == DrumParamTable::kNoSlot)
            return;
        const float value = params.value_at(id, offset);
        field = value;
        if (changed(instance, id, value))
            response_changed = true;
    };
    axis(kVelocityLevelDb, response.level_db);
    axis(kVelocityBendOctaves, response.bend_octaves);
    axis(kVelocityBrightnessOctaves, response.brightness_octaves);
    axis(kVelocityNoiseBalance, response.noise_balance);
    if (response_changed)
        instance.voice->set_velocity_response(response);
    auto* stage_ptr = output_stage(instance);
    if (stage_ptr == nullptr)
        return;
    auto& stage = *stage_ptr;
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
        if (instance.id != EngineId::kick_circuit) {
            v.set_tune_hz(p.value_at(kTuneHz, s));
            v.set_body_decay_ms(p.value_at(kDecay, s));
        }
        if (instance.id == EngineId::kick_oscillator) {
            v.set_pitch_sweep_octaves(p.value_at(kPitchSweepOctaves, s));
            v.set_pitch_sweep_ms(p.value_at(kPitchSweepMs, s));
        }
        apply_noise_color(instance, v, p, s);
        v.set_click_level(p.value_at(kKickClickLevel, s));
        v.set_click_tone_hz(p.value_at(kKickClickToneHz, s));
        v.set_click_decay_ms(p.value_at(kKickClickDecayMs, s));
        v.set_noise_level(p.value_at(kKickNoiseLevel, s));
        v.set_noise_decay_ms(p.value_at(kKickNoiseDecayMs, s));
        v.set_sub_level(p.value_at(kKickSubLevel, s));
        if (instance.id == EngineId::kick_oscillator) {
            v.set_oscillator_triangle(p.value_at(kKickTriangle, s) >= 0.5f);
            v.set_fm_amount(p.value_at(kKickFmAmount, s));
            v.set_fm_ratio(p.value_at(kKickFmRatio, s));
        } else if (instance.id == EngineId::kick_circuit) {
            v.set_circuit_feedback(p.value_at(kKickCircuitFeedback, s));
            v.set_circuit_drive(p.value_at(kKickCircuitDrive, s));
            v.set_circuit_attack_ms(p.value_at(kKickCircuitAttackMs, s));
            v.set_circuit_pulse_ms(p.value_at(kKickCircuitPulseMs, s));
            v.set_circuit_sigh(p.value_at(kKickCircuitSigh, s) >= 0.5f);
            v.set_circuit_hit_life(static_cast<HitLifeMode>(discrete(p, kKickCircuitHitLife, s)));
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
        v.set_tone_ratio(p.value_at(kSnareToneRatio, s));
        v.set_tone_level(p.value_at(kSnareToneLevel, s));
        v.set_fm_amount(p.value_at(kSnareFmAmount, s));
        v.set_ring(p.value_at(kSnareRing, s));
        v.set_noise_level(p.value_at(kSnareNoiseLevel, s));
        v.set_noise_decay_ms(p.value_at(kSnareNoiseDecayMs, s));
        v.set_noise_cutoff_hz(p.value_at(kSnareNoiseCutoffHz, s));
        v.set_noise_resonance(p.value_at(kSnareNoiseResonance, s));
        v.set_noise_sweep_octaves(p.value_at(kSnareNoiseSweepOctaves, s));
        v.set_rattle(p.value_at(kSnareRattle, s));
        v.set_rattle_hz(p.value_at(kSnareRattleHz, s));
        v.set_snap_level(p.value_at(kSnareSnapLevel, s));
        v.set_snap_cutoff_hz(p.value_at(kSnareSnapCutoffHz, s));
        v.set_snap_decay_ms(p.value_at(kSnareSnapDecayMs, s));
        v.set_shell_level(p.value_at(kSnareShellLevel, s));
        v.set_shell_resonance(p.value_at(kSnareShellResonance, s));
        apply_lofi(instance, v.tone_lofi(), kToneLofiBits, p, s);
        apply_lofi(instance, v.noise_lofi(), kNoiseLofiBits, p, s);
        break;
    }
    case EngineId::hat: {
        auto& v = static_cast<HatVoice&>(*instance.voice);
        v.set_tune_hz(p.value_at(kTuneHz, s));
        v.set_decay_ms(p.value_at(kDecay, s));
        apply_noise_color(instance, v, p, s);
        v.set_spread(p.value_at(kHatSpread, s));
        v.set_metal(p.value_at(kHatMetal, s));
        v.set_grit(p.value_at(kHatGrit, s));
        v.set_grit_ratio(p.value_at(kHatGritRatio, s));
        v.set_cutoff_hz(p.value_at(kHatCutoffHz, s));
        v.set_resonance(p.value_at(kHatResonance, s));
        v.set_bandpass(p.value_at(kHatBandpass, s) >= 0.5f);
        break;
    }
    case EngineId::clap: {
        auto& v = static_cast<ClapVoice&>(*instance.voice);
        apply_noise_color(instance, v, p, s);
        v.set_burst_count(discrete(p, kClapBurstCount, s));
        v.set_burst_spacing_ms(p.value_at(kClapBurstSpacingMs, s));
        v.set_burst_decay_ms(p.value_at(kClapBurstDecayMs, s));
        v.set_burst_falloff(p.value_at(kClapBurstFalloff, s));
        v.set_gap_jitter(p.value_at(kClapGapJitter, s));
        v.set_alternate_polarity(p.value_at(kClapAlternatePolarity, s) >= 0.5f);
        v.set_stereo_width(p.value_at(kClapStereoWidth, s));
        v.set_tail_level(p.value_at(kClapTailLevel, s));
        v.set_tail_decay_ms(p.value_at(kClapTailDecayMs, s));
        v.set_cutoff_hz(p.value_at(kClapCutoffHz, s));
        v.set_resonance(p.value_at(kClapResonance, s));
        v.set_body_level(p.value_at(kClapBodyLevel, s));
        v.set_body_hz(p.value_at(kClapBodyHz, s));
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
        v.set_wave(static_cast<TomVoice::Wave>(discrete(p, kTomWave, s)));
        v.set_noise_balance(p.value_at(kTomNoiseBalance, s));
        v.set_noise_cutoff_hz(p.value_at(kTomNoiseCutoffHz, s));
        v.set_noise_resonance(p.value_at(kTomNoiseResonance, s));
        v.set_click_level(p.value_at(kTomClickLevel, s));
        v.set_click_cutoff_hz(p.value_at(kTomClickCutoffHz, s));
        v.set_click_decay_ms(p.value_at(kTomClickDecayMs, s));
        break;
    }
    case EngineId::membrane_modal: {
        auto& v = static_cast<MembraneVoice&>(*instance.voice);
        v.set_tune_hz(p.value_at(kTuneHz, s));
        v.set_decay_ms(p.value_at(kDecay, s));
        v.set_structure(p.value_at(kMembraneStructure, s));
        v.set_stretch(p.value_at(kMembraneStretch, s));
        v.set_damping(p.value_at(kMembraneDamping, s));
        v.set_brightness(p.value_at(kMembraneBrightness, s));
        v.set_position(p.value_at(kMembranePosition, s));
        v.set_spread(p.value_at(kMembraneSpread, s));
        v.set_exciter_ms(p.value_at(kMembraneExciterMs, s));
        v.set_exciter_cutoff_hz(p.value_at(kMembraneExciterCutoffHz, s));
        v.set_exciter(static_cast<MembraneExciter>(discrete(p, kMembraneExciter, s)));
        v.set_sub_level(p.value_at(kMembraneSubLevel, s));
        v.set_air_level(p.value_at(kMembraneAirLevel, s));
        v.set_air_decay_ms(p.value_at(kMembraneAirDecayMs, s));
        v.set_click_level(p.value_at(kMembraneClickLevel, s));
        v.set_click_decay_ms(p.value_at(kMembraneClickDecayMs, s));
        apply_gate(instance, v.gate(), p, s);
        break;
    }
    case EngineId::cymbal_comb: {
        auto& v = static_cast<CymbalVoice&>(*instance.voice);
        v.set_tune_hz(p.value_at(kTuneHz, s));
        v.set_decay_ms(p.value_at(kDecay, s));
        apply_noise_color(instance, v, p, s);
        v.set_decay_tilt(p.value_at(kCymbalDecayTilt, s));
        v.set_high_mode_emphasis_db(p.value_at(kCymbalHighModeEmphasisDb, s));
        v.set_velocity_feedback(p.value_at(kCymbalVelocityFeedback, s));
        v.set_velocity_high_mode_db(p.value_at(kCymbalVelocityHighModeDb, s));
        v.set_upper_highpass_hz(p.value_at(kCymbalUpperHighpassHz, s));
        v.set_spread(p.value_at(kCymbalSpread, s));
        v.set_inharmonicity(p.value_at(kCymbalInharmonicity, s));
        v.set_shift_hz(p.value_at(kCymbalShiftHz, s));
        v.set_noise_level(p.value_at(kCymbalNoiseLevel, s));
        v.set_strike_level(p.value_at(kCymbalStrikeLevel, s));
        v.set_strike_ms(p.value_at(kCymbalStrikeMs, s));
        v.set_tone_hz(p.value_at(kCymbalToneHz, s));
        // Guarded because HitLife restarts its hit counter on a real mode
        // change; the shared applied-value cache already reports first
        // observation, so the node no longer pre-seeds a mode it never
        // applied.
        const int life = discrete(p, kCymbalHitLife, s);
        if (changed(instance, kCymbalHitLife, static_cast<float>(life)))
            v.set_hit_life(static_cast<HitLifeMode>(life));
        v.set_low_cut_hz(p.value_at(kCymbalLowCutHz, s));
        break;
    }
    case EngineId::string_karplus_strong: {
        auto& v = static_cast<StringVoice&>(*instance.voice);
        v.set_tune_hz(p.value_at(kTuneHz, s));
        v.set_decay_seconds(p.value_at(kDecay, s));
        apply_noise_color(instance, v, p, s);
        v.set_damping(p.value_at(kStringDamping, s));
        v.set_stiffness(p.value_at(kStringStiffness, s));
        v.set_pluck_position(p.value_at(kStringPluckPosition, s));
        v.set_exciter_ms(p.value_at(kStringExciterMs, s));
        v.set_brightness_hz(p.value_at(kStringBrightnessHz, s));
        v.set_pick_direction(p.value_at(kStringPickDirection, s));
        v.set_restart_on_hit(p.value_at(kStringRestartOnHit, s) >= 0.5f);
        v.set_modulation(static_cast<StringModulation>(discrete(p, kStringModulation, s)));
        v.set_modulation_mix(p.value_at(kStringModulationMix, s));
        v.set_modulation_ratio(p.value_at(kStringModulationRatio, s));
        v.set_fm_depth_octaves(p.value_at(kStringFmDepthOctaves, s));
        v.set_lpg_amount(p.value_at(kStringLpgAmount, s));
        apply_gate(instance, v.gate(), p, s);
        break;
    }
    case EngineId::zap_cz: {
        auto& v = static_cast<ZapVoice&>(*instance.voice);
        v.set_tune_hz(p.value_at(kTuneHz, s));
        v.set_decay_ms(p.value_at(kDecay, s));
        v.set_pitch_sweep_octaves(p.value_at(kPitchSweepOctaves, s));
        v.set_pitch_sweep_ms(p.value_at(kPitchSweepMs, s));
        v.set_shape(static_cast<signal::PhaseDistortionShape>(discrete(p, kZapShape, s)));
        v.set_distortion(p.value_at(kZapDistortion, s));
        v.set_distortion_ms(p.value_at(kZapDistortionMs, s));
        v.set_resonant_depth(p.value_at(kZapResonantDepth, s));
        v.set_detune_cents(p.value_at(kZapDetuneCents, s));
        v.set_ring(p.value_at(kZapRing, s));
        v.set_ring_ratio(p.value_at(kZapRingRatio, s));
        apply_gate(instance, v.gate(), p, s);
        break;
    }
    case EngineId::fm2: {
        auto& v = static_cast<FmDrumVoice&>(*instance.voice);
        v.set_tune_hz(p.value_at(kTuneHz, s));
        v.set_decay_ms(p.value_at(kDecay, s));
        v.set_pitch_sweep_octaves(p.value_at(kPitchSweepOctaves, s));
        v.set_pitch_sweep_ms(p.value_at(kPitchSweepMs, s));
        v.set_ratio(p.value_at(kFm2Ratio, s));
        v.set_index(p.value_at(kFm2Index, s));
        v.set_index_ms(p.value_at(kFm2IndexMs, s));
        v.set_feedback(p.value_at(kFm2Feedback, s));
        v.set_carrier_wave(discrete(p, kFm2CarrierWave, s));
        v.set_modulator_wave(discrete(p, kFm2ModulatorWave, s));
        v.set_carrier_warp(p.value_at(kFm2CarrierWarp, s));
        v.set_modulator_warp(p.value_at(kFm2ModulatorWarp, s));
        v.set_carrier_warp_ms(p.value_at(kFm2CarrierWarpMs, s));
        v.set_modulator_warp_ms(p.value_at(kFm2ModulatorWarpMs, s));
        v.set_lfo_rate_hz(p.value_at(kFm2LfoRateHz, s));
        v.set_lfo_depth_octaves(p.value_at(kFm2LfoDepthOctaves, s));
        v.set_lfo_delay_ms(p.value_at(kFm2LfoDelayMs, s));
        v.set_lfo_fade_ms(p.value_at(kFm2LfoFadeMs, s));
        v.set_hard_sync(p.value_at(kFm2HardSync, s) >= 0.5f);
        v.set_cutoff_hz(p.value_at(kFm2CutoffHz, s));
        v.set_resonance(p.value_at(kFm2Resonance, s));
        v.set_bandpass(p.value_at(kFm2Bandpass, s) >= 0.5f);
        const int transient = discrete(p, kFm2Transient, s);
        const bool transient_changed = changed(instance, kFm2Transient, static_cast<float>(transient));
        if (transient >= 0) {
            if (transient_changed)
                v.set_transient(transient);
        } else {
            if (transient_changed)
                invalidate_applied(instance, kNoiseColor);
            apply_noise_color(instance, v, p, s);
            v.set_noise_level(p.value_at(kFm2NoiseLevel, s));
            v.set_noise_decay_ms(p.value_at(kFm2NoiseDecayMs, s));
            v.set_click_level(p.value_at(kFm2ClickLevel, s));
            v.set_click_cutoff_hz(p.value_at(kFm2ClickCutoffHz, s));
        }
        break;
    }
    case EngineId::fm6: {
        auto& v = static_cast<Fm6DrumVoice&>(*instance.voice);
        v.set_tune_hz(p.value_at(kTuneHz, s));
        v.set_algorithm(discrete(p, kFmAlgorithm, s));
        v.set_depth(p.value_at(kFmDepth, s));
        v.set_formant_hz(p.value_at(kFmFormantHz, s));
        v.set_formant_q(p.value_at(kFmFormantQ, s));
        v.set_feedback(p.value_at(kFm6Feedback, s));
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
        v.set_algorithm(discrete(p, kFmAlgorithm, s));
        v.set_depth(p.value_at(kFmDepth, s));
        v.set_formant_hz(p.value_at(kFmFormantHz, s));
        v.set_formant_q(p.value_at(kFmFormantQ, s));
        const int transient = discrete(p, kFm8Transient, s);
        const bool transient_changed = changed(instance, kFm8Transient, static_cast<float>(transient));
        if (transient >= 0) {
            if (transient_changed)
                v.set_transient(transient);
        } else {
            if (transient_changed)
                invalidate_applied(instance, kNoiseColor);
            apply_noise_color(instance, v, p, s);
            v.set_noise_level(p.value_at(kFm8NoiseLevel, s));
            v.set_noise_decay_ms(p.value_at(kFm8NoiseDecayMs, s));
            v.set_click_level(p.value_at(kFm8ClickLevel, s));
        }
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
