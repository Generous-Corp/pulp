#pragma once

#include <pulp/host/detail/forge_drum_catalog_processing.hpp>

namespace pulp::host::forge_drum::detail {

inline void add(CustomNodeType& type, state::ParamID id, float minimum, float maximum,
                float initial) {
    type.baked_params.push_back({id, minimum, maximum, initial});
}

inline const char* type_id(EngineId id) noexcept {
    switch (id) {
    case EngineId::kick_oscillator:
        return kKickOscillatorTypeId;
    case EngineId::kick_resonant:
        return kKickResonantTypeId;
    case EngineId::kick_circuit:
        return kKickCircuitTypeId;
    case EngineId::snare:
        return kSnareTypeId;
    case EngineId::hat:
        return kHatTypeId;
    case EngineId::clap:
        return kClapTypeId;
    case EngineId::tom_generic:
        return kTomGenericTypeId;
    case EngineId::tom_simmons:
        return kTomSimmonsTypeId;
    case EngineId::cymbal_comb:
        return kCymbalTypeId;
    case EngineId::membrane_modal:
        return kMembraneTypeId;
    case EngineId::string_karplus_strong:
        return kStringTypeId;
    case EngineId::zap_cz:
        return kZapTypeId;
    case EngineId::fm2:
        return kFm2TypeId;
    case EngineId::fm6:
        return kFm6TypeId;
    case EngineId::fm8:
        return kFm8TypeId;
    case EngineId::dx7_msfa:
        return "";
    }
    return "";
}

inline signal::drum::VelocityResponse default_velocity_response(EngineId id) noexcept {
    signal::drum::VelocityResponse response;
    response.level_db = 15.0f;
    switch (id) {
    case EngineId::kick_oscillator:
    case EngineId::kick_resonant:
    case EngineId::kick_circuit:
        response = {14.0f, 0.15f, 1.5f, 0.0f};
        break;
    case EngineId::snare:
        response = {16.0f, 0.08f, 1.2f, 0.25f};
        break;
    case EngineId::hat:
        response = {16.0f, 0.0f, 0.8f, 0.0f};
        break;
    case EngineId::clap:
        response = {15.0f, 0.0f, 0.7f, 0.0f};
        break;
    case EngineId::tom_generic:
    case EngineId::tom_simmons:
        response = {15.0f, 1.5f, 1.0f, 0.0f};
        break;
    case EngineId::cymbal_comb:
        response = {15.0f, 0.0f, 1.2f, 0.0f};
        break;
    case EngineId::membrane_modal:
        response = {16.0f, 0.15f, 1.6f, 0.0f};
        break;
    case EngineId::string_karplus_strong:
        response = {16.0f, 0.05f, 2.0f, 0.0f};
        break;
    case EngineId::zap_cz:
        response = {15.0f, 1.0f, 0.8f, 0.0f};
        break;
    case EngineId::fm2:
        response = {15.0f, 0.1f, 1.0f, 0.0f};
        break;
    case EngineId::fm6:
    case EngineId::fm8:
        response = {15.0f, 0.0f, 1.0f, 0.0f};
        break;
    case EngineId::dx7_msfa:
        break;
    }
    return response;
}

/// Which velocity axes a voice actually consumes.
///
/// A control that cannot move the sound is worse than a missing one: a preset
/// saves a value for it, a UI gives it a knob, and a player spends time on it,
/// all for nothing. So the catalog declares a velocity axis only where the
/// voice reads it. `level` and `brightness` are universal; the other two are
/// not, and saying so here keeps every registration honest without a
/// per-engine special case at the call site.
struct VelocityAxes {
    bool bend = false;
    bool noise_balance = false;
};

inline VelocityAxes velocity_axes(EngineId id) noexcept {
    switch (id) {
    case EngineId::snare:
        // The only voice that crossfades a tone and a noise generator by
        // velocity, so the only one with a noise-balance axis.
        return {true, true};
    case EngineId::kick_oscillator:
    case EngineId::tom_generic:
    case EngineId::tom_simmons:
    case EngineId::membrane_modal:
    case EngineId::string_karplus_strong:
    case EngineId::zap_cz:
    case EngineId::fm2:
        return {true, false};
    case EngineId::kick_resonant:
    case EngineId::kick_circuit:
        // Both bodies take their pitch from a fixed resonator or from the
        // bridged-T network, neither of which is retuned per hit.
        return {false, false};
    case EngineId::hat:
    case EngineId::clap:
    case EngineId::cymbal_comb:
    case EngineId::fm6:
    case EngineId::fm8:
        // Struck-metal and multi-operator voices answer velocity with level
        // and brightness; none of them bends pitch.
        return {false, false};
    case EngineId::dx7_msfa:
        break;
    }
    return {};
}

inline void add_common_params(CustomNodeType& type, EngineId id) {
    add(type, kTrigger, 0.0f, 1.0f, 0.0f);
    add(type, kVelocity, 0.0f, 1.0f, 1.0f);
    add(type, kChoke, 0.0f, 1.0f, 0.0f);
    add(type, kChokeMs, 0.01f, 100.0f, 4.0f);
    const auto response = default_velocity_response(id);
    const auto axes = velocity_axes(id);
    add(type, kVelocityLevelDb, 0.0f, 48.0f, response.level_db);
    if (axes.bend)
        add(type, kVelocityBendOctaves, -6.0f, 6.0f, response.bend_octaves);
    add(type, kVelocityBrightnessOctaves, -6.0f, 6.0f, response.brightness_octaves);
    if (axes.noise_balance)
        add(type, kVelocityNoiseBalance, 0.0f, 1.0f, response.noise_balance);
    add(type, kOutputDrive, 0.0f, 1.0f, 0.0f);
    add(type, kOutputFold, 0.0f, 1.0f, 0.0f);
    add(type, kOutputLevel, 0.0f, 4.0f, 1.0f);
    add(type, kOutputAhdEnabled, 0.0f, 1.0f, 0.0f);
    add(type, kOutputAttackMs, 0.0f, 2000.0f, 0.0f);
    add(type, kOutputHoldMs, 0.0f, 2000.0f, 0.0f);
    add(type, kOutputDecayMs, 0.01f, 8000.0f, 1000.0f);
    add(type, kOutputBits, 1.0f, 24.0f, 24.0f);
    add(type, kOutputHoldRateHz, 1.0f, kHoldRateBypassHz, kHoldRateBypassHz);
    add(type, kOutputJitter, 0.0f, 1.0f, 0.0f);
    add(type, kOutputSmoothing, 0.0f, 1.0f, 0.0f);
    add(type, kOutputDeadZone, 0.0f, 0.9f, 0.0f);
}

inline void add_gate_params(CustomNodeType& type) {
    add(type, kGateRiseMs, 0.01f, 2000, 2);
    add(type, kGateFallMs, 0.01f, 8000, 200);
    add(type, kGateColour, 0, 1, 0.5f);
    add(type, kGateClosedHz, 10, 20000, 60);
    add(type, kGateOpenHz, 10, 20000, 12000);
    add(type, kGateGainExponent, 0.25f, 4, 1.5f);
}

inline void add_lofi_params(CustomNodeType& type, state::ParamID base) {
    add(type, base, 1, 24, 24);
    add(type, base + 1, 1, kHoldRateBypassHz, kHoldRateBypassHz);
    add(type, base + 2, 0, 1, 0);
    add(type, base + 3, 0, 1, 0);
    add(type, base + 4, 0, 0.9f, 0);
}

inline void add_voice_params(CustomNodeType& type, EngineId id) {
    switch (id) {
    case EngineId::kick_oscillator:
    case EngineId::kick_resonant:
    case EngineId::kick_circuit:
        // The three kick bodies do not share a tuning surface. The oscillator
        // body is swept explicitly; the resonator is struck at a fixed
        // frequency; the bridged-T network produces its own pitch and decay
        // from its component values, which are declared below. Declaring a
        // control on a body that cannot consume it would ship a dead knob.
        if (id != EngineId::kick_circuit) {
            add(type, kTuneHz, 20, 400, 55);
            add(type, kDecay, 10, 4000, 400);
        }
        if (id == EngineId::kick_oscillator) {
            add(type, kPitchSweepOctaves, 0, 6, 2);
            add(type, kPitchSweepMs, 0.5f, 500, 30);
        }
        add(type, kNoiseColor, 0, 4, 0);
        add(type, kKickClickLevel, 0, 2, 0.3f);
        add(type, kKickClickToneHz, 20, 20000, 4000);
        add(type, kKickClickDecayMs, 0.05f, 200, 2);
        add(type, kKickNoiseLevel, 0, 2, 0);
        add(type, kKickNoiseDecayMs, 0.5f, 4000, 60);
        add(type, kKickSubLevel, 0, 2, 0);
        if (id == EngineId::kick_oscillator) {
            add(type, kKickTriangle, 0, 1, 0);
            add(type, kKickFmAmount, 0, 8, 0);
            add(type, kKickFmRatio, 0.25f, 16, 1);
        }
        if (id == EngineId::kick_circuit) {
            add(type, kKickCircuitFeedback, 0, 1, 0.85f);
            add(type, kKickCircuitDrive, 0, 1, 0.3f);
            add(type, kKickCircuitAttackMs, 0, 50, 4);
            add(type, kKickCircuitPulseMs, 0.2f, 20, 2);
            add(type, kKickCircuitSigh, 0, 1, 1);
            add(type, kKickCircuitHitLife, 0, 2, 2);
            add(type, kCircuitC41, 1.0e-10f, 1.0e-6f, 15.0e-9f);
            add(type, kCircuitC42, 1.0e-10f, 1.0e-6f, 15.0e-9f);
            add(type, kCircuitR161, 100, 1.0e7f, 1.0e6f);
            add(type, kCircuitR165, 100, 1.0e7f, 47.0e3f);
            add(type, kCircuitR166, 100, 1.0e7f, 6.8e3f);
            add(type, kCircuitR167, 100, 1.0e7f, 1.0e6f);
            add(type, kCircuitR170, 100, 1.0e7f, 470.0e3f);
        }
        break;
    case EngineId::snare:
        add(type, kTuneHz, 60, 800, 180);
        add(type, kDecay, 5, 2000, 120);
        add(type, kPitchSweepOctaves, 0, 4, 0.5f);
        add(type, kPitchSweepMs, 0.5f, 300, 25);
        add(type, kNoiseColor, 0, 4, 0);
        add(type, kSnareToneRatio, 1, 4, 1.6f);
        add(type, kSnareToneLevel, 0, 2, 0.5f);
        add(type, kSnareFmAmount, 0, 8, 0);
        add(type, kSnareRing, 0, 1, 0);
        add(type, kSnareNoiseLevel, 0, 2, 0.6f);
        add(type, kSnareNoiseDecayMs, 5, 3000, 180);
        add(type, kSnareNoiseCutoffHz, 100, 18000, 3000);
        add(type, kSnareNoiseResonance, 0.5f, 12, 1);
        add(type, kSnareNoiseSweepOctaves, -4, 4, 0);
        add(type, kSnareRattle, 0, 1, 0);
        add(type, kSnareRattleHz, 5, 400, 45);
        add(type, kSnareSnapLevel, 0, 2, 0.4f);
        add(type, kSnareSnapCutoffHz, 20, 20000, 6000);
        add(type, kSnareSnapDecayMs, 0.05f, 200, 4);
        add(type, kSnareShellLevel, 0, 2, 0);
        add(type, kSnareShellResonance, 1, 30, 12);
        add_lofi_params(type, kToneLofiBits);
        add_lofi_params(type, kNoiseLofiBits);
        break;
    case EngineId::hat:
        add(type, kTuneHz, 40, 4000, 320);
        add(type, kDecay, 5, 8000, 60);
        add(type, kNoiseColor, 0, 4, 0);
        add(type, kHatSpread, 0, 1, 1);
        add(type, kHatMetal, 0, 1, 0.8f);
        add(type, kHatGrit, 0, 1, 0);
        add(type, kHatGritRatio, 0.25f, 16, 1.4f);
        add(type, kHatCutoffHz, 200, 18000, 7000);
        add(type, kHatResonance, 0.5f, 12, 1.2f);
        add(type, kHatBandpass, 0, 1, 0);
        break;
    case EngineId::clap:
        add(type, kNoiseColor, 0, 4, 0);
        add(type, kClapBurstCount, 1, 8, 4);
        add(type, kClapBurstSpacingMs, 1, 60, 11);
        add(type, kClapBurstDecayMs, 0.5f, 60, 6);
        add(type, kClapBurstFalloff, 0.2f, 1.5f, 0.82f);
        add(type, kClapGapJitter, 0, 1, 0.35f);
        add(type, kClapAlternatePolarity, 0, 1, 0);
        add(type, kClapStereoWidth, 0, 1, 1);
        add(type, kClapTailLevel, 0, 2, 0.35f);
        add(type, kClapTailDecayMs, 10, 3000, 180);
        add(type, kClapCutoffHz, 200, 16000, 1400);
        add(type, kClapResonance, 0.5f, 12, 1.4f);
        add(type, kClapBodyLevel, 0, 2, 0);
        add(type, kClapBodyHz, 40, 2000, 300);
        break;
    case EngineId::tom_generic:
    case EngineId::tom_simmons:
        add(type, kTuneHz, 30, 1200, id == EngineId::tom_generic ? 130 : 75);
        add(type, kDecay, 10, 4000, id == EngineId::tom_generic ? 450 : 700);
        add(type, kPitchSweepOctaves, 0, 6, id == EngineId::tom_generic ? .35f : .8f);
        add(type, kPitchSweepMs, 0.5f, 500, id == EngineId::tom_generic ? 30 : 45);
        add(type, kNoiseColor, 0, 4, 0);
        add(type, kTomWave, 0, 1, 0);
        add(type, kTomNoiseBalance, 0, 1, id == EngineId::tom_generic ? .25f : .08f);
        add(type, kTomNoiseCutoffHz, 50, 16000, id == EngineId::tom_generic ? 2200 : 1100);
        add(type, kTomNoiseResonance, 0, 1, .25f);
        add(type, kTomClickLevel, 0, 2, id == EngineId::tom_generic ? .35f : .30f);
        add(type, kTomClickCutoffHz, 20, 20000, 4000);
        add(type, kTomClickDecayMs, 0.05f, 200, 2);
        add_lofi_params(type, kToneLofiBits);
        add_lofi_params(type, kNoiseLofiBits);
        break;
    case EngineId::membrane_modal:
        add(type, kTuneHz, 20, 2000, 90);
        add(type, kDecay, 20, 8000, 700);
        add(type, kMembraneStructure, 0, 1, 1);
        add(type, kMembraneStretch, 0, 1, 0);
        add(type, kMembraneDamping, 0, 1, 0.5f);
        add(type, kMembraneBrightness, 0, 1, 0.5f);
        add(type, kMembranePosition, 0.02f, 0.5f, 0.28f);
        add(type, kMembraneSpread, 0, 1, 0.1f);
        add(type, kMembraneExciterMs, 0.1f, 50, 1.5f);
        add(type, kMembraneExciterCutoffHz, 100, 18000, 6000);
        add(type, kMembraneExciter, 0, 1, 0);
        add(type, kMembraneSubLevel, 0, 2, 0);
        add(type, kMembraneAirLevel, 0, 2, 0);
        add(type, kMembraneAirDecayMs, 1, 500, 20);
        add(type, kMembraneClickLevel, 0, 2, 0);
        add(type, kMembraneClickDecayMs, 0.1f, 50, 2);
        add_gate_params(type);
        break;
    case EngineId::cymbal_comb:
        add(type, kTuneHz, 40, 2000, 320);
        add(type, kDecay, 50, 8000, 1800);
        add(type, kNoiseColor, 0, 4, 0);
        add(type, kCymbalDecayTilt, 0.5f, 1, 0.93f);
        add(type, kCymbalHighModeEmphasisDb, -18, 18, 0);
        add(type, kCymbalVelocityFeedback, 0, 1, 0.35f);
        add(type, kCymbalVelocityHighModeDb, 0, 18, 4);
        add(type, kCymbalUpperHighpassHz, 0, 8000, 500);
        add(type, kCymbalSpread, 0, 1, 1);
        add(type, kCymbalInharmonicity, 0, 1, 0.4f);
        add(type, kCymbalShiftHz, -400, 400, 45);
        add(type, kCymbalNoiseLevel, 0, 2, 0.7f);
        add(type, kCymbalStrikeLevel, 0, 2, 0.3f);
        add(type, kCymbalStrikeMs, 0.5f, 200, 6);
        add(type, kCymbalToneHz, 500, 20000, 12000);
        add(type, kCymbalLowCutHz, 20, 2000, 300);
        // Body-preserving by default, matching CymbalVoice: a cymbal that
        // reset its body on every hit would cut its own ring off.
        add(type, kCymbalHitLife, 0, 4, 4);
        break;
    case EngineId::string_karplus_strong:
        add(type, kTuneHz, 30, 4000, 220);
        add(type, kDecay, 0.05f, 20, 2);
        add(type, kNoiseColor, 0, 4, 0);
        add(type, kStringDamping, 0, 1, 0.3f);
        add(type, kStringStiffness, 0, 1, 0);
        add(type, kStringPluckPosition, 0, 0.5f, 0.25f);
        add(type, kStringExciterMs, 0.1f, 50, 1);
        add(type, kStringBrightnessHz, 200, 18000, 1800);
        add(type, kStringPickDirection, 0, 0.99f, 0);
        add(type, kStringRestartOnHit, 0, 1, 0);
        add(type, kStringModulation, 0, 3, 0);
        add(type, kStringModulationMix, 0, 1, 0);
        add(type, kStringModulationRatio, 0.125f, 16, 2);
        add(type, kStringFmDepthOctaves, 0, 2, 0.25f);
        add(type, kStringLpgAmount, 0, 1, 0);
        add_gate_params(type);
        break;
    case EngineId::zap_cz:
        add(type, kTuneHz, 20, 4000, 220);
        add(type, kDecay, 10, 4000, 350);
        add(type, kPitchSweepOctaves, 0, 6, 2.5f);
        add(type, kPitchSweepMs, 0.5f, 500, 35);
        add(type, kZapShape, 0, 4, 2);
        add(type, kZapDistortion, 0, 1, 0.8f);
        add(type, kZapDistortionMs, 0.5f, 500, 60);
        add(type, kZapResonantDepth, 1, 32, 12);
        add(type, kZapDetuneCents, 0, 100, 9);
        add(type, kZapRing, 0, 1, 0);
        add(type, kZapRingRatio, 0.25f, 16, 1.5f);
        add_gate_params(type);
        break;
    case EngineId::fm2:
        add(type, kTuneHz, 20, 4000, 110);
        add(type, kDecay, 10, 4000, 400);
        add(type, kPitchSweepOctaves, 0, 6, 0);
        add(type, kPitchSweepMs, 0.5f, 500, 30);
        add(type, kNoiseColor, 0, 4, 0);
        add(type, kFm2Ratio, 0.1f, 24, 1.41f);
        add(type, kFm2Index, 0, 24, 6);
        add(type, kFm2IndexMs, 0.5f, 2000, 40);
        add(type, kFm2Feedback, 0, 1, 0);
        add(type, kFm2CarrierWave, 0, 25, 0);
        add(type, kFm2ModulatorWave, 0, 25, 0);
        add(type, kFm2CarrierWarp, 0, 1, 0);
        add(type, kFm2ModulatorWarp, 0, 1, 0);
        add(type, kFm2CarrierWarpMs, 0.5f, 2000, 40);
        add(type, kFm2ModulatorWarpMs, 0.5f, 2000, 40);
        add(type, kFm2LfoRateHz, 0.05f, 40, 5);
        add(type, kFm2LfoDepthOctaves, 0, 2, 0);
        add(type, kFm2LfoDelayMs, 0, 4000, 0);
        add(type, kFm2LfoFadeMs, 0, 4000, 0);
        add(type, kFm2HardSync, 0, 1, 0);
        add(type, kFm2Transient, -1, 23, -1);
        add(type, kFm2NoiseLevel, 0, 2, 0);
        add(type, kFm2NoiseDecayMs, 0.5f, 500, 10);
        add(type, kFm2CutoffHz, 40, 18000, 12000);
        add(type, kFm2Resonance, 0.5f, 12, 0.8f);
        add(type, kFm2Bandpass, 0, 1, 0);
        add(type, kFm2ClickLevel, 0, 2, 0.2f);
        add(type, kFm2ClickCutoffHz, 20, 20000, 6000);
        break;
    case EngineId::fm6:
    case EngineId::fm8: {
        const int count = id == EngineId::fm6 ? 6 : 8;
        add(type, kTuneHz, 20, 4000, 110);
        add(type, kFmAlgorithm, 0, id == EngineId::fm6 ? 31 : 15, id == EngineId::fm6 ? 4 : 9);
        add(type, kFmDepth, 0, 12, 3);
        add(type, kFmFormantHz, 40, 18000, id == EngineId::fm6 ? 3000 : 2000);
        add(type, kFmFormantQ, 0.5f, 12, id == EngineId::fm6 ? 0.9f : 1);
        if (id == EngineId::fm6) {
            add(type, kPitchSweepOctaves, -6, 6, 0);
            add(type, kPitchSweepMs, 0.5f, 1000, 40);
            add(type, kFm6Feedback, 0, 1, 0);
        } else {
            add(type, kNoiseColor, 0, 4, 0);
            add(type, kFm8Transient, -1, 23, -1);
            add(type, kFm8NoiseLevel, 0, 2, 0);
            add(type, kFm8NoiseDecayMs, 0.5f, 500, 10);
            add(type, kFm8ClickLevel, 0, 2, 0);
        }
        constexpr float ratios6[] = {1, 1, 2, 3, 4, 7};
        constexpr float levels6[] = {1, .8f, .7f, .6f, .5f, .4f};
        constexpr float decays6[] = {400, 260, 180, 120, 80, 50};
        constexpr float ratios8[] = {1, 1.41f, 2, 3.17f, 4, 5.63f, 7, 9.41f};
        constexpr float levels8[] = {1, .8f, .7f, .6f, .5f, .4f, .35f, .3f};
        constexpr float decays8[] = {400, 220, 160, 110, 80, 60, 45, 30};
        for (int op = 0; op < count; ++op) {
            add(type, kOperatorRatioBase + op, 0.1f, 32,
                id == EngineId::fm6 ? ratios6[op] : ratios8[op]);
            add(type, kOperatorLevelBase + op, 0, 1,
                id == EngineId::fm6 ? levels6[op] : levels8[op]);
            add(type, kOperatorDecayBase + op, 1, 4000,
                id == EngineId::fm6 ? decays6[op] : decays8[op]);
            if (id == EngineId::fm8) {
                add(type, kOperatorFeedbackBase + op, 0, 1, 0);
                add(type, kOperatorWaveBase + op, 0, 25, 0);
            }
        }
        break;
    }
    case EngineId::dx7_msfa:
        break;
    }
}

inline CustomNodeType make_drum_node_impl(EngineId id) {
    const auto metadata =
        std::find_if(signal::drum::engine_registry.begin(), signal::drum::engine_registry.end(),
                     [id](const auto& item) { return item.id == id; });
    CustomNodeType type;
    if (metadata == signal::drum::engine_registry.end() || !metadata->available)
        return type;
    type.type_id = type_id(id);
    type.version = 1;
    type.num_input_ports = 0;
    type.num_output_ports = 2;
    type.default_name = std::string(metadata->display_name);
    type.lowerable = true;
    type.destroy = [](void* p) { delete static_cast<DrumInstance*>(p); };
    type.prepare = [](void* p, double sample_rate, int /*max_block*/) {
        auto& instance = *static_cast<DrumInstance*>(p);
        instance.voice->set_output_oversampling(signal::drum::OutputOversampling::bypass);
        instance.voice->prepare(sample_rate);
    };
    type.reset = [](void* p) {
        auto& instance = *static_cast<DrumInstance*>(p);
        instance.voice->reset();
        instance.trigger_high = false;
        instance.choke_high = false;
    };
    add_common_params(type, id);
    add_voice_params(type, id);

    // The declaration is fixed once the node type is built, so the dense
    // projection is built here -- off the audio thread -- and shared by every
    // instance of the type.
    auto table = std::make_shared<const DrumParamTable>(type.baked_params);
    std::vector<std::uint16_t> voice_param_slots;
    for (const auto& parameter : type.baked_params) {
        if ((parameter.id >= kTuneHz && parameter.id <= kNoiseColor) || parameter.id >= kGateRiseMs)
            voice_param_slots.push_back(table->slot(parameter.id));
    }
    type.create = [id, table, voice_param_slots]() -> void* {
        return new DrumInstance(id, table, voice_param_slots);
    };
    type.process_instance_baked_param = [](void* opaque, audio::BufferView<float>& output,
                                           const audio::BufferView<const float>& /*input*/,
                                           int num_samples, const BakedParamView& params) {
        auto& instance = *static_cast<DrumInstance*>(opaque);
        float* left = output.channel_ptr(0);
        float* right = output.channel_ptr(1);
        for (int sample = 0; sample < num_samples; ++sample) {
            const auto offset = static_cast<std::int32_t>(sample);
            apply_common(instance, params, offset);
            if (voice_values_changed(instance, params, offset))
                apply_voice(instance, params, offset);
            const bool trigger = params.value_at(kTrigger, offset) >= 0.5f;
            if (trigger && !instance.trigger_high)
                instance.voice->note_on(params.value_at(kVelocity, offset));
            instance.trigger_high = trigger;
            const bool choke = params.value_at(kChoke, offset) >= 0.5f;
            if (choke && !instance.choke_high)
                instance.voice->choke(params.value_at(kChokeMs, offset));
            instance.choke_high = choke;
            left[static_cast<std::size_t>(sample)] = 0.0f;
            right[static_cast<std::size_t>(sample)] = 0.0f;
            instance.voice->process_stereo(left + sample, right + sample, 1);
        }
    };
    return type;
}

} // namespace pulp::host::forge_drum::detail
