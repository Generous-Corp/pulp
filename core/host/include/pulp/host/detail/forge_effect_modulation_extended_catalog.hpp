#pragma once

// Flanger, Leslie rotary, and scanner-vibrato Forge catalog nodes.
//
// These families live separately from the original modulation catalog so each
// implementation stays readable without pushing the public umbrella header
// past its maintainability boundary.  The umbrella header includes this file,
// preserving its existing include and symbol surface.

#include <pulp/host/forge_param_descriptor.hpp>
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
        {kWave, 0.0f, 6.0f, 0.0f},
    };
    if (mode == Mode::barberpole)
        t.baked_params.push_back(
            {kBarberpoleHz, static_cast<float>(-Engine::kBarberpoleShiftMaxHz),
             static_cast<float>(Engine::kBarberpoleShiftMaxHz), 3.0f});
    t.process_instance_baked_param =
        [mode](void* p, audio::BufferView<float>& out,
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
                if (mode == Mode::barberpole)
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

inline ForgeNodeDescriptor flanger_descriptor() {
    static const std::string through_zero_id = type_id(Mode::through_zero, 4.0);
    ForgeNodeDescriptor d;
    d.key = "flanger";
    d.label = "Flanger";
    d.description = "Stereo flanger with classic, through-zero, and barberpole topologies.";
    d.axes = {{"mode",
               "Mode",
               "Delay-line topology and its fixed latency behavior.",
               {{"classic", "Classic", 0.0f},
                {"through_zero", "Through-Zero", 1.0f},
                {"barberpole", "Barberpole", 2.0f}}}};
    d.realizations = {{"classic", kTypeId, {{"mode", "classic"}}},
                      {"through_zero", through_zero_id, {{"mode", "through_zero"}}},
                      {"barberpole", "modulation.flanger.barberpole",
                       {{"mode", "barberpole"}}}};
    d.params = {
        {"rate_hz", kRate, "Rate", "Hz", "Delay-sweep rate.", ForgeParamKind::continuous,
         ForgeParamCurve::logarithmic},
        {"depth_ms", kDepth, "Depth", "ms", "Delay-sweep excursion.", ForgeParamKind::continuous,
         ForgeParamCurve::linear},
        {"center_ms", kCenter, "Center", "ms", "Center delay around which the sweep moves.",
         ForgeParamKind::continuous, ForgeParamCurve::linear},
        {"feedback", kFeedback, "Feedback", "%",
         "Signed delayed feedback that sharpens the comb notches.", ForgeParamKind::continuous,
         ForgeParamCurve::linear},
        {"mix", kMix, "Mix", "%", "Blend between dry and flanged signals.",
         ForgeParamKind::continuous, ForgeParamCurve::linear},
        {"spread_deg", kSpreadDeg, "Stereo Spread", "deg",
         "Phase separation of the left and right modulation.", ForgeParamKind::continuous,
         ForgeParamCurve::linear},
        {"polarity",
         kPolarity,
         "Polarity",
         "",
         "Polarity of the delayed path.",
         ForgeParamKind::stepped,
         ForgeParamCurve::linear,
         {{"positive", "Positive", 0.0f}, {"negative", "Negative", 1.0f}}},
        {"engine",
         kEngine,
         "Delay Engine",
         "",
         "Clean or bucket-brigade delay coloration.",
         ForgeParamKind::stepped,
         ForgeParamCurve::linear,
         {{"clean", "Clean", 0.0f}, {"bbd", "BBD", 1.0f}}},
        {"barberpole_hz",
         kBarberpoleHz,
         "Barberpole Shift",
         "Hz",
         "Continuous spectral translation used by barberpole mode.",
         ForgeParamKind::continuous,
         ForgeParamCurve::linear,
         {},
         {"barberpole"}},
        {"wave",
         kWave,
         "Wave",
         "",
         "Delay-sweep waveform.",
         ForgeParamKind::stepped,
         ForgeParamCurve::linear,
         {{"sine", "Sine", 0.0f},
          {"triangle", "Triangle", 1.0f},
          {"saw_up", "Saw Up", 2.0f},
          {"saw_down", "Saw Down", 3.0f},
          {"square", "Square", 4.0f},
          {"sample_hold", "Sample & Hold", 5.0f},
          {"smooth_random", "Smooth Random", 6.0f}}},
    };
    return d;
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

inline ForgeNodeDescriptor leslie_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "leslie";
    d.label = "Leslie";
    d.description = "True-stereo horn-and-drum rotary speaker with acceleration, microphone, "
                    "and early-reflection modeling.";
    d.realizations = {{"default", kTypeId}};
    d.params = {
        {"speed",
         kSpeed,
         "Speed",
         "",
         "Stopped, chorale, or tremolo motor target.",
         ForgeParamKind::stepped,
         ForgeParamCurve::linear,
         {{"stop", "Stop", 0.0f}, {"slow", "Chorale", 1.0f}, {"fast", "Tremolo", 2.0f}}},
        {"horn_fast_hz", kHornFast, "Horn Fast", "Hz", "Horn tremolo speed.",
         ForgeParamKind::continuous, ForgeParamCurve::linear},
        {"horn_slow_hz", kHornSlow, "Horn Slow", "Hz", "Horn chorale speed.",
         ForgeParamKind::continuous, ForgeParamCurve::linear},
        {"drum_fast_hz", kDrumFast, "Drum Fast", "Hz", "Drum tremolo speed.",
         ForgeParamKind::continuous, ForgeParamCurve::linear},
        {"drum_slow_hz", kDrumSlow, "Drum Slow", "Hz", "Drum chorale speed.",
         ForgeParamKind::continuous, ForgeParamCurve::linear},
        {"horn_accel_s", kHornAccel, "Horn Acceleration", "s", "Horn motor transition time.",
         ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
        {"drum_accel_s", kDrumAccel, "Drum Acceleration", "s", "Drum motor transition time.",
         ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
        {"crossover_hz", kCrossover, "Crossover", "Hz",
         "Frequency dividing the horn and drum paths.", ForgeParamKind::continuous,
         ForgeParamCurve::logarithmic},
        {"horn_radius_m", kHornRadius, "Horn Radius", "m",
         "Horn rotor radius used by the Doppler model.", ForgeParamKind::continuous,
         ForgeParamCurve::linear},
        {"drum_radius_m", kDrumRadius, "Drum Radius", "m",
         "Drum rotor radius used by the Doppler model.", ForgeParamKind::continuous,
         ForgeParamCurve::linear},
        {"am_depth", kAmDepth, "AM Depth", "%", "Horn amplitude-modulation depth.",
         ForgeParamKind::continuous, ForgeParamCurve::linear},
        {"horn_directivity_db", kDirDepth, "Horn Directivity", "dB",
         "High-frequency horn directivity modulation.", ForgeParamKind::continuous,
         ForgeParamCurve::linear},
        {"horn_directivity_hz", kDirCorner, "Horn Directivity Corner", "Hz",
         "Corner frequency of horn directivity.", ForgeParamKind::continuous,
         ForgeParamCurve::logarithmic},
        {"drum_directivity_db", kDrumDirDepth, "Drum Directivity", "dB",
         "Drum directivity modulation.", ForgeParamKind::continuous, ForgeParamCurve::linear},
        {"doppler_bias_ms", kDBias, "Doppler Bias", "ms",
         "Static delay bias supporting Doppler motion.", ForgeParamKind::continuous,
         ForgeParamCurve::linear},
        {"mic_angle_deg", kMicAngle, "Mic Angle", "deg",
         "Angular separation of the stereo microphones.", ForgeParamKind::continuous,
         ForgeParamCurve::linear},
        {"mic_distance_m", kMicDistance, "Mic Distance", "m",
         "Distance from rotors to microphones.", ForgeParamKind::continuous,
         ForgeParamCurve::logarithmic},
        {"reflection_db", kReflectionDb, "Reflection Level", "dB",
         "Level of cabinet and room reflections.", ForgeParamKind::continuous,
         ForgeParamCurve::linear},
        {"reflections",
         kReflections,
         "Reflections",
         "",
         "Number of modeled early reflections.",
         ForgeParamKind::stepped,
         ForgeParamCurve::linear,
         {{"one", "1", 1.0f}, {"two", "2", 2.0f}, {"three", "3", 3.0f}, {"four", "4", 4.0f}}},
        {"reflection_delay_ms", kReflDelay, "Reflection Delay", "ms",
         "Delay of the first reflection.", ForgeParamKind::continuous, ForgeParamCurve::linear},
        {"reflection_spacing_ms", kReflSpacing, "Reflection Spacing", "ms",
         "Spacing between successive reflections.", ForgeParamKind::continuous,
         ForgeParamCurve::linear},
        {"reflection_corner_hz", kReflCorner, "Reflection Corner", "Hz",
         "Low-pass corner of the reflected paths.", ForgeParamKind::continuous,
         ForgeParamCurve::logarithmic},
        {"drift_cents", kDrift, "Drift", "cent", "Slow mechanical speed instability.",
         ForgeParamKind::continuous, ForgeParamCurve::linear},
        {"mix", kWetMix, "Mix", "%", "Blend between stationary and rotary signals.",
         ForgeParamKind::continuous, ForgeParamCurve::linear},
    };
    return d;
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

inline ForgeNodeDescriptor scanner_vibrato_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "scanner_vibrato";
    d.label = "Scanner Vibrato";
    d.description = "Hammond-style tapped-delay scanner with vibrato and chorus positions.";
    d.realizations = {{"default", kScannerTypeId}};
    d.params = {
        {"mode",
         kScannerMode,
         "Mode",
         "",
         "Scanner switch position.",
         ForgeParamKind::stepped,
         ForgeParamCurve::linear,
         {{"off", "Off", 0.0f},
          {"v1", "V1", 1.0f},
          {"v2", "V2", 2.0f},
          {"v3", "V3", 3.0f},
          {"c1", "C1", 4.0f},
          {"c2", "C2", 5.0f},
          {"c3", "C3", 6.0f}}},
        {"scan_hz", kScanHz, "Scan Rate", "Hz", "Mechanical scanner rotation rate.",
         ForgeParamKind::continuous, ForgeParamCurve::linear},
        {"line_ms", kLineMs, "Line Time", "ms", "Total delay-line length.",
         ForgeParamKind::continuous, ForgeParamCurve::linear},
        {"v1", kV1, "V1 Depth", "%", "Tap fraction used by the V1 position.",
         ForgeParamKind::continuous, ForgeParamCurve::linear},
        {"v2", kV2, "V2 Depth", "%", "Tap fraction used by the V2 position.",
         ForgeParamKind::continuous, ForgeParamCurve::linear},
        {"v3", kV3, "V3 Depth", "%", "Tap fraction used by the V3 position.",
         ForgeParamKind::continuous, ForgeParamCurve::linear},
        {"chorus_mix", kChorusMix, "Chorus Mix", "%", "Direct-path blend in the chorus positions.",
         ForgeParamKind::continuous, ForgeParamCurve::linear},
    };
    return d;
}

inline float leslie_worst_case_gain() {
    return static_cast<float>(signal::LeslieRotary::kWorstCaseGain);
}

inline float scanner_worst_case_gain() {
    return static_cast<float>(signal::ScannerVibrato::kWorstCaseGain);
}

}  // namespace leslie

}  // namespace pulp::host::modulation
