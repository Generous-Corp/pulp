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
// CHANNEL COUNT IS PER MEMBER, not per family — the one thing here that is easy
// to get wrong by pattern-matching. The feedforward design is TRUE STEREO, two
// ports in and two out as ONE logical wire, because it genuinely couples the
// channels: `stereo_link` feeds both detectors from the louder channel so a
// hard-panned hit does not pull the stereo image toward centre. Instancing THAT
// dual-mono would silently discard the link, in the direction that sounds fine
// until someone pans something hard.
//
// The three lineage members below are MONO, one port in and one out, and that is
// not an oversight: none of them has a stereo link, so a stereo instrument would
// be exactly two independent copies — which is what instancing two nodes already
// gives, without pretending there is a stereo image to preserve. Their DSP
// headers say the same thing (stereo composes as two instances driven from a
// caller-computed shared detector signal). Giving them two ports would claim a
// coupling that does not exist in the code.
//
// REALIZATION vs INJECTABLE PARAM, applied per member:
//
//   - Anything that moves `latency_samples()` is a REGISTRATION-TIME argument,
//     because a node whose reported latency changes under the audio thread
//     breaks the host's delay compensation. That is why the VCA member takes
//     its lookahead at construction.
//   - A genuine TOPOLOGY change is a realization too. The diode-bridge member's
//     feedback switch moves the detector from the input to the output, which
//     re-maps the static curve — the module ships a separate
//     `static_curve_feedback_db()` accessor precisely because the measured curve
//     is a different function. Automating that would move the measured ratio
//     under the user, so it is two registered type ids instead.
//   - An ANTIALIASING policy is a realization, following the saturator.
//   - Everything that is a coefficient is an injectable param, including the
//     stepped ones. The FET member's five ratio buttons and the VCA member's
//     negative-ratio mode are front-panel switches over one unchanged signal
//     path with invariant latency, so they inject.
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

#include <pulp/host/detail/forge_realization_identity.hpp>
#include <pulp/host/forge_param_descriptor.hpp>
#include <pulp/host/signal_graph.hpp>

#include <pulp/signal/diode_bridge_compressor.hpp>
#include <pulp/signal/feedforward_compressor.hpp>
#include <pulp/signal/fet_compressor.hpp>
#include <pulp/signal/true_peak_limiter.hpp>
#include <pulp/signal/vca_compressor.hpp>

#include <algorithm>
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
// Param id 8 is intentionally reserved. Lookahead changes the node's latency,
// so it is fixed by the realization factory rather than injectable automation.
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
    return static_cast<float>(std::pow(10.0, signal::FeedforwardCompressor::kMakeupDbMax / 20.0));
}

/// The transparent/modern compressor as a lowerable custom node.
///
/// `lookahead_ms` is a construction-time realization axis because it changes
/// intrinsic latency. Non-zero values therefore receive stable, collision-free
/// type identities; the historical zero-lookahead node retains its base id.
inline CustomNodeType make_feedforward_compressor_node(float lookahead_ms = 0.0f) {
    using Comp = signal::FeedforwardCompressor;
    const double fixed_lookahead_ms = std::clamp(
        std::isfinite(static_cast<double>(lookahead_ms)) ? static_cast<double>(lookahead_ms) : 0.0,
        0.0, static_cast<double>(kNodeMaxLookaheadMs));

    CustomNodeType t;
    t.type_id = kFeedforwardCompressorTypeId;
    if (fixed_lookahead_ms != 0.0)
        t.type_id += ".la_" + detail::realization_real_token(fixed_lookahead_ms);
    t.version = 1;
    t.num_input_ports = 2;  // 0 = left, 1 = right (ONE logical stereo wire)
    t.num_output_ports = 2;
    t.default_name = "Compressor";
    t.lowerable = true;
    t.latency_samples = [fixed_lookahead_ms](double sample_rate) {
        Comp probe;
        probe.prepare(sample_rate, kNodeMaxLookaheadMs);
        probe.set_lookahead_ms(fixed_lookahead_ms);
        return probe.latency_samples();
    };

    t.create = []() -> void* { return new FeedforwardCompressorInstance{}; };
    t.destroy = [](void* p) { delete static_cast<FeedforwardCompressorInstance*>(p); };
    t.prepare = [fixed_lookahead_ms](void* p, double sr, int /*max_block*/) {
        auto* s = static_cast<FeedforwardCompressorInstance*>(p);
        s->compressor.prepare(sr, kNodeMaxLookaheadMs);
        s->compressor.set_lookahead_ms(fixed_lookahead_ms);
    };
    t.reset = [](void* p) { static_cast<FeedforwardCompressorInstance*>(p)->compressor.reset(); };

    // Ranges and defaults are the module's canonical contract, in REAL units.
    // Each row is a design-parameter declaration per the series contract; the
    // `set_*` comments in the DSP header mirror these same numbers for
    // readability at the call site and are not a second declaration.
    t.baked_params.push_back({kThresholdDb, static_cast<float>(Comp::kThresholdDbMin),
                              static_cast<float>(Comp::kThresholdDbMax), -18.0f});
    t.baked_params.push_back(
        {kRatio, static_cast<float>(Comp::kRatioMin), static_cast<float>(Comp::kRatioMax), 4.0f});
    t.baked_params.push_back({kKneeDb, static_cast<float>(Comp::kKneeDbMin),
                              static_cast<float>(Comp::kKneeDbMax), 6.0f});
    t.baked_params.push_back({kAttackMs, static_cast<float>(Comp::kAttackMsMin),
                              static_cast<float>(Comp::kAttackMsMax), 10.0f});
    t.baked_params.push_back({kReleaseMs, static_cast<float>(Comp::kReleaseMsMin),
                              static_cast<float>(Comp::kReleaseMsMax), 120.0f});
    t.baked_params.push_back({kDetectorMode, 0.0f, 1.0f, 0.0f});
    t.baked_params.push_back({kRmsWindowMs, static_cast<float>(Comp::kRmsWindowMsMin),
                              static_cast<float>(Comp::kRmsWindowMsMax), 10.0f});
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

inline ForgeNodeDescriptor feedforward_compressor_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "feedforward_compressor";
    d.label = "Feedforward Compressor";
    d.description = "Transparent true-stereo compressor with linked detection and optional "
                    "program-dependent release.";
    d.axes = {{"lookahead_ms",
               "Lookahead",
               "Fixed detector lookahead; non-zero values report matching latency.",
               {{"zero_latency", "Zero latency", 0.0f},
                {"lookahead_3ms", "3 ms", 3.0f},
                {"lookahead_10ms", "10 ms", 10.0f}}}};
    d.realizations = {{"zero_lookahead",
                       make_feedforward_compressor_node(0.0f).type_id,
                       {{"lookahead_ms", "zero_latency"}}},
                      {"lookahead_3ms",
                       make_feedforward_compressor_node(3.0f).type_id,
                       {{"lookahead_ms", "lookahead_3ms"}}},
                      {"lookahead_10ms",
                       make_feedforward_compressor_node(10.0f).type_id,
                       {{"lookahead_ms", "lookahead_10ms"}}}};
    d.params = {
        {"threshold_db", kThresholdDb, "Threshold", "dB",
         "Level above which gain reduction begins.", ForgeParamKind::continuous,
         ForgeParamCurve::linear},
        {"ratio", kRatio, "Ratio", ":1", "Gain-reduction ratio above threshold.",
         ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
        {"knee_db", kKneeDb, "Knee", "dB", "Width of the transition into compression.",
         ForgeParamKind::continuous, ForgeParamCurve::linear},
        {"attack_ms", kAttackMs, "Attack", "ms", "Time for gain reduction to engage.",
         ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
        {"release_ms", kReleaseMs, "Release", "ms", "Time for gain reduction to recover.",
         ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
        {"detector_mode",
         kDetectorMode,
         "Detector",
         "",
         "Peak or RMS level detection.",
         ForgeParamKind::stepped,
         ForgeParamCurve::linear,
         {{"peak", "Peak", 0.0f}, {"rms", "RMS", 1.0f}}},
        {"rms_window_ms", kRmsWindowMs, "RMS Window", "ms",
         "Averaging time used by the RMS detector.", ForgeParamKind::continuous,
         ForgeParamCurve::logarithmic},
        {"program_dependent",
         kProgramDependent,
         "Program Release",
         "",
         "Adapts release timing to sustained program material.",
         ForgeParamKind::stepped,
         ForgeParamCurve::linear,
         {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}},
        {"makeup_db", kMakeupDb, "Makeup", "dB", "Output gain after compression.",
         ForgeParamKind::continuous, ForgeParamCurve::linear},
        {"auto_makeup",
         kAutoMakeup,
         "Auto Makeup",
         "",
         "Automatically compensates for expected gain reduction.",
         ForgeParamKind::stepped,
         ForgeParamCurve::linear,
         {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}},
        {"stereo_link", kStereoLink, "Stereo Link", "%",
         "Couples channel detectors to preserve stereo balance.", ForgeParamKind::continuous,
         ForgeParamCurve::linear},
    };
    return d;
}

namespace true_peak {

inline constexpr const char* kTypeId = "dynamics.true_peak_limiter";
inline constexpr state::ParamID kCeilingDbtp = 1;
inline constexpr state::ParamID kReleaseMs = 2;

struct Instance {
    static constexpr std::size_t kControlTableSize = 4097;
    signal::TruePeakLimiter limiter;
    std::array<double, kControlTableSize> ceiling_table{};
    std::array<double, kControlTableSize> release_table{};
    float last_ceiling_dbtp = -1.0f;
    float last_release_ms = 100.0f;

    static double table_value(const std::array<double, kControlTableSize>& table, double value,
                              double minimum, double maximum) noexcept {
        const double position = std::clamp((value - minimum) / (maximum - minimum), 0.0, 1.0) *
                                static_cast<double>(kControlTableSize - 1);
        const auto lower = static_cast<std::size_t>(position);
        const auto upper = std::min(lower + 1, kControlTableSize - 1);
        const double fraction = position - static_cast<double>(lower);
        return table[lower] + fraction * (table[upper] - table[lower]);
    }

    void prepare(double sample_rate) {
        for (std::size_t i = 0; i < kControlTableSize; ++i) {
            const double unit = static_cast<double>(i) / static_cast<double>(kControlTableSize - 1);
            const double ceiling = -24.0 + 24.0 * unit;
            const double release = 5.0 + 1995.0 * unit;
            ceiling_table[i] = std::pow(
                10.0, (ceiling - signal::TruePeakLimiter::detector_guard_db()) / 20.0);
            release_table[i] = signal::dynamics::one_pole_retain(release * 0.001, sample_rate);
        }
        last_ceiling_dbtp = -1.0f;
        last_release_ms = 100.0f;
    }

    void set_controls(float ceiling_dbtp, float release_ms) noexcept {
        if (ceiling_dbtp == last_ceiling_dbtp && release_ms == last_release_ms)
            return;
        last_ceiling_dbtp = ceiling_dbtp;
        last_release_ms = release_ms;
        limiter.set_realtime_control_coefficients(
            ceiling_dbtp, table_value(ceiling_table, ceiling_dbtp, -24.0, 0.0), release_ms,
            table_value(release_table, release_ms, 5.0, 2000.0));
    }
};

inline CustomNodeType make_node(float lookahead_ms = 5.0f, bool linked = true) {
    using Limiter = signal::TruePeakLimiter;
    const double fixed_lookahead = std::clamp(
        std::isfinite(static_cast<double>(lookahead_ms)) ? static_cast<double>(lookahead_ms) : 5.0,
        0.0, Limiter::maximum_lookahead_ms());

    CustomNodeType type;
    type.type_id = kTypeId;
    type.type_id += ".la_" + detail::realization_real_token(fixed_lookahead);
    type.type_id += linked ? ".linked" : ".independent";
    type.version = 1;
    type.num_input_ports = 2;
    type.num_output_ports = 2;
    type.default_name = "True-Peak Limiter";
    type.lowerable = true;
    type.latency_samples = [fixed_lookahead](double sample_rate) {
        return Limiter::detector_latency_samples() +
               static_cast<int>(std::ceil(fixed_lookahead * 0.001 * sample_rate));
    };
    type.create = []() -> void* { return new Instance{}; };
    type.destroy = [](void* pointer) { delete static_cast<Instance*>(pointer); };
    type.prepare = [fixed_lookahead, linked](void* pointer, double sample_rate, int) {
        auto& instance = *static_cast<Instance*>(pointer);
        Limiter::Params params;
        params.lookahead_ms = fixed_lookahead;
        params.channel_link =
            linked ? Limiter::ChannelLink::linked : Limiter::ChannelLink::independent;
        instance.limiter.prepare(sample_rate, 2, params);
        instance.prepare(sample_rate);
    };
    type.reset = [](void* pointer) { static_cast<Instance*>(pointer)->limiter.reset(); };
    type.baked_params.push_back({kCeilingDbtp, -24.0f, 0.0f, -1.0f});
    type.baked_params.push_back({kReleaseMs, 5.0f, 2000.0f, 100.0f});
    type.process_instance_baked_param = [](void* pointer, audio::BufferView<float>& output,
                                           const audio::BufferView<const float>& input, int frames,
                                           const BakedParamView& params) {
        auto& limiter = static_cast<Instance*>(pointer)->limiter;
        auto& instance = *static_cast<Instance*>(pointer);
        for (int frame = 0; frame < frames; ++frame) {
            const auto offset = static_cast<std::int32_t>(frame);
            instance.set_controls(params.value_at(kCeilingDbtp, offset),
                                  params.value_at(kReleaseMs, offset));
            const std::array<float, 2> in{input.channel_ptr(0)[frame], input.channel_ptr(1)[frame]};
            std::array<float, 2> out{};
            limiter.process_frame(in, out);
            output.channel_ptr(0)[frame] = out[0];
            output.channel_ptr(1)[frame] = out[1];
        }
    };
    return type;
}

inline ForgeNodeDescriptor descriptor() {
    ForgeNodeDescriptor descriptor;
    descriptor.key = "true_peak_limiter";
    descriptor.label = "True-Peak Limiter";
    descriptor.description =
        "Look-ahead stereo limiter with oversampled intersample-peak detection.";
    descriptor.axes = {
        {"lookahead_ms",
         "Lookahead",
         "Fixed lookahead and matching host latency.",
         {{"zero", "0 ms", 0.0f}, {"five", "5 ms", 5.0f}, {"ten", "10 ms", 10.0f}}},
        {"channel_link",
         "Channel link",
         "Linked preserves the stereo image; independent limits each channel separately.",
         {{"linked", "Linked", 1.0f}, {"independent", "Independent", 0.0f}}},
    };
    descriptor.realizations = {
        {"linked_0ms",
         make_node(0.0f, true).type_id,
         {{"lookahead_ms", "zero"}, {"channel_link", "linked"}}},
        {"linked_5ms",
         make_node(5.0f, true).type_id,
         {{"lookahead_ms", "five"}, {"channel_link", "linked"}}},
        {"linked_10ms",
         make_node(10.0f, true).type_id,
         {{"lookahead_ms", "ten"}, {"channel_link", "linked"}}},
        {"independent_5ms",
         make_node(5.0f, false).type_id,
         {{"lookahead_ms", "five"}, {"channel_link", "independent"}}},
    };
    descriptor.params = {
        {"ceiling_dbtp", kCeilingDbtp, "Ceiling", "dBTP", "Maximum reconstructed peak level.",
         ForgeParamKind::continuous, ForgeParamCurve::linear},
        {"release_ms", kReleaseMs, "Release", "ms", "Gain-reduction recovery time.",
         ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
    };
    return descriptor;
}

}  // namespace true_peak

// ── The VCA lineage (Blackmer/dbx) ────────────────────────────────────────
//
// One node type. Nothing here changes topology and nothing changes latency
// except the lookahead, which is therefore taken at construction.
namespace vca {

inline constexpr const char* kTypeId = "dynamics.vca_compressor";

// Node-local ids; the framework namespaces per node, so these numbers may
// restart at 1 without colliding with the feedforward member's.
inline constexpr state::ParamID kThresholdDb = 1;     // dB
inline constexpr state::ParamID kRatio = 2;           // :1
inline constexpr state::ParamID kKneeDb = 3;          // dB, 0 = hard, > 0 = OverEasy
inline constexpr state::ParamID kTimeMs = 4;          // ms — ONE control, both directions
inline constexpr state::ParamID kMakeupDb = 5;        // dB
inline constexpr state::ParamID kMix = 6;             // 0..1
inline constexpr state::ParamID kNegativeRatio = 7;   // stepped 0/1 — "infinity+"
inline constexpr state::ParamID kNegRatioAmount = 8;  // :1, negative
inline constexpr state::ParamID kCeilingDb = 9;       // dB, positive magnitude

using Comp = signal::VcaCompressor;

struct Instance {
    signal::VcaCompressor compressor;
};

/// Worst-case linear gain for the Forge registry (series law 8).
///
/// Feedforward topology: the detector reads the input, so there is no loop to
/// bound. The gain command is bounded above by the makeup ceiling and below by
/// `−ceiling_db`, and the DSP suite asserts both rather than assuming them —
/// this is that asserted upper bound, not a fresh estimate.
inline float vca_compressor_worst_case_gain() {
    return static_cast<float>(std::pow(10.0, Comp::kMakeupDbMax / 20.0));
}

/// `lookahead_ms` is a construction-time argument, not a param: it IS this
/// member's `latency_samples()`. The DSP sizes its ring for the whole declared
/// range at `prepare()`, so any value in range is allocation-free — it is frozen
/// here for the host's sake, not the allocator's.
inline CustomNodeType make_vca_compressor_node(float lookahead_ms = 0.0f,
                                               double attack_release_k = Comp::kRatioKDefault) {
    const double fixed_lookahead_ms = std::clamp(
        std::isfinite(static_cast<double>(lookahead_ms)) ? static_cast<double>(lookahead_ms) : 0.0,
        0.0, Comp::kLookaheadMsMax);
    const double fixed_attack_release_k =
        std::clamp(std::isfinite(attack_release_k) ? attack_release_k : Comp::kRatioKDefault,
        Comp::kRatioKMin, Comp::kRatioKMax);
    CustomNodeType t;
    t.type_id = kTypeId;
    if (fixed_lookahead_ms != 0.0 || fixed_attack_release_k != Comp::kRatioKDefault) {
        t.type_id += ".la_" + detail::realization_real_token(fixed_lookahead_ms) + ".ark_" +
                     detail::realization_real_token(fixed_attack_release_k);
    }
    t.version = 1;
    t.num_input_ports = 1;
    t.num_output_ports = 1;
    t.default_name = "VCA Compressor";
    t.lowerable = true;
    t.latency_samples = [fixed_lookahead_ms](double sample_rate) {
        Comp probe;
        probe.prepare(sample_rate);
        probe.set_lookahead_ms(fixed_lookahead_ms);
        return probe.latency_samples();
    };

    t.create = []() -> void* { return new Instance{}; };
    t.destroy = [](void* p) { delete static_cast<Instance*>(p); };
    t.prepare = [fixed_lookahead_ms, fixed_attack_release_k](void* p, double sr,
                                                             int /*max_block*/) {
        auto* s = static_cast<Instance*>(p);
        s->compressor.prepare(sr);
        // After `prepare()`, which is what sized the ring and would otherwise
        // reset these to their defaults.
        s->compressor.set_attack_release_ratio_k(fixed_attack_release_k);
        s->compressor.set_lookahead_ms(fixed_lookahead_ms);
    };
    t.reset = [](void* p) { static_cast<Instance*>(p)->compressor.reset(); };

    t.baked_params.push_back({kThresholdDb, static_cast<float>(Comp::kThresholdDbMin),
                              static_cast<float>(Comp::kThresholdDbMax), -20.0f});
    t.baked_params.push_back(
        {kRatio, static_cast<float>(Comp::kRatioMin), static_cast<float>(Comp::kRatioMax), 4.0f});
    t.baked_params.push_back({kKneeDb, static_cast<float>(Comp::kKneeDbMin),
                              static_cast<float>(Comp::kKneeDbMax), 10.0f});
    t.baked_params.push_back({kTimeMs, static_cast<float>(Comp::kTimeMsMin),
                              static_cast<float>(Comp::kTimeMsMax), 30.0f});
    t.baked_params.push_back({kMakeupDb, -static_cast<float>(Comp::kMakeupDbMax),
                              static_cast<float>(Comp::kMakeupDbMax), 0.0f});
    t.baked_params.push_back({kMix, 0.0f, 1.0f, 1.0f});
    t.baked_params.push_back({kNegativeRatio, 0.0f, 1.0f, 0.0f});
    t.baked_params.push_back({kNegRatioAmount, static_cast<float>(Comp::kNegRatioMin),
                              static_cast<float>(Comp::kNegRatioMax), -4.0f});
    t.baked_params.push_back({kCeilingDb, static_cast<float>(Comp::kCeilingDbMin),
                              static_cast<float>(Comp::kCeilingDbMax),
                              static_cast<float>(Comp::kCeilingDbDefault)});

    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& in, int n,
                                        const BakedParamView& params) {
        auto* s = static_cast<Instance*>(p);
        const float* input = in.channel_ptr(0);
        float* output = out.channel_ptr(0);
        for (int k = 0; k < n; ++k) {
            const auto offset = static_cast<std::int32_t>(k);
            s->compressor.set_threshold_db(params.value_at(kThresholdDb, offset));
            s->compressor.set_ratio(params.value_at(kRatio, offset));
            s->compressor.set_knee_db(params.value_at(kKneeDb, offset));
            s->compressor.set_time_ms(params.value_at(kTimeMs, offset));
            s->compressor.set_makeup_db(params.value_at(kMakeupDb, offset));
            s->compressor.set_mix(params.value_at(kMix, offset));
            s->compressor.set_negative_ratio_mode(params.value_at(kNegativeRatio, offset) >= 0.5f);
            s->compressor.set_neg_ratio_amount(params.value_at(kNegRatioAmount, offset));
            s->compressor.set_ceiling_db(params.value_at(kCeilingDb, offset));
            output[static_cast<std::size_t>(k)] =
                s->compressor.process(input[static_cast<std::size_t>(k)]);
        }
    };
    return t;
}

inline ForgeNodeDescriptor vca_compressor_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "vca_compressor";
    d.label = "VCA Compressor";
    d.description = "Fast VCA-style compressor with OverEasy knee and optional negative-ratio "
                    "behavior.";
    d.axes = {{"lookahead_ms",
               "Lookahead",
               "Fixed detector lookahead; non-zero values report matching latency.",
               {{"zero_latency", "Zero latency", 0.0f},
                {"lookahead_3ms", "3 ms", 3.0f},
                {"lookahead_10ms", "10 ms", 10.0f}}},
              {"attack_release_k",
               "Attack/Release Lock",
               "Fixed release-to-attack timing ratio.",
               {{"k2", "2:1", 2.0f}, {"k4", "4:1", 4.0f}, {"k8", "8:1", 8.0f}}}};
    // Axes describe the construction dimensions; realizations are the supported
    // finite subset, not an implied Cartesian product. These five are exactly
    // the VCA registrations exposed to Forge.
    d.realizations = {{"default",
                       make_vca_compressor_node(0.0f, 4.0).type_id,
                       {{"lookahead_ms", "zero_latency"}, {"attack_release_k", "k4"}}},
                      {"lookahead_3ms_k4",
                       make_vca_compressor_node(3.0f, 4.0).type_id,
                       {{"lookahead_ms", "lookahead_3ms"}, {"attack_release_k", "k4"}}},
                      {"lookahead_10ms_k4",
                       make_vca_compressor_node(10.0f, 4.0).type_id,
                       {{"lookahead_ms", "lookahead_10ms"}, {"attack_release_k", "k4"}}},
                      {"zero_latency_k2",
                       make_vca_compressor_node(0.0f, 2.0).type_id,
                       {{"lookahead_ms", "zero_latency"}, {"attack_release_k", "k2"}}},
                      {"zero_latency_k8",
                       make_vca_compressor_node(0.0f, 8.0).type_id,
                       {{"lookahead_ms", "zero_latency"}, {"attack_release_k", "k8"}}}};
    d.params = {
        {"threshold_db", kThresholdDb, "Threshold", "dB",
         "Level above which gain reduction begins.", ForgeParamKind::continuous,
         ForgeParamCurve::linear},
        {"ratio", kRatio, "Ratio", ":1", "Gain-reduction ratio above threshold.",
         ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
        {"knee_db", kKneeDb, "Knee", "dB", "Width of the soft-knee transition.",
         ForgeParamKind::continuous, ForgeParamCurve::linear},
        {"time_ms", kTimeMs, "Time", "ms", "Coupled attack and release timing.",
         ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
        {"makeup_db", kMakeupDb, "Makeup", "dB", "Output gain after compression.",
         ForgeParamKind::continuous, ForgeParamCurve::linear},
        {"mix", kMix, "Mix", "%", "Blend between dry and compressed signals.",
         ForgeParamKind::continuous, ForgeParamCurve::linear},
        {"negative_ratio",
         kNegativeRatio,
         "Negative Ratio",
         "",
         "Enables the beyond-limiting negative-ratio curve.",
         ForgeParamKind::stepped,
         ForgeParamCurve::linear,
         {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}},
        {"negative_ratio_amount", kNegRatioAmount, "Negative Ratio Amount", ":1",
         "Slope used while negative-ratio mode is active.", ForgeParamKind::continuous,
         ForgeParamCurve::linear},
        {"ceiling_db", kCeilingDb, "Ceiling", "dB", "Maximum output level in negative-ratio mode.",
         ForgeParamKind::continuous, ForgeParamCurve::linear},
    };
    return d;
}

}  // namespace vca

// ── The FET lineage (1176) ────────────────────────────────────────────────
//
// One node type, and no realization axis at all: `latency_samples()` is a
// constant 16 for every parameter setting, so there is nothing here that has to
// be frozen at registration.
namespace fet {

inline constexpr const char* kTypeId = "dynamics.fet_compressor";

inline constexpr state::ParamID kInputGainDb = 1;        // dB — the only lever in
inline constexpr state::ParamID kOutputGainDb = 2;       // dB — makeup
inline constexpr state::ParamID kRatio = 3;              // stepped 0..4, see kRatioSteps
inline constexpr state::ParamID kAttackUs = 4;           // µs
inline constexpr state::ParamID kReleaseMs = 5;          // ms
inline constexpr state::ParamID kKneeDb = 6;             // dB
inline constexpr state::ParamID kTransformerAmount = 7;  // 0..1
inline constexpr state::ParamID kMix = 8;                // 0..1

/// The ratio switch's positions, in injection order: 4:1, 8:1, 12:1, 20:1,
/// all-buttons-in. ABI is the documented distinct circuit state, not a fifth
/// ratio — but in this model it is five coefficient changes over an unchanged
/// signal path with unchanged latency, so it is a stepped PARAM (a front-panel
/// switch a user automates) rather than a fifth registered realization.
inline constexpr float kRatioSteps = 4.0f;

using Comp = signal::FetCompressor;

struct Instance {
    signal::FetCompressor compressor;
};

/// Worst-case linear gain for the Forge registry (series law 8).
///
/// This member has a feedback loop, so series law 8 bites hardest here. The
/// number is NOT an estimate: the DSP exposes a closed-form ℓ∞ bound assembled
/// from its shipped stages — input gain × divider supremum × the resampling
/// pair's ℓ1 product × output gain × transformer supremum — and its suite
/// asserts both that realised gain never exceeds it and that the divider
/// supremum really is exactly 1. Reported at the node's PARAMETER CEILINGS,
/// because a baked param can be automated anywhere in its declared range.
inline float fet_compressor_worst_case_gain() {
    signal::FetCompressor probe;
    probe.set_input_gain_db(Comp::kInputGainDbMax);
    probe.set_output_gain_db(Comp::kOutputGainDbMax);
    probe.set_mix(1.0);
    return static_cast<float>(probe.worst_case_gain());
}

inline CustomNodeType make_fet_compressor_node() {
    CustomNodeType t;
    t.type_id = kTypeId;
    t.version = 1;
    t.num_input_ports = 1;
    t.num_output_ports = 1;
    t.default_name = "FET Compressor";
    t.lowerable = true;
    t.latency_samples = [](double) { return Comp::kLatencySamples; };

    t.create = []() -> void* { return new Instance{}; };
    t.destroy = [](void* p) { delete static_cast<Instance*>(p); };
    t.prepare = [](void* p, double sr, int /*max_block*/) {
        static_cast<Instance*>(p)->compressor.prepare(sr);
    };
    t.reset = [](void* p) { static_cast<Instance*>(p)->compressor.reset(); };

    t.baked_params.push_back({kInputGainDb, static_cast<float>(Comp::kInputGainDbMin),
                              static_cast<float>(Comp::kInputGainDbMax), 0.0f});
    t.baked_params.push_back({kOutputGainDb, static_cast<float>(Comp::kOutputGainDbMin),
                              static_cast<float>(Comp::kOutputGainDbMax), 0.0f});
    t.baked_params.push_back({kRatio, 0.0f, kRatioSteps, 0.0f});
    t.baked_params.push_back({kAttackUs, static_cast<float>(Comp::kAttackUsMin),
                              static_cast<float>(Comp::kAttackUsMax), 200.0f});
    t.baked_params.push_back({kReleaseMs, static_cast<float>(Comp::kReleaseMsMin),
                              static_cast<float>(Comp::kReleaseMsMax), 300.0f});
    t.baked_params.push_back({kKneeDb, static_cast<float>(Comp::kKneeDbMin),
                              static_cast<float>(Comp::kKneeDbMax), 1.0f});
    t.baked_params.push_back({kTransformerAmount, 0.0f, 1.0f, 0.6f});
    t.baked_params.push_back({kMix, 0.0f, 1.0f, 1.0f});

    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& in, int n,
                                        const BakedParamView& params) {
        auto* s = static_cast<Instance*>(p);
        const float* input = in.channel_ptr(0);
        float* output = out.channel_ptr(0);
        for (int k = 0; k < n; ++k) {
            const auto offset = static_cast<std::int32_t>(k);
            s->compressor.set_input_gain_db(params.value_at(kInputGainDb, offset));
            s->compressor.set_output_gain_db(params.value_at(kOutputGainDb, offset));
            const int step =
                std::clamp(static_cast<int>(std::lround(params.value_at(kRatio, offset))), 0,
                static_cast<int>(kRatioSteps));
            s->compressor.set_ratio(static_cast<signal::FetRatio>(step));
            s->compressor.set_attack_us(params.value_at(kAttackUs, offset));
            s->compressor.set_release_ms(params.value_at(kReleaseMs, offset));
            s->compressor.set_knee_db(params.value_at(kKneeDb, offset));
            s->compressor.set_transformer_amount(params.value_at(kTransformerAmount, offset));
            s->compressor.set_mix(params.value_at(kMix, offset));
            output[static_cast<std::size_t>(k)] =
                s->compressor.process(input[static_cast<std::size_t>(k)]);
        }
    };
    return t;
}

inline ForgeNodeDescriptor fet_compressor_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "fet_compressor";
    d.label = "FET Compressor";
    d.description = "Fast 1176-style FET compressor with switched ratios and transformer color.";
    d.realizations = {{"default", kTypeId}};
    d.params = {
        {"input_gain_db", kInputGainDb, "Input", "dB",
         "Input drive into the gain-reduction circuit.", ForgeParamKind::continuous,
         ForgeParamCurve::linear},
        {"output_gain_db", kOutputGainDb, "Output", "dB", "Output makeup gain.",
         ForgeParamKind::continuous, ForgeParamCurve::linear},
        {"ratio",
         kRatio,
         "Ratio",
         "",
         "Front-panel ratio-button selection.",
         ForgeParamKind::stepped,
         ForgeParamCurve::linear,
         {{"four", "4:1", 0.0f},
          {"eight", "8:1", 1.0f},
          {"twelve", "12:1", 2.0f},
          {"twenty", "20:1", 3.0f},
          {"all_buttons", "All Buttons", 4.0f}}},
        {"attack_us", kAttackUs, "Attack", "us", "Time for compression to engage.",
         ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
        {"release_ms", kReleaseMs, "Release", "ms", "Time for compression to recover.",
         ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
        {"knee_db", kKneeDb, "Knee", "dB", "Softness of the compression knee.",
         ForgeParamKind::continuous, ForgeParamCurve::linear},
        {"transformer", kTransformerAmount, "Transformer", "%",
         "Amount of output-transformer coloration.", ForgeParamKind::continuous,
         ForgeParamCurve::linear},
        {"mix", kMix, "Mix", "%", "Blend between dry and compressed signals.",
         ForgeParamKind::continuous, ForgeParamCurve::linear},
    };
    return d;
}

}  // namespace fet

// ── The diode-bridge lineage ──────────────────────────────────────────────
//
// TWO node types, split on the detection topology. See the header note: moving
// the detector from the input to the output re-maps the static curve, which is
// a different design rather than a mode of one.
namespace diode {

/// Feedback detection — the lineage's own topology, and the DSP's default.
inline constexpr const char* kTypeId = "dynamics.diode_bridge_compressor";
/// Feedforward detection — the same bridge, sensed from the input.
inline constexpr const char* kFeedforwardTypeId = "dynamics.diode_bridge_compressor_feedforward";

inline constexpr state::ParamID kThresholdDb = 1;   // dB
inline constexpr state::ParamID kRatio = 2;         // :1, kLimitRatio and above = limit
inline constexpr state::ParamID kKneeDb = 3;        // dB
inline constexpr state::ParamID kAttackMs = 4;      // ms
inline constexpr state::ParamID kReleaseMs = 5;     // ms
inline constexpr state::ParamID kMakeupDb = 6;      // dB, positive only
inline constexpr state::ParamID kCharacter = 7;     // 0..1, bridge + transformer drive
inline constexpr state::ParamID kMixPercent = 8;    // %
inline constexpr state::ParamID kScHpfHz = 9;       // Hz — LF de-sensitisation
inline constexpr state::ParamID kAutoRelease = 10;  // stepped 0/1

using Comp = signal::DiodeBridgeCompressor;

struct Instance {
    signal::DiodeBridgeCompressor compressor;
};

/// Worst-case linear gain for the Forge registry (series law 8).
///
/// The DSP's own static bound: makeup ceiling times both transformer brackets'
/// peak gain, with the bridge contributing at most 1 because it is an
/// attenuator. Its suite asserts that bound; this reports it rather than
/// re-deriving it, so the two cannot drift.
inline float diode_bridge_compressor_worst_case_gain() {
    return static_cast<float>(Comp::worst_case_gain());
}

/// `feedback` picks the registered type id; `adaa` is the antialiasing policy,
/// frozen at registration following the saturator. ADAA does not move this
/// member's latency — it reports 0 either way, because the bridge is memoryless
/// and the one-poles add phase rather than delay — so the reason it is frozen is
/// the saturator's other one: it is a fidelity/CPU policy for the artifact's
/// life, not a control anyone automates, and flipping it per sample would swap a
/// memoryless evaluation for a difference quotient mid-waveform and click.
inline CustomNodeType make_diode_bridge_compressor_node(bool feedback = true, bool adaa = true) {
    CustomNodeType t;
    t.type_id = feedback ? kTypeId : kFeedforwardTypeId;
    t.version = 1;
    t.num_input_ports = 1;
    t.num_output_ports = 1;
    t.default_name = feedback ? "Diode Bridge Compressor" : "Diode Bridge Compressor (FF)";
    t.lowerable = true;

    t.create = []() -> void* { return new Instance{}; };
    t.destroy = [](void* p) { delete static_cast<Instance*>(p); };
    t.prepare = [feedback, adaa](void* p, double sr, int /*max_block*/) {
        auto* s = static_cast<Instance*>(p);
        s->compressor.prepare(sr);
        s->compressor.set_feedback(feedback);
        s->compressor.set_adaa(adaa);
    };
    t.reset = [](void* p) { static_cast<Instance*>(p)->compressor.reset(); };

    t.baked_params.push_back({kThresholdDb, static_cast<float>(Comp::kThresholdDbMin),
                              static_cast<float>(Comp::kThresholdDbMax), -12.0f});
    t.baked_params.push_back(
        {kRatio, static_cast<float>(Comp::kRatioMin), static_cast<float>(Comp::kRatioMax), 4.0f});
    t.baked_params.push_back({kKneeDb, static_cast<float>(Comp::kKneeDbMin),
                              static_cast<float>(Comp::kKneeDbMax), 6.0f});
    t.baked_params.push_back({kAttackMs, static_cast<float>(Comp::kAttackMsMin),
                              static_cast<float>(Comp::kAttackMsMax), 3.0f});
    t.baked_params.push_back({kReleaseMs, static_cast<float>(Comp::kReleaseMsMin),
                              static_cast<float>(Comp::kReleaseMsMax), 400.0f});
    t.baked_params.push_back({kMakeupDb, 0.0f, static_cast<float>(Comp::kMakeupDbMax), 0.0f});
    t.baked_params.push_back({kCharacter, 0.0f, 1.0f, 0.35f});
    t.baked_params.push_back({kMixPercent, 0.0f, 100.0f, 100.0f});
    t.baked_params.push_back({kScHpfHz, static_cast<float>(Comp::kScHpfHzMin),
                              static_cast<float>(Comp::kScHpfHzMax), 100.0f});
    t.baked_params.push_back({kAutoRelease, 0.0f, 1.0f, 0.0f});

    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& in, int n,
                                        const BakedParamView& params) {
        auto* s = static_cast<Instance*>(p);
        const float* input = in.channel_ptr(0);
        float* output = out.channel_ptr(0);
        for (int k = 0; k < n; ++k) {
            const auto offset = static_cast<std::int32_t>(k);
            s->compressor.set_threshold_db(params.value_at(kThresholdDb, offset));
            s->compressor.set_ratio(params.value_at(kRatio, offset));
            s->compressor.set_knee_db(params.value_at(kKneeDb, offset));
            s->compressor.set_attack_ms(params.value_at(kAttackMs, offset));
            s->compressor.set_release_ms(params.value_at(kReleaseMs, offset));
            s->compressor.set_makeup_db(params.value_at(kMakeupDb, offset));
            s->compressor.set_character(params.value_at(kCharacter, offset));
            s->compressor.set_mix_percent(params.value_at(kMixPercent, offset));
            s->compressor.set_sc_hpf_hz(params.value_at(kScHpfHz, offset));
            s->compressor.set_auto_release(params.value_at(kAutoRelease, offset) >= 0.5f);
            output[static_cast<std::size_t>(k)] =
                s->compressor.process(input[static_cast<std::size_t>(k)]);
        }
    };
    return t;
}

inline ForgeNodeDescriptor diode_bridge_compressor_descriptor() {
    ForgeNodeDescriptor d;
    d.key = "diode_bridge_compressor";
    d.label = "Diode Bridge Compressor";
    d.description = "Diode-bridge dynamics with transformer character and feedback or "
                    "feedforward detection.";
    d.axes = {{"topology",
               "Topology",
               "Position of the detector relative to the gain-control bridge.",
               {{"feedback", "Feedback", 0.0f}, {"feedforward", "Feedforward", 1.0f}}}};
    d.realizations = {{"feedback", kTypeId, {{"topology", "feedback"}}},
                      {"feedforward", kFeedforwardTypeId, {{"topology", "feedforward"}}}};
    d.params = {
        {"threshold_db", kThresholdDb, "Threshold", "dB",
         "Level above which gain reduction begins.", ForgeParamKind::continuous,
         ForgeParamCurve::linear},
        {"ratio", kRatio, "Ratio", ":1", "Gain-reduction ratio above threshold.",
         ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
        {"knee_db", kKneeDb, "Knee", "dB", "Width of the soft-knee transition.",
         ForgeParamKind::continuous, ForgeParamCurve::linear},
        {"attack_ms", kAttackMs, "Attack", "ms", "Time for compression to engage.",
         ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
        {"release_ms", kReleaseMs, "Release", "ms", "Time for compression to recover.",
         ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
        {"makeup_db", kMakeupDb, "Makeup", "dB", "Output gain after compression.",
         ForgeParamKind::continuous, ForgeParamCurve::linear},
        {"character", kCharacter, "Character", "%",
         "Drive through the diode bridge and transformer stages.", ForgeParamKind::continuous,
         ForgeParamCurve::linear},
        {"mix", kMixPercent, "Mix", "%", "Blend between dry and compressed signals.",
         ForgeParamKind::continuous, ForgeParamCurve::linear},
        {"sidechain_hpf_hz", kScHpfHz, "Sidechain HPF", "Hz",
         "High-pass cutoff that reduces low-frequency detector sensitivity.",
         ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
        {"auto_release",
         kAutoRelease,
         "Auto Release",
         "",
         "Adapts release timing to the detected program.",
         ForgeParamKind::stepped,
         ForgeParamCurve::linear,
         {{"off", "Off", 0.0f}, {"on", "On", 1.0f}}},
    };
    return d;
}

}  // namespace diode

}  // namespace pulp::host::dynamics
