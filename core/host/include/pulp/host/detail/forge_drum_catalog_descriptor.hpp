#pragma once

#include <pulp/host/detail/forge_drum_catalog_impl.hpp>
#include <pulp/host/forge_param_descriptor.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <utility>
#include <vector>

namespace pulp::host::forge_drum::detail {
namespace descriptor {

inline ForgeParamDescriptor p(std::string_view key, state::ParamID id, std::string_view label,
                              std::string_view unit, std::string_view description,
                              ForgeParamKind kind = ForgeParamKind::continuous,
                              ForgeParamCurve curve = ForgeParamCurve::linear,
                              std::vector<ForgeParamChoice> choices = {}) {
    return {key, id, label, unit, description, kind, curve, std::move(choices)};
}

inline std::vector<ForgeParamChoice> off_on() {
    return {{"off", "Off", 0.0f}, {"on", "On", 1.0f}};
}

inline std::vector<ForgeParamDescriptor> common() {
    using K = ForgeParamKind;
    using C = ForgeParamCurve;
    return {
        p("trigger", kTrigger, "Trigger", "", "Starts a new drum hit.", K::stepped, C::linear,
          off_on()),
        p("velocity", kVelocity, "Velocity", "%",
          "Strike velocity driving the voice's level and timbre response."),
        p("choke", kChoke, "Choke", "", "Stops the current voice.", K::stepped, C::linear,
          off_on()),
        p("choke_ms", kChokeMs, "Choke Time", "ms", "Fade time used to choke the voice.",
          K::continuous, C::logarithmic),
        p("velocity_level_db", kVelocityLevelDb, "Velocity Level", "dB",
          "Level range controlled by strike velocity."),
        p("velocity_bend_oct", kVelocityBendOctaves, "Velocity Bend", "oct",
          "Pitch-bend range controlled by strike velocity."),
        p("velocity_brightness_oct", kVelocityBrightnessOctaves, "Velocity Brightness", "oct",
          "Brightness shift controlled by strike velocity."),
        p("velocity_noise_balance", kVelocityNoiseBalance, "Velocity Noise Balance", "%",
          "Velocity-driven balance between tonal and noise components."),
        p("tune_hz", kTuneHz, "Tune", "Hz", "Fundamental pitch or body tuning.", K::continuous,
          C::logarithmic),
        p("decay", kDecay, "Decay", "ms", "Primary voice decay time.", K::continuous,
          C::logarithmic),
        p("pitch_sweep_oct", kPitchSweepOctaves, "Pitch Sweep", "oct", "Pitch-envelope excursion."),
        p("pitch_sweep_ms", kPitchSweepMs, "Pitch Sweep Time", "ms", "Pitch-envelope decay time.",
          K::continuous, C::logarithmic),
        p("noise_color", kNoiseColor, "Noise Color", "", "Spectral character of the noise source."),
        p("output_drive", kOutputDrive, "Output Drive", "%",
          "Saturation in the shared output stage."),
        p("output_fold", kOutputFold, "Output Fold", "%",
          "Wavefolding in the shared output stage."),
        p("output_level", kOutputLevel, "Output Level", "x", "Final linear output gain."),
        p("output_envelope", kOutputAhdEnabled, "Output Envelope", "",
          "Enables the attack-hold-decay output envelope.", K::stepped, C::linear, off_on()),
        p("output_attack_ms", kOutputAttackMs, "Output Attack", "ms",
          "Attack time of the output envelope.", K::continuous, C::logarithmic),
        p("output_hold_ms", kOutputHoldMs, "Output Hold", "ms",
          "Hold time of the output envelope."),
        p("output_decay_ms", kOutputDecayMs, "Output Decay", "ms",
          "Decay time of the output envelope.", K::continuous, C::logarithmic),
        p("output_bits", kOutputBits, "Output Bits", "bit",
          "Amplitude resolution of the output reducer."),
        p("output_hold_rate_hz", kOutputHoldRateHz, "Output Hold Rate", "Hz",
          "Sample-update rate of the output reducer.", K::continuous, C::logarithmic),
        p("output_jitter", kOutputJitter, "Output Jitter", "%",
          "Timing instability in the output reducer."),
        p("output_smoothing", kOutputSmoothing, "Output Smoothing", "%",
          "Interpolation between held output samples."),
        p("output_dead_zone", kOutputDeadZone, "Output Dead Zone", "%",
          "Low-level quantizer dead zone."),
        p("gate_rise_ms", kGateRiseMs, "Gate Rise", "ms", "Opening time of the low-pass gate.",
          K::continuous, C::logarithmic),
        p("gate_fall_ms", kGateFallMs, "Gate Fall", "ms", "Closing time of the low-pass gate.",
          K::continuous, C::logarithmic),
        p("gate_color", kGateColour, "Gate Color", "%",
          "Balance of gain and cutoff movement in the low-pass gate."),
        p("gate_closed_hz", kGateClosedHz, "Gate Closed", "Hz",
          "Low-pass cutoff while the gate is closed.", K::continuous, C::logarithmic),
        p("gate_open_hz", kGateOpenHz, "Gate Open", "Hz", "Low-pass cutoff while the gate is open.",
          K::continuous, C::logarithmic),
        p("gate_gain_exponent", kGateGainExponent, "Gate Gain Curve", "",
          "Curvature of the gate's amplitude response."),
        p("tone_bits", kToneLofiBits, "Tone Bits", "bit",
          "Amplitude resolution of the tonal path."),
        p("tone_hold_rate_hz", kToneLofiHoldRateHz, "Tone Hold Rate", "Hz",
          "Sample-update rate of the tonal path.", K::continuous, C::logarithmic),
        p("tone_jitter", kToneLofiJitter, "Tone Jitter", "%",
          "Timing instability in the tonal reducer."),
        p("tone_smoothing", kToneLofiSmoothing, "Tone Smoothing", "%",
          "Interpolation between held tonal samples."),
        p("tone_dead_zone", kToneLofiDeadZone, "Tone Dead Zone", "%",
          "Low-level quantizer dead zone in the tonal path."),
        p("noise_bits", kNoiseLofiBits, "Noise Bits", "bit",
          "Amplitude resolution of the noise path."),
        p("noise_hold_rate_hz", kNoiseLofiHoldRateHz, "Noise Hold Rate", "Hz",
          "Sample-update rate of the noise path.", K::continuous, C::logarithmic),
        p("noise_jitter", kNoiseLofiJitter, "Noise Jitter", "%",
          "Timing instability in the noise reducer."),
        p("noise_smoothing", kNoiseLofiSmoothing, "Noise Smoothing", "%",
          "Interpolation between held noise samples."),
        p("noise_dead_zone", kNoiseLofiDeadZone, "Noise Dead Zone", "%",
          "Low-level quantizer dead zone in the noise path."),
        p("circuit_c41", kCircuitC41, "C41", "F", "Bridged-T capacitor C41.", K::continuous,
          C::logarithmic),
        p("circuit_c42", kCircuitC42, "C42", "F", "Bridged-T capacitor C42.", K::continuous,
          C::logarithmic),
        p("circuit_r161", kCircuitR161, "R161", "Ohm", "Bridged-T resistor R161.", K::continuous,
          C::logarithmic),
        p("circuit_r165", kCircuitR165, "R165", "Ohm", "Bridged-T resistor R165.", K::continuous,
          C::logarithmic),
        p("circuit_r166", kCircuitR166, "R166", "Ohm", "Bridged-T resistor R166.", K::continuous,
          C::logarithmic),
        p("circuit_r167", kCircuitR167, "R167", "Ohm", "Bridged-T resistor R167.", K::continuous,
          C::logarithmic),
        p("circuit_r170", kCircuitR170, "R170", "Ohm", "Bridged-T resistor R170.", K::continuous,
          C::logarithmic),
    };
}

inline std::vector<ForgeParamDescriptor> kick(EngineId id) {
    auto out = std::vector<ForgeParamDescriptor>{
        p("click_level", kKickClickLevel, "Click Level", "%", "Level of the attack click."),
        p("click_tone_hz", kKickClickToneHz, "Click Tone", "Hz", "Cutoff shaping the attack click.",
          ForgeParamKind::continuous, ForgeParamCurve::logarithmic),
        p("click_decay_ms", kKickClickDecayMs, "Click Decay", "ms", "Decay of the attack click.",
          ForgeParamKind::continuous, ForgeParamCurve::logarithmic),
        p("noise_level", kKickNoiseLevel, "Noise Level", "%", "Level of the noise body."),
        p("noise_decay_ms", kKickNoiseDecayMs, "Noise Decay", "ms", "Decay of the noise body.",
          ForgeParamKind::continuous, ForgeParamCurve::logarithmic),
        p("sub_level", kKickSubLevel, "Sub Level", "%", "Level of the sub-frequency body."),
    };
    if (id == EngineId::kick_oscillator) {
        out.push_back(
            p("triangle", kKickTriangle, "Triangle", "%", "Blend toward a triangle-wave body."));
        out.push_back(
            p("fm_amount", kKickFmAmount, "FM Amount", "", "Frequency-modulation depth."));
        out.push_back(p("fm_ratio", kKickFmRatio, "FM Ratio", ":1",
                        "Frequency ratio of the modulator.", ForgeParamKind::continuous,
                        ForgeParamCurve::logarithmic));
    } else if (id == EngineId::kick_circuit) {
        out.push_back(p("circuit_feedback", kKickCircuitFeedback, "Circuit Feedback", "%",
                        "Feedback within the bridged-T network."));
        out.push_back(p("circuit_drive", kKickCircuitDrive, "Circuit Drive", "%",
                        "Nonlinear drive in the bridged-T network."));
        out.push_back(p("circuit_attack_ms", kKickCircuitAttackMs, "Circuit Attack", "ms",
                        "Attack contour of the circuit excitation."));
        out.push_back(p("circuit_pulse_ms", kKickCircuitPulseMs, "Trigger Pulse", "ms",
                        "Width of the circuit trigger pulse."));
        out.push_back(p("circuit_sigh", kKickCircuitSigh, "Sigh", "",
                        "Enables the long pitch relaxation.", ForgeParamKind::stepped,
                        ForgeParamCurve::linear, off_on()));
        out.push_back(p("hit_life", kKickCircuitHitLife, "Hit Life", "",
                        "How a strike interacts with the ringing circuit."));
    }
    return out;
}

} // namespace descriptor

namespace descriptor {

inline std::vector<ForgeParamDescriptor> snare();
inline std::vector<ForgeParamDescriptor> hat();
inline std::vector<ForgeParamDescriptor> clap();
inline std::vector<ForgeParamDescriptor> tom();
inline std::vector<ForgeParamDescriptor> membrane();
inline std::vector<ForgeParamDescriptor> cymbal();
inline std::vector<ForgeParamDescriptor> string_voice();
inline std::vector<ForgeParamDescriptor> zap();

inline std::vector<ForgeParamDescriptor> fm2() {
    using K = ForgeParamKind;
    using C = ForgeParamCurve;
    return {
        p("ratio", kFm2Ratio, "Modulator Ratio", ":1", "Modulator-to-carrier frequency ratio.",
          K::continuous, C::logarithmic),
        p("index", kFm2Index, "FM Index", "", "Initial frequency-modulation index."),
        p("index_ms", kFm2IndexMs, "Index Decay", "ms", "Decay time of the modulation index.",
          K::continuous, C::logarithmic),
        p("feedback", kFm2Feedback, "Feedback", "%", "Modulator self-feedback."),
        p("carrier_wave", kFm2CarrierWave, "Carrier Wave", "", "Carrier wavetable selection."),
        p("modulator_wave", kFm2ModulatorWave, "Modulator Wave", "",
          "Modulator wavetable selection."),
        p("carrier_warp", kFm2CarrierWarp, "Carrier Warp", "%", "Phase-warp depth of the carrier."),
        p("modulator_warp", kFm2ModulatorWarp, "Modulator Warp", "%",
          "Phase-warp depth of the modulator."),
        p("carrier_warp_ms", kFm2CarrierWarpMs, "Carrier Warp Decay", "ms",
          "Decay time of carrier phase warp.", K::continuous, C::logarithmic),
        p("modulator_warp_ms", kFm2ModulatorWarpMs, "Modulator Warp Decay", "ms",
          "Decay time of modulator phase warp.", K::continuous, C::logarithmic),
        p("lfo_rate_hz", kFm2LfoRateHz, "LFO Rate", "Hz", "Rate of the pitch LFO.", K::continuous,
          C::logarithmic),
        p("lfo_depth_oct", kFm2LfoDepthOctaves, "LFO Depth", "oct", "Pitch excursion of the LFO."),
        p("lfo_delay_ms", kFm2LfoDelayMs, "LFO Delay", "ms",
          "Delay before pitch modulation begins."),
        p("lfo_fade_ms", kFm2LfoFadeMs, "LFO Fade", "ms", "Fade-in time of pitch modulation."),
        p("hard_sync", kFm2HardSync, "Hard Sync", "", "Synchronizes the modulator to the carrier.",
          K::stepped, C::linear, off_on()),
        p("transient", kFm2Transient, "Transient", "", "Selects the transient excitation model."),
        p("noise_level", kFm2NoiseLevel, "Noise Level", "%", "Level of the noise transient."),
        p("noise_decay_ms", kFm2NoiseDecayMs, "Noise Decay", "ms",
          "Decay time of the noise transient.", K::continuous, C::logarithmic),
        p("cutoff_hz", kFm2CutoffHz, "Cutoff", "Hz", "Cutoff of the output filter.", K::continuous,
          C::logarithmic),
        p("resonance", kFm2Resonance, "Resonance", "Q", "Resonance of the output filter.",
          K::continuous, C::logarithmic),
        p("bandpass", kFm2Bandpass, "Band-Pass", "", "Selects a band-pass output response.",
          K::stepped, C::linear, off_on()),
        p("click_level", kFm2ClickLevel, "Click Level", "%", "Level of the attack click."),
        p("click_cutoff_hz", kFm2ClickCutoffHz, "Click Cutoff", "Hz",
          "Filter cutoff of the attack click.", K::continuous, C::logarithmic),
    };
}

inline std::vector<ForgeParamDescriptor> fm_many(EngineId id) {
    using K = ForgeParamKind;
    using C = ForgeParamCurve;
    auto out = std::vector<ForgeParamDescriptor>{
        p("algorithm", kFmAlgorithm, "Algorithm", "",
          "Routing algorithm connecting the FM operators."),
        p("depth", kFmDepth, "FM Depth", "", "Global modulation depth."),
        p("formant_hz", kFmFormantHz, "Formant", "Hz", "Center frequency of the output formant.",
          K::continuous, C::logarithmic),
        p("formant_q", kFmFormantQ, "Formant Q", "Q", "Resonance of the output formant.",
          K::continuous, C::logarithmic),
    };
    if (id == EngineId::fm6) {
        out.push_back(p("feedback", kFm6Feedback, "Feedback", "%",
                        "Feedback around the algorithm's designated operator."));
    } else {
        out.push_back(p("transient", kFm8Transient, "Transient", "",
                        "Selects the transient excitation model."));
        out.push_back(
            p("noise_level", kFm8NoiseLevel, "Noise Level", "%", "Level of the noise transient."));
        out.push_back(p("noise_decay_ms", kFm8NoiseDecayMs, "Noise Decay", "ms",
                        "Decay time of the noise transient.", K::continuous, C::logarithmic));
        out.push_back(
            p("click_level", kFm8ClickLevel, "Click Level", "%", "Level of the attack click."));
    }

    static constexpr std::array<std::string_view, 8> ratio_keys = {
        "operator_1_ratio", "operator_2_ratio", "operator_3_ratio", "operator_4_ratio",
        "operator_5_ratio", "operator_6_ratio", "operator_7_ratio", "operator_8_ratio"};
    static constexpr std::array<std::string_view, 8> level_keys = {
        "operator_1_level", "operator_2_level", "operator_3_level", "operator_4_level",
        "operator_5_level", "operator_6_level", "operator_7_level", "operator_8_level"};
    static constexpr std::array<std::string_view, 8> decay_keys = {
        "operator_1_decay_ms", "operator_2_decay_ms", "operator_3_decay_ms", "operator_4_decay_ms",
        "operator_5_decay_ms", "operator_6_decay_ms", "operator_7_decay_ms", "operator_8_decay_ms"};
    static constexpr std::array<std::string_view, 8> feedback_keys = {
        "operator_1_feedback", "operator_2_feedback", "operator_3_feedback", "operator_4_feedback",
        "operator_5_feedback", "operator_6_feedback", "operator_7_feedback", "operator_8_feedback"};
    static constexpr std::array<std::string_view, 8> wave_keys = {
        "operator_1_wave", "operator_2_wave", "operator_3_wave", "operator_4_wave",
        "operator_5_wave", "operator_6_wave", "operator_7_wave", "operator_8_wave"};
    static constexpr std::array<std::string_view, 8> labels = {
        "Operator 1", "Operator 2", "Operator 3", "Operator 4",
        "Operator 5", "Operator 6", "Operator 7", "Operator 8"};

    const int count = id == EngineId::fm6 ? 6 : 8;
    for (int op = 0; op < count; ++op) {
        const auto index = static_cast<std::size_t>(op);
        out.push_back(p(ratio_keys[index], kOperatorRatioBase + op, labels[index], ":1",
                        "Frequency ratio of this operator.", K::continuous, C::logarithmic));
        out.push_back(p(level_keys[index], kOperatorLevelBase + op, labels[index], "%",
                        "Output level of this operator."));
        out.push_back(p(decay_keys[index], kOperatorDecayBase + op, labels[index], "ms",
                        "Envelope decay time of this operator.", K::continuous, C::logarithmic));
        if (id == EngineId::fm8) {
            out.push_back(p(feedback_keys[index], kOperatorFeedbackBase + op, labels[index], "%",
                            "Self-feedback of this operator."));
            out.push_back(p(wave_keys[index], kOperatorWaveBase + op, labels[index], "",
                            "Wavetable selection for this operator."));
        }
    }
    return out;
}

inline std::vector<ForgeParamDescriptor> controls(EngineId id) {
    switch (id) {
    case EngineId::kick_oscillator:
    case EngineId::kick_resonant:
    case EngineId::kick_circuit:
        return kick(id);
    case EngineId::snare:
        return snare();
    case EngineId::hat:
        return hat();
    case EngineId::clap:
        return clap();
    case EngineId::tom_generic:
    case EngineId::tom_simmons:
        return tom();
    case EngineId::cymbal_comb:
        return cymbal();
    case EngineId::membrane_modal:
        return membrane();
    case EngineId::string_karplus_strong:
        return string_voice();
    case EngineId::zap_cz:
        return zap();
    case EngineId::fm2:
        return fm2();
    case EngineId::fm6:
    case EngineId::fm8:
        return fm_many(id);
    case EngineId::dx7_msfa:
        return {};
    }
    return {};
}

inline std::string_view key(EngineId id) {
    switch (id) {
    case EngineId::kick_oscillator:
        return "drum_kick_oscillator";
    case EngineId::kick_resonant:
        return "drum_kick_resonant";
    case EngineId::kick_circuit:
        return "drum_kick_circuit";
    case EngineId::snare:
        return "drum_snare";
    case EngineId::hat:
        return "drum_hat";
    case EngineId::clap:
        return "drum_clap";
    case EngineId::tom_generic:
        return "drum_tom_generic";
    case EngineId::tom_simmons:
        return "drum_tom_simmons";
    case EngineId::cymbal_comb:
        return "drum_cymbal";
    case EngineId::membrane_modal:
        return "drum_membrane";
    case EngineId::string_karplus_strong:
        return "drum_string";
    case EngineId::zap_cz:
        return "drum_zap";
    case EngineId::fm2:
        return "drum_fm2";
    case EngineId::fm6:
        return "drum_fm6";
    case EngineId::fm8:
        return "drum_fm8";
    case EngineId::dx7_msfa:
        return "drum_dx7_unavailable";
    }
    return "drum_unknown";
}

inline std::string_view description(EngineId id) {
    switch (id) {
    case EngineId::kick_oscillator:
        return "Synthesized oscillator kick with pitch envelope, click, noise, sub, and FM layers.";
    case EngineId::kick_resonant:
        return "Struck-resonator kick with click, noise, and sub layers.";
    case EngineId::kick_circuit:
        return "Physical bridged-T circuit kick with editable component values.";
    case EngineId::snare:
        return "Layered tonal, noise, rattle, snap, and shell snare synthesizer.";
    case EngineId::hat:
        return "Metallic oscillator-bank hi-hat with filtered grit.";
    case EngineId::clap:
        return "Multi-burst hand clap with noise tail and optional tonal body.";
    case EngineId::tom_generic:
        return "General synthesized tom with pitch sweep, noise, and click layers.";
    case EngineId::tom_simmons:
        return "SDS-V-inspired electronic tom with pitch sweep and click.";
    case EngineId::cymbal_comb:
        return "Inharmonic comb-resonator cymbal with strike and noise excitation.";
    case EngineId::membrane_modal:
        return "Circular-membrane modal percussion voice with position-dependent excitation.";
    case EngineId::string_karplus_strong:
        return "Karplus-Strong plucked-string percussion voice with modulation and low-pass gate.";
    case EngineId::zap_cz:
        return "CZ-style phase-distortion zap with pitch sweep and ring modulation.";
    case EngineId::fm2:
        return "Two-operator FM percussion voice with waveform, warp, transient, and filter "
               "controls.";
    case EngineId::fm6:
        return "Six-operator algorithmic FM percussion voice.";
    case EngineId::fm8:
        return "Eight-operator algorithmic FM percussion voice with per-operator feedback and "
               "waves.";
    case EngineId::dx7_msfa:
        return "Unavailable held-license drum engine.";
    }
    return "Synthesized percussion voice.";
}

} // namespace descriptor

inline ForgeNodeDescriptor drum_descriptor_impl(EngineId id) {
    const auto metadata =
        std::find_if(signal::drum::engine_registry.begin(), signal::drum::engine_registry.end(),
                     [id](const auto& item) { return item.id == id; });
    ForgeNodeDescriptor d;
    d.key = descriptor::key(id);
    d.label = metadata != signal::drum::engine_registry.end() ? metadata->display_name
                                                              : std::string_view{"Unknown Drum"};
    d.description = descriptor::description(id);
    d.realizations = {{"default", type_id(id)}};

    const auto node = make_drum_node_impl(id);
    auto candidates = descriptor::common();
    auto controls = descriptor::controls(id);
    candidates.insert(candidates.end(), controls.begin(), controls.end());
    for (const auto& baked : node.baked_params) {
        const auto found =
            std::find_if(candidates.begin(), candidates.end(),
                         [&](const auto& candidate) { return candidate.id == baked.id; });
        if (found != candidates.end())
            d.params.push_back(*found);
    }
    return d;
}

} // namespace pulp::host::forge_drum::detail

namespace pulp::host::forge_drum::detail::descriptor {

inline std::vector<ForgeParamDescriptor> snare() {
    using K = ForgeParamKind;
    using C = ForgeParamCurve;
    return {
        p("tone_ratio", kSnareToneRatio, "Tone Ratio", ":1", "Ratio of the snare's tonal modes."),
        p("tone_level", kSnareToneLevel, "Tone Level", "%", "Level of the tonal body."),
        p("fm_amount", kSnareFmAmount, "FM Amount", "", "Frequency-modulation depth."),
        p("ring", kSnareRing, "Ring", "%", "Amount of sustained shell ringing."),
        p("noise_level", kSnareNoiseLevel, "Noise Level", "%", "Level of the noise body."),
        p("noise_decay_ms", kSnareNoiseDecayMs, "Noise Decay", "ms", "Decay of the noise body.",
          K::continuous, C::logarithmic),
        p("noise_cutoff_hz", kSnareNoiseCutoffHz, "Noise Cutoff", "Hz",
          "Filter cutoff of the noise body.", K::continuous, C::logarithmic),
        p("noise_resonance", kSnareNoiseResonance, "Noise Resonance", "Q",
          "Resonance of the noise filter.", K::continuous, C::logarithmic),
        p("noise_sweep_oct", kSnareNoiseSweepOctaves, "Noise Sweep", "oct",
          "Filter-envelope excursion of the noise body."),
        p("rattle", kSnareRattle, "Rattle", "%", "Amount of irregular wire rattle."),
        p("rattle_hz", kSnareRattleHz, "Rattle Rate", "Hz", "Rate of wire-rattle modulation.",
          K::continuous, C::logarithmic),
        p("snap_level", kSnareSnapLevel, "Snap Level", "%", "Level of the initial snap."),
        p("snap_cutoff_hz", kSnareSnapCutoffHz, "Snap Cutoff", "Hz", "Filter cutoff of the snap.",
          K::continuous, C::logarithmic),
        p("snap_decay_ms", kSnareSnapDecayMs, "Snap Decay", "ms", "Decay of the initial snap.",
          K::continuous, C::logarithmic),
        p("shell_level", kSnareShellLevel, "Shell Level", "%", "Level of the resonant shell."),
        p("shell_resonance", kSnareShellResonance, "Shell Resonance", "Q",
          "Resonance of the shell modes.", K::continuous, C::logarithmic),
    };
}

inline std::vector<ForgeParamDescriptor> hat() {
    using K = ForgeParamKind;
    using C = ForgeParamCurve;
    return {
        p("spread", kHatSpread, "Spread", "%", "Detuning spread of metallic oscillators."),
        p("metal", kHatMetal, "Metal", "%", "Balance toward the metallic oscillator bank."),
        p("grit", kHatGrit, "Grit", "%", "Amount of gritty nonlinear modulation."),
        p("grit_ratio", kHatGritRatio, "Grit Ratio", ":1", "Frequency ratio of the grit modulator.",
          K::continuous, C::logarithmic),
        p("cutoff_hz", kHatCutoffHz, "Cutoff", "Hz", "High-pass cutoff of the hat body.",
          K::continuous, C::logarithmic),
        p("resonance", kHatResonance, "Resonance", "Q", "Resonance of the hat filter.",
          K::continuous, C::logarithmic),
        p("bandpass", kHatBandpass, "Band-Pass", "",
          "Selects a band-pass response instead of high-pass.", K::stepped, C::linear, off_on()),
    };
}

inline std::vector<ForgeParamDescriptor> clap() {
    using K = ForgeParamKind;
    using C = ForgeParamCurve;
    return {
        p("burst_count", kClapBurstCount, "Burst Count", "", "Number of initial hand-clap bursts."),
        p("burst_spacing_ms", kClapBurstSpacingMs, "Burst Spacing", "ms",
          "Time between initial bursts."),
        p("burst_decay_ms", kClapBurstDecayMs, "Burst Decay", "ms", "Decay time of each burst."),
        p("burst_falloff", kClapBurstFalloff, "Burst Falloff", "",
          "Level falloff across successive bursts."),
        p("gap_jitter", kClapGapJitter, "Gap Jitter", "%", "Irregularity of burst timing."),
        p("alternate_polarity", kClapAlternatePolarity, "Alternate Polarity", "",
          "Alternates successive burst polarity.", K::stepped, C::linear, off_on()),
        p("stereo_width", kClapStereoWidth, "Stereo Width", "%",
          "Stereo separation of the bursts."),
        p("tail_level", kClapTailLevel, "Tail Level", "%", "Level of the noise tail."),
        p("tail_decay_ms", kClapTailDecayMs, "Tail Decay", "ms", "Decay time of the noise tail.",
          K::continuous, C::logarithmic),
        p("cutoff_hz", kClapCutoffHz, "Cutoff", "Hz", "Filter cutoff of the clap noise.",
          K::continuous, C::logarithmic),
        p("resonance", kClapResonance, "Resonance", "Q", "Resonance of the clap filter.",
          K::continuous, C::logarithmic),
        p("body_level", kClapBodyLevel, "Body Level", "%", "Level of the tonal body."),
        p("body_hz", kClapBodyHz, "Body Tune", "Hz", "Frequency of the tonal body.", K::continuous,
          C::logarithmic),
    };
}

inline std::vector<ForgeParamDescriptor> tom() {
    using K = ForgeParamKind;
    using C = ForgeParamCurve;
    return {
        p("wave", kTomWave, "Wave", "%", "Blend of the tom oscillator waveforms."),
        p("noise_balance", kTomNoiseBalance, "Noise Balance", "%",
          "Balance between tone and noise."),
        p("noise_cutoff_hz", kTomNoiseCutoffHz, "Noise Cutoff", "Hz",
          "Filter cutoff of the noise layer.", K::continuous, C::logarithmic),
        p("noise_resonance", kTomNoiseResonance, "Noise Resonance", "Q",
          "Resonance of the noise filter."),
        p("click_level", kTomClickLevel, "Click Level", "%", "Level of the attack click."),
        p("click_cutoff_hz", kTomClickCutoffHz, "Click Cutoff", "Hz",
          "Filter cutoff of the attack click.", K::continuous, C::logarithmic),
        p("click_decay_ms", kTomClickDecayMs, "Click Decay", "ms", "Decay of the attack click.",
          K::continuous, C::logarithmic),
    };
}

inline std::vector<ForgeParamDescriptor> membrane() {
    using K = ForgeParamKind;
    using C = ForgeParamCurve;
    return {
        p("structure", kMembraneStructure, "Structure", "%",
          "Balance among membrane modal structures."),
        p("stretch", kMembraneStretch, "Stretch", "%",
          "Inharmonic stretching of modal frequencies."),
        p("damping", kMembraneDamping, "Damping", "%", "Loss in the membrane modes."),
        p("brightness", kMembraneBrightness, "Brightness", "%", "High-mode emphasis."),
        p("position", kMembranePosition, "Strike Position", "%",
          "Radial strike position on the membrane."),
        p("spread", kMembraneSpread, "Spread", "%", "Stereo spread of modal resonances."),
        p("exciter_ms", kMembraneExciterMs, "Exciter Time", "ms", "Duration of the strike exciter.",
          K::continuous, C::logarithmic),
        p("exciter_cutoff_hz", kMembraneExciterCutoffHz, "Exciter Cutoff", "Hz",
          "Filter cutoff of the strike exciter.", K::continuous, C::logarithmic),
        p("exciter", kMembraneExciter, "Exciter", "%", "Blend of exciter character."),
        p("sub_level", kMembraneSubLevel, "Sub Level", "%", "Level of the sub layer."),
        p("air_level", kMembraneAirLevel, "Air Level", "%", "Level of the air-noise layer."),
        p("air_decay_ms", kMembraneAirDecayMs, "Air Decay", "ms", "Decay of the air-noise layer.",
          K::continuous, C::logarithmic),
        p("click_level", kMembraneClickLevel, "Click Level", "%", "Level of the strike click."),
        p("click_decay_ms", kMembraneClickDecayMs, "Click Decay", "ms",
          "Decay of the strike click.", K::continuous, C::logarithmic),
    };
}

inline std::vector<ForgeParamDescriptor> cymbal() {
    using K = ForgeParamKind;
    using C = ForgeParamCurve;
    return {
        p("decay_tilt", kCymbalDecayTilt, "Decay Tilt", "", "Relative decay of upper comb modes."),
        p("high_mode_db", kCymbalHighModeEmphasisDb, "High-Mode Emphasis", "dB",
          "Static level of upper modes."),
        p("velocity_feedback", kCymbalVelocityFeedback, "Velocity Feedback", "%",
          "Velocity modulation of comb feedback."),
        p("velocity_high_mode_db", kCymbalVelocityHighModeDb, "Velocity High Modes", "dB",
          "Velocity modulation of upper-mode level."),
        p("upper_highpass_hz", kCymbalUpperHighpassHz, "Upper High-Pass", "Hz",
          "High-pass cutoff of upper modes.", K::continuous, C::logarithmic),
        p("spread", kCymbalSpread, "Spread", "%", "Stereo spread of comb modes."),
        p("inharmonicity", kCymbalInharmonicity, "Inharmonicity", "%",
          "Departure of comb modes from harmonic ratios."),
        p("shift_hz", kCymbalShiftHz, "Shift", "Hz", "Frequency translation of upper modes."),
        p("noise_level", kCymbalNoiseLevel, "Noise Level", "%", "Level of the noise wash."),
        p("strike_level", kCymbalStrikeLevel, "Strike Level", "%", "Level of the initial strike."),
        p("strike_ms", kCymbalStrikeMs, "Strike Time", "ms", "Duration of the initial strike."),
        p("tone_hz", kCymbalToneHz, "Tone", "Hz", "Brightness of the cymbal body.", K::continuous,
          C::logarithmic),
        p("low_cut_hz", kCymbalLowCutHz, "Low Cut", "Hz", "High-pass cutoff of the output.",
          K::continuous, C::logarithmic),
        p("hit_life", kCymbalHitLife, "Hit Life", "",
          "How a strike interacts with the ringing body."),
    };
}

inline std::vector<ForgeParamDescriptor> string_voice() {
    using K = ForgeParamKind;
    using C = ForgeParamCurve;
    return {
        p("damping", kStringDamping, "Damping", "%", "Per-cycle string energy loss."),
        p("stiffness", kStringStiffness, "Stiffness", "%",
          "Dispersion caused by string stiffness."),
        p("pluck_position", kStringPluckPosition, "Pluck Position", "%",
          "Position of the pluck along the string."),
        p("exciter_ms", kStringExciterMs, "Exciter Time", "ms", "Duration of the pluck exciter.",
          K::continuous, C::logarithmic),
        p("brightness_hz", kStringBrightnessHz, "Brightness", "Hz",
          "Low-pass cutoff in the string loop.", K::continuous, C::logarithmic),
        p("pick_direction", kStringPickDirection, "Pick Direction", "%",
          "Asymmetry of the pluck impulse."),
        p("restart_on_hit", kStringRestartOnHit, "Restart on Hit", "",
          "Clears the string before each strike.", K::stepped, C::linear, off_on()),
        p("modulation", kStringModulation, "Modulation", "", "String modulation mode."),
        p("modulation_mix", kStringModulationMix, "Modulation Mix", "%",
          "Amount of the modulated path."),
        p("modulation_ratio", kStringModulationRatio, "Modulation Ratio", ":1",
          "Frequency ratio of the modulator.", K::continuous, C::logarithmic),
        p("fm_depth_oct", kStringFmDepthOctaves, "FM Depth", "oct", "Pitch-modulation depth."),
        p("lpg_amount", kStringLpgAmount, "LPG Amount", "%",
          "Low-pass-gate response to the strike envelope."),
    };
}

inline std::vector<ForgeParamDescriptor> zap() {
    using K = ForgeParamKind;
    using C = ForgeParamCurve;
    return {
        p("shape", kZapShape, "Shape", "", "Phase-distortion waveshape."),
        p("distortion", kZapDistortion, "Distortion", "%", "Phase-distortion depth."),
        p("distortion_ms", kZapDistortionMs, "Distortion Decay", "ms",
          "Decay time of phase distortion.", K::continuous, C::logarithmic),
        p("resonant_depth", kZapResonantDepth, "Resonant Depth", "",
          "Resonant phase-distortion emphasis."),
        p("detune_cents", kZapDetuneCents, "Detune", "cent",
          "Detuning between paired oscillators."),
        p("ring", kZapRing, "Ring", "%", "Ring-modulation amount."),
        p("ring_ratio", kZapRingRatio, "Ring Ratio", ":1", "Frequency ratio of the ring modulator.",
          K::continuous, C::logarithmic),
    };
}

} // namespace pulp::host::forge_drum::detail::descriptor
