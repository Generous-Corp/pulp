#pragma once

#include <pulp/host/custom_node_diagnostics.hpp>
#include <pulp/host/forge_param_descriptor.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/signal/nonlin_ambience.hpp>
#include <pulp/signal/speaker_cabinet.hpp>
#include <pulp/signal/zero_latency_convolver.hpp>
#include <cstdint>
#include <memory>
#include <vector>

#if defined(PULP_HOST_ENABLE_GPU_CONVOLUTION)
#ifndef PULP_GPU_CONVOLUTION_TRACE_CONFIG_API
#define PULP_GPU_CONVOLUTION_TRACE_CONFIG_API 1
#endif
#include <pulp/gpu_audio/gpu_convolution_reverb.hpp>
#endif

namespace pulp::host::space {
namespace convolution {
inline constexpr const char* kTypeId = "space.convolution_reverb";
inline constexpr state::ParamID kIrGainDb = 1;
inline constexpr state::ParamID kPredelayMs = 2;
inline constexpr state::ParamID kWetPercent = 3;
inline constexpr state::ParamID kDryPercent = 4;
inline constexpr state::ParamID kWidthPercent = 5;
inline constexpr state::ParamID kLowcutHz = 6;
inline constexpr state::ParamID kHighcutHz = 7;
using Engine = signal::ZeroLatencyConvolver;
struct ImpulseResponse { std::vector<std::vector<float>> channels; double sample_rate = 48000.0; };
bool valid_impulse_response(const ImpulseResponse&);
struct IrPolicy {
    signal::IrNormalizeMode normalize = signal::IrNormalizeMode::energy;
    double tail_trim_db = Engine::kTailTrimDbDefault;
    double tail_fade_ms = Engine::kTailFadeMsDefault;
    int resample_taps_per_phase = Engine::kResampTapsPerPhaseDefault;
    bool true_stereo = false;
};
float convolution_reverb_worst_case_gain(const ImpulseResponse&, const IrPolicy&, double sample_rate, int max_block);
CustomNodeType make_convolution_reverb_node(ImpulseResponse, IrPolicy = {});
CustomNodeType catalog_probe_node();
ForgeNodeDescriptor descriptor();
#if defined(PULP_HOST_ENABLE_GPU_CONVOLUTION)
inline constexpr const char* kGpuTypeId = "space.convolution_reverb_gpu";
inline constexpr std::uint64_t kGpuDiagnosticSchema = 0x4750554352560001ULL;
enum class GpuDiagnosticCounterUnit : std::uint8_t { TransportQuantum = 1 };
struct GpuConvolutionDiagnostics {
    std::uint32_t schema_version = 1;
    std::uint32_t lane_count = 2;
    gpu_audio::GpuConvolutionReverbReport report{};
    GpuDiagnosticCounterUnit delivery_counter_unit = GpuDiagnosticCounterUnit::TransportQuantum;
};
CustomNodeDiagnosticsDescriptor gpu_convolution_diagnostics();
CustomNodeType make_gpu_convolution_reverb_node(ImpulseResponse, IrPolicy = {}, gpu_audio::GpuConvolverTraceConfig = {});
ForgeNodeDescriptor descriptor_with_gpu();
#endif
} // namespace convolution

namespace nonlin_ambience {
inline constexpr const char* kTypeId = "space.nonlin_ambience";
inline constexpr state::ParamID kProgram = 1;
inline constexpr state::ParamID kLengthMs = 2;
inline constexpr state::ParamID kPredelayMs = 3;
inline constexpr state::ParamID kDensityPct = 4;
inline constexpr state::ParamID kDensityGrowth = 5;
inline constexpr state::ParamID kGateHoldPct = 6;
inline constexpr state::ParamID kAttackPct = 7;
inline constexpr state::ParamID kDiffusion = 8;
inline constexpr state::ParamID kTone = 9;
inline constexpr state::ParamID kHfDampHz = 10;
inline constexpr state::ParamID kWidthPct = 11;
inline constexpr state::ParamID kConverterAmount = 12;
inline constexpr state::ParamID kOutputGainDb = 13;
inline constexpr state::ParamID kMixPct = 14;
using Engine = signal::NonlinAmbience;
namespace cal = signal::nonlin_ambience;
inline constexpr float kProgramSteps = 3.0f;
inline constexpr float kLengthMsMin = static_cast<float>(cal::kMinLengthMs);
inline constexpr float kLengthMsDefault = 350.0f;
inline constexpr float kPredelayMsMax = 200.0f;
inline constexpr float kDensityPctMin = static_cast<float>(cal::kMinDensityPct);
inline constexpr float kGateHoldPctMin = 10.0f;
inline constexpr float kGateHoldPctMax = 95.0f;
inline constexpr float kAttackPctMin = 5.0f;
inline constexpr float kAttackPctMax = 98.0f;
inline constexpr float kOutputGainDbMax = 24.0f;
float nonlin_ambience_worst_case_gain();
CustomNodeType make_nonlin_ambience_node(std::uint32_t seed = cal::kDefaultSeed, double max_length_ms = cal::kMaxLengthMs);
ForgeNodeDescriptor descriptor();
} // namespace nonlin_ambience

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
float speaker_cabinet_worst_case_gain();
CustomNodeType make_speaker_cabinet_node();
CustomNodeType make_speaker_emulation_node();
ForgeNodeDescriptor descriptor();
} // namespace cabinet
} // namespace pulp::host::space
