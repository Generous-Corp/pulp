#pragma once

// Forge bake-layer adapter for Pulp's license-clean percussion engines.
//
// Engine identity is construction-time topology. Everything in baked_params is
// an RT-safe, node-local performance or voicing control consumed through
// BakedParamView. Output oversampling is deliberately fixed to bypass because
// CustomNodeType has no latency-reporting surface; changing it would both reset
// the output stage and make parallel graph paths misaligned.

#include <pulp/host/signal_graph.hpp>
#include <pulp/signal/drum/engine_registry.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

namespace pulp::host::forge_drum {

using signal::drum::EngineId;

inline constexpr const char* kKickOscillatorTypeId = "drum.kick.oscillator";
inline constexpr const char* kKickResonantTypeId = "drum.kick.resonant";
inline constexpr const char* kKickCircuitTypeId = "drum.kick.circuit";
inline constexpr const char* kSnareTypeId = "drum.snare";
inline constexpr const char* kHatTypeId = "drum.hat";
inline constexpr const char* kClapTypeId = "drum.clap";
inline constexpr const char* kTomGenericTypeId = "drum.tom.generic";
inline constexpr const char* kTomSimmonsTypeId = "drum.tom.simmons";
inline constexpr const char* kCymbalTypeId = "drum.cymbal.comb";
inline constexpr const char* kMembraneTypeId = "drum.membrane.modal";
inline constexpr const char* kStringTypeId = "drum.string.karplus-strong";
inline constexpr const char* kZapTypeId = "drum.zap.cz";
inline constexpr const char* kFm2TypeId = "drum.fm2";
inline constexpr const char* kFm6TypeId = "drum.fm6";
inline constexpr const char* kFm8TypeId = "drum.fm8";

// Stable node-local IDs. A semantic keeps the same ID wherever it appears;
// engine-specific controls occupy disjoint ranges so future additions do not
// renumber an existing preset.
inline constexpr state::ParamID kTrigger = 1;
inline constexpr state::ParamID kVelocity = 2;
inline constexpr state::ParamID kChoke = 3;
inline constexpr state::ParamID kChokeMs = 4;
inline constexpr state::ParamID kVelocityLevelDb = 5;
inline constexpr state::ParamID kVelocityBendOctaves = 6;
inline constexpr state::ParamID kVelocityBrightnessOctaves = 7;
inline constexpr state::ParamID kVelocityNoiseBalance = 8;
inline constexpr state::ParamID kTuneHz = 10;
inline constexpr state::ParamID kDecay = 11;
inline constexpr state::ParamID kPitchSweepOctaves = 12;
inline constexpr state::ParamID kPitchSweepMs = 13;
inline constexpr state::ParamID kNoiseColor = 14;
inline constexpr state::ParamID kOutputDrive = 20;
inline constexpr state::ParamID kOutputFold = 21;
inline constexpr state::ParamID kOutputLevel = 22;
inline constexpr state::ParamID kOutputAhdEnabled = 23;
inline constexpr state::ParamID kOutputAttackMs = 24;
inline constexpr state::ParamID kOutputHoldMs = 25;
inline constexpr state::ParamID kOutputDecayMs = 26;
inline constexpr state::ParamID kOutputBits = 27;
inline constexpr state::ParamID kOutputHoldRateHz = 28;
inline constexpr state::ParamID kOutputJitter = 29;
inline constexpr state::ParamID kOutputSmoothing = 30;
inline constexpr state::ParamID kOutputDeadZone = 31;
inline constexpr state::ParamID kGateRiseMs = 40;
inline constexpr state::ParamID kGateFallMs = 41;
inline constexpr state::ParamID kGateColour = 42;
inline constexpr state::ParamID kGateClosedHz = 43;
inline constexpr state::ParamID kGateOpenHz = 44;
inline constexpr state::ParamID kGateGainExponent = 45;
inline constexpr state::ParamID kToneLofiBits = 50;
inline constexpr state::ParamID kToneLofiHoldRateHz = 51;
inline constexpr state::ParamID kToneLofiJitter = 52;
inline constexpr state::ParamID kToneLofiSmoothing = 53;
inline constexpr state::ParamID kToneLofiDeadZone = 54;
inline constexpr state::ParamID kNoiseLofiBits = 55;
inline constexpr state::ParamID kNoiseLofiHoldRateHz = 56;
inline constexpr state::ParamID kNoiseLofiJitter = 57;
inline constexpr state::ParamID kNoiseLofiSmoothing = 58;
inline constexpr state::ParamID kNoiseLofiDeadZone = 59;
inline constexpr state::ParamID kCircuitC41 = 60;
inline constexpr state::ParamID kCircuitC42 = 61;
inline constexpr state::ParamID kCircuitR161 = 62;
inline constexpr state::ParamID kCircuitR165 = 63;
inline constexpr state::ParamID kCircuitR166 = 64;
inline constexpr state::ParamID kCircuitR167 = 65;
inline constexpr state::ParamID kCircuitR170 = 66;

inline constexpr state::ParamID kControl0 = 100;
inline constexpr state::ParamID kControl1 = 101;
inline constexpr state::ParamID kControl2 = 102;
inline constexpr state::ParamID kControl3 = 103;
inline constexpr state::ParamID kControl4 = 104;
inline constexpr state::ParamID kControl5 = 105;
inline constexpr state::ParamID kControl6 = 106;
inline constexpr state::ParamID kControl7 = 107;
inline constexpr state::ParamID kControl8 = 108;
inline constexpr state::ParamID kControl9 = 109;
inline constexpr state::ParamID kControl10 = 110;
inline constexpr state::ParamID kControl11 = 111;
inline constexpr state::ParamID kControl12 = 112;
inline constexpr state::ParamID kControl13 = 113;
inline constexpr state::ParamID kControl14 = 114;
inline constexpr state::ParamID kControl15 = 115;
inline constexpr state::ParamID kControl16 = 116;
inline constexpr state::ParamID kControl17 = 117;
inline constexpr state::ParamID kControl18 = 118;
inline constexpr state::ParamID kControl19 = 119;
inline constexpr state::ParamID kControl20 = 120;
inline constexpr state::ParamID kControl21 = 121;
inline constexpr state::ParamID kControl22 = 122;

// Voice-local semantic aliases. Reuse is intentional: ParamIDs are namespaced
// by node, while these names make the persistence contract unambiguous.
inline constexpr auto kKickClickLevel = kControl0;
inline constexpr auto kKickClickToneHz = kControl1;
inline constexpr auto kKickClickDecayMs = kControl2;
inline constexpr auto kKickNoiseLevel = kControl3;
inline constexpr auto kKickNoiseDecayMs = kControl4;
inline constexpr auto kKickSubLevel = kControl5;
inline constexpr auto kKickTriangle = kControl6;
inline constexpr auto kKickFmAmount = kControl7;
inline constexpr auto kKickFmRatio = kControl8;
inline constexpr auto kKickCircuitFeedback = kControl6;
inline constexpr auto kKickCircuitDrive = kControl7;
inline constexpr auto kKickCircuitAttackMs = kControl8;
inline constexpr auto kKickCircuitPulseMs = kControl9;
inline constexpr auto kKickCircuitSigh = kControl10;
inline constexpr auto kKickCircuitHitLife = kControl11;
inline constexpr auto kSnareToneRatio = kControl0;
inline constexpr auto kSnareToneLevel = kControl1;
inline constexpr auto kSnareFmAmount = kControl2;
inline constexpr auto kSnareRing = kControl3;
inline constexpr auto kSnareNoiseLevel = kControl4;
inline constexpr auto kSnareNoiseDecayMs = kControl5;
inline constexpr auto kSnareNoiseCutoffHz = kControl6;
inline constexpr auto kSnareNoiseResonance = kControl7;
inline constexpr auto kSnareNoiseSweepOctaves = kControl8;
inline constexpr auto kSnareRattle = kControl9;
inline constexpr auto kSnareRattleHz = kControl10;
inline constexpr auto kSnareSnapLevel = kControl11;
inline constexpr auto kSnareSnapCutoffHz = kControl12;
inline constexpr auto kSnareSnapDecayMs = kControl13;
inline constexpr auto kSnareShellLevel = kControl14;
inline constexpr auto kSnareShellResonance = kControl15;
inline constexpr auto kHatSpread = kControl0;
inline constexpr auto kHatMetal = kControl1;
inline constexpr auto kHatGrit = kControl2;
inline constexpr auto kHatGritRatio = kControl3;
inline constexpr auto kHatCutoffHz = kControl4;
inline constexpr auto kHatResonance = kControl5;
inline constexpr auto kHatBandpass = kControl6;
inline constexpr auto kClapBurstCount = kControl0;
inline constexpr auto kClapBurstSpacingMs = kControl1;
inline constexpr auto kClapBurstDecayMs = kControl2;
inline constexpr auto kClapBurstFalloff = kControl3;
inline constexpr auto kClapGapJitter = kControl4;
inline constexpr auto kClapAlternatePolarity = kControl5;
inline constexpr auto kClapStereoWidth = kControl6;
inline constexpr auto kClapTailLevel = kControl7;
inline constexpr auto kClapTailDecayMs = kControl8;
inline constexpr auto kClapCutoffHz = kControl9;
inline constexpr auto kClapResonance = kControl10;
inline constexpr auto kClapBodyLevel = kControl11;
inline constexpr auto kClapBodyHz = kControl12;
inline constexpr auto kTomWave = kControl0;
inline constexpr auto kTomNoiseBalance = kControl1;
inline constexpr auto kTomNoiseCutoffHz = kControl2;
inline constexpr auto kTomNoiseResonance = kControl3;
inline constexpr auto kTomClickLevel = kControl4;
inline constexpr auto kTomClickCutoffHz = kControl5;
inline constexpr auto kTomClickDecayMs = kControl6;
inline constexpr auto kMembraneStructure = kControl0;
inline constexpr auto kMembraneStretch = kControl1;
inline constexpr auto kMembraneDamping = kControl2;
inline constexpr auto kMembraneBrightness = kControl3;
inline constexpr auto kMembranePosition = kControl4;
inline constexpr auto kMembraneSpread = kControl5;
inline constexpr auto kMembraneExciterMs = kControl6;
inline constexpr auto kMembraneExciterCutoffHz = kControl7;
inline constexpr auto kMembraneExciter = kControl8;
inline constexpr auto kMembraneSubLevel = kControl9;
inline constexpr auto kMembraneAirLevel = kControl10;
inline constexpr auto kMembraneAirDecayMs = kControl11;
inline constexpr auto kMembraneClickLevel = kControl12;
inline constexpr auto kMembraneClickDecayMs = kControl13;
inline constexpr auto kCymbalDecayTilt = kControl0;
inline constexpr auto kCymbalHighModeEmphasisDb = kControl1;
inline constexpr auto kCymbalVelocityFeedback = kControl2;
inline constexpr auto kCymbalVelocityHighModeDb = kControl3;
inline constexpr auto kCymbalUpperHighpassHz = kControl4;
inline constexpr auto kCymbalSpread = kControl5;
inline constexpr auto kCymbalInharmonicity = kControl6;
inline constexpr auto kCymbalShiftHz = kControl7;
inline constexpr auto kCymbalNoiseLevel = kControl8;
inline constexpr auto kCymbalStrikeLevel = kControl9;
inline constexpr auto kCymbalStrikeMs = kControl10;
inline constexpr auto kCymbalToneHz = kControl11;
inline constexpr auto kCymbalLowCutHz = kControl12;
inline constexpr auto kCymbalHitLife = kControl14;
inline constexpr auto kStringDamping = kControl0;
inline constexpr auto kStringStiffness = kControl1;
inline constexpr auto kStringPluckPosition = kControl2;
inline constexpr auto kStringExciterMs = kControl3;
inline constexpr auto kStringBrightnessHz = kControl4;
inline constexpr auto kStringPickDirection = kControl5;
inline constexpr auto kStringRestartOnHit = kControl6;
inline constexpr auto kStringModulation = kControl7;
inline constexpr auto kStringModulationMix = kControl8;
inline constexpr auto kStringModulationRatio = kControl9;
inline constexpr auto kStringFmDepthOctaves = kControl10;
inline constexpr auto kStringLpgAmount = kControl11;
inline constexpr auto kZapShape = kControl0;
inline constexpr auto kZapDistortion = kControl1;
inline constexpr auto kZapDistortionMs = kControl2;
inline constexpr auto kZapResonantDepth = kControl3;
inline constexpr auto kZapDetuneCents = kControl4;
inline constexpr auto kZapRing = kControl5;
inline constexpr auto kZapRingRatio = kControl6;
inline constexpr auto kFmAlgorithm = kControl0;
inline constexpr auto kFmDepth = kControl1;
inline constexpr auto kFmFormantHz = kControl2;
inline constexpr auto kFmFormantQ = kControl3;
inline constexpr auto kFm2ClickCutoffHz = kControl22;

inline constexpr state::ParamID kOperatorRatioBase = 200;
inline constexpr state::ParamID kOperatorLevelBase = 220;
inline constexpr state::ParamID kOperatorDecayBase = 240;
inline constexpr state::ParamID kOperatorFeedbackBase = 260;
inline constexpr state::ParamID kOperatorWaveBase = 280;

} // namespace pulp::host::forge_drum
