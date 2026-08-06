#pragma once

// Eurorack CV-utility bake catalog.
//
// These are the modular primitives with no DAW-plugin analogue -- nobody ships
// a "Multiple" as a VST -- so they live in their own pack rather than being
// retrofitted into the musical-DSP packs, which stay shared across targets.
//
// Voltage conventions follow the published modular standards: audio +/-5 V,
// unipolar CV 0..10 V, bipolar CV +/-5 V, gates and triggers 10 V, and Schmitt
// detection with a 0.1 V low / 1.0 V high hysteresis band.

#include <pulp/host/forge_param_descriptor.hpp>
#include <pulp/host/signal_graph.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pulp::host::eurorack {

/// Published voltage standards, in one place so nodes cannot drift apart.
inline constexpr float kAudioPeak = 5.0f;
inline constexpr float kCvUnipolarMax = 10.0f;
inline constexpr float kCvBipolarPeak = 5.0f;
inline constexpr float kGateHigh = 10.0f;
inline constexpr float kSchmittLow = 0.1f;
inline constexpr float kSchmittHigh = 1.0f;

inline constexpr state::ParamID kAmount = 1;
inline constexpr state::ParamID kOffset = 2;
inline constexpr state::ParamID kSlewRise = 3;
inline constexpr state::ParamID kSlewFall = 4;
inline constexpr state::ParamID kDivision = 5;

// ── Attenuverter ─────────────────────────────────────────────────────────────
// The control that makes a knob "first-class CV": scales and optionally inverts
// an incoming signal, so a patched CV can add to or subtract from a knob value.
// Amount is bipolar -1..+1; offset is added afterwards in volts.

struct AttenuverterInstance {};

inline CustomNodeType make_attenuverter_node() {
    CustomNodeType t;
    t.type_id = "eurorack.attenuverter";
    t.version = 1;
    t.num_input_ports = t.num_output_ports = 1;
    t.default_name = "Attenuverter";
    t.lowerable = true;
    t.create = []() -> void* { return new AttenuverterInstance{}; };
    t.destroy = [](void* p) { delete static_cast<AttenuverterInstance*>(p); };
    t.prepare = [](void*, double, int) {};
    t.reset = [](void*) {};
    t.baked_params = {{kAmount, -1.0f, 1.0f, 1.0f},
                      {kOffset, -kCvBipolarPeak, kCvBipolarPeak, 0.0f}};
    t.process_instance_baked_param = [](void*, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& in, int n,
                                        const BakedParamView& params) {
        const float* input = in.channel_ptr(0);
        float* output = out.channel_ptr(0);
        for (int i = 0; i < n; ++i) {
            const auto o = static_cast<std::int32_t>(i);
            output[static_cast<std::size_t>(i)] =
                input[static_cast<std::size_t>(i)] * params.value_at(kAmount, o)
                + params.value_at(kOffset, o);
        }
    };
    return t;
}

// ── Slew limiter ─────────────────────────────────────────────────────────────
// Independent rise and fall times in seconds. Doubles as a portamento/glide and
// as a lag processor on a CV.

struct SlewInstance {
    float y = 0.0f;
    double sample_time = 1.0 / 48000.0;
};

inline CustomNodeType make_slew_node() {
    CustomNodeType t;
    t.type_id = "eurorack.slew";
    t.version = 1;
    t.num_input_ports = t.num_output_ports = 1;
    t.default_name = "Slew Limiter";
    t.lowerable = true;
    t.create = []() -> void* { return new SlewInstance{}; };
    t.destroy = [](void* p) { delete static_cast<SlewInstance*>(p); };
    t.prepare = [](void* p, double sr, int) {
        auto& s = *static_cast<SlewInstance*>(p);
        s.sample_time = sr > 0.0 ? 1.0 / sr : 1.0 / 48000.0;
        s.y = 0.0f;
    };
    t.reset = [](void* p) { static_cast<SlewInstance*>(p)->y = 0.0f; };
    // Times in seconds; 0 means "instant", which must not divide by zero.
    t.baked_params = {{kSlewRise, 0.0f, 10.0f, 0.0f}, {kSlewFall, 0.0f, 10.0f, 0.0f}};
    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& in, int n,
                                        const BakedParamView& params) {
        auto& s = *static_cast<SlewInstance*>(p);
        const float* input = in.channel_ptr(0);
        float* output = out.channel_ptr(0);
        const auto dt = static_cast<float>(s.sample_time);
        for (int i = 0; i < n; ++i) {
            const auto o = static_cast<std::int32_t>(i);
            const float x = input[static_cast<std::size_t>(i)];
            const float t_s = (x > s.y) ? params.value_at(kSlewRise, o)
                                        : params.value_at(kSlewFall, o);
            if (t_s <= 0.0f) {
                s.y = x;
            } else {
                // Both published CV ranges span 10 V (0..10 or -5..+5).
                const float slope = kCvUnipolarMax / t_s;
                const float step = slope * dt;
                s.y += std::clamp(x - s.y, -step, step);
            }
            output[static_cast<std::size_t>(i)] = s.y;
        }
    };
    return t;
}

// ── Clock divider ────────────────────────────────────────────────────────────
// Schmitt-triggered on the published thresholds, emitting a 10 V gate for every
// Nth rising edge. Division is an integer 1..64 carried as a baked param.

struct ClockDividerInstance {
    bool high = false;
    std::int64_t count = 0;
    bool out_high = false;
};

inline CustomNodeType make_clock_divider_node() {
    CustomNodeType t;
    t.type_id = "eurorack.clock_divider";
    t.version = 1;
    t.num_input_ports = t.num_output_ports = 1;
    t.default_name = "Clock Divider";
    t.lowerable = true;
    t.create = []() -> void* { return new ClockDividerInstance{}; };
    t.destroy = [](void* p) { delete static_cast<ClockDividerInstance*>(p); };
    t.prepare = [](void* p, double, int) { *static_cast<ClockDividerInstance*>(p) = {}; };
    t.reset = [](void* p) { *static_cast<ClockDividerInstance*>(p) = {}; };
    t.baked_params = {{kDivision, 1.0f, 64.0f, 2.0f}};
    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& in, int n,
                                        const BakedParamView& params) {
        auto& s = *static_cast<ClockDividerInstance*>(p);
        const float* input = in.channel_ptr(0);
        float* output = out.channel_ptr(0);
        for (int i = 0; i < n; ++i) {
            const auto o = static_cast<std::int32_t>(i);
            const float v = input[static_cast<std::size_t>(i)];
            // Schmitt hysteresis: only these two thresholds change state.
            if (!s.high && v >= kSchmittHigh) {
                s.high = true;
                const auto div = static_cast<std::int64_t>(
                    std::max(1.0f, std::round(params.value_at(kDivision, o))));
                if (++s.count >= div) {
                    s.count = 0;
                    s.out_high = true;
                } else {
                    s.out_high = false;
                }
            } else if (s.high && v <= kSchmittLow) {
                s.high = false;
                s.out_high = false;
            }
            output[static_cast<std::size_t>(i)] = s.out_high ? kGateHigh : 0.0f;
        }
    };
    return t;
}

// ── Sample and hold ──────────────────────────────────────────────────────────
// Input 0 is the signal to capture; input 1 is an independent Schmitt trigger.

struct SampleHoldInstance {
    bool high = false;
    float held = 0.0f;
};

inline CustomNodeType make_sample_hold_node() {
    CustomNodeType t;
    t.type_id = "eurorack.sample_hold";
    t.version = 1;
    t.num_input_ports = 2;
    t.num_output_ports = 1;
    t.default_name = "Sample & Hold";
    t.lowerable = true;
    t.create = []() -> void* { return new SampleHoldInstance{}; };
    t.destroy = [](void* p) { delete static_cast<SampleHoldInstance*>(p); };
    t.prepare = [](void* p, double, int) { *static_cast<SampleHoldInstance*>(p) = {}; };
    t.reset = [](void* p) { *static_cast<SampleHoldInstance*>(p) = {}; };
    t.process_instance = [](void* p, audio::BufferView<float>& out,
                            const audio::BufferView<const float>& in, int n) {
        auto& s = *static_cast<SampleHoldInstance*>(p);
        const float* signal = in.channel_ptr(0);
        const float* trigger = in.channel_ptr(1);
        float* output = out.channel_ptr(0);
        for (int i = 0; i < n; ++i) {
            const auto index = static_cast<std::size_t>(i);
            const float v = trigger[index];
            if (!s.high && v >= kSchmittHigh) {
                s.high = true;
                s.held = signal[index];
            } else if (s.high && v <= kSchmittLow) {
                s.high = false;
            }
            output[index] = s.held;
        }
    };
    return t;
}

// ── Forge semantic descriptors ──────────────────────────────────────────────
// The factories above remain the numeric authority. These descriptors add the
// vocabulary needed to discover and author the nodes through the installed
// Forge catalog without duplicating ranges or defaults.

inline ForgeNodeDescriptor attenuverter_descriptor() {
    return {"eurorack_attenuverter", "Attenuverter",
            "Scales, inverts, and offsets a Eurorack control-voltage signal.",
            {}, {{"default", "eurorack.attenuverter"}},
            {{"amount", kAmount, "Amount", "",
              "Scales the input from full inversion through zero to unity.",
              ForgeParamKind::continuous, ForgeParamCurve::linear},
             {"offset_v", kOffset, "Offset", "V",
              "Adds a bipolar voltage offset after scaling.",
              ForgeParamKind::continuous, ForgeParamCurve::linear}}};
}

inline ForgeNodeDescriptor slew_descriptor() {
    return {"eurorack_slew", "Slew Limiter",
            "Limits independent rising and falling CV slopes for glide and lag processing.",
            {}, {{"default", "eurorack.slew"}},
            {{"rise_s", kSlewRise, "Rise", "s",
              "Sets the time for a ten-volt upward transition.",
              ForgeParamKind::continuous, ForgeParamCurve::linear},
             {"fall_s", kSlewFall, "Fall", "s",
              "Sets the time for a ten-volt downward transition.",
              ForgeParamKind::continuous, ForgeParamCurve::linear}}};
}

inline ForgeNodeDescriptor clock_divider_descriptor() {
    return {"eurorack_clock_divider", "Clock Divider",
            "Emits a ten-volt gate on every Nth Schmitt-detected input clock edge.",
            {}, {{"default", "eurorack.clock_divider"}},
            {{"division", kDivision, "Division", "",
              "Sets the integer input-clock division from one through sixty-four.",
              ForgeParamKind::continuous, ForgeParamCurve::linear}}};
}

inline ForgeNodeDescriptor sample_hold_descriptor() {
    return {"eurorack_sample_hold", "Sample & Hold",
            "Captures the signal input on each Schmitt-detected trigger edge and holds it.",
            {}, {{"default", "eurorack.sample_hold"}}, {}};
}

}  // namespace pulp::host::eurorack
