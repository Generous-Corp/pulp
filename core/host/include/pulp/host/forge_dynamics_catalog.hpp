#pragma once

// Dynamics — bake-layer catalog nodes.
//
// The home for the compressor family. It starts with the feedforward
// (transparent/modern) design and is named for the family rather than for that
// one member, because the VCA, FET and diode-bridge lineages that follow share
// this file's conventions and differ only in topology and colour stage —
// splitting them across four headers named after their circuits would scatter
// one set of decisions across four places.
//
// TRUE STEREO, two ports in and two out as ONE logical wire. Unlike the
// saturator (a per-sample memoryless function with no cross-channel state, and
// therefore correctly instanced per rail), a compressor genuinely couples the
// channels: `stereo_link` feeds both detectors from the louder channel so a
// hard-panned hit does not pull the stereo image toward centre. Instancing it
// dual-mono would silently discard that, and discard it in the direction that
// sounds fine until someone pans something hard.
//
// The DETECTOR MODE is an injectable param rather than a registration-time
// realization, unlike the saturator's curve. Peak and RMS share one topology
// and one parameter layout — the switch selects which pre-stage feeds the same
// gain computer — so flipping it is a coefficient-level change, not a change of
// what the node IS. It also has no effect on latency, which is what forces the
// saturator's alias policy to be frozen at registration.
//
// WET ONLY: a compressor's output IS the processed signal. Parallel ("New
// York") compression composes this with make_drywet_node(), which is the
// correct shape for it — the dry path in that topology is the mix bus, not
// something this node should own.

#include <pulp/host/signal_graph.hpp>

#include <pulp/signal/feedforward_compressor.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace pulp::host::dynamics {

// ── Stable type ids ───────────────────────────────────────────────────────
inline constexpr const char* kFeedforwardCompressorTypeId = "dynamics.feedforward_compressor";

// ── Injectable param ids ──────────────────────────────────────────────────
// Node-local; the framework namespaces per node so two nodes never collide.
inline constexpr state::ParamID kThresholdDb = 1;      // dB
inline constexpr state::ParamID kRatio = 2;            // :1
inline constexpr state::ParamID kKneeDb = 3;           // dB
inline constexpr state::ParamID kAttackMs = 4;         // ms
inline constexpr state::ParamID kReleaseMs = 5;        // ms
inline constexpr state::ParamID kDetectorMode = 6;     // stepped 0 = peak, 1 = RMS
inline constexpr state::ParamID kRmsWindowMs = 7;      // ms
inline constexpr state::ParamID kLookaheadMs = 8;      // ms
inline constexpr state::ParamID kProgramDependent = 9; // stepped 0/1
inline constexpr state::ParamID kMakeupDb = 10;        // dB
inline constexpr state::ParamID kAutoMakeup = 11;      // stepped 0/1
inline constexpr state::ParamID kStereoLink = 12;      // 0..1

/// This node's lookahead ceiling, in ms. The DSP header supports any value up
/// to `kMaxLookaheadMsCeiling`; this is the value THIS node instantiates, which
/// fixes the ring-buffer size at `prepare()` and therefore the largest latency
/// the node can ever report. A future node can bake a larger ceiling without a
/// header change.
/// [design parameter] default 10 ms, range 0 .. 50 ms.
inline constexpr float kNodeMaxLookaheadMs = 10.0f;

struct FeedforwardCompressorInstance {
    signal::FeedforwardCompressor compressor;
};

/// Worst-case linear gain for the Forge registry (series law 8).
///
/// The topology is feedforward — the sidechain reads the input, never the
/// output — so there is no loop to bound and no small-signal-gain argument to
/// make. The gain computer can only ever reduce, so the bound is exactly the
/// makeup ceiling: `10^(24/20)`. Asserted directly by the DSP suite's
/// makeup-gain bound test rather than reasoned about here.
inline float feedforward_compressor_worst_case_gain() {
    return static_cast<float>(
        std::pow(10.0, signal::FeedforwardCompressor::kMakeupDbMax / 20.0));
}

/// The transparent/modern compressor as a lowerable custom node.
inline CustomNodeType make_feedforward_compressor_node() {
    CustomNodeType t;
    t.type_id = kFeedforwardCompressorTypeId;
    t.version = 1;
    t.num_input_ports = 2;  // 0 = left, 1 = right (ONE logical stereo wire)
    t.num_output_ports = 2;
    t.default_name = "Compressor";
    t.lowerable = true;

    t.create = []() -> void* { return new FeedforwardCompressorInstance{}; };
    t.destroy = [](void* p) { delete static_cast<FeedforwardCompressorInstance*>(p); };
    t.prepare = [](void* p, double sr, int /*max_block*/) {
        auto* s = static_cast<FeedforwardCompressorInstance*>(p);
        s->compressor.prepare(sr, kNodeMaxLookaheadMs);
    };
    t.reset = [](void* p) {
        static_cast<FeedforwardCompressorInstance*>(p)->compressor.reset();
    };

    // Ranges and defaults are the module's canonical contract, in REAL units.
    // Each row is a design-parameter declaration per the series contract; the
    // `set_*` comments in the DSP header mirror these same numbers for
    // readability at the call site and are not a second declaration.
    using Comp = signal::FeedforwardCompressor;
    t.baked_params.push_back({kThresholdDb, static_cast<float>(Comp::kThresholdDbMin),
                              static_cast<float>(Comp::kThresholdDbMax), -18.0f});
    t.baked_params.push_back({kRatio, static_cast<float>(Comp::kRatioMin),
                              static_cast<float>(Comp::kRatioMax), 4.0f});
    t.baked_params.push_back({kKneeDb, static_cast<float>(Comp::kKneeDbMin),
                              static_cast<float>(Comp::kKneeDbMax), 6.0f});
    t.baked_params.push_back({kAttackMs, static_cast<float>(Comp::kAttackMsMin),
                              static_cast<float>(Comp::kAttackMsMax), 10.0f});
    t.baked_params.push_back({kReleaseMs, static_cast<float>(Comp::kReleaseMsMin),
                              static_cast<float>(Comp::kReleaseMsMax), 120.0f});
    t.baked_params.push_back({kDetectorMode, 0.0f, 1.0f, 0.0f});
    t.baked_params.push_back({kRmsWindowMs, static_cast<float>(Comp::kRmsWindowMsMin),
                              static_cast<float>(Comp::kRmsWindowMsMax), 10.0f});
    t.baked_params.push_back({kLookaheadMs, 0.0f, kNodeMaxLookaheadMs, 0.0f});
    t.baked_params.push_back({kProgramDependent, 0.0f, 1.0f, 1.0f});
    t.baked_params.push_back({kMakeupDb, -static_cast<float>(Comp::kMakeupDbMax),
                              static_cast<float>(Comp::kMakeupDbMax), 0.0f});
    t.baked_params.push_back({kAutoMakeup, 0.0f, 1.0f, 1.0f});
    t.baked_params.push_back({kStereoLink, 0.0f, 1.0f, 1.0f});

    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& in, int n,
                                        const BakedParamView& params) {
        auto* s = static_cast<FeedforwardCompressorInstance*>(p);
        const float* in_left = in.channel_ptr(0);
        const float* in_right = in.channel_ptr(1);
        float* out_left = out.channel_ptr(0);
        float* out_right = out.channel_ptr(1);

        // Sample at a time so every knob is sample-accurate. The setters are a
        // clamp, a store, and at most one `exp` for a coefficient — the module
        // recomputes only what changed and never redesigns a filter bank, so
        // this costs a handful of inlined calls per sample rather than a
        // re-preparation.
        for (int k = 0; k < n; ++k) {
            const auto offset = static_cast<std::int32_t>(k);
            s->compressor.set_threshold_db(params.value_at(kThresholdDb, offset));
            s->compressor.set_ratio(params.value_at(kRatio, offset));
            s->compressor.set_knee_width_db(params.value_at(kKneeDb, offset));
            s->compressor.set_attack_ms(params.value_at(kAttackMs, offset));
            s->compressor.set_release_ms(params.value_at(kReleaseMs, offset));
            s->compressor.set_detector(params.value_at(kDetectorMode, offset) >= 0.5f
                                           ? signal::CompressorDetector::rms
                                           : signal::CompressorDetector::peak);
            s->compressor.set_rms_window_ms(params.value_at(kRmsWindowMs, offset));
            s->compressor.set_lookahead_ms(params.value_at(kLookaheadMs, offset));
            s->compressor.set_program_dependent_release(
                params.value_at(kProgramDependent, offset) >= 0.5f);
            s->compressor.set_makeup_gain_db(params.value_at(kMakeupDb, offset));
            s->compressor.set_auto_makeup(params.value_at(kAutoMakeup, offset) >= 0.5f);
            s->compressor.set_stereo_link(params.value_at(kStereoLink, offset));

            float left = in_left[static_cast<std::size_t>(k)];
            float right = in_right[static_cast<std::size_t>(k)];
            s->compressor.process_stereo(left, right);
            out_left[static_cast<std::size_t>(k)] = left;
            out_right[static_cast<std::size_t>(k)] = right;
        }
    };
    return t;
}

}  // namespace pulp::host::dynamics
