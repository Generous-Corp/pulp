#include <pulp/host/forge_dynamics_catalog.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace pulp::host::dynamics {

float feedforward_compressor_worst_case_gain() {
    return static_cast<float>(std::pow(10.0, signal::FeedforwardCompressor::kMakeupDbMax / 20.0));
}


CustomNodeType make_feedforward_compressor_node(float lookahead_ms) {
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


ForgeNodeDescriptor feedforward_compressor_descriptor() {
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

double Instance::table_value(const std::array<double, kControlTableSize>& table, double value,
                             double minimum, double maximum) noexcept {
    const double position = std::clamp((value - minimum) / (maximum - minimum), 0.0, 1.0) *
                            static_cast<double>(kControlTableSize - 1);
    const auto lower = static_cast<std::size_t>(position);
    const auto upper = std::min(lower + 1, kControlTableSize - 1);
    const double fraction = position - static_cast<double>(lower);
    return table[lower] + fraction * (table[upper] - table[lower]);
}

void Instance::prepare(double sample_rate) {
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

void Instance::set_controls(float ceiling_dbtp, float release_ms) noexcept {
    if (ceiling_dbtp == last_ceiling_dbtp && release_ms == last_release_ms)
        return;
    last_ceiling_dbtp = ceiling_dbtp;
    last_release_ms = release_ms;
    limiter.set_realtime_control_coefficients(
        ceiling_dbtp, table_value(ceiling_table, ceiling_dbtp, -24.0, 0.0), release_ms,
        table_value(release_table, release_ms, 5.0, 2000.0));
}

CustomNodeType make_node(float lookahead_ms, bool linked) {
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
        if (!std::isfinite(sample_rate) || sample_rate < 8000.0 ||
            sample_rate > Limiter::maximum_supported_sample_rate())
            return 0;
        return Limiter::detector_latency_samples() + Limiter::internal_gain_lookahead_samples() +
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
        instance.prepared = instance.limiter.prepare(sample_rate, 2, params);
        if (instance.prepared)
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
        if (!instance.prepared) {
            for (int channel = 0; channel < 2; ++channel)
                std::copy_n(input.channel_ptr(channel), frames,
                            output.channel_ptr(channel));
            return;
        }
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


ForgeNodeDescriptor descriptor() {
    ForgeNodeDescriptor descriptor;
    descriptor.key = "true_peak_limiter";
    descriptor.label = "True-Peak Limiter";
    descriptor.description =
        "Look-ahead stereo limiter with oversampled intersample-peak detection.";
    descriptor.axes = {
        {"lookahead_ms",
         "Lookahead",
         "Optional user lookahead added to the fixed internal detector horizon "
         "and reported host latency.",
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

namespace vca {

float vca_compressor_worst_case_gain() {
    return static_cast<float>(std::pow(10.0, Comp::kMakeupDbMax / 20.0));
}


CustomNodeType make_vca_compressor_node(float lookahead_ms,
                                               double attack_release_k) {
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


ForgeNodeDescriptor vca_compressor_descriptor() {
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

namespace fet {

float fet_compressor_worst_case_gain() {
    signal::FetCompressor probe;
    probe.set_input_gain_db(Comp::kInputGainDbMax);
    probe.set_output_gain_db(Comp::kOutputGainDbMax);
    probe.set_mix(1.0);
    return static_cast<float>(probe.worst_case_gain());
}


CustomNodeType make_fet_compressor_node() {
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


ForgeNodeDescriptor fet_compressor_descriptor() {
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

namespace diode {

float diode_bridge_compressor_worst_case_gain() {
    return static_cast<float>(Comp::worst_case_gain());
}


CustomNodeType make_diode_bridge_compressor_node(bool feedback, bool adaa) {
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


ForgeNodeDescriptor diode_bridge_compressor_descriptor() {
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
