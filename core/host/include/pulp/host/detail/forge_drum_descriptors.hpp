#pragma once

// Derived from the authoritative drum engine inventory and bake contract.

#include <pulp/host/forge_param_descriptor.hpp>

#include <vector>

namespace pulp::host::forge_drum {

inline ForgeNodeDescriptor kick_oscillator_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "kick.oscillator";
    d.label = "Oscillator Kick";
    d.description = "Pulp percussion engine: Oscillator Kick.";
    d.realizations = {{"kick.oscillator", "drum.kick.oscillator"}};
    d.params = {
        {"trigger", 1, "Trigger", "", "Trigger parameter. Pulp percussion engine: Oscillator Kick.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"velocity", 2, "Velocity", "", "Velocity parameter. Pulp percussion engine: Oscillator Kick.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"choke", 3, "Choke", "", "Choke parameter. Pulp percussion engine: Oscillator Kick.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"choke_ms", 4, "Choke", "ms", "Choke parameter measured in ms. Pulp percussion engine: Oscillator Kick.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"velocity_level_db", 5, "Velocity Level", "dB", "Velocity Level parameter measured in dB. Pulp percussion engine: Oscillator Kick.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"velocity_bend_octaves", 6, "Velocity Bend", "oct", "Velocity Bend parameter measured in oct. Pulp percussion engine: Oscillator Kick.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"velocity_brightness_octaves", 7, "Velocity Brightness", "oct", "Velocity Brightness parameter measured in oct. Pulp percussion engine: Oscillator Kick.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_drive", 20, "Output Drive", "", "Output Drive parameter. Pulp percussion engine: Oscillator Kick.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_fold", 21, "Output Fold", "", "Output Fold parameter. Pulp percussion engine: Oscillator Kick.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_level", 22, "Output Level", "", "Output Level parameter. Pulp percussion engine: Oscillator Kick.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_ahd_enabled", 23, "Output AHD Enabled", "", "Output AHD Enabled parameter. Pulp percussion engine: Oscillator Kick.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"output_attack_ms", 24, "Output Attack", "ms", "Output Attack parameter measured in ms. Pulp percussion engine: Oscillator Kick.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_hold_ms", 25, "Output Hold", "ms", "Output Hold parameter measured in ms. Pulp percussion engine: Oscillator Kick.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_decay_ms", 26, "Output Decay", "ms", "Output Decay parameter measured in ms. Pulp percussion engine: Oscillator Kick.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"output_bits", 27, "Output Bits", "bit", "Output Bits parameter measured in bit. Pulp percussion engine: Oscillator Kick.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"1", "1", 1.0f}, {"2", "2", 2.0f}, {"3", "3", 3.0f}, {"4", "4", 4.0f}, {"5", "5", 5.0f}, {"6", "6", 6.0f}, {"7", "7", 7.0f}, {"8", "8", 8.0f}, {"9", "9", 9.0f}, {"10", "10", 10.0f}, {"11", "11", 11.0f}, {"12", "12", 12.0f}, {"13", "13", 13.0f}, {"14", "14", 14.0f}, {"15", "15", 15.0f}, {"16", "16", 16.0f}, {"17", "17", 17.0f}, {"18", "18", 18.0f}, {"19", "19", 19.0f}, {"20", "20", 20.0f}, {"21", "21", 21.0f}, {"22", "22", 22.0f}, {"23", "23", 23.0f}, {"24", "24", 24.0f}}, {}},
        {"output_hold_rate_hz", 28, "Output Hold Rate", "Hz", "Output Hold Rate parameter measured in Hz. Pulp percussion engine: Oscillator Kick.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"output_jitter", 29, "Output Jitter", "", "Output Jitter parameter. Pulp percussion engine: Oscillator Kick.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_smoothing", 30, "Output Smoothing", "", "Output Smoothing parameter. Pulp percussion engine: Oscillator Kick.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_dead_zone", 31, "Output Dead Zone", "", "Output Dead Zone parameter. Pulp percussion engine: Oscillator Kick.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"tune_hz", 10, "Tune", "Hz", "Tune parameter measured in Hz. Pulp percussion engine: Oscillator Kick.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"decay", 11, "Decay", "", "Decay parameter. Pulp percussion engine: Oscillator Kick.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"pitch_sweep_octaves", 12, "Pitch Sweep", "oct", "Pitch Sweep parameter measured in oct. Pulp percussion engine: Oscillator Kick.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"pitch_sweep_ms", 13, "Pitch Sweep", "ms", "Pitch Sweep parameter measured in ms. Pulp percussion engine: Oscillator Kick.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"noise_color", 14, "Noise Color", "", "Noise Color parameter. Pulp percussion engine: Oscillator Kick.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"0", "0", 0.0f}, {"1", "1", 1.0f}, {"2", "2", 2.0f}, {"3", "3", 3.0f}, {"4", "4", 4.0f}}, {}},
        {"kick_click_level", 100, "Kick Click Level", "", "Kick Click Level parameter. Pulp percussion engine: Oscillator Kick.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"kick_click_tone_hz", 101, "Kick Click Tone", "Hz", "Kick Click Tone parameter measured in Hz. Pulp percussion engine: Oscillator Kick.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"kick_click_decay_ms", 102, "Kick Click Decay", "ms", "Kick Click Decay parameter measured in ms. Pulp percussion engine: Oscillator Kick.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"kick_noise_level", 103, "Kick Noise Level", "", "Kick Noise Level parameter. Pulp percussion engine: Oscillator Kick.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"kick_noise_decay_ms", 104, "Kick Noise Decay", "ms", "Kick Noise Decay parameter measured in ms. Pulp percussion engine: Oscillator Kick.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"kick_sub_level", 105, "Kick Sub Level", "", "Kick Sub Level parameter. Pulp percussion engine: Oscillator Kick.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"kick_triangle", 106, "Kick Triangle", "", "Kick Triangle parameter. Pulp percussion engine: Oscillator Kick.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"kick_fm_amount", 107, "Kick FM Amount", "", "Kick FM Amount parameter. Pulp percussion engine: Oscillator Kick.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"kick_fm_ratio", 108, "Kick FM Ratio", "", "Kick FM Ratio parameter. Pulp percussion engine: Oscillator Kick.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
    };
    return d;
}

inline ForgeNodeDescriptor kick_resonant_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "kick.resonant";
    d.label = "Resonant Kick";
    d.description = "Pulp percussion engine: Resonant Kick.";
    d.realizations = {{"kick.resonant", "drum.kick.resonant"}};
    d.params = {
        {"trigger", 1, "Trigger", "", "Trigger parameter. Pulp percussion engine: Resonant Kick.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"velocity", 2, "Velocity", "", "Velocity parameter. Pulp percussion engine: Resonant Kick.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"choke", 3, "Choke", "", "Choke parameter. Pulp percussion engine: Resonant Kick.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"choke_ms", 4, "Choke", "ms", "Choke parameter measured in ms. Pulp percussion engine: Resonant Kick.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"velocity_level_db", 5, "Velocity Level", "dB", "Velocity Level parameter measured in dB. Pulp percussion engine: Resonant Kick.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"velocity_brightness_octaves", 7, "Velocity Brightness", "oct", "Velocity Brightness parameter measured in oct. Pulp percussion engine: Resonant Kick.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_drive", 20, "Output Drive", "", "Output Drive parameter. Pulp percussion engine: Resonant Kick.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_fold", 21, "Output Fold", "", "Output Fold parameter. Pulp percussion engine: Resonant Kick.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_level", 22, "Output Level", "", "Output Level parameter. Pulp percussion engine: Resonant Kick.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_ahd_enabled", 23, "Output AHD Enabled", "", "Output AHD Enabled parameter. Pulp percussion engine: Resonant Kick.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"output_attack_ms", 24, "Output Attack", "ms", "Output Attack parameter measured in ms. Pulp percussion engine: Resonant Kick.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_hold_ms", 25, "Output Hold", "ms", "Output Hold parameter measured in ms. Pulp percussion engine: Resonant Kick.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_decay_ms", 26, "Output Decay", "ms", "Output Decay parameter measured in ms. Pulp percussion engine: Resonant Kick.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"output_bits", 27, "Output Bits", "bit", "Output Bits parameter measured in bit. Pulp percussion engine: Resonant Kick.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"1", "1", 1.0f}, {"2", "2", 2.0f}, {"3", "3", 3.0f}, {"4", "4", 4.0f}, {"5", "5", 5.0f}, {"6", "6", 6.0f}, {"7", "7", 7.0f}, {"8", "8", 8.0f}, {"9", "9", 9.0f}, {"10", "10", 10.0f}, {"11", "11", 11.0f}, {"12", "12", 12.0f}, {"13", "13", 13.0f}, {"14", "14", 14.0f}, {"15", "15", 15.0f}, {"16", "16", 16.0f}, {"17", "17", 17.0f}, {"18", "18", 18.0f}, {"19", "19", 19.0f}, {"20", "20", 20.0f}, {"21", "21", 21.0f}, {"22", "22", 22.0f}, {"23", "23", 23.0f}, {"24", "24", 24.0f}}, {}},
        {"output_hold_rate_hz", 28, "Output Hold Rate", "Hz", "Output Hold Rate parameter measured in Hz. Pulp percussion engine: Resonant Kick.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"output_jitter", 29, "Output Jitter", "", "Output Jitter parameter. Pulp percussion engine: Resonant Kick.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_smoothing", 30, "Output Smoothing", "", "Output Smoothing parameter. Pulp percussion engine: Resonant Kick.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_dead_zone", 31, "Output Dead Zone", "", "Output Dead Zone parameter. Pulp percussion engine: Resonant Kick.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"tune_hz", 10, "Tune", "Hz", "Tune parameter measured in Hz. Pulp percussion engine: Resonant Kick.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"decay", 11, "Decay", "", "Decay parameter. Pulp percussion engine: Resonant Kick.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"noise_color", 14, "Noise Color", "", "Noise Color parameter. Pulp percussion engine: Resonant Kick.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"0", "0", 0.0f}, {"1", "1", 1.0f}, {"2", "2", 2.0f}, {"3", "3", 3.0f}, {"4", "4", 4.0f}}, {}},
        {"kick_click_level", 100, "Kick Click Level", "", "Kick Click Level parameter. Pulp percussion engine: Resonant Kick.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"kick_click_tone_hz", 101, "Kick Click Tone", "Hz", "Kick Click Tone parameter measured in Hz. Pulp percussion engine: Resonant Kick.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"kick_click_decay_ms", 102, "Kick Click Decay", "ms", "Kick Click Decay parameter measured in ms. Pulp percussion engine: Resonant Kick.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"kick_noise_level", 103, "Kick Noise Level", "", "Kick Noise Level parameter. Pulp percussion engine: Resonant Kick.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"kick_noise_decay_ms", 104, "Kick Noise Decay", "ms", "Kick Noise Decay parameter measured in ms. Pulp percussion engine: Resonant Kick.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"kick_sub_level", 105, "Kick Sub Level", "", "Kick Sub Level parameter. Pulp percussion engine: Resonant Kick.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
    };
    return d;
}

inline ForgeNodeDescriptor kick_circuit_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "kick.circuit";
    d.label = "Bridged-T Circuit Kick";
    d.description = "Pulp percussion engine: Bridged-T Circuit Kick.";
    d.realizations = {{"kick.circuit", "drum.kick.circuit"}};
    d.params = {
        {"trigger", 1, "Trigger", "", "Trigger parameter. Pulp percussion engine: Bridged-T Circuit Kick.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"velocity", 2, "Velocity", "", "Velocity parameter. Pulp percussion engine: Bridged-T Circuit Kick.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"choke", 3, "Choke", "", "Choke parameter. Pulp percussion engine: Bridged-T Circuit Kick.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"choke_ms", 4, "Choke", "ms", "Choke parameter measured in ms. Pulp percussion engine: Bridged-T Circuit Kick.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"velocity_level_db", 5, "Velocity Level", "dB", "Velocity Level parameter measured in dB. Pulp percussion engine: Bridged-T Circuit Kick.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"velocity_brightness_octaves", 7, "Velocity Brightness", "oct", "Velocity Brightness parameter measured in oct. Pulp percussion engine: Bridged-T Circuit Kick.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_drive", 20, "Output Drive", "", "Output Drive parameter. Pulp percussion engine: Bridged-T Circuit Kick.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_fold", 21, "Output Fold", "", "Output Fold parameter. Pulp percussion engine: Bridged-T Circuit Kick.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_level", 22, "Output Level", "", "Output Level parameter. Pulp percussion engine: Bridged-T Circuit Kick.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_ahd_enabled", 23, "Output AHD Enabled", "", "Output AHD Enabled parameter. Pulp percussion engine: Bridged-T Circuit Kick.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"output_attack_ms", 24, "Output Attack", "ms", "Output Attack parameter measured in ms. Pulp percussion engine: Bridged-T Circuit Kick.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_hold_ms", 25, "Output Hold", "ms", "Output Hold parameter measured in ms. Pulp percussion engine: Bridged-T Circuit Kick.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_decay_ms", 26, "Output Decay", "ms", "Output Decay parameter measured in ms. Pulp percussion engine: Bridged-T Circuit Kick.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"output_bits", 27, "Output Bits", "bit", "Output Bits parameter measured in bit. Pulp percussion engine: Bridged-T Circuit Kick.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"1", "1", 1.0f}, {"2", "2", 2.0f}, {"3", "3", 3.0f}, {"4", "4", 4.0f}, {"5", "5", 5.0f}, {"6", "6", 6.0f}, {"7", "7", 7.0f}, {"8", "8", 8.0f}, {"9", "9", 9.0f}, {"10", "10", 10.0f}, {"11", "11", 11.0f}, {"12", "12", 12.0f}, {"13", "13", 13.0f}, {"14", "14", 14.0f}, {"15", "15", 15.0f}, {"16", "16", 16.0f}, {"17", "17", 17.0f}, {"18", "18", 18.0f}, {"19", "19", 19.0f}, {"20", "20", 20.0f}, {"21", "21", 21.0f}, {"22", "22", 22.0f}, {"23", "23", 23.0f}, {"24", "24", 24.0f}}, {}},
        {"output_hold_rate_hz", 28, "Output Hold Rate", "Hz", "Output Hold Rate parameter measured in Hz. Pulp percussion engine: Bridged-T Circuit Kick.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"output_jitter", 29, "Output Jitter", "", "Output Jitter parameter. Pulp percussion engine: Bridged-T Circuit Kick.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_smoothing", 30, "Output Smoothing", "", "Output Smoothing parameter. Pulp percussion engine: Bridged-T Circuit Kick.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_dead_zone", 31, "Output Dead Zone", "", "Output Dead Zone parameter. Pulp percussion engine: Bridged-T Circuit Kick.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"tune_hz", 10, "Tune", "Hz", "Tune parameter measured in Hz. Pulp percussion engine: Bridged-T Circuit Kick.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"noise_color", 14, "Noise Color", "", "Noise Color parameter. Pulp percussion engine: Bridged-T Circuit Kick.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"0", "0", 0.0f}, {"1", "1", 1.0f}, {"2", "2", 2.0f}, {"3", "3", 3.0f}, {"4", "4", 4.0f}}, {}},
        {"kick_click_level", 100, "Kick Click Level", "", "Kick Click Level parameter. Pulp percussion engine: Bridged-T Circuit Kick.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"kick_click_tone_hz", 101, "Kick Click Tone", "Hz", "Kick Click Tone parameter measured in Hz. Pulp percussion engine: Bridged-T Circuit Kick.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"kick_click_decay_ms", 102, "Kick Click Decay", "ms", "Kick Click Decay parameter measured in ms. Pulp percussion engine: Bridged-T Circuit Kick.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"kick_noise_level", 103, "Kick Noise Level", "", "Kick Noise Level parameter. Pulp percussion engine: Bridged-T Circuit Kick.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"kick_noise_decay_ms", 104, "Kick Noise Decay", "ms", "Kick Noise Decay parameter measured in ms. Pulp percussion engine: Bridged-T Circuit Kick.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"kick_sub_level", 105, "Kick Sub Level", "", "Kick Sub Level parameter. Pulp percussion engine: Bridged-T Circuit Kick.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"kick_circuit_feedback", 106, "Kick Circuit Feedback", "", "Kick Circuit Feedback parameter. Pulp percussion engine: Bridged-T Circuit Kick.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"kick_circuit_drive", 107, "Kick Circuit Drive", "", "Kick Circuit Drive parameter. Pulp percussion engine: Bridged-T Circuit Kick.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"kick_circuit_attack_ms", 108, "Kick Circuit Attack", "ms", "Kick Circuit Attack parameter measured in ms. Pulp percussion engine: Bridged-T Circuit Kick.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"kick_circuit_pulse_ms", 109, "Kick Circuit Pulse", "ms", "Kick Circuit Pulse parameter measured in ms. Pulp percussion engine: Bridged-T Circuit Kick.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"kick_circuit_sigh", 110, "Kick Circuit Sigh", "", "Kick Circuit Sigh parameter. Pulp percussion engine: Bridged-T Circuit Kick.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"kick_circuit_hit_life", 111, "Kick Circuit Hit Life", "", "Kick Circuit Hit Life parameter. Pulp percussion engine: Bridged-T Circuit Kick.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"0", "0", 0.0f}, {"1", "1", 1.0f}, {"2", "2", 2.0f}}, {}},
        {"circuit_c41", 60, "Circuit C41", "F", "Circuit C41 parameter measured in F. Pulp percussion engine: Bridged-T Circuit Kick.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"circuit_c42", 61, "Circuit C42", "F", "Circuit C42 parameter measured in F. Pulp percussion engine: Bridged-T Circuit Kick.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"circuit_r161", 62, "Circuit R161", "Ohm", "Circuit R161 parameter measured in Ohm. Pulp percussion engine: Bridged-T Circuit Kick.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"circuit_r165", 63, "Circuit R165", "Ohm", "Circuit R165 parameter measured in Ohm. Pulp percussion engine: Bridged-T Circuit Kick.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"circuit_r166", 64, "Circuit R166", "Ohm", "Circuit R166 parameter measured in Ohm. Pulp percussion engine: Bridged-T Circuit Kick.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"circuit_r167", 65, "Circuit R167", "Ohm", "Circuit R167 parameter measured in Ohm. Pulp percussion engine: Bridged-T Circuit Kick.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"circuit_r170", 66, "Circuit R170", "Ohm", "Circuit R170 parameter measured in Ohm. Pulp percussion engine: Bridged-T Circuit Kick.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
    };
    return d;
}

inline ForgeNodeDescriptor snare_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "snare";
    d.label = "Snare";
    d.description = "Pulp percussion engine: Snare.";
    d.realizations = {{"snare", "drum.snare"}};
    d.params = {
        {"trigger", 1, "Trigger", "", "Trigger parameter. Pulp percussion engine: Snare.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"velocity", 2, "Velocity", "", "Velocity parameter. Pulp percussion engine: Snare.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"choke", 3, "Choke", "", "Choke parameter. Pulp percussion engine: Snare.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"choke_ms", 4, "Choke", "ms", "Choke parameter measured in ms. Pulp percussion engine: Snare.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"velocity_level_db", 5, "Velocity Level", "dB", "Velocity Level parameter measured in dB. Pulp percussion engine: Snare.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"velocity_bend_octaves", 6, "Velocity Bend", "oct", "Velocity Bend parameter measured in oct. Pulp percussion engine: Snare.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"velocity_brightness_octaves", 7, "Velocity Brightness", "oct", "Velocity Brightness parameter measured in oct. Pulp percussion engine: Snare.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"velocity_noise_balance", 8, "Velocity Noise Balance", "", "Velocity Noise Balance parameter. Pulp percussion engine: Snare.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_drive", 20, "Output Drive", "", "Output Drive parameter. Pulp percussion engine: Snare.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_fold", 21, "Output Fold", "", "Output Fold parameter. Pulp percussion engine: Snare.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_level", 22, "Output Level", "", "Output Level parameter. Pulp percussion engine: Snare.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_ahd_enabled", 23, "Output AHD Enabled", "", "Output AHD Enabled parameter. Pulp percussion engine: Snare.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"output_attack_ms", 24, "Output Attack", "ms", "Output Attack parameter measured in ms. Pulp percussion engine: Snare.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_hold_ms", 25, "Output Hold", "ms", "Output Hold parameter measured in ms. Pulp percussion engine: Snare.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_decay_ms", 26, "Output Decay", "ms", "Output Decay parameter measured in ms. Pulp percussion engine: Snare.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"output_bits", 27, "Output Bits", "bit", "Output Bits parameter measured in bit. Pulp percussion engine: Snare.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"1", "1", 1.0f}, {"2", "2", 2.0f}, {"3", "3", 3.0f}, {"4", "4", 4.0f}, {"5", "5", 5.0f}, {"6", "6", 6.0f}, {"7", "7", 7.0f}, {"8", "8", 8.0f}, {"9", "9", 9.0f}, {"10", "10", 10.0f}, {"11", "11", 11.0f}, {"12", "12", 12.0f}, {"13", "13", 13.0f}, {"14", "14", 14.0f}, {"15", "15", 15.0f}, {"16", "16", 16.0f}, {"17", "17", 17.0f}, {"18", "18", 18.0f}, {"19", "19", 19.0f}, {"20", "20", 20.0f}, {"21", "21", 21.0f}, {"22", "22", 22.0f}, {"23", "23", 23.0f}, {"24", "24", 24.0f}}, {}},
        {"output_hold_rate_hz", 28, "Output Hold Rate", "Hz", "Output Hold Rate parameter measured in Hz. Pulp percussion engine: Snare.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"output_jitter", 29, "Output Jitter", "", "Output Jitter parameter. Pulp percussion engine: Snare.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_smoothing", 30, "Output Smoothing", "", "Output Smoothing parameter. Pulp percussion engine: Snare.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_dead_zone", 31, "Output Dead Zone", "", "Output Dead Zone parameter. Pulp percussion engine: Snare.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"tune_hz", 10, "Tune", "Hz", "Tune parameter measured in Hz. Pulp percussion engine: Snare.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"decay", 11, "Decay", "", "Decay parameter. Pulp percussion engine: Snare.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"pitch_sweep_octaves", 12, "Pitch Sweep", "oct", "Pitch Sweep parameter measured in oct. Pulp percussion engine: Snare.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"pitch_sweep_ms", 13, "Pitch Sweep", "ms", "Pitch Sweep parameter measured in ms. Pulp percussion engine: Snare.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"noise_color", 14, "Noise Color", "", "Noise Color parameter. Pulp percussion engine: Snare.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"0", "0", 0.0f}, {"1", "1", 1.0f}, {"2", "2", 2.0f}, {"3", "3", 3.0f}, {"4", "4", 4.0f}}, {}},
        {"snare_tone_ratio", 100, "Snare Tone Ratio", "", "Snare Tone Ratio parameter. Pulp percussion engine: Snare.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"snare_tone_level", 101, "Snare Tone Level", "", "Snare Tone Level parameter. Pulp percussion engine: Snare.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"snare_fm_amount", 102, "Snare FM Amount", "", "Snare FM Amount parameter. Pulp percussion engine: Snare.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"snare_ring", 103, "Snare Ring", "", "Snare Ring parameter. Pulp percussion engine: Snare.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"snare_noise_level", 104, "Snare Noise Level", "", "Snare Noise Level parameter. Pulp percussion engine: Snare.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"snare_noise_decay_ms", 105, "Snare Noise Decay", "ms", "Snare Noise Decay parameter measured in ms. Pulp percussion engine: Snare.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"snare_noise_cutoff_hz", 106, "Snare Noise Cutoff", "Hz", "Snare Noise Cutoff parameter measured in Hz. Pulp percussion engine: Snare.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"snare_noise_resonance", 107, "Snare Noise Resonance", "", "Snare Noise Resonance parameter. Pulp percussion engine: Snare.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"snare_noise_sweep_octaves", 108, "Snare Noise Sweep", "oct", "Snare Noise Sweep parameter measured in oct. Pulp percussion engine: Snare.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"snare_rattle", 109, "Snare Rattle", "", "Snare Rattle parameter. Pulp percussion engine: Snare.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"snare_rattle_hz", 110, "Snare Rattle", "Hz", "Snare Rattle parameter measured in Hz. Pulp percussion engine: Snare.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"snare_snap_level", 111, "Snare Snap Level", "", "Snare Snap Level parameter. Pulp percussion engine: Snare.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"snare_snap_cutoff_hz", 112, "Snare Snap Cutoff", "Hz", "Snare Snap Cutoff parameter measured in Hz. Pulp percussion engine: Snare.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"snare_snap_decay_ms", 113, "Snare Snap Decay", "ms", "Snare Snap Decay parameter measured in ms. Pulp percussion engine: Snare.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"snare_shell_level", 114, "Snare Shell Level", "", "Snare Shell Level parameter. Pulp percussion engine: Snare.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"snare_shell_resonance", 115, "Snare Shell Resonance", "", "Snare Shell Resonance parameter. Pulp percussion engine: Snare.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"tone_lofi_bits", 50, "Tone Lofi Bits", "bit", "Tone Lofi Bits parameter measured in bit. Pulp percussion engine: Snare.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"1", "1", 1.0f}, {"2", "2", 2.0f}, {"3", "3", 3.0f}, {"4", "4", 4.0f}, {"5", "5", 5.0f}, {"6", "6", 6.0f}, {"7", "7", 7.0f}, {"8", "8", 8.0f}, {"9", "9", 9.0f}, {"10", "10", 10.0f}, {"11", "11", 11.0f}, {"12", "12", 12.0f}, {"13", "13", 13.0f}, {"14", "14", 14.0f}, {"15", "15", 15.0f}, {"16", "16", 16.0f}, {"17", "17", 17.0f}, {"18", "18", 18.0f}, {"19", "19", 19.0f}, {"20", "20", 20.0f}, {"21", "21", 21.0f}, {"22", "22", 22.0f}, {"23", "23", 23.0f}, {"24", "24", 24.0f}}, {}},
        {"tone_lofi_hold_rate_hz", 51, "Tone Lofi Hold Rate", "Hz", "Tone Lofi Hold Rate parameter measured in Hz. Pulp percussion engine: Snare.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"tone_lofi_jitter", 52, "Tone Lofi Jitter", "", "Tone Lofi Jitter parameter. Pulp percussion engine: Snare.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"tone_lofi_smoothing", 53, "Tone Lofi Smoothing", "", "Tone Lofi Smoothing parameter. Pulp percussion engine: Snare.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"tone_lofi_dead_zone", 54, "Tone Lofi Dead Zone", "", "Tone Lofi Dead Zone parameter. Pulp percussion engine: Snare.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"noise_lofi_bits", 55, "Noise Lofi Bits", "bit", "Noise Lofi Bits parameter measured in bit. Pulp percussion engine: Snare.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"1", "1", 1.0f}, {"2", "2", 2.0f}, {"3", "3", 3.0f}, {"4", "4", 4.0f}, {"5", "5", 5.0f}, {"6", "6", 6.0f}, {"7", "7", 7.0f}, {"8", "8", 8.0f}, {"9", "9", 9.0f}, {"10", "10", 10.0f}, {"11", "11", 11.0f}, {"12", "12", 12.0f}, {"13", "13", 13.0f}, {"14", "14", 14.0f}, {"15", "15", 15.0f}, {"16", "16", 16.0f}, {"17", "17", 17.0f}, {"18", "18", 18.0f}, {"19", "19", 19.0f}, {"20", "20", 20.0f}, {"21", "21", 21.0f}, {"22", "22", 22.0f}, {"23", "23", 23.0f}, {"24", "24", 24.0f}}, {}},
        {"noise_lofi_hold_rate_hz", 56, "Noise Lofi Hold Rate", "Hz", "Noise Lofi Hold Rate parameter measured in Hz. Pulp percussion engine: Snare.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"noise_lofi_jitter", 57, "Noise Lofi Jitter", "", "Noise Lofi Jitter parameter. Pulp percussion engine: Snare.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"noise_lofi_smoothing", 58, "Noise Lofi Smoothing", "", "Noise Lofi Smoothing parameter. Pulp percussion engine: Snare.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"noise_lofi_dead_zone", 59, "Noise Lofi Dead Zone", "", "Noise Lofi Dead Zone parameter. Pulp percussion engine: Snare.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
    };
    return d;
}

inline ForgeNodeDescriptor hat_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "hat";
    d.label = "Metallic Hi-Hat";
    d.description = "Pulp percussion engine: Metallic Hi-Hat.";
    d.realizations = {{"hat", "drum.hat"}};
    d.params = {
        {"trigger", 1, "Trigger", "", "Trigger parameter. Pulp percussion engine: Metallic Hi-Hat.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"velocity", 2, "Velocity", "", "Velocity parameter. Pulp percussion engine: Metallic Hi-Hat.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"choke", 3, "Choke", "", "Choke parameter. Pulp percussion engine: Metallic Hi-Hat.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"choke_ms", 4, "Choke", "ms", "Choke parameter measured in ms. Pulp percussion engine: Metallic Hi-Hat.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"velocity_level_db", 5, "Velocity Level", "dB", "Velocity Level parameter measured in dB. Pulp percussion engine: Metallic Hi-Hat.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"velocity_brightness_octaves", 7, "Velocity Brightness", "oct", "Velocity Brightness parameter measured in oct. Pulp percussion engine: Metallic Hi-Hat.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_drive", 20, "Output Drive", "", "Output Drive parameter. Pulp percussion engine: Metallic Hi-Hat.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_fold", 21, "Output Fold", "", "Output Fold parameter. Pulp percussion engine: Metallic Hi-Hat.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_level", 22, "Output Level", "", "Output Level parameter. Pulp percussion engine: Metallic Hi-Hat.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_ahd_enabled", 23, "Output AHD Enabled", "", "Output AHD Enabled parameter. Pulp percussion engine: Metallic Hi-Hat.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"output_attack_ms", 24, "Output Attack", "ms", "Output Attack parameter measured in ms. Pulp percussion engine: Metallic Hi-Hat.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_hold_ms", 25, "Output Hold", "ms", "Output Hold parameter measured in ms. Pulp percussion engine: Metallic Hi-Hat.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_decay_ms", 26, "Output Decay", "ms", "Output Decay parameter measured in ms. Pulp percussion engine: Metallic Hi-Hat.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"output_bits", 27, "Output Bits", "bit", "Output Bits parameter measured in bit. Pulp percussion engine: Metallic Hi-Hat.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"1", "1", 1.0f}, {"2", "2", 2.0f}, {"3", "3", 3.0f}, {"4", "4", 4.0f}, {"5", "5", 5.0f}, {"6", "6", 6.0f}, {"7", "7", 7.0f}, {"8", "8", 8.0f}, {"9", "9", 9.0f}, {"10", "10", 10.0f}, {"11", "11", 11.0f}, {"12", "12", 12.0f}, {"13", "13", 13.0f}, {"14", "14", 14.0f}, {"15", "15", 15.0f}, {"16", "16", 16.0f}, {"17", "17", 17.0f}, {"18", "18", 18.0f}, {"19", "19", 19.0f}, {"20", "20", 20.0f}, {"21", "21", 21.0f}, {"22", "22", 22.0f}, {"23", "23", 23.0f}, {"24", "24", 24.0f}}, {}},
        {"output_hold_rate_hz", 28, "Output Hold Rate", "Hz", "Output Hold Rate parameter measured in Hz. Pulp percussion engine: Metallic Hi-Hat.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"output_jitter", 29, "Output Jitter", "", "Output Jitter parameter. Pulp percussion engine: Metallic Hi-Hat.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_smoothing", 30, "Output Smoothing", "", "Output Smoothing parameter. Pulp percussion engine: Metallic Hi-Hat.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_dead_zone", 31, "Output Dead Zone", "", "Output Dead Zone parameter. Pulp percussion engine: Metallic Hi-Hat.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"tune_hz", 10, "Tune", "Hz", "Tune parameter measured in Hz. Pulp percussion engine: Metallic Hi-Hat.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"decay", 11, "Decay", "", "Decay parameter. Pulp percussion engine: Metallic Hi-Hat.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"noise_color", 14, "Noise Color", "", "Noise Color parameter. Pulp percussion engine: Metallic Hi-Hat.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"0", "0", 0.0f}, {"1", "1", 1.0f}, {"2", "2", 2.0f}, {"3", "3", 3.0f}, {"4", "4", 4.0f}}, {}},
        {"hat_spread", 100, "Hat Spread", "", "Hat Spread parameter. Pulp percussion engine: Metallic Hi-Hat.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"hat_metal", 101, "Hat Metal", "", "Hat Metal parameter. Pulp percussion engine: Metallic Hi-Hat.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"hat_grit", 102, "Hat Grit", "", "Hat Grit parameter. Pulp percussion engine: Metallic Hi-Hat.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"hat_grit_ratio", 103, "Hat Grit Ratio", "", "Hat Grit Ratio parameter. Pulp percussion engine: Metallic Hi-Hat.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"hat_cutoff_hz", 104, "Hat Cutoff", "Hz", "Hat Cutoff parameter measured in Hz. Pulp percussion engine: Metallic Hi-Hat.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"hat_resonance", 105, "Hat Resonance", "", "Hat Resonance parameter. Pulp percussion engine: Metallic Hi-Hat.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"hat_bandpass", 106, "Hat Bandpass", "", "Hat Bandpass parameter. Pulp percussion engine: Metallic Hi-Hat.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
    };
    return d;
}

inline ForgeNodeDescriptor clap_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "clap";
    d.label = "Hand Clap";
    d.description = "Pulp percussion engine: Hand Clap.";
    d.realizations = {{"clap", "drum.clap"}};
    d.params = {
        {"trigger", 1, "Trigger", "", "Trigger parameter. Pulp percussion engine: Hand Clap.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"velocity", 2, "Velocity", "", "Velocity parameter. Pulp percussion engine: Hand Clap.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"choke", 3, "Choke", "", "Choke parameter. Pulp percussion engine: Hand Clap.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"choke_ms", 4, "Choke", "ms", "Choke parameter measured in ms. Pulp percussion engine: Hand Clap.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"velocity_level_db", 5, "Velocity Level", "dB", "Velocity Level parameter measured in dB. Pulp percussion engine: Hand Clap.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"velocity_brightness_octaves", 7, "Velocity Brightness", "oct", "Velocity Brightness parameter measured in oct. Pulp percussion engine: Hand Clap.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_drive", 20, "Output Drive", "", "Output Drive parameter. Pulp percussion engine: Hand Clap.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_fold", 21, "Output Fold", "", "Output Fold parameter. Pulp percussion engine: Hand Clap.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_level", 22, "Output Level", "", "Output Level parameter. Pulp percussion engine: Hand Clap.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_ahd_enabled", 23, "Output AHD Enabled", "", "Output AHD Enabled parameter. Pulp percussion engine: Hand Clap.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"output_attack_ms", 24, "Output Attack", "ms", "Output Attack parameter measured in ms. Pulp percussion engine: Hand Clap.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_hold_ms", 25, "Output Hold", "ms", "Output Hold parameter measured in ms. Pulp percussion engine: Hand Clap.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_decay_ms", 26, "Output Decay", "ms", "Output Decay parameter measured in ms. Pulp percussion engine: Hand Clap.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"output_bits", 27, "Output Bits", "bit", "Output Bits parameter measured in bit. Pulp percussion engine: Hand Clap.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"1", "1", 1.0f}, {"2", "2", 2.0f}, {"3", "3", 3.0f}, {"4", "4", 4.0f}, {"5", "5", 5.0f}, {"6", "6", 6.0f}, {"7", "7", 7.0f}, {"8", "8", 8.0f}, {"9", "9", 9.0f}, {"10", "10", 10.0f}, {"11", "11", 11.0f}, {"12", "12", 12.0f}, {"13", "13", 13.0f}, {"14", "14", 14.0f}, {"15", "15", 15.0f}, {"16", "16", 16.0f}, {"17", "17", 17.0f}, {"18", "18", 18.0f}, {"19", "19", 19.0f}, {"20", "20", 20.0f}, {"21", "21", 21.0f}, {"22", "22", 22.0f}, {"23", "23", 23.0f}, {"24", "24", 24.0f}}, {}},
        {"output_hold_rate_hz", 28, "Output Hold Rate", "Hz", "Output Hold Rate parameter measured in Hz. Pulp percussion engine: Hand Clap.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"output_jitter", 29, "Output Jitter", "", "Output Jitter parameter. Pulp percussion engine: Hand Clap.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_smoothing", 30, "Output Smoothing", "", "Output Smoothing parameter. Pulp percussion engine: Hand Clap.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_dead_zone", 31, "Output Dead Zone", "", "Output Dead Zone parameter. Pulp percussion engine: Hand Clap.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"noise_color", 14, "Noise Color", "", "Noise Color parameter. Pulp percussion engine: Hand Clap.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"0", "0", 0.0f}, {"1", "1", 1.0f}, {"2", "2", 2.0f}, {"3", "3", 3.0f}, {"4", "4", 4.0f}}, {}},
        {"clap_burst_count", 100, "Clap Burst Count", "", "Clap Burst Count parameter. Pulp percussion engine: Hand Clap.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"1", "1", 1.0f}, {"2", "2", 2.0f}, {"3", "3", 3.0f}, {"4", "4", 4.0f}, {"5", "5", 5.0f}, {"6", "6", 6.0f}, {"7", "7", 7.0f}, {"8", "8", 8.0f}}, {}},
        {"clap_burst_spacing_ms", 101, "Clap Burst Spacing", "ms", "Clap Burst Spacing parameter measured in ms. Pulp percussion engine: Hand Clap.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"clap_burst_decay_ms", 102, "Clap Burst Decay", "ms", "Clap Burst Decay parameter measured in ms. Pulp percussion engine: Hand Clap.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"clap_burst_falloff", 103, "Clap Burst Falloff", "", "Clap Burst Falloff parameter. Pulp percussion engine: Hand Clap.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"clap_gap_jitter", 104, "Clap Gap Jitter", "", "Clap Gap Jitter parameter. Pulp percussion engine: Hand Clap.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"clap_alternate_polarity", 105, "Clap Alternate Polarity", "", "Clap Alternate Polarity parameter. Pulp percussion engine: Hand Clap.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"clap_stereo_width", 106, "Clap Stereo Width", "", "Clap Stereo Width parameter. Pulp percussion engine: Hand Clap.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"clap_tail_level", 107, "Clap Tail Level", "", "Clap Tail Level parameter. Pulp percussion engine: Hand Clap.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"clap_tail_decay_ms", 108, "Clap Tail Decay", "ms", "Clap Tail Decay parameter measured in ms. Pulp percussion engine: Hand Clap.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"clap_cutoff_hz", 109, "Clap Cutoff", "Hz", "Clap Cutoff parameter measured in Hz. Pulp percussion engine: Hand Clap.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"clap_resonance", 110, "Clap Resonance", "", "Clap Resonance parameter. Pulp percussion engine: Hand Clap.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"clap_body_level", 111, "Clap Body Level", "", "Clap Body Level parameter. Pulp percussion engine: Hand Clap.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"clap_body_hz", 112, "Clap Body", "Hz", "Clap Body parameter measured in Hz. Pulp percussion engine: Hand Clap.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
    };
    return d;
}

inline ForgeNodeDescriptor tom_generic_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "tom.generic";
    d.label = "Generic Tom";
    d.description = "Pulp percussion engine: Generic Tom.";
    d.realizations = {{"tom.generic", "drum.tom.generic"}};
    d.params = {
        {"trigger", 1, "Trigger", "", "Trigger parameter. Pulp percussion engine: Generic Tom.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"velocity", 2, "Velocity", "", "Velocity parameter. Pulp percussion engine: Generic Tom.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"choke", 3, "Choke", "", "Choke parameter. Pulp percussion engine: Generic Tom.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"choke_ms", 4, "Choke", "ms", "Choke parameter measured in ms. Pulp percussion engine: Generic Tom.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"velocity_level_db", 5, "Velocity Level", "dB", "Velocity Level parameter measured in dB. Pulp percussion engine: Generic Tom.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"velocity_bend_octaves", 6, "Velocity Bend", "oct", "Velocity Bend parameter measured in oct. Pulp percussion engine: Generic Tom.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"velocity_brightness_octaves", 7, "Velocity Brightness", "oct", "Velocity Brightness parameter measured in oct. Pulp percussion engine: Generic Tom.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_drive", 20, "Output Drive", "", "Output Drive parameter. Pulp percussion engine: Generic Tom.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_fold", 21, "Output Fold", "", "Output Fold parameter. Pulp percussion engine: Generic Tom.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_level", 22, "Output Level", "", "Output Level parameter. Pulp percussion engine: Generic Tom.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_ahd_enabled", 23, "Output AHD Enabled", "", "Output AHD Enabled parameter. Pulp percussion engine: Generic Tom.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"output_attack_ms", 24, "Output Attack", "ms", "Output Attack parameter measured in ms. Pulp percussion engine: Generic Tom.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_hold_ms", 25, "Output Hold", "ms", "Output Hold parameter measured in ms. Pulp percussion engine: Generic Tom.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_decay_ms", 26, "Output Decay", "ms", "Output Decay parameter measured in ms. Pulp percussion engine: Generic Tom.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"output_bits", 27, "Output Bits", "bit", "Output Bits parameter measured in bit. Pulp percussion engine: Generic Tom.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"1", "1", 1.0f}, {"2", "2", 2.0f}, {"3", "3", 3.0f}, {"4", "4", 4.0f}, {"5", "5", 5.0f}, {"6", "6", 6.0f}, {"7", "7", 7.0f}, {"8", "8", 8.0f}, {"9", "9", 9.0f}, {"10", "10", 10.0f}, {"11", "11", 11.0f}, {"12", "12", 12.0f}, {"13", "13", 13.0f}, {"14", "14", 14.0f}, {"15", "15", 15.0f}, {"16", "16", 16.0f}, {"17", "17", 17.0f}, {"18", "18", 18.0f}, {"19", "19", 19.0f}, {"20", "20", 20.0f}, {"21", "21", 21.0f}, {"22", "22", 22.0f}, {"23", "23", 23.0f}, {"24", "24", 24.0f}}, {}},
        {"output_hold_rate_hz", 28, "Output Hold Rate", "Hz", "Output Hold Rate parameter measured in Hz. Pulp percussion engine: Generic Tom.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"output_jitter", 29, "Output Jitter", "", "Output Jitter parameter. Pulp percussion engine: Generic Tom.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_smoothing", 30, "Output Smoothing", "", "Output Smoothing parameter. Pulp percussion engine: Generic Tom.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_dead_zone", 31, "Output Dead Zone", "", "Output Dead Zone parameter. Pulp percussion engine: Generic Tom.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"tune_hz", 10, "Tune", "Hz", "Tune parameter measured in Hz. Pulp percussion engine: Generic Tom.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"decay", 11, "Decay", "", "Decay parameter. Pulp percussion engine: Generic Tom.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"pitch_sweep_octaves", 12, "Pitch Sweep", "oct", "Pitch Sweep parameter measured in oct. Pulp percussion engine: Generic Tom.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"pitch_sweep_ms", 13, "Pitch Sweep", "ms", "Pitch Sweep parameter measured in ms. Pulp percussion engine: Generic Tom.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"noise_color", 14, "Noise Color", "", "Noise Color parameter. Pulp percussion engine: Generic Tom.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"0", "0", 0.0f}, {"1", "1", 1.0f}, {"2", "2", 2.0f}, {"3", "3", 3.0f}, {"4", "4", 4.0f}}, {}},
        {"tom_wave", 100, "Tom Wave", "", "Tom Wave parameter. Pulp percussion engine: Generic Tom.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"0", "0", 0.0f}, {"1", "1", 1.0f}}, {}},
        {"tom_noise_balance", 101, "Tom Noise Balance", "", "Tom Noise Balance parameter. Pulp percussion engine: Generic Tom.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"tom_noise_cutoff_hz", 102, "Tom Noise Cutoff", "Hz", "Tom Noise Cutoff parameter measured in Hz. Pulp percussion engine: Generic Tom.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"tom_noise_resonance", 103, "Tom Noise Resonance", "", "Tom Noise Resonance parameter. Pulp percussion engine: Generic Tom.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"tom_click_level", 104, "Tom Click Level", "", "Tom Click Level parameter. Pulp percussion engine: Generic Tom.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"tom_click_cutoff_hz", 105, "Tom Click Cutoff", "Hz", "Tom Click Cutoff parameter measured in Hz. Pulp percussion engine: Generic Tom.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"tom_click_decay_ms", 106, "Tom Click Decay", "ms", "Tom Click Decay parameter measured in ms. Pulp percussion engine: Generic Tom.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"tone_lofi_bits", 50, "Tone Lofi Bits", "bit", "Tone Lofi Bits parameter measured in bit. Pulp percussion engine: Generic Tom.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"1", "1", 1.0f}, {"2", "2", 2.0f}, {"3", "3", 3.0f}, {"4", "4", 4.0f}, {"5", "5", 5.0f}, {"6", "6", 6.0f}, {"7", "7", 7.0f}, {"8", "8", 8.0f}, {"9", "9", 9.0f}, {"10", "10", 10.0f}, {"11", "11", 11.0f}, {"12", "12", 12.0f}, {"13", "13", 13.0f}, {"14", "14", 14.0f}, {"15", "15", 15.0f}, {"16", "16", 16.0f}, {"17", "17", 17.0f}, {"18", "18", 18.0f}, {"19", "19", 19.0f}, {"20", "20", 20.0f}, {"21", "21", 21.0f}, {"22", "22", 22.0f}, {"23", "23", 23.0f}, {"24", "24", 24.0f}}, {}},
        {"tone_lofi_hold_rate_hz", 51, "Tone Lofi Hold Rate", "Hz", "Tone Lofi Hold Rate parameter measured in Hz. Pulp percussion engine: Generic Tom.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"tone_lofi_jitter", 52, "Tone Lofi Jitter", "", "Tone Lofi Jitter parameter. Pulp percussion engine: Generic Tom.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"tone_lofi_smoothing", 53, "Tone Lofi Smoothing", "", "Tone Lofi Smoothing parameter. Pulp percussion engine: Generic Tom.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"tone_lofi_dead_zone", 54, "Tone Lofi Dead Zone", "", "Tone Lofi Dead Zone parameter. Pulp percussion engine: Generic Tom.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"noise_lofi_bits", 55, "Noise Lofi Bits", "bit", "Noise Lofi Bits parameter measured in bit. Pulp percussion engine: Generic Tom.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"1", "1", 1.0f}, {"2", "2", 2.0f}, {"3", "3", 3.0f}, {"4", "4", 4.0f}, {"5", "5", 5.0f}, {"6", "6", 6.0f}, {"7", "7", 7.0f}, {"8", "8", 8.0f}, {"9", "9", 9.0f}, {"10", "10", 10.0f}, {"11", "11", 11.0f}, {"12", "12", 12.0f}, {"13", "13", 13.0f}, {"14", "14", 14.0f}, {"15", "15", 15.0f}, {"16", "16", 16.0f}, {"17", "17", 17.0f}, {"18", "18", 18.0f}, {"19", "19", 19.0f}, {"20", "20", 20.0f}, {"21", "21", 21.0f}, {"22", "22", 22.0f}, {"23", "23", 23.0f}, {"24", "24", 24.0f}}, {}},
        {"noise_lofi_hold_rate_hz", 56, "Noise Lofi Hold Rate", "Hz", "Noise Lofi Hold Rate parameter measured in Hz. Pulp percussion engine: Generic Tom.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"noise_lofi_jitter", 57, "Noise Lofi Jitter", "", "Noise Lofi Jitter parameter. Pulp percussion engine: Generic Tom.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"noise_lofi_smoothing", 58, "Noise Lofi Smoothing", "", "Noise Lofi Smoothing parameter. Pulp percussion engine: Generic Tom.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"noise_lofi_dead_zone", 59, "Noise Lofi Dead Zone", "", "Noise Lofi Dead Zone parameter. Pulp percussion engine: Generic Tom.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
    };
    return d;
}

inline ForgeNodeDescriptor tom_simmons_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "tom.simmons";
    d.label = "SDS-V-Family Tom";
    d.description = "Pulp percussion engine: SDS-V-Family Tom.";
    d.realizations = {{"tom.simmons", "drum.tom.simmons"}};
    d.params = {
        {"trigger", 1, "Trigger", "", "Trigger parameter. Pulp percussion engine: SDS-V-Family Tom.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"velocity", 2, "Velocity", "", "Velocity parameter. Pulp percussion engine: SDS-V-Family Tom.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"choke", 3, "Choke", "", "Choke parameter. Pulp percussion engine: SDS-V-Family Tom.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"choke_ms", 4, "Choke", "ms", "Choke parameter measured in ms. Pulp percussion engine: SDS-V-Family Tom.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"velocity_level_db", 5, "Velocity Level", "dB", "Velocity Level parameter measured in dB. Pulp percussion engine: SDS-V-Family Tom.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"velocity_bend_octaves", 6, "Velocity Bend", "oct", "Velocity Bend parameter measured in oct. Pulp percussion engine: SDS-V-Family Tom.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"velocity_brightness_octaves", 7, "Velocity Brightness", "oct", "Velocity Brightness parameter measured in oct. Pulp percussion engine: SDS-V-Family Tom.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_drive", 20, "Output Drive", "", "Output Drive parameter. Pulp percussion engine: SDS-V-Family Tom.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_fold", 21, "Output Fold", "", "Output Fold parameter. Pulp percussion engine: SDS-V-Family Tom.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_level", 22, "Output Level", "", "Output Level parameter. Pulp percussion engine: SDS-V-Family Tom.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_ahd_enabled", 23, "Output AHD Enabled", "", "Output AHD Enabled parameter. Pulp percussion engine: SDS-V-Family Tom.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"output_attack_ms", 24, "Output Attack", "ms", "Output Attack parameter measured in ms. Pulp percussion engine: SDS-V-Family Tom.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_hold_ms", 25, "Output Hold", "ms", "Output Hold parameter measured in ms. Pulp percussion engine: SDS-V-Family Tom.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_decay_ms", 26, "Output Decay", "ms", "Output Decay parameter measured in ms. Pulp percussion engine: SDS-V-Family Tom.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"output_bits", 27, "Output Bits", "bit", "Output Bits parameter measured in bit. Pulp percussion engine: SDS-V-Family Tom.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"1", "1", 1.0f}, {"2", "2", 2.0f}, {"3", "3", 3.0f}, {"4", "4", 4.0f}, {"5", "5", 5.0f}, {"6", "6", 6.0f}, {"7", "7", 7.0f}, {"8", "8", 8.0f}, {"9", "9", 9.0f}, {"10", "10", 10.0f}, {"11", "11", 11.0f}, {"12", "12", 12.0f}, {"13", "13", 13.0f}, {"14", "14", 14.0f}, {"15", "15", 15.0f}, {"16", "16", 16.0f}, {"17", "17", 17.0f}, {"18", "18", 18.0f}, {"19", "19", 19.0f}, {"20", "20", 20.0f}, {"21", "21", 21.0f}, {"22", "22", 22.0f}, {"23", "23", 23.0f}, {"24", "24", 24.0f}}, {}},
        {"output_hold_rate_hz", 28, "Output Hold Rate", "Hz", "Output Hold Rate parameter measured in Hz. Pulp percussion engine: SDS-V-Family Tom.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"output_jitter", 29, "Output Jitter", "", "Output Jitter parameter. Pulp percussion engine: SDS-V-Family Tom.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_smoothing", 30, "Output Smoothing", "", "Output Smoothing parameter. Pulp percussion engine: SDS-V-Family Tom.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_dead_zone", 31, "Output Dead Zone", "", "Output Dead Zone parameter. Pulp percussion engine: SDS-V-Family Tom.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"tune_hz", 10, "Tune", "Hz", "Tune parameter measured in Hz. Pulp percussion engine: SDS-V-Family Tom.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"decay", 11, "Decay", "", "Decay parameter. Pulp percussion engine: SDS-V-Family Tom.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"pitch_sweep_octaves", 12, "Pitch Sweep", "oct", "Pitch Sweep parameter measured in oct. Pulp percussion engine: SDS-V-Family Tom.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"pitch_sweep_ms", 13, "Pitch Sweep", "ms", "Pitch Sweep parameter measured in ms. Pulp percussion engine: SDS-V-Family Tom.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"noise_color", 14, "Noise Color", "", "Noise Color parameter. Pulp percussion engine: SDS-V-Family Tom.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"0", "0", 0.0f}, {"1", "1", 1.0f}, {"2", "2", 2.0f}, {"3", "3", 3.0f}, {"4", "4", 4.0f}}, {}},
        {"tom_wave", 100, "Tom Wave", "", "Tom Wave parameter. Pulp percussion engine: SDS-V-Family Tom.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"0", "0", 0.0f}, {"1", "1", 1.0f}}, {}},
        {"tom_noise_balance", 101, "Tom Noise Balance", "", "Tom Noise Balance parameter. Pulp percussion engine: SDS-V-Family Tom.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"tom_noise_cutoff_hz", 102, "Tom Noise Cutoff", "Hz", "Tom Noise Cutoff parameter measured in Hz. Pulp percussion engine: SDS-V-Family Tom.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"tom_noise_resonance", 103, "Tom Noise Resonance", "", "Tom Noise Resonance parameter. Pulp percussion engine: SDS-V-Family Tom.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"tom_click_level", 104, "Tom Click Level", "", "Tom Click Level parameter. Pulp percussion engine: SDS-V-Family Tom.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"tom_click_cutoff_hz", 105, "Tom Click Cutoff", "Hz", "Tom Click Cutoff parameter measured in Hz. Pulp percussion engine: SDS-V-Family Tom.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"tom_click_decay_ms", 106, "Tom Click Decay", "ms", "Tom Click Decay parameter measured in ms. Pulp percussion engine: SDS-V-Family Tom.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"tone_lofi_bits", 50, "Tone Lofi Bits", "bit", "Tone Lofi Bits parameter measured in bit. Pulp percussion engine: SDS-V-Family Tom.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"1", "1", 1.0f}, {"2", "2", 2.0f}, {"3", "3", 3.0f}, {"4", "4", 4.0f}, {"5", "5", 5.0f}, {"6", "6", 6.0f}, {"7", "7", 7.0f}, {"8", "8", 8.0f}, {"9", "9", 9.0f}, {"10", "10", 10.0f}, {"11", "11", 11.0f}, {"12", "12", 12.0f}, {"13", "13", 13.0f}, {"14", "14", 14.0f}, {"15", "15", 15.0f}, {"16", "16", 16.0f}, {"17", "17", 17.0f}, {"18", "18", 18.0f}, {"19", "19", 19.0f}, {"20", "20", 20.0f}, {"21", "21", 21.0f}, {"22", "22", 22.0f}, {"23", "23", 23.0f}, {"24", "24", 24.0f}}, {}},
        {"tone_lofi_hold_rate_hz", 51, "Tone Lofi Hold Rate", "Hz", "Tone Lofi Hold Rate parameter measured in Hz. Pulp percussion engine: SDS-V-Family Tom.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"tone_lofi_jitter", 52, "Tone Lofi Jitter", "", "Tone Lofi Jitter parameter. Pulp percussion engine: SDS-V-Family Tom.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"tone_lofi_smoothing", 53, "Tone Lofi Smoothing", "", "Tone Lofi Smoothing parameter. Pulp percussion engine: SDS-V-Family Tom.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"tone_lofi_dead_zone", 54, "Tone Lofi Dead Zone", "", "Tone Lofi Dead Zone parameter. Pulp percussion engine: SDS-V-Family Tom.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"noise_lofi_bits", 55, "Noise Lofi Bits", "bit", "Noise Lofi Bits parameter measured in bit. Pulp percussion engine: SDS-V-Family Tom.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"1", "1", 1.0f}, {"2", "2", 2.0f}, {"3", "3", 3.0f}, {"4", "4", 4.0f}, {"5", "5", 5.0f}, {"6", "6", 6.0f}, {"7", "7", 7.0f}, {"8", "8", 8.0f}, {"9", "9", 9.0f}, {"10", "10", 10.0f}, {"11", "11", 11.0f}, {"12", "12", 12.0f}, {"13", "13", 13.0f}, {"14", "14", 14.0f}, {"15", "15", 15.0f}, {"16", "16", 16.0f}, {"17", "17", 17.0f}, {"18", "18", 18.0f}, {"19", "19", 19.0f}, {"20", "20", 20.0f}, {"21", "21", 21.0f}, {"22", "22", 22.0f}, {"23", "23", 23.0f}, {"24", "24", 24.0f}}, {}},
        {"noise_lofi_hold_rate_hz", 56, "Noise Lofi Hold Rate", "Hz", "Noise Lofi Hold Rate parameter measured in Hz. Pulp percussion engine: SDS-V-Family Tom.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"noise_lofi_jitter", 57, "Noise Lofi Jitter", "", "Noise Lofi Jitter parameter. Pulp percussion engine: SDS-V-Family Tom.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"noise_lofi_smoothing", 58, "Noise Lofi Smoothing", "", "Noise Lofi Smoothing parameter. Pulp percussion engine: SDS-V-Family Tom.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"noise_lofi_dead_zone", 59, "Noise Lofi Dead Zone", "", "Noise Lofi Dead Zone parameter. Pulp percussion engine: SDS-V-Family Tom.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
    };
    return d;
}

inline ForgeNodeDescriptor cymbal_comb_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "cymbal.comb";
    d.label = "Comb Cymbal";
    d.description = "Pulp percussion engine: Comb Cymbal.";
    d.realizations = {{"cymbal.comb", "drum.cymbal.comb"}};
    d.params = {
        {"trigger", 1, "Trigger", "", "Trigger parameter. Pulp percussion engine: Comb Cymbal.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"velocity", 2, "Velocity", "", "Velocity parameter. Pulp percussion engine: Comb Cymbal.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"choke", 3, "Choke", "", "Choke parameter. Pulp percussion engine: Comb Cymbal.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"choke_ms", 4, "Choke", "ms", "Choke parameter measured in ms. Pulp percussion engine: Comb Cymbal.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"velocity_level_db", 5, "Velocity Level", "dB", "Velocity Level parameter measured in dB. Pulp percussion engine: Comb Cymbal.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"velocity_brightness_octaves", 7, "Velocity Brightness", "oct", "Velocity Brightness parameter measured in oct. Pulp percussion engine: Comb Cymbal.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_drive", 20, "Output Drive", "", "Output Drive parameter. Pulp percussion engine: Comb Cymbal.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_fold", 21, "Output Fold", "", "Output Fold parameter. Pulp percussion engine: Comb Cymbal.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_level", 22, "Output Level", "", "Output Level parameter. Pulp percussion engine: Comb Cymbal.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_ahd_enabled", 23, "Output AHD Enabled", "", "Output AHD Enabled parameter. Pulp percussion engine: Comb Cymbal.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"output_attack_ms", 24, "Output Attack", "ms", "Output Attack parameter measured in ms. Pulp percussion engine: Comb Cymbal.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_hold_ms", 25, "Output Hold", "ms", "Output Hold parameter measured in ms. Pulp percussion engine: Comb Cymbal.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_decay_ms", 26, "Output Decay", "ms", "Output Decay parameter measured in ms. Pulp percussion engine: Comb Cymbal.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"output_bits", 27, "Output Bits", "bit", "Output Bits parameter measured in bit. Pulp percussion engine: Comb Cymbal.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"1", "1", 1.0f}, {"2", "2", 2.0f}, {"3", "3", 3.0f}, {"4", "4", 4.0f}, {"5", "5", 5.0f}, {"6", "6", 6.0f}, {"7", "7", 7.0f}, {"8", "8", 8.0f}, {"9", "9", 9.0f}, {"10", "10", 10.0f}, {"11", "11", 11.0f}, {"12", "12", 12.0f}, {"13", "13", 13.0f}, {"14", "14", 14.0f}, {"15", "15", 15.0f}, {"16", "16", 16.0f}, {"17", "17", 17.0f}, {"18", "18", 18.0f}, {"19", "19", 19.0f}, {"20", "20", 20.0f}, {"21", "21", 21.0f}, {"22", "22", 22.0f}, {"23", "23", 23.0f}, {"24", "24", 24.0f}}, {}},
        {"output_hold_rate_hz", 28, "Output Hold Rate", "Hz", "Output Hold Rate parameter measured in Hz. Pulp percussion engine: Comb Cymbal.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"output_jitter", 29, "Output Jitter", "", "Output Jitter parameter. Pulp percussion engine: Comb Cymbal.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_smoothing", 30, "Output Smoothing", "", "Output Smoothing parameter. Pulp percussion engine: Comb Cymbal.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_dead_zone", 31, "Output Dead Zone", "", "Output Dead Zone parameter. Pulp percussion engine: Comb Cymbal.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"tune_hz", 10, "Tune", "Hz", "Tune parameter measured in Hz. Pulp percussion engine: Comb Cymbal.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"decay", 11, "Decay", "", "Decay parameter. Pulp percussion engine: Comb Cymbal.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"noise_color", 14, "Noise Color", "", "Noise Color parameter. Pulp percussion engine: Comb Cymbal.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"0", "0", 0.0f}, {"1", "1", 1.0f}, {"2", "2", 2.0f}, {"3", "3", 3.0f}, {"4", "4", 4.0f}}, {}},
        {"cymbal_decay_tilt", 100, "Cymbal Decay Tilt", "", "Cymbal Decay Tilt parameter. Pulp percussion engine: Comb Cymbal.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"cymbal_high_mode_emphasis_db", 101, "Cymbal High Mode Emphasis", "dB", "Cymbal High Mode Emphasis parameter measured in dB. Pulp percussion engine: Comb Cymbal.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"cymbal_velocity_feedback", 102, "Cymbal Velocity Feedback", "", "Cymbal Velocity Feedback parameter. Pulp percussion engine: Comb Cymbal.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"cymbal_velocity_high_mode_db", 103, "Cymbal Velocity High Mode", "dB", "Cymbal Velocity High Mode parameter measured in dB. Pulp percussion engine: Comb Cymbal.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"cymbal_upper_highpass_hz", 104, "Cymbal Upper Highpass", "Hz", "Cymbal Upper Highpass parameter measured in Hz. Pulp percussion engine: Comb Cymbal.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"cymbal_spread", 105, "Cymbal Spread", "", "Cymbal Spread parameter. Pulp percussion engine: Comb Cymbal.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"cymbal_inharmonicity", 106, "Cymbal Inharmonicity", "", "Cymbal Inharmonicity parameter. Pulp percussion engine: Comb Cymbal.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"cymbal_shift_hz", 107, "Cymbal Shift", "Hz", "Cymbal Shift parameter measured in Hz. Pulp percussion engine: Comb Cymbal.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"cymbal_noise_level", 108, "Cymbal Noise Level", "", "Cymbal Noise Level parameter. Pulp percussion engine: Comb Cymbal.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"cymbal_strike_level", 109, "Cymbal Strike Level", "", "Cymbal Strike Level parameter. Pulp percussion engine: Comb Cymbal.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"cymbal_strike_ms", 110, "Cymbal Strike", "ms", "Cymbal Strike parameter measured in ms. Pulp percussion engine: Comb Cymbal.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"cymbal_tone_hz", 111, "Cymbal Tone", "Hz", "Cymbal Tone parameter measured in Hz. Pulp percussion engine: Comb Cymbal.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"cymbal_low_cut_hz", 112, "Cymbal Low Cut", "Hz", "Cymbal Low Cut parameter measured in Hz. Pulp percussion engine: Comb Cymbal.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"cymbal_hit_life", 114, "Cymbal Hit Life", "", "Cymbal Hit Life parameter. Pulp percussion engine: Comb Cymbal.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"0", "0", 0.0f}, {"1", "1", 1.0f}, {"2", "2", 2.0f}, {"3", "3", 3.0f}, {"4", "4", 4.0f}}, {}},
    };
    return d;
}

inline ForgeNodeDescriptor membrane_modal_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "membrane.modal";
    d.label = "Modal Membrane";
    d.description = "Pulp percussion engine: Modal Membrane.";
    d.realizations = {{"membrane.modal", "drum.membrane.modal"}};
    d.params = {
        {"trigger", 1, "Trigger", "", "Trigger parameter. Pulp percussion engine: Modal Membrane.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"velocity", 2, "Velocity", "", "Velocity parameter. Pulp percussion engine: Modal Membrane.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"choke", 3, "Choke", "", "Choke parameter. Pulp percussion engine: Modal Membrane.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"choke_ms", 4, "Choke", "ms", "Choke parameter measured in ms. Pulp percussion engine: Modal Membrane.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"velocity_level_db", 5, "Velocity Level", "dB", "Velocity Level parameter measured in dB. Pulp percussion engine: Modal Membrane.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"velocity_bend_octaves", 6, "Velocity Bend", "oct", "Velocity Bend parameter measured in oct. Pulp percussion engine: Modal Membrane.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"velocity_brightness_octaves", 7, "Velocity Brightness", "oct", "Velocity Brightness parameter measured in oct. Pulp percussion engine: Modal Membrane.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_drive", 20, "Output Drive", "", "Output Drive parameter. Pulp percussion engine: Modal Membrane.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_fold", 21, "Output Fold", "", "Output Fold parameter. Pulp percussion engine: Modal Membrane.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_level", 22, "Output Level", "", "Output Level parameter. Pulp percussion engine: Modal Membrane.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_ahd_enabled", 23, "Output AHD Enabled", "", "Output AHD Enabled parameter. Pulp percussion engine: Modal Membrane.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"output_attack_ms", 24, "Output Attack", "ms", "Output Attack parameter measured in ms. Pulp percussion engine: Modal Membrane.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_hold_ms", 25, "Output Hold", "ms", "Output Hold parameter measured in ms. Pulp percussion engine: Modal Membrane.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_decay_ms", 26, "Output Decay", "ms", "Output Decay parameter measured in ms. Pulp percussion engine: Modal Membrane.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"output_bits", 27, "Output Bits", "bit", "Output Bits parameter measured in bit. Pulp percussion engine: Modal Membrane.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"1", "1", 1.0f}, {"2", "2", 2.0f}, {"3", "3", 3.0f}, {"4", "4", 4.0f}, {"5", "5", 5.0f}, {"6", "6", 6.0f}, {"7", "7", 7.0f}, {"8", "8", 8.0f}, {"9", "9", 9.0f}, {"10", "10", 10.0f}, {"11", "11", 11.0f}, {"12", "12", 12.0f}, {"13", "13", 13.0f}, {"14", "14", 14.0f}, {"15", "15", 15.0f}, {"16", "16", 16.0f}, {"17", "17", 17.0f}, {"18", "18", 18.0f}, {"19", "19", 19.0f}, {"20", "20", 20.0f}, {"21", "21", 21.0f}, {"22", "22", 22.0f}, {"23", "23", 23.0f}, {"24", "24", 24.0f}}, {}},
        {"output_hold_rate_hz", 28, "Output Hold Rate", "Hz", "Output Hold Rate parameter measured in Hz. Pulp percussion engine: Modal Membrane.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"output_jitter", 29, "Output Jitter", "", "Output Jitter parameter. Pulp percussion engine: Modal Membrane.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_smoothing", 30, "Output Smoothing", "", "Output Smoothing parameter. Pulp percussion engine: Modal Membrane.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_dead_zone", 31, "Output Dead Zone", "", "Output Dead Zone parameter. Pulp percussion engine: Modal Membrane.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"tune_hz", 10, "Tune", "Hz", "Tune parameter measured in Hz. Pulp percussion engine: Modal Membrane.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"decay", 11, "Decay", "", "Decay parameter. Pulp percussion engine: Modal Membrane.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"membrane_structure", 100, "Membrane Structure", "", "Membrane Structure parameter. Pulp percussion engine: Modal Membrane.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"membrane_stretch", 101, "Membrane Stretch", "", "Membrane Stretch parameter. Pulp percussion engine: Modal Membrane.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"membrane_damping", 102, "Membrane Damping", "", "Membrane Damping parameter. Pulp percussion engine: Modal Membrane.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"membrane_brightness", 103, "Membrane Brightness", "", "Membrane Brightness parameter. Pulp percussion engine: Modal Membrane.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"membrane_position", 104, "Membrane Position", "", "Membrane Position parameter. Pulp percussion engine: Modal Membrane.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"membrane_spread", 105, "Membrane Spread", "", "Membrane Spread parameter. Pulp percussion engine: Modal Membrane.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"membrane_exciter_ms", 106, "Membrane Exciter", "ms", "Membrane Exciter parameter measured in ms. Pulp percussion engine: Modal Membrane.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"membrane_exciter_cutoff_hz", 107, "Membrane Exciter Cutoff", "Hz", "Membrane Exciter Cutoff parameter measured in Hz. Pulp percussion engine: Modal Membrane.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"membrane_exciter", 108, "Membrane Exciter", "", "Membrane Exciter parameter. Pulp percussion engine: Modal Membrane.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"membrane_sub_level", 109, "Membrane Sub Level", "", "Membrane Sub Level parameter. Pulp percussion engine: Modal Membrane.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"membrane_air_level", 110, "Membrane Air Level", "", "Membrane Air Level parameter. Pulp percussion engine: Modal Membrane.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"membrane_air_decay_ms", 111, "Membrane Air Decay", "ms", "Membrane Air Decay parameter measured in ms. Pulp percussion engine: Modal Membrane.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"membrane_click_level", 112, "Membrane Click Level", "", "Membrane Click Level parameter. Pulp percussion engine: Modal Membrane.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"membrane_click_decay_ms", 113, "Membrane Click Decay", "ms", "Membrane Click Decay parameter measured in ms. Pulp percussion engine: Modal Membrane.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"gate_rise_ms", 40, "Gate Rise", "ms", "Gate Rise parameter measured in ms. Pulp percussion engine: Modal Membrane.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"gate_fall_ms", 41, "Gate Fall", "ms", "Gate Fall parameter measured in ms. Pulp percussion engine: Modal Membrane.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"gate_colour", 42, "Gate Colour", "", "Gate Colour parameter. Pulp percussion engine: Modal Membrane.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"gate_closed_hz", 43, "Gate Closed", "Hz", "Gate Closed parameter measured in Hz. Pulp percussion engine: Modal Membrane.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"gate_open_hz", 44, "Gate Open", "Hz", "Gate Open parameter measured in Hz. Pulp percussion engine: Modal Membrane.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"gate_gain_exponent", 45, "Gate Gain Exponent", "", "Gate Gain Exponent parameter. Pulp percussion engine: Modal Membrane.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
    };
    return d;
}

inline ForgeNodeDescriptor string_karplus_strong_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "string.karplus-strong";
    d.label = "Karplus-Strong String";
    d.description = "Pulp percussion engine: Karplus-Strong String.";
    d.realizations = {{"string.karplus-strong", "drum.string.karplus-strong"}};
    d.params = {
        {"trigger", 1, "Trigger", "", "Trigger parameter. Pulp percussion engine: Karplus-Strong String.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"velocity", 2, "Velocity", "", "Velocity parameter. Pulp percussion engine: Karplus-Strong String.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"choke", 3, "Choke", "", "Choke parameter. Pulp percussion engine: Karplus-Strong String.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"choke_ms", 4, "Choke", "ms", "Choke parameter measured in ms. Pulp percussion engine: Karplus-Strong String.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"velocity_level_db", 5, "Velocity Level", "dB", "Velocity Level parameter measured in dB. Pulp percussion engine: Karplus-Strong String.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"velocity_bend_octaves", 6, "Velocity Bend", "oct", "Velocity Bend parameter measured in oct. Pulp percussion engine: Karplus-Strong String.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"velocity_brightness_octaves", 7, "Velocity Brightness", "oct", "Velocity Brightness parameter measured in oct. Pulp percussion engine: Karplus-Strong String.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_drive", 20, "Output Drive", "", "Output Drive parameter. Pulp percussion engine: Karplus-Strong String.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_fold", 21, "Output Fold", "", "Output Fold parameter. Pulp percussion engine: Karplus-Strong String.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_level", 22, "Output Level", "", "Output Level parameter. Pulp percussion engine: Karplus-Strong String.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_ahd_enabled", 23, "Output AHD Enabled", "", "Output AHD Enabled parameter. Pulp percussion engine: Karplus-Strong String.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"output_attack_ms", 24, "Output Attack", "ms", "Output Attack parameter measured in ms. Pulp percussion engine: Karplus-Strong String.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_hold_ms", 25, "Output Hold", "ms", "Output Hold parameter measured in ms. Pulp percussion engine: Karplus-Strong String.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_decay_ms", 26, "Output Decay", "ms", "Output Decay parameter measured in ms. Pulp percussion engine: Karplus-Strong String.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"output_bits", 27, "Output Bits", "bit", "Output Bits parameter measured in bit. Pulp percussion engine: Karplus-Strong String.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"1", "1", 1.0f}, {"2", "2", 2.0f}, {"3", "3", 3.0f}, {"4", "4", 4.0f}, {"5", "5", 5.0f}, {"6", "6", 6.0f}, {"7", "7", 7.0f}, {"8", "8", 8.0f}, {"9", "9", 9.0f}, {"10", "10", 10.0f}, {"11", "11", 11.0f}, {"12", "12", 12.0f}, {"13", "13", 13.0f}, {"14", "14", 14.0f}, {"15", "15", 15.0f}, {"16", "16", 16.0f}, {"17", "17", 17.0f}, {"18", "18", 18.0f}, {"19", "19", 19.0f}, {"20", "20", 20.0f}, {"21", "21", 21.0f}, {"22", "22", 22.0f}, {"23", "23", 23.0f}, {"24", "24", 24.0f}}, {}},
        {"output_hold_rate_hz", 28, "Output Hold Rate", "Hz", "Output Hold Rate parameter measured in Hz. Pulp percussion engine: Karplus-Strong String.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"output_jitter", 29, "Output Jitter", "", "Output Jitter parameter. Pulp percussion engine: Karplus-Strong String.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_smoothing", 30, "Output Smoothing", "", "Output Smoothing parameter. Pulp percussion engine: Karplus-Strong String.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_dead_zone", 31, "Output Dead Zone", "", "Output Dead Zone parameter. Pulp percussion engine: Karplus-Strong String.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"tune_hz", 10, "Tune", "Hz", "Tune parameter measured in Hz. Pulp percussion engine: Karplus-Strong String.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"decay", 11, "Decay", "", "Decay parameter. Pulp percussion engine: Karplus-Strong String.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"noise_color", 14, "Noise Color", "", "Noise Color parameter. Pulp percussion engine: Karplus-Strong String.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"0", "0", 0.0f}, {"1", "1", 1.0f}, {"2", "2", 2.0f}, {"3", "3", 3.0f}, {"4", "4", 4.0f}}, {}},
        {"string_damping", 100, "String Damping", "", "String Damping parameter. Pulp percussion engine: Karplus-Strong String.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"string_stiffness", 101, "String Stiffness", "", "String Stiffness parameter. Pulp percussion engine: Karplus-Strong String.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"string_pluck_position", 102, "String Pluck Position", "", "String Pluck Position parameter. Pulp percussion engine: Karplus-Strong String.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"string_exciter_ms", 103, "String Exciter", "ms", "String Exciter parameter measured in ms. Pulp percussion engine: Karplus-Strong String.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"string_brightness_hz", 104, "String Brightness", "Hz", "String Brightness parameter measured in Hz. Pulp percussion engine: Karplus-Strong String.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"string_pick_direction", 105, "String Pick Direction", "", "String Pick Direction parameter. Pulp percussion engine: Karplus-Strong String.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"string_restart_on_hit", 106, "String Restart On Hit", "", "String Restart On Hit parameter. Pulp percussion engine: Karplus-Strong String.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"string_modulation", 107, "String Modulation", "", "String Modulation parameter. Pulp percussion engine: Karplus-Strong String.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"string_modulation_mix", 108, "String Modulation Mix", "", "String Modulation Mix parameter. Pulp percussion engine: Karplus-Strong String.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"string_modulation_ratio", 109, "String Modulation Ratio", "", "String Modulation Ratio parameter. Pulp percussion engine: Karplus-Strong String.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"string_fm_depth_octaves", 110, "String FM Depth", "oct", "String FM Depth parameter measured in oct. Pulp percussion engine: Karplus-Strong String.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"string_lpg_amount", 111, "String LPG Amount", "", "String LPG Amount parameter. Pulp percussion engine: Karplus-Strong String.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"gate_rise_ms", 40, "Gate Rise", "ms", "Gate Rise parameter measured in ms. Pulp percussion engine: Karplus-Strong String.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"gate_fall_ms", 41, "Gate Fall", "ms", "Gate Fall parameter measured in ms. Pulp percussion engine: Karplus-Strong String.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"gate_colour", 42, "Gate Colour", "", "Gate Colour parameter. Pulp percussion engine: Karplus-Strong String.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"gate_closed_hz", 43, "Gate Closed", "Hz", "Gate Closed parameter measured in Hz. Pulp percussion engine: Karplus-Strong String.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"gate_open_hz", 44, "Gate Open", "Hz", "Gate Open parameter measured in Hz. Pulp percussion engine: Karplus-Strong String.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"gate_gain_exponent", 45, "Gate Gain Exponent", "", "Gate Gain Exponent parameter. Pulp percussion engine: Karplus-Strong String.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
    };
    return d;
}

inline ForgeNodeDescriptor zap_cz_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "zap.cz";
    d.label = "CZ Phase-Distortion Zap";
    d.description = "Pulp percussion engine: CZ Phase-Distortion Zap.";
    d.realizations = {{"zap.cz", "drum.zap.cz"}};
    d.params = {
        {"trigger", 1, "Trigger", "", "Trigger parameter. Pulp percussion engine: CZ Phase-Distortion Zap.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"velocity", 2, "Velocity", "", "Velocity parameter. Pulp percussion engine: CZ Phase-Distortion Zap.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"choke", 3, "Choke", "", "Choke parameter. Pulp percussion engine: CZ Phase-Distortion Zap.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"choke_ms", 4, "Choke", "ms", "Choke parameter measured in ms. Pulp percussion engine: CZ Phase-Distortion Zap.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"velocity_level_db", 5, "Velocity Level", "dB", "Velocity Level parameter measured in dB. Pulp percussion engine: CZ Phase-Distortion Zap.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"velocity_bend_octaves", 6, "Velocity Bend", "oct", "Velocity Bend parameter measured in oct. Pulp percussion engine: CZ Phase-Distortion Zap.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"velocity_brightness_octaves", 7, "Velocity Brightness", "oct", "Velocity Brightness parameter measured in oct. Pulp percussion engine: CZ Phase-Distortion Zap.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_drive", 20, "Output Drive", "", "Output Drive parameter. Pulp percussion engine: CZ Phase-Distortion Zap.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_fold", 21, "Output Fold", "", "Output Fold parameter. Pulp percussion engine: CZ Phase-Distortion Zap.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_level", 22, "Output Level", "", "Output Level parameter. Pulp percussion engine: CZ Phase-Distortion Zap.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_ahd_enabled", 23, "Output AHD Enabled", "", "Output AHD Enabled parameter. Pulp percussion engine: CZ Phase-Distortion Zap.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"output_attack_ms", 24, "Output Attack", "ms", "Output Attack parameter measured in ms. Pulp percussion engine: CZ Phase-Distortion Zap.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_hold_ms", 25, "Output Hold", "ms", "Output Hold parameter measured in ms. Pulp percussion engine: CZ Phase-Distortion Zap.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_decay_ms", 26, "Output Decay", "ms", "Output Decay parameter measured in ms. Pulp percussion engine: CZ Phase-Distortion Zap.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"output_bits", 27, "Output Bits", "bit", "Output Bits parameter measured in bit. Pulp percussion engine: CZ Phase-Distortion Zap.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"1", "1", 1.0f}, {"2", "2", 2.0f}, {"3", "3", 3.0f}, {"4", "4", 4.0f}, {"5", "5", 5.0f}, {"6", "6", 6.0f}, {"7", "7", 7.0f}, {"8", "8", 8.0f}, {"9", "9", 9.0f}, {"10", "10", 10.0f}, {"11", "11", 11.0f}, {"12", "12", 12.0f}, {"13", "13", 13.0f}, {"14", "14", 14.0f}, {"15", "15", 15.0f}, {"16", "16", 16.0f}, {"17", "17", 17.0f}, {"18", "18", 18.0f}, {"19", "19", 19.0f}, {"20", "20", 20.0f}, {"21", "21", 21.0f}, {"22", "22", 22.0f}, {"23", "23", 23.0f}, {"24", "24", 24.0f}}, {}},
        {"output_hold_rate_hz", 28, "Output Hold Rate", "Hz", "Output Hold Rate parameter measured in Hz. Pulp percussion engine: CZ Phase-Distortion Zap.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"output_jitter", 29, "Output Jitter", "", "Output Jitter parameter. Pulp percussion engine: CZ Phase-Distortion Zap.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_smoothing", 30, "Output Smoothing", "", "Output Smoothing parameter. Pulp percussion engine: CZ Phase-Distortion Zap.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_dead_zone", 31, "Output Dead Zone", "", "Output Dead Zone parameter. Pulp percussion engine: CZ Phase-Distortion Zap.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"tune_hz", 10, "Tune", "Hz", "Tune parameter measured in Hz. Pulp percussion engine: CZ Phase-Distortion Zap.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"decay", 11, "Decay", "", "Decay parameter. Pulp percussion engine: CZ Phase-Distortion Zap.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"pitch_sweep_octaves", 12, "Pitch Sweep", "oct", "Pitch Sweep parameter measured in oct. Pulp percussion engine: CZ Phase-Distortion Zap.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"pitch_sweep_ms", 13, "Pitch Sweep", "ms", "Pitch Sweep parameter measured in ms. Pulp percussion engine: CZ Phase-Distortion Zap.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"zap_shape", 100, "Zap Shape", "", "Zap Shape parameter. Pulp percussion engine: CZ Phase-Distortion Zap.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"zap_distortion", 101, "Zap Distortion", "", "Zap Distortion parameter. Pulp percussion engine: CZ Phase-Distortion Zap.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"zap_distortion_ms", 102, "Zap Distortion", "ms", "Zap Distortion parameter measured in ms. Pulp percussion engine: CZ Phase-Distortion Zap.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"zap_resonant_depth", 103, "Zap Resonant Depth", "", "Zap Resonant Depth parameter. Pulp percussion engine: CZ Phase-Distortion Zap.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"zap_detune_cents", 104, "Zap Detune", "cent", "Zap Detune parameter measured in cent. Pulp percussion engine: CZ Phase-Distortion Zap.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"zap_ring", 105, "Zap Ring", "", "Zap Ring parameter. Pulp percussion engine: CZ Phase-Distortion Zap.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"zap_ring_ratio", 106, "Zap Ring Ratio", "", "Zap Ring Ratio parameter. Pulp percussion engine: CZ Phase-Distortion Zap.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"gate_rise_ms", 40, "Gate Rise", "ms", "Gate Rise parameter measured in ms. Pulp percussion engine: CZ Phase-Distortion Zap.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"gate_fall_ms", 41, "Gate Fall", "ms", "Gate Fall parameter measured in ms. Pulp percussion engine: CZ Phase-Distortion Zap.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"gate_colour", 42, "Gate Colour", "", "Gate Colour parameter. Pulp percussion engine: CZ Phase-Distortion Zap.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"gate_closed_hz", 43, "Gate Closed", "Hz", "Gate Closed parameter measured in Hz. Pulp percussion engine: CZ Phase-Distortion Zap.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"gate_open_hz", 44, "Gate Open", "Hz", "Gate Open parameter measured in Hz. Pulp percussion engine: CZ Phase-Distortion Zap.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"gate_gain_exponent", 45, "Gate Gain Exponent", "", "Gate Gain Exponent parameter. Pulp percussion engine: CZ Phase-Distortion Zap.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
    };
    return d;
}

inline ForgeNodeDescriptor fm2_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "fm2";
    d.label = "Two-Operator FM Drum";
    d.description = "Pulp percussion engine: Two-Operator FM Drum.";
    d.realizations = {{"fm2", "drum.fm2"}};
    d.params = {
        {"trigger", 1, "Trigger", "", "Trigger parameter. Pulp percussion engine: Two-Operator FM Drum.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"velocity", 2, "Velocity", "", "Velocity parameter. Pulp percussion engine: Two-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"choke", 3, "Choke", "", "Choke parameter. Pulp percussion engine: Two-Operator FM Drum.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"choke_ms", 4, "Choke", "ms", "Choke parameter measured in ms. Pulp percussion engine: Two-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"velocity_level_db", 5, "Velocity Level", "dB", "Velocity Level parameter measured in dB. Pulp percussion engine: Two-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"velocity_bend_octaves", 6, "Velocity Bend", "oct", "Velocity Bend parameter measured in oct. Pulp percussion engine: Two-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"velocity_brightness_octaves", 7, "Velocity Brightness", "oct", "Velocity Brightness parameter measured in oct. Pulp percussion engine: Two-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_drive", 20, "Output Drive", "", "Output Drive parameter. Pulp percussion engine: Two-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_fold", 21, "Output Fold", "", "Output Fold parameter. Pulp percussion engine: Two-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_level", 22, "Output Level", "", "Output Level parameter. Pulp percussion engine: Two-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_ahd_enabled", 23, "Output AHD Enabled", "", "Output AHD Enabled parameter. Pulp percussion engine: Two-Operator FM Drum.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"output_attack_ms", 24, "Output Attack", "ms", "Output Attack parameter measured in ms. Pulp percussion engine: Two-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_hold_ms", 25, "Output Hold", "ms", "Output Hold parameter measured in ms. Pulp percussion engine: Two-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_decay_ms", 26, "Output Decay", "ms", "Output Decay parameter measured in ms. Pulp percussion engine: Two-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"output_bits", 27, "Output Bits", "bit", "Output Bits parameter measured in bit. Pulp percussion engine: Two-Operator FM Drum.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"1", "1", 1.0f}, {"2", "2", 2.0f}, {"3", "3", 3.0f}, {"4", "4", 4.0f}, {"5", "5", 5.0f}, {"6", "6", 6.0f}, {"7", "7", 7.0f}, {"8", "8", 8.0f}, {"9", "9", 9.0f}, {"10", "10", 10.0f}, {"11", "11", 11.0f}, {"12", "12", 12.0f}, {"13", "13", 13.0f}, {"14", "14", 14.0f}, {"15", "15", 15.0f}, {"16", "16", 16.0f}, {"17", "17", 17.0f}, {"18", "18", 18.0f}, {"19", "19", 19.0f}, {"20", "20", 20.0f}, {"21", "21", 21.0f}, {"22", "22", 22.0f}, {"23", "23", 23.0f}, {"24", "24", 24.0f}}, {}},
        {"output_hold_rate_hz", 28, "Output Hold Rate", "Hz", "Output Hold Rate parameter measured in Hz. Pulp percussion engine: Two-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"output_jitter", 29, "Output Jitter", "", "Output Jitter parameter. Pulp percussion engine: Two-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_smoothing", 30, "Output Smoothing", "", "Output Smoothing parameter. Pulp percussion engine: Two-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_dead_zone", 31, "Output Dead Zone", "", "Output Dead Zone parameter. Pulp percussion engine: Two-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"tune_hz", 10, "Tune", "Hz", "Tune parameter measured in Hz. Pulp percussion engine: Two-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"decay", 11, "Decay", "", "Decay parameter. Pulp percussion engine: Two-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"pitch_sweep_octaves", 12, "Pitch Sweep", "oct", "Pitch Sweep parameter measured in oct. Pulp percussion engine: Two-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"pitch_sweep_ms", 13, "Pitch Sweep", "ms", "Pitch Sweep parameter measured in ms. Pulp percussion engine: Two-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"noise_color", 14, "Noise Color", "", "Noise Color parameter. Pulp percussion engine: Two-Operator FM Drum.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"0", "0", 0.0f}, {"1", "1", 1.0f}, {"2", "2", 2.0f}, {"3", "3", 3.0f}, {"4", "4", 4.0f}}, {}},
        {"fm2_ratio", 100, "Fm2 Ratio", "", "Fm2 Ratio parameter. Pulp percussion engine: Two-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"fm2_index", 101, "Fm2 Index", "", "Fm2 Index parameter. Pulp percussion engine: Two-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"fm2_index_ms", 102, "Fm2 Index", "ms", "Fm2 Index parameter measured in ms. Pulp percussion engine: Two-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"fm2_feedback", 103, "Fm2 Feedback", "", "Fm2 Feedback parameter. Pulp percussion engine: Two-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"fm2_carrier_wave", 104, "Fm2 Carrier Wave", "", "Fm2 Carrier Wave parameter. Pulp percussion engine: Two-Operator FM Drum.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"0", "0", 0.0f}, {"1", "1", 1.0f}, {"2", "2", 2.0f}, {"3", "3", 3.0f}, {"4", "4", 4.0f}, {"5", "5", 5.0f}, {"6", "6", 6.0f}, {"7", "7", 7.0f}, {"8", "8", 8.0f}, {"9", "9", 9.0f}, {"10", "10", 10.0f}, {"11", "11", 11.0f}, {"12", "12", 12.0f}, {"13", "13", 13.0f}, {"14", "14", 14.0f}, {"15", "15", 15.0f}, {"16", "16", 16.0f}, {"17", "17", 17.0f}, {"18", "18", 18.0f}, {"19", "19", 19.0f}, {"20", "20", 20.0f}, {"21", "21", 21.0f}, {"22", "22", 22.0f}, {"23", "23", 23.0f}, {"24", "24", 24.0f}, {"25", "25", 25.0f}}, {}},
        {"fm2_modulator_wave", 105, "Fm2 Modulator Wave", "", "Fm2 Modulator Wave parameter. Pulp percussion engine: Two-Operator FM Drum.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"0", "0", 0.0f}, {"1", "1", 1.0f}, {"2", "2", 2.0f}, {"3", "3", 3.0f}, {"4", "4", 4.0f}, {"5", "5", 5.0f}, {"6", "6", 6.0f}, {"7", "7", 7.0f}, {"8", "8", 8.0f}, {"9", "9", 9.0f}, {"10", "10", 10.0f}, {"11", "11", 11.0f}, {"12", "12", 12.0f}, {"13", "13", 13.0f}, {"14", "14", 14.0f}, {"15", "15", 15.0f}, {"16", "16", 16.0f}, {"17", "17", 17.0f}, {"18", "18", 18.0f}, {"19", "19", 19.0f}, {"20", "20", 20.0f}, {"21", "21", 21.0f}, {"22", "22", 22.0f}, {"23", "23", 23.0f}, {"24", "24", 24.0f}, {"25", "25", 25.0f}}, {}},
        {"fm2_carrier_warp", 106, "Fm2 Carrier Warp", "", "Fm2 Carrier Warp parameter. Pulp percussion engine: Two-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"fm2_modulator_warp", 107, "Fm2 Modulator Warp", "", "Fm2 Modulator Warp parameter. Pulp percussion engine: Two-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"fm2_carrier_warp_ms", 108, "Fm2 Carrier Warp", "ms", "Fm2 Carrier Warp parameter measured in ms. Pulp percussion engine: Two-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"fm2_modulator_warp_ms", 109, "Fm2 Modulator Warp", "ms", "Fm2 Modulator Warp parameter measured in ms. Pulp percussion engine: Two-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"fm2_lfo_rate_hz", 110, "Fm2 LFO Rate", "Hz", "Fm2 LFO Rate parameter measured in Hz. Pulp percussion engine: Two-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"fm2_lfo_depth_octaves", 111, "Fm2 LFO Depth", "oct", "Fm2 LFO Depth parameter measured in oct. Pulp percussion engine: Two-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"fm2_lfo_delay_ms", 112, "Fm2 LFO Delay", "ms", "Fm2 LFO Delay parameter measured in ms. Pulp percussion engine: Two-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"fm2_lfo_fade_ms", 113, "Fm2 LFO Fade", "ms", "Fm2 LFO Fade parameter measured in ms. Pulp percussion engine: Two-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"fm2_hard_sync", 114, "Fm2 Hard Sync", "", "Fm2 Hard Sync parameter. Pulp percussion engine: Two-Operator FM Drum.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"fm2_transient", 115, "Fm2 Transient", "", "Fm2 Transient parameter. Pulp percussion engine: Two-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"fm2_noise_level", 116, "Fm2 Noise Level", "", "Fm2 Noise Level parameter. Pulp percussion engine: Two-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"fm2_noise_decay_ms", 117, "Fm2 Noise Decay", "ms", "Fm2 Noise Decay parameter measured in ms. Pulp percussion engine: Two-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"fm2_cutoff_hz", 118, "Fm2 Cutoff", "Hz", "Fm2 Cutoff parameter measured in Hz. Pulp percussion engine: Two-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"fm2_resonance", 119, "Fm2 Resonance", "", "Fm2 Resonance parameter. Pulp percussion engine: Two-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"fm2_bandpass", 120, "Fm2 Bandpass", "", "Fm2 Bandpass parameter. Pulp percussion engine: Two-Operator FM Drum.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"fm2_click_level", 121, "Fm2 Click Level", "", "Fm2 Click Level parameter. Pulp percussion engine: Two-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"fm2_click_cutoff_hz", 122, "Fm2 Click Cutoff", "Hz", "Fm2 Click Cutoff parameter measured in Hz. Pulp percussion engine: Two-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
    };
    return d;
}

inline ForgeNodeDescriptor fm6_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "fm6";
    d.label = "Six-Operator FM Drum";
    d.description = "Pulp percussion engine: Six-Operator FM Drum.";
    d.realizations = {{"fm6", "drum.fm6"}};
    d.params = {
        {"trigger", 1, "Trigger", "", "Trigger parameter. Pulp percussion engine: Six-Operator FM Drum.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"velocity", 2, "Velocity", "", "Velocity parameter. Pulp percussion engine: Six-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"choke", 3, "Choke", "", "Choke parameter. Pulp percussion engine: Six-Operator FM Drum.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"choke_ms", 4, "Choke", "ms", "Choke parameter measured in ms. Pulp percussion engine: Six-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"velocity_level_db", 5, "Velocity Level", "dB", "Velocity Level parameter measured in dB. Pulp percussion engine: Six-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"velocity_brightness_octaves", 7, "Velocity Brightness", "oct", "Velocity Brightness parameter measured in oct. Pulp percussion engine: Six-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_drive", 20, "Output Drive", "", "Output Drive parameter. Pulp percussion engine: Six-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_fold", 21, "Output Fold", "", "Output Fold parameter. Pulp percussion engine: Six-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_level", 22, "Output Level", "", "Output Level parameter. Pulp percussion engine: Six-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_ahd_enabled", 23, "Output AHD Enabled", "", "Output AHD Enabled parameter. Pulp percussion engine: Six-Operator FM Drum.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"output_attack_ms", 24, "Output Attack", "ms", "Output Attack parameter measured in ms. Pulp percussion engine: Six-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_hold_ms", 25, "Output Hold", "ms", "Output Hold parameter measured in ms. Pulp percussion engine: Six-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_decay_ms", 26, "Output Decay", "ms", "Output Decay parameter measured in ms. Pulp percussion engine: Six-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"output_bits", 27, "Output Bits", "bit", "Output Bits parameter measured in bit. Pulp percussion engine: Six-Operator FM Drum.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"1", "1", 1.0f}, {"2", "2", 2.0f}, {"3", "3", 3.0f}, {"4", "4", 4.0f}, {"5", "5", 5.0f}, {"6", "6", 6.0f}, {"7", "7", 7.0f}, {"8", "8", 8.0f}, {"9", "9", 9.0f}, {"10", "10", 10.0f}, {"11", "11", 11.0f}, {"12", "12", 12.0f}, {"13", "13", 13.0f}, {"14", "14", 14.0f}, {"15", "15", 15.0f}, {"16", "16", 16.0f}, {"17", "17", 17.0f}, {"18", "18", 18.0f}, {"19", "19", 19.0f}, {"20", "20", 20.0f}, {"21", "21", 21.0f}, {"22", "22", 22.0f}, {"23", "23", 23.0f}, {"24", "24", 24.0f}}, {}},
        {"output_hold_rate_hz", 28, "Output Hold Rate", "Hz", "Output Hold Rate parameter measured in Hz. Pulp percussion engine: Six-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"output_jitter", 29, "Output Jitter", "", "Output Jitter parameter. Pulp percussion engine: Six-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_smoothing", 30, "Output Smoothing", "", "Output Smoothing parameter. Pulp percussion engine: Six-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_dead_zone", 31, "Output Dead Zone", "", "Output Dead Zone parameter. Pulp percussion engine: Six-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"tune_hz", 10, "Tune", "Hz", "Tune parameter measured in Hz. Pulp percussion engine: Six-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"fm_algorithm", 100, "FM Algorithm", "", "FM Algorithm parameter. Pulp percussion engine: Six-Operator FM Drum.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"0", "0", 0.0f}, {"1", "1", 1.0f}, {"2", "2", 2.0f}, {"3", "3", 3.0f}, {"4", "4", 4.0f}, {"5", "5", 5.0f}, {"6", "6", 6.0f}, {"7", "7", 7.0f}, {"8", "8", 8.0f}, {"9", "9", 9.0f}, {"10", "10", 10.0f}, {"11", "11", 11.0f}, {"12", "12", 12.0f}, {"13", "13", 13.0f}, {"14", "14", 14.0f}, {"15", "15", 15.0f}, {"16", "16", 16.0f}, {"17", "17", 17.0f}, {"18", "18", 18.0f}, {"19", "19", 19.0f}, {"20", "20", 20.0f}, {"21", "21", 21.0f}, {"22", "22", 22.0f}, {"23", "23", 23.0f}, {"24", "24", 24.0f}, {"25", "25", 25.0f}, {"26", "26", 26.0f}, {"27", "27", 27.0f}, {"28", "28", 28.0f}, {"29", "29", 29.0f}, {"30", "30", 30.0f}, {"31", "31", 31.0f}}, {}},
        {"fm_depth", 101, "FM Depth", "", "FM Depth parameter. Pulp percussion engine: Six-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"fm_formant_hz", 102, "FM Formant", "Hz", "FM Formant parameter measured in Hz. Pulp percussion engine: Six-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"fm_formant_q", 103, "FM Formant Q", "", "FM Formant Q parameter. Pulp percussion engine: Six-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"pitch_sweep_octaves", 12, "Pitch Sweep", "oct", "Pitch Sweep parameter measured in oct. Pulp percussion engine: Six-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"pitch_sweep_ms", 13, "Pitch Sweep", "ms", "Pitch Sweep parameter measured in ms. Pulp percussion engine: Six-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"fm6_feedback", 104, "Fm6 Feedback", "", "Fm6 Feedback parameter. Pulp percussion engine: Six-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"operator_1_ratio", 200, "Operator 1 Ratio", "", "Operator 1 Ratio parameter. Pulp percussion engine: Six-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"operator_1_level", 220, "Operator 1 Level", "", "Operator 1 Level parameter. Pulp percussion engine: Six-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"operator_1_decay_ms", 240, "Operator 1 Decay", "ms", "Operator 1 Decay parameter measured in ms. Pulp percussion engine: Six-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"operator_2_ratio", 201, "Operator 2 Ratio", "", "Operator 2 Ratio parameter. Pulp percussion engine: Six-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"operator_2_level", 221, "Operator 2 Level", "", "Operator 2 Level parameter. Pulp percussion engine: Six-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"operator_2_decay_ms", 241, "Operator 2 Decay", "ms", "Operator 2 Decay parameter measured in ms. Pulp percussion engine: Six-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"operator_3_ratio", 202, "Operator 3 Ratio", "", "Operator 3 Ratio parameter. Pulp percussion engine: Six-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"operator_3_level", 222, "Operator 3 Level", "", "Operator 3 Level parameter. Pulp percussion engine: Six-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"operator_3_decay_ms", 242, "Operator 3 Decay", "ms", "Operator 3 Decay parameter measured in ms. Pulp percussion engine: Six-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"operator_4_ratio", 203, "Operator 4 Ratio", "", "Operator 4 Ratio parameter. Pulp percussion engine: Six-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"operator_4_level", 223, "Operator 4 Level", "", "Operator 4 Level parameter. Pulp percussion engine: Six-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"operator_4_decay_ms", 243, "Operator 4 Decay", "ms", "Operator 4 Decay parameter measured in ms. Pulp percussion engine: Six-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"operator_5_ratio", 204, "Operator 5 Ratio", "", "Operator 5 Ratio parameter. Pulp percussion engine: Six-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"operator_5_level", 224, "Operator 5 Level", "", "Operator 5 Level parameter. Pulp percussion engine: Six-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"operator_5_decay_ms", 244, "Operator 5 Decay", "ms", "Operator 5 Decay parameter measured in ms. Pulp percussion engine: Six-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"operator_6_ratio", 205, "Operator 6 Ratio", "", "Operator 6 Ratio parameter. Pulp percussion engine: Six-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"operator_6_level", 225, "Operator 6 Level", "", "Operator 6 Level parameter. Pulp percussion engine: Six-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"operator_6_decay_ms", 245, "Operator 6 Decay", "ms", "Operator 6 Decay parameter measured in ms. Pulp percussion engine: Six-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
    };
    return d;
}

inline ForgeNodeDescriptor fm8_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "fm8";
    d.label = "Eight-Operator FM Drum";
    d.description = "Pulp percussion engine: Eight-Operator FM Drum.";
    d.realizations = {{"fm8", "drum.fm8"}};
    d.params = {
        {"trigger", 1, "Trigger", "", "Trigger parameter. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"velocity", 2, "Velocity", "", "Velocity parameter. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"choke", 3, "Choke", "", "Choke parameter. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"choke_ms", 4, "Choke", "ms", "Choke parameter measured in ms. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"velocity_level_db", 5, "Velocity Level", "dB", "Velocity Level parameter measured in dB. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"velocity_brightness_octaves", 7, "Velocity Brightness", "oct", "Velocity Brightness parameter measured in oct. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_drive", 20, "Output Drive", "", "Output Drive parameter. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_fold", 21, "Output Fold", "", "Output Fold parameter. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_level", 22, "Output Level", "", "Output Level parameter. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_ahd_enabled", 23, "Output AHD Enabled", "", "Output AHD Enabled parameter. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}, {}},
        {"output_attack_ms", 24, "Output Attack", "ms", "Output Attack parameter measured in ms. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_hold_ms", 25, "Output Hold", "ms", "Output Hold parameter measured in ms. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_decay_ms", 26, "Output Decay", "ms", "Output Decay parameter measured in ms. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"output_bits", 27, "Output Bits", "bit", "Output Bits parameter measured in bit. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"1", "1", 1.0f}, {"2", "2", 2.0f}, {"3", "3", 3.0f}, {"4", "4", 4.0f}, {"5", "5", 5.0f}, {"6", "6", 6.0f}, {"7", "7", 7.0f}, {"8", "8", 8.0f}, {"9", "9", 9.0f}, {"10", "10", 10.0f}, {"11", "11", 11.0f}, {"12", "12", 12.0f}, {"13", "13", 13.0f}, {"14", "14", 14.0f}, {"15", "15", 15.0f}, {"16", "16", 16.0f}, {"17", "17", 17.0f}, {"18", "18", 18.0f}, {"19", "19", 19.0f}, {"20", "20", 20.0f}, {"21", "21", 21.0f}, {"22", "22", 22.0f}, {"23", "23", 23.0f}, {"24", "24", 24.0f}}, {}},
        {"output_hold_rate_hz", 28, "Output Hold Rate", "Hz", "Output Hold Rate parameter measured in Hz. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"output_jitter", 29, "Output Jitter", "", "Output Jitter parameter. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_smoothing", 30, "Output Smoothing", "", "Output Smoothing parameter. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"output_dead_zone", 31, "Output Dead Zone", "", "Output Dead Zone parameter. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"tune_hz", 10, "Tune", "Hz", "Tune parameter measured in Hz. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"fm_algorithm", 100, "FM Algorithm", "", "FM Algorithm parameter. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"0", "0", 0.0f}, {"1", "1", 1.0f}, {"2", "2", 2.0f}, {"3", "3", 3.0f}, {"4", "4", 4.0f}, {"5", "5", 5.0f}, {"6", "6", 6.0f}, {"7", "7", 7.0f}, {"8", "8", 8.0f}, {"9", "9", 9.0f}, {"10", "10", 10.0f}, {"11", "11", 11.0f}, {"12", "12", 12.0f}, {"13", "13", 13.0f}, {"14", "14", 14.0f}, {"15", "15", 15.0f}}, {}},
        {"fm_depth", 101, "FM Depth", "", "FM Depth parameter. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"fm_formant_hz", 102, "FM Formant", "Hz", "FM Formant parameter measured in Hz. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"fm_formant_q", 103, "FM Formant Q", "", "FM Formant Q parameter. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"noise_color", 14, "Noise Color", "", "Noise Color parameter. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"0", "0", 0.0f}, {"1", "1", 1.0f}, {"2", "2", 2.0f}, {"3", "3", 3.0f}, {"4", "4", 4.0f}}, {}},
        {"fm8_transient", 104, "Fm8 Transient", "", "Fm8 Transient parameter. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"fm8_noise_level", 105, "Fm8 Noise Level", "", "Fm8 Noise Level parameter. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"fm8_noise_decay_ms", 106, "Fm8 Noise Decay", "ms", "Fm8 Noise Decay parameter measured in ms. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"fm8_click_level", 107, "Fm8 Click Level", "", "Fm8 Click Level parameter. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"operator_1_ratio", 200, "Operator 1 Ratio", "", "Operator 1 Ratio parameter. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"operator_1_level", 220, "Operator 1 Level", "", "Operator 1 Level parameter. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"operator_1_decay_ms", 240, "Operator 1 Decay", "ms", "Operator 1 Decay parameter measured in ms. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"operator_1_feedback", 260, "Operator 1 Feedback", "", "Operator 1 Feedback parameter. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"operator_1_wave", 280, "Operator 1 Wave", "", "Operator 1 Wave parameter. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"0", "0", 0.0f}, {"1", "1", 1.0f}, {"2", "2", 2.0f}, {"3", "3", 3.0f}, {"4", "4", 4.0f}, {"5", "5", 5.0f}, {"6", "6", 6.0f}, {"7", "7", 7.0f}, {"8", "8", 8.0f}, {"9", "9", 9.0f}, {"10", "10", 10.0f}, {"11", "11", 11.0f}, {"12", "12", 12.0f}, {"13", "13", 13.0f}, {"14", "14", 14.0f}, {"15", "15", 15.0f}, {"16", "16", 16.0f}, {"17", "17", 17.0f}, {"18", "18", 18.0f}, {"19", "19", 19.0f}, {"20", "20", 20.0f}, {"21", "21", 21.0f}, {"22", "22", 22.0f}, {"23", "23", 23.0f}, {"24", "24", 24.0f}, {"25", "25", 25.0f}}, {}},
        {"operator_2_ratio", 201, "Operator 2 Ratio", "", "Operator 2 Ratio parameter. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"operator_2_level", 221, "Operator 2 Level", "", "Operator 2 Level parameter. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"operator_2_decay_ms", 241, "Operator 2 Decay", "ms", "Operator 2 Decay parameter measured in ms. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"operator_2_feedback", 261, "Operator 2 Feedback", "", "Operator 2 Feedback parameter. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"operator_2_wave", 281, "Operator 2 Wave", "", "Operator 2 Wave parameter. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"0", "0", 0.0f}, {"1", "1", 1.0f}, {"2", "2", 2.0f}, {"3", "3", 3.0f}, {"4", "4", 4.0f}, {"5", "5", 5.0f}, {"6", "6", 6.0f}, {"7", "7", 7.0f}, {"8", "8", 8.0f}, {"9", "9", 9.0f}, {"10", "10", 10.0f}, {"11", "11", 11.0f}, {"12", "12", 12.0f}, {"13", "13", 13.0f}, {"14", "14", 14.0f}, {"15", "15", 15.0f}, {"16", "16", 16.0f}, {"17", "17", 17.0f}, {"18", "18", 18.0f}, {"19", "19", 19.0f}, {"20", "20", 20.0f}, {"21", "21", 21.0f}, {"22", "22", 22.0f}, {"23", "23", 23.0f}, {"24", "24", 24.0f}, {"25", "25", 25.0f}}, {}},
        {"operator_3_ratio", 202, "Operator 3 Ratio", "", "Operator 3 Ratio parameter. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"operator_3_level", 222, "Operator 3 Level", "", "Operator 3 Level parameter. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"operator_3_decay_ms", 242, "Operator 3 Decay", "ms", "Operator 3 Decay parameter measured in ms. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"operator_3_feedback", 262, "Operator 3 Feedback", "", "Operator 3 Feedback parameter. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"operator_3_wave", 282, "Operator 3 Wave", "", "Operator 3 Wave parameter. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"0", "0", 0.0f}, {"1", "1", 1.0f}, {"2", "2", 2.0f}, {"3", "3", 3.0f}, {"4", "4", 4.0f}, {"5", "5", 5.0f}, {"6", "6", 6.0f}, {"7", "7", 7.0f}, {"8", "8", 8.0f}, {"9", "9", 9.0f}, {"10", "10", 10.0f}, {"11", "11", 11.0f}, {"12", "12", 12.0f}, {"13", "13", 13.0f}, {"14", "14", 14.0f}, {"15", "15", 15.0f}, {"16", "16", 16.0f}, {"17", "17", 17.0f}, {"18", "18", 18.0f}, {"19", "19", 19.0f}, {"20", "20", 20.0f}, {"21", "21", 21.0f}, {"22", "22", 22.0f}, {"23", "23", 23.0f}, {"24", "24", 24.0f}, {"25", "25", 25.0f}}, {}},
        {"operator_4_ratio", 203, "Operator 4 Ratio", "", "Operator 4 Ratio parameter. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"operator_4_level", 223, "Operator 4 Level", "", "Operator 4 Level parameter. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"operator_4_decay_ms", 243, "Operator 4 Decay", "ms", "Operator 4 Decay parameter measured in ms. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"operator_4_feedback", 263, "Operator 4 Feedback", "", "Operator 4 Feedback parameter. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"operator_4_wave", 283, "Operator 4 Wave", "", "Operator 4 Wave parameter. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"0", "0", 0.0f}, {"1", "1", 1.0f}, {"2", "2", 2.0f}, {"3", "3", 3.0f}, {"4", "4", 4.0f}, {"5", "5", 5.0f}, {"6", "6", 6.0f}, {"7", "7", 7.0f}, {"8", "8", 8.0f}, {"9", "9", 9.0f}, {"10", "10", 10.0f}, {"11", "11", 11.0f}, {"12", "12", 12.0f}, {"13", "13", 13.0f}, {"14", "14", 14.0f}, {"15", "15", 15.0f}, {"16", "16", 16.0f}, {"17", "17", 17.0f}, {"18", "18", 18.0f}, {"19", "19", 19.0f}, {"20", "20", 20.0f}, {"21", "21", 21.0f}, {"22", "22", 22.0f}, {"23", "23", 23.0f}, {"24", "24", 24.0f}, {"25", "25", 25.0f}}, {}},
        {"operator_5_ratio", 204, "Operator 5 Ratio", "", "Operator 5 Ratio parameter. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"operator_5_level", 224, "Operator 5 Level", "", "Operator 5 Level parameter. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"operator_5_decay_ms", 244, "Operator 5 Decay", "ms", "Operator 5 Decay parameter measured in ms. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"operator_5_feedback", 264, "Operator 5 Feedback", "", "Operator 5 Feedback parameter. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"operator_5_wave", 284, "Operator 5 Wave", "", "Operator 5 Wave parameter. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"0", "0", 0.0f}, {"1", "1", 1.0f}, {"2", "2", 2.0f}, {"3", "3", 3.0f}, {"4", "4", 4.0f}, {"5", "5", 5.0f}, {"6", "6", 6.0f}, {"7", "7", 7.0f}, {"8", "8", 8.0f}, {"9", "9", 9.0f}, {"10", "10", 10.0f}, {"11", "11", 11.0f}, {"12", "12", 12.0f}, {"13", "13", 13.0f}, {"14", "14", 14.0f}, {"15", "15", 15.0f}, {"16", "16", 16.0f}, {"17", "17", 17.0f}, {"18", "18", 18.0f}, {"19", "19", 19.0f}, {"20", "20", 20.0f}, {"21", "21", 21.0f}, {"22", "22", 22.0f}, {"23", "23", 23.0f}, {"24", "24", 24.0f}, {"25", "25", 25.0f}}, {}},
        {"operator_6_ratio", 205, "Operator 6 Ratio", "", "Operator 6 Ratio parameter. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"operator_6_level", 225, "Operator 6 Level", "", "Operator 6 Level parameter. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"operator_6_decay_ms", 245, "Operator 6 Decay", "ms", "Operator 6 Decay parameter measured in ms. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"operator_6_feedback", 265, "Operator 6 Feedback", "", "Operator 6 Feedback parameter. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"operator_6_wave", 285, "Operator 6 Wave", "", "Operator 6 Wave parameter. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"0", "0", 0.0f}, {"1", "1", 1.0f}, {"2", "2", 2.0f}, {"3", "3", 3.0f}, {"4", "4", 4.0f}, {"5", "5", 5.0f}, {"6", "6", 6.0f}, {"7", "7", 7.0f}, {"8", "8", 8.0f}, {"9", "9", 9.0f}, {"10", "10", 10.0f}, {"11", "11", 11.0f}, {"12", "12", 12.0f}, {"13", "13", 13.0f}, {"14", "14", 14.0f}, {"15", "15", 15.0f}, {"16", "16", 16.0f}, {"17", "17", 17.0f}, {"18", "18", 18.0f}, {"19", "19", 19.0f}, {"20", "20", 20.0f}, {"21", "21", 21.0f}, {"22", "22", 22.0f}, {"23", "23", 23.0f}, {"24", "24", 24.0f}, {"25", "25", 25.0f}}, {}},
        {"operator_7_ratio", 206, "Operator 7 Ratio", "", "Operator 7 Ratio parameter. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"operator_7_level", 226, "Operator 7 Level", "", "Operator 7 Level parameter. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"operator_7_decay_ms", 246, "Operator 7 Decay", "ms", "Operator 7 Decay parameter measured in ms. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"operator_7_feedback", 266, "Operator 7 Feedback", "", "Operator 7 Feedback parameter. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"operator_7_wave", 286, "Operator 7 Wave", "", "Operator 7 Wave parameter. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"0", "0", 0.0f}, {"1", "1", 1.0f}, {"2", "2", 2.0f}, {"3", "3", 3.0f}, {"4", "4", 4.0f}, {"5", "5", 5.0f}, {"6", "6", 6.0f}, {"7", "7", 7.0f}, {"8", "8", 8.0f}, {"9", "9", 9.0f}, {"10", "10", 10.0f}, {"11", "11", 11.0f}, {"12", "12", 12.0f}, {"13", "13", 13.0f}, {"14", "14", 14.0f}, {"15", "15", 15.0f}, {"16", "16", 16.0f}, {"17", "17", 17.0f}, {"18", "18", 18.0f}, {"19", "19", 19.0f}, {"20", "20", 20.0f}, {"21", "21", 21.0f}, {"22", "22", 22.0f}, {"23", "23", 23.0f}, {"24", "24", 24.0f}, {"25", "25", 25.0f}}, {}},
        {"operator_8_ratio", 207, "Operator 8 Ratio", "", "Operator 8 Ratio parameter. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"operator_8_level", 227, "Operator 8 Level", "", "Operator 8 Level parameter. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"operator_8_decay_ms", 247, "Operator 8 Decay", "ms", "Operator 8 Decay parameter measured in ms. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic, {}, {}},
        {"operator_8_feedback", 267, "Operator 8 Feedback", "", "Operator 8 Feedback parameter. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::continuous, ForgeParamCurve::linear, {}, {}},
        {"operator_8_wave", 287, "Operator 8 Wave", "", "Operator 8 Wave parameter. Pulp percussion engine: Eight-Operator FM Drum.", ForgeParamKind::stepped, ForgeParamCurve::linear, {{"0", "0", 0.0f}, {"1", "1", 1.0f}, {"2", "2", 2.0f}, {"3", "3", 3.0f}, {"4", "4", 4.0f}, {"5", "5", 5.0f}, {"6", "6", 6.0f}, {"7", "7", 7.0f}, {"8", "8", 8.0f}, {"9", "9", 9.0f}, {"10", "10", 10.0f}, {"11", "11", 11.0f}, {"12", "12", 12.0f}, {"13", "13", 13.0f}, {"14", "14", 14.0f}, {"15", "15", 15.0f}, {"16", "16", 16.0f}, {"17", "17", 17.0f}, {"18", "18", 18.0f}, {"19", "19", 19.0f}, {"20", "20", 20.0f}, {"21", "21", 21.0f}, {"22", "22", 22.0f}, {"23", "23", 23.0f}, {"24", "24", 24.0f}, {"25", "25", 25.0f}}, {}},
    };
    return d;
}

inline std::vector<ForgeNodeDescriptor> drum_descriptors() {
    return {
        kick_oscillator_descriptor(),
        kick_resonant_descriptor(),
        kick_circuit_descriptor(),
        snare_descriptor(),
        hat_descriptor(),
        clap_descriptor(),
        tom_generic_descriptor(),
        tom_simmons_descriptor(),
        cymbal_comb_descriptor(),
        membrane_modal_descriptor(),
        string_karplus_strong_descriptor(),
        zap_cz_descriptor(),
        fm2_descriptor(),
        fm6_descriptor(),
        fm8_descriptor(),
    };
}

}  // namespace pulp::host::forge_drum
