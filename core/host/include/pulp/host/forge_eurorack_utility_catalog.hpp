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
                // Volts-per-second slope over the full +/-10 V CV span.
                const float slope = (2.0f * kCvUnipolarMax) / t_s;
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
// Single input carrying the signal; samples on its own rising edge is not
// meaningful, so this node samples on the Schmitt edge of the SAME input, which
// makes it a track-and-hold usable as a one-port bake node. A two-input variant
// (signal + trigger) belongs on the graph, not inside one node.

struct SampleHoldInstance {
    bool high = false;
    float held = 0.0f;
};

inline CustomNodeType make_sample_hold_node() {
    CustomNodeType t;
    t.type_id = "eurorack.sample_hold";
    t.version = 1;
    t.num_input_ports = t.num_output_ports = 1;
    t.default_name = "Sample & Hold";
    t.lowerable = true;
    t.create = []() -> void* { return new SampleHoldInstance{}; };
    t.destroy = [](void* p) { delete static_cast<SampleHoldInstance*>(p); };
    t.prepare = [](void* p, double, int) { *static_cast<SampleHoldInstance*>(p) = {}; };
    t.reset = [](void* p) { *static_cast<SampleHoldInstance*>(p) = {}; };
    t.process_instance = [](void* p, audio::BufferView<float>& out,
                            const audio::BufferView<const float>& in, int n) {
        auto& s = *static_cast<SampleHoldInstance*>(p);
        const float* input = in.channel_ptr(0);
        float* output = out.channel_ptr(0);
        for (int i = 0; i < n; ++i) {
            const float v = input[static_cast<std::size_t>(i)];
            if (!s.high && v >= kSchmittHigh) {
                s.high = true;
                s.held = v;
            } else if (s.high && v <= kSchmittLow) {
                s.high = false;
            }
            output[static_cast<std::size_t>(i)] = s.held;
        }
    };
    return t;
}

}  // namespace pulp::host::eurorack
