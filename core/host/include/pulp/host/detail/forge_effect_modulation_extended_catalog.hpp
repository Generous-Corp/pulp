#pragma once

// Flanger, Leslie rotary, and scanner-vibrato Forge catalog nodes.
//
// These families live separately from the original modulation catalog so each
// implementation stays readable without pushing the public umbrella header
// past its maintainability boundary.  The umbrella header includes this file,
// preserving its existing include and symbol surface.

#include <pulp/host/signal_graph.hpp>
#include <pulp/host/detail/forge_realization_identity.hpp>

#include <pulp/signal/flanger.hpp>
#include <pulp/signal/leslie.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>

namespace pulp::host::modulation {

namespace flanger {
inline constexpr const char* kTypeId = "modulation.flanger";
using Engine = signal::Flanger;
using Mode = signal::FlangerMode;

enum : state::ParamID {
    kRate = 1,
    kDepth,
    kCenter,
    kOffset,  // reserved: registration-time since it determines TZ latency
    kFeedback,
    kMix,
    kSpreadDeg,
    kMode,  // reserved: registration-time topology/latency realization
    kPolarity,
    kEngine,
    kBarberpoleHz,
    kWave,
};

inline signal::LfoWave wave_from_param(float value) noexcept {
    switch (static_cast<int>(std::lround(value))) {
        case 1: return signal::LfoWave::triangle;
        case 2: return signal::LfoWave::saw_up;
        case 3: return signal::LfoWave::saw_down;
        case 4: return signal::LfoWave::square;
        case 5: return signal::LfoWave::sample_hold;
        case 6: return signal::LfoWave::smooth_random;
        default: return signal::LfoWave::sine;
    }
}

struct Instance {
    Engine engine;
};

inline std::string type_id(Mode mode, double offset_ms) {
    if (mode == Mode::classic) return kTypeId;
    if (mode == Mode::barberpole) return std::string{kTypeId} + ".barberpole";
    const double frozen_offset =
        std::clamp(offset_ms, Engine::kOffsetMinMs, Engine::kOffsetMaxMs);
    return std::string{kTypeId} + ".through_zero." +
           detail::realization_real_token(frozen_offset);
}

inline int latency_samples(Mode mode, double offset_ms, double sample_rate) {
    Engine probe;
    probe.prepare(sample_rate);
    probe.set_offset_ms(offset_ms);
    probe.set_mode(mode);
    return probe.latency_samples();
}

inline CustomNodeType make_flanger_node(Mode mode = Mode::classic,
                                        double offset_ms = 4.0) {
    const double frozen_offset =
        std::clamp(offset_ms, Engine::kOffsetMinMs, Engine::kOffsetMaxMs);

    CustomNodeType t;
    t.type_id = type_id(mode, frozen_offset);
    t.version = 1;
    t.num_input_ports = 2;
    t.num_output_ports = 2;
    t.default_name = "Flanger";
    t.lowerable = true;
    t.latency_samples = [mode, frozen_offset](double sample_rate) {
        return latency_samples(mode, frozen_offset, sample_rate);
    };
    t.create = []() -> void* { return new Instance{}; };
    t.destroy = [](void* p) { delete static_cast<Instance*>(p); };
    t.prepare = [mode, frozen_offset](void* p, double sr, int /*max_block*/) {
        auto& engine = static_cast<Instance*>(p)->engine;
        engine.prepare(sr);
        engine.set_offset_ms(frozen_offset);
        engine.set_mode(mode);
        engine.reset();
    };
    t.reset = [](void* p) { static_cast<Instance*>(p)->engine.reset(); };

    t.baked_params = {
        {kRate, static_cast<float>(Engine::kRateMinHz),
         static_cast<float>(Engine::kRateMaxHz), 0.5f},
        {kDepth, static_cast<float>(Engine::kDepthMinMs),
         static_cast<float>(Engine::kDepthMaxMs), 1.5f},
        {kCenter, static_cast<float>(Engine::kCenterMinMs),
         static_cast<float>(Engine::kCenterMaxMs), 3.0f},
        {kFeedback, static_cast<float>(-Engine::kFbClamp),
         static_cast<float>(Engine::kFbClamp), 0.6f},
        {kMix, 0.0f, 1.0f, 0.5f},
        {kSpreadDeg, 0.0f, 180.0f, 90.0f},
        {kPolarity, 0.0f, 1.0f, 0.0f},
        {kEngine, 0.0f, 1.0f, 0.0f},
        {kBarberpoleHz, static_cast<float>(-Engine::kBarberpoleShiftMaxHz),
         static_cast<float>(Engine::kBarberpoleShiftMaxHz), 3.0f},
        {kWave, 0.0f, 6.0f, 0.0f},
    };
    t.process_instance_baked_param =
        [](void* p, audio::BufferView<float>& out,
           const audio::BufferView<const float>& in, int n,
           const BakedParamView& params) {
            auto& engine = static_cast<Instance*>(p)->engine;
            for (int i = 0; i < n; ++i) {
                const auto offset = static_cast<std::int32_t>(i);
                engine.set_rate_hz(params.value_at(kRate, offset));
                engine.set_depth_ms(params.value_at(kDepth, offset));
                engine.set_center_delay_ms(params.value_at(kCenter, offset));
                engine.set_feedback(params.value_at(kFeedback, offset));
                engine.set_mix(params.value_at(kMix, offset));
                engine.set_stereo_spread(params.value_at(kSpreadDeg, offset) / 360.0f);
                engine.set_polarity(params.value_at(kPolarity, offset) >= 0.5f
                                        ? signal::FlangerPolarity::negative
                                        : signal::FlangerPolarity::positive);
                engine.set_delay_engine(params.value_at(kEngine, offset) >= 0.5f
                                            ? signal::FlangerDelayEngine::bbd
                                            : signal::FlangerDelayEngine::clean);
                engine.set_barberpole_shift_hz(params.value_at(kBarberpoleHz, offset));
                engine.set_waveform(wave_from_param(params.value_at(kWave, offset)));

                float left = in.channel_ptr(0)[i];
                float right = in.channel_ptr(1)[i];
                engine.process_stereo(&left, &right, out.channel_ptr(0) + i,
                                      out.channel_ptr(1) + i, 1);
            }
        };
    return t;
}

inline float worst_case_gain() {
    return static_cast<float>(Engine::worst_case_gain());
}

}  // namespace flanger

namespace leslie {
inline constexpr const char* kTypeId = "modulation.leslie";
inline constexpr const char* kScannerTypeId = "modulation.scanner_vibrato";

enum : state::ParamID {
    kSpeed = 1,
    kHornFast,
    kHornSlow,
    kDrumFast,
    kDrumSlow,
    kHornAccel,
    kDrumAccel,
    kCrossover,
    kHornRadius,
    kDrumRadius,
    kAmDepth,
    kDirDepth,
    kDirCorner,
    kDrumDirDepth,
    kDBias,
    kMicAngle,
    kMicDistance,
    kReflectionDb,
    kReflections,
    kReflDelay,
    kReflSpacing,
    kReflCorner,
    kDrift,
    kWetMix,
};

struct Instance {
    signal::LeslieRotary engine;
};

inline CustomNodeType make_leslie_node() {
    CustomNodeType t;
    t.type_id = kTypeId;
    t.version = 1;
    t.num_input_ports = 2;
    t.num_output_ports = 2;
    t.default_name = "Leslie";
    t.lowerable = true;
    t.create = []() -> void* { return new Instance{}; };
    t.destroy = [](void* p) { delete static_cast<Instance*>(p); };
    t.prepare = [](void* p, double sr, int /*max_block*/) {
        static_cast<Instance*>(p)->engine.prepare(sr);
    };
    t.reset = [](void* p) { static_cast<Instance*>(p)->engine.reset(); };
    t.baked_params = {
        {kSpeed, 0.0f, 2.0f, 2.0f},       {kHornFast, 5.5f, 7.5f, 6.7f},
        {kHornSlow, 0.6f, 1.0f, 0.8f},    {kDrumFast, 4.5f, 6.5f, 5.7f},
        {kDrumSlow, 0.5f, 0.9f, 0.6f},    {kHornAccel, 0.3f, 3.0f, 1.0f},
        {kDrumAccel, 1.0f, 8.0f, 3.0f},   {kCrossover, 700.0f, 900.0f, 800.0f},
        {kHornRadius, 0.1f, 0.25f, 0.18f}, {kDrumRadius, 0.08f, 0.18f, 0.12f},
        {kAmDepth, 0.0f, 0.9f, 0.5f},     {kDirDepth, 0.0f, 12.0f, 6.0f},
        {kDirCorner, 1000.0f, 4000.0f, 2000.0f},
        {kDrumDirDepth, 0.0f, 6.0f, 2.0f},
        {kDBias, 0.2f, 1.0f, 0.5f},       {kMicAngle, 0.0f, 90.0f, 45.0f},
        {kMicDistance, 0.3f, 3.0f, 1.0f}, {kReflectionDb, -60.0f, -6.0f, -12.0f},
        {kReflections, 1.0f, 4.0f, 2.0f}, {kReflDelay, 2.5f, 6.0f, 3.5f},
        {kReflSpacing, 1.0f, 3.0f, 1.5f}, {kReflCorner, 1000.0f, 8000.0f, 3000.0f},
        {kDrift, 0.0f, 10.0f, 3.0f},      {kWetMix, 0.0f, 1.0f, 1.0f},
    };
    t.process_instance_baked_param =
        [](void* p, audio::BufferView<float>& out,
           const audio::BufferView<const float>& in, int n,
           const BakedParamView& params) {
            auto& engine = static_cast<Instance*>(p)->engine;
            for (int i = 0; i < n; ++i) {
                const auto offset = static_cast<std::int32_t>(i);
                const int speed = std::clamp(
                    static_cast<int>(std::lround(params.value_at(kSpeed, offset))), 0, 2);
                engine.set_speed(static_cast<signal::LeslieSpeed>(speed));
                engine.set_horn_fast_hz(params.value_at(kHornFast, offset));
                engine.set_horn_slow_hz(params.value_at(kHornSlow, offset));
                engine.set_drum_fast_hz(params.value_at(kDrumFast, offset));
                engine.set_drum_slow_hz(params.value_at(kDrumSlow, offset));
                engine.set_horn_accel_s(params.value_at(kHornAccel, offset));
                engine.set_drum_accel_s(params.value_at(kDrumAccel, offset));
                engine.set_crossover_hz(params.value_at(kCrossover, offset));
                engine.set_horn_radius_m(params.value_at(kHornRadius, offset));
                engine.set_drum_radius_m(params.value_at(kDrumRadius, offset));
                engine.set_am_depth(params.value_at(kAmDepth, offset));
                engine.set_dir_depth_db(params.value_at(kDirDepth, offset));
                engine.set_dir_corner_hz(params.value_at(kDirCorner, offset));
                engine.set_drum_dir_depth_db(params.value_at(kDrumDirDepth, offset));
                engine.set_d_bias_ms(params.value_at(kDBias, offset));
                engine.set_mic_angle_deg(params.value_at(kMicAngle, offset));
                engine.set_mic_distance_m(params.value_at(kMicDistance, offset));
                engine.set_reflection_db(params.value_at(kReflectionDb, offset));
                engine.set_num_reflections(static_cast<int>(
                    std::lround(params.value_at(kReflections, offset))));
                engine.set_refl_delay_ms(params.value_at(kReflDelay, offset));
                engine.set_refl_spacing_ms(params.value_at(kReflSpacing, offset));
                engine.set_refl_corner_hz(params.value_at(kReflCorner, offset));
                engine.set_drift_cents(params.value_at(kDrift, offset));
                engine.set_mix(params.value_at(kWetMix, offset));
                engine.process(in.channel_ptr(0)[i], in.channel_ptr(1)[i],
                               out.channel_ptr(0)[i], out.channel_ptr(1)[i]);
            }
        };
    return t;
}

enum : state::ParamID {
    kScannerMode = 1,
    kScanHz,
    kLineMs,
    kV1,
    kV2,
    kV3,
    kChorusMix,
};

struct ScannerInstance {
    signal::ScannerVibrato engine;
};

inline CustomNodeType make_scanner_vibrato_node() {
    CustomNodeType t;
    t.type_id = kScannerTypeId;
    t.version = 1;
    t.num_input_ports = 1;
    t.num_output_ports = 1;
    t.default_name = "Scanner Vibrato";
    t.lowerable = true;
    t.create = []() -> void* { return new ScannerInstance{}; };
    t.destroy = [](void* p) { delete static_cast<ScannerInstance*>(p); };
    t.prepare = [](void* p, double sr, int /*max_block*/) {
        static_cast<ScannerInstance*>(p)->engine.prepare(sr);
    };
    t.reset = [](void* p) { static_cast<ScannerInstance*>(p)->engine.reset(); };
    t.baked_params = {
        {kScannerMode, 0.0f, 6.0f, 0.0f}, {kScanHz, 6.0f, 7.5f, 6.9f},
        {kLineMs, 0.6f, 1.4f, 1.0f},      {kV1, 0.1f, 0.5f, 0.33f},
        {kV2, 0.4f, 0.8f, 0.66f},         {kV3, 0.7f, 1.0f, 1.0f},
        {kChorusMix, 0.0f, 1.0f, 0.5f},
    };
    t.process_instance_baked_param =
        [](void* p, audio::BufferView<float>& out,
           const audio::BufferView<const float>& in, int n,
           const BakedParamView& params) {
            auto& engine = static_cast<ScannerInstance*>(p)->engine;
            for (int i = 0; i < n; ++i) {
                const auto offset = static_cast<std::int32_t>(i);
                const int mode = std::clamp(
                    static_cast<int>(std::lround(params.value_at(kScannerMode, offset))),
                    0, 6);
                engine.set_mode(static_cast<signal::ScannerMode>(mode));
                engine.set_scan_hz(params.value_at(kScanHz, offset));
                engine.set_line_ms(params.value_at(kLineMs, offset));
                engine.set_v1_frac(params.value_at(kV1, offset));
                engine.set_v2_frac(params.value_at(kV2, offset));
                engine.set_v3_frac(params.value_at(kV3, offset));
                engine.set_chorus_mix(params.value_at(kChorusMix, offset));
                out.channel_ptr(0)[i] = engine.process(in.channel_ptr(0)[i]);
            }
        };
    return t;
}

inline float leslie_worst_case_gain() {
    return static_cast<float>(signal::LeslieRotary::kWorstCaseGain);
}

inline float scanner_worst_case_gain() {
    return static_cast<float>(signal::ScannerVibrato::kWorstCaseGain);
}

}  // namespace leslie

}  // namespace pulp::host::modulation
