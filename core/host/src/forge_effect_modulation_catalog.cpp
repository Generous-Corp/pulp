#include <pulp/host/forge_effect_modulation_catalog.hpp>

#include <cstddef>
#include <cstdint>

namespace pulp::host::modulation {

namespace {
struct SsbFrequencyShifterInstance {
    signal::SsbFrequencyShifter shifter;
};
} // namespace

float ssb_frequency_shifter_worst_case_gain() {
    return static_cast<float>(signal::SsbFrequencyShifter::worst_case_gain());
}

CustomNodeType make_frequency_shifter_node() {
    CustomNodeType t;
    t.type_id = kSsbFrequencyShifterTypeId;
    t.version = 1;
    t.num_input_ports = 2;
    t.num_output_ports = 2;
    t.default_name = "Frequency Shifter";
    t.lowerable = true;

    t.create = []() -> void* { return new SsbFrequencyShifterInstance{}; };
    t.destroy = [](void* p) { delete static_cast<SsbFrequencyShifterInstance*>(p); };
    t.prepare = [](void* p, double sr, int /*max_block*/) {
        static_cast<SsbFrequencyShifterInstance*>(p)->shifter.prepare(sr);
    };
    t.reset = [](void* p) { static_cast<SsbFrequencyShifterInstance*>(p)->shifter.reset(); };

    using Shifter = signal::SsbFrequencyShifter;
    t.baked_params.push_back({kShiftHz, static_cast<float>(-Shifter::kMaxShiftHz),
                              static_cast<float>(Shifter::kMaxShiftHz), 0.0f});
    t.baked_params.push_back({kFeedback, 0.0f, static_cast<float>(Shifter::kMaxFeedback), 0.0f});
    t.baked_params.push_back({kFeedbackDelayMs, static_cast<float>(Shifter::kMinDelayMs),
                              static_cast<float>(Shifter::kMaxLoopMs), 8.0f});
    t.baked_params.push_back({kMix, 0.0f, 100.0f, 100.0f});
    t.baked_params.push_back({kShiftMode, kModeUp, kModeStereoSplit, kModeUp});
    t.baked_params.push_back({kStereoSpread, 0.0f, 100.0f, 100.0f});

    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& in, int n,
                                        const BakedParamView& params) {
        auto* s = static_cast<SsbFrequencyShifterInstance*>(p);
        const float* in_left = in.channel_ptr(0);
        const float* in_right = in.channel_ptr(1);
        float* out_left = out.channel_ptr(0);
        float* out_right = out.channel_ptr(1);
        for (int k = 0; k < n; ++k) {
            const auto offset = static_cast<std::int32_t>(k);
            s->shifter.set_shift_hz(params.value_at(kShiftHz, offset));
            s->shifter.set_feedback(params.value_at(kFeedback, offset));
            s->shifter.set_feedback_delay_ms(params.value_at(kFeedbackDelayMs, offset));
            s->shifter.set_mix(params.value_at(kMix, offset) * 0.01f);
            s->shifter.set_stereo_spread(params.value_at(kStereoSpread, offset) * 0.01f);
            s->shifter.set_mode(mode_from_param(params.value_at(kShiftMode, offset)));
            float left = in_left[static_cast<std::size_t>(k)];
            float right = in_right[static_cast<std::size_t>(k)];
            s->shifter.process_stereo(left, right);
            out_left[static_cast<std::size_t>(k)] = left;
            out_right[static_cast<std::size_t>(k)] = right;
        }
    };
    return t;
}

} // namespace pulp::host::modulation
