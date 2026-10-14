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

// Out-of-line effect-modulation factories keep the public catalog contract in the
// header while avoiding repeated factory implementation code in consumers.

namespace chorus {

std::string chorus_type_id(Voicing voicing, JunoMode juno_mode, bool bbd_color) {
    std::string id = "modulation.chorus.";
    switch (voicing) {
    case Voicing::ce2:
        id += "ce2";
        break;
    case Voicing::juno_ensemble:
        id += "juno_";
        id += juno_mode == JunoMode::mode_I    ? "i"
              : juno_mode == JunoMode::mode_II ? "ii"
                                               : "i_plus_ii";
        break;
    case Voicing::dimension_d:
        id += "dimension_d";
        break;
    case Voicing::tri_chorus:
        id += "tri_chorus";
        break;
    }
    if (bbd_color)
        id += ".bbd";
    return id;
}

std::string chorus_display_name(Voicing voicing, JunoMode juno_mode, bool bbd_color) {
    std::string name;
    switch (voicing) {
    case Voicing::ce2:
        name = "Chorus (CE-2)";
        break;
    case Voicing::juno_ensemble:
        name = "Chorus (Juno ";
        name += juno_mode == JunoMode::mode_I    ? "I"
                : juno_mode == JunoMode::mode_II ? "II"
                                                 : "I+II";
        name += ")";
        break;
    case Voicing::dimension_d:
        name = "Chorus (Dimension D)";
        break;
    case Voicing::tri_chorus:
        name = "Chorus (Tri)";
        break;
    }
    if (bbd_color)
        name += " BBD";
    return name;
}

float chorus_worst_case_gain(Voicing voicing, double sample_rate) {
    Engine engine;
    engine.prepare(sample_rate);
    engine.set_voicing(voicing);
    engine.set_stereo_width(1.0f);
    return static_cast<float>(engine.worst_case_gain());
}

CustomNodeType make_chorus_node(Voicing voicing, JunoMode juno_mode, bool bbd_color) {
    CustomNodeType t;
    t.type_id = chorus_type_id(voicing, juno_mode, bbd_color);
    t.version = 1;
    t.num_input_ports = 2;  // ONE logical stereo wire: the mix matrix crosses
    t.num_output_ports = 2; // the channels, so instancing dual-mono loses it
    t.default_name = chorus_display_name(voicing, juno_mode, bbd_color);
    t.lowerable = true;

    t.create = []() -> void* { return new Instance{}; };
    t.destroy = [](void* p) { delete static_cast<Instance*>(p); };
    t.prepare = [voicing, juno_mode, bbd_color](void* p, double sr, int /*max_block*/) {
        auto* s = static_cast<Instance*>(p);
        s->engine.prepare(sr);
        // Order matters and the DSP says so: `set_voicing` adopts the voicing's
        // shipped rate, so the mode must follow it, and any rate the params
        // carry is applied per sample afterwards.
        s->engine.set_juno_mode(juno_mode);
        s->engine.set_voicing(voicing);
        s->engine.set_bbd_color(bbd_color);
    };
    t.reset = [](void* p) { static_cast<Instance*>(p)->engine.reset(); };

    t.baked_params.push_back({kRateHz, static_cast<float>(kRateMinHz),
                              static_cast<float>(kRateMaxHz),
                              static_cast<float>(Engine::calibration(voicing).rate_hz)});
    t.baked_params.push_back({kDepth, 0.0f, 100.0f, 100.0f});
    const auto mix_default = static_cast<float>(Engine::calibration(voicing).mix_default * 100.0);
    t.baked_params.push_back({kMix, 0.0f, 100.0f, mix_default});
    t.baked_params.push_back({kStereoWidth, 0.0f, 100.0f, 100.0f});

    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& in, int n,
                                        const BakedParamView& params) {
        auto* s = static_cast<Instance*>(p);
        const float* in_left = in.channel_ptr(0);
        const float* in_right = in.channel_ptr(1);
        float* out_left = out.channel_ptr(0);
        float* out_right = out.channel_ptr(1);

        for (int k = 0; k < n; ++k) {
            const auto offset = static_cast<std::int32_t>(k);
            s->engine.set_rate_hz(params.value_at(kRateHz, offset));
            s->engine.set_depth(params.value_at(kDepth, offset) * 0.01f);
            s->engine.set_mix(params.value_at(kMix, offset) * 0.01f);
            s->engine.set_stereo_width(params.value_at(kStereoWidth, offset) * 0.01f);

            // The engine processes in place, so the frame is staged in locals
            // rather than written to the output first — which also keeps this
            // correct when a caller aliases the input and output views.
            float left = in_left[static_cast<std::size_t>(k)];
            float right = in_right[static_cast<std::size_t>(k)];
            s->engine.process(&left, &right, 1);
            out_left[static_cast<std::size_t>(k)] = left;
            out_right[static_cast<std::size_t>(k)] = right;
        }
    };
    return t;
}

} // namespace chorus

namespace phaser {

std::string phaser_type_id(int stages) {
    return "modulation.phaser." + std::to_string(stages);
}

float phaser_worst_case_gain() {
    return static_cast<float>(Engine::worst_case_gain());
}

CustomNodeType make_phaser_node(int stages) {
    // Normalised the same way the DSP normalises it, so the registered id and
    // the running engine can never disagree about what was asked for.
    const int clamped = std::clamp(stages, Engine::kMinStages, Engine::kMaxStages);
    const int even_stages = clamped - (clamped % 2);

    CustomNodeType t;
    t.type_id = phaser_type_id(even_stages);
    t.version = 1;
    t.num_input_ports = 2;  // ONE logical stereo wire: the stereo spread puts
    t.num_output_ports = 2; // the two channels' LFOs in quadrature
    t.default_name = "Phaser (" + std::to_string(even_stages) + "-stage)";
    t.lowerable = true;

    t.create = []() -> void* { return new Instance{}; };
    t.destroy = [](void* p) { delete static_cast<Instance*>(p); };
    t.prepare = [even_stages](void* p, double sr, int /*max_block*/) {
        auto* s = static_cast<Instance*>(p);
        s->engine.prepare(sr);
        s->engine.set_stage_count(even_stages);
    };
    t.reset = [](void* p) { static_cast<Instance*>(p)->engine.reset(); };

    t.baked_params.push_back({kRateHz, static_cast<float>(kRateMinHz),
                              static_cast<float>(kRateMaxHz),
                              static_cast<float>(Engine::kRateDefaultHz)});
    t.baked_params.push_back({kDepth, 0.0f, 100.0f, 100.0f});
    t.baked_params.push_back(
        {kCenterHz, static_cast<float>(kCenterMinHz), static_cast<float>(kCenterMaxHz), 400.0f});
    t.baked_params.push_back({kFeedback, -static_cast<float>(Engine::kFeedbackMax),
                              static_cast<float>(Engine::kFeedbackMax),
                              static_cast<float>(Engine::kColorOffFeedback)});
    t.baked_params.push_back({kMix, 0.0f, 100.0f, static_cast<float>(Engine::kMixDefault * 100.0)});
    t.baked_params.push_back({kStereoSpread, 0.0f, 0.5f, 0.25f});
    t.baked_params.push_back({kStaggerRatio, static_cast<float>(Engine::kStaggerMin),
                              static_cast<float>(Engine::kStaggerMax),
                              static_cast<float>(Engine::kStaggerDefault)});
    t.baked_params.push_back({kWave, kWaveSine, kWaveSmoothRandom, kWaveTriangle});

    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& in, int n,
                                        const BakedParamView& params) {
        auto* s = static_cast<Instance*>(p);
        const float* in_left = in.channel_ptr(0);
        const float* in_right = in.channel_ptr(1);
        float* out_left = out.channel_ptr(0);
        float* out_right = out.channel_ptr(1);

        for (int k = 0; k < n; ++k) {
            const auto offset = static_cast<std::int32_t>(k);
            s->engine.set_rate_hz(params.value_at(kRateHz, offset));
            s->engine.set_depth(params.value_at(kDepth, offset) * 0.01f);
            s->engine.set_center_hz(params.value_at(kCenterHz, offset));
            s->engine.set_feedback(params.value_at(kFeedback, offset));
            s->engine.set_mix(params.value_at(kMix, offset) * 0.01f);
            s->engine.set_stereo_spread(params.value_at(kStereoSpread, offset));
            s->engine.set_stagger_ratio(params.value_at(kStaggerRatio, offset));
            s->engine.set_wave(wave_from_param(params.value_at(kWave, offset)));

            const float left = in_left[static_cast<std::size_t>(k)];
            const float right = in_right[static_cast<std::size_t>(k)];
            s->engine.process(&left, &right, &out_left[static_cast<std::size_t>(k)],
                              &out_right[static_cast<std::size_t>(k)], 1);
        }
    };
    return t;
}

} // namespace phaser

} // namespace pulp::host::modulation
