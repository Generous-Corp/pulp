// Forge stage-sequencer catalog factory implementation.

#include <pulp/host/forge_sequencing_catalog.hpp>

namespace pulp::host::sequencing::stage_seq {

float stage_seq_worst_case_gain() {
    return 1.0f;
}

float stage_seq_pitch_bound_v(const Pattern& pattern) {
    float m = 0.0f;
    for (const auto& s : pattern)
        if (std::isfinite(s.pitch_v))
            m = std::max(m, std::fabs(s.pitch_v));
    return m;
}

CustomNodeType make_stage_seq_node(Pattern pattern, std::uint32_t seed) {
    CustomNodeType t;
    t.type_id = kTypeId;
    t.version = 1;
    t.num_input_ports = 2;  // 0 = clock, 1 = reset
    t.num_output_ports = 2; // 0 = pitch CV (volts), 1 = gate
    t.default_name = "Stage Sequencer";
    t.lowerable = true;

    t.create = []() -> void* { return new Instance{}; };
    t.destroy = [](void* p) { delete static_cast<Instance*>(p); };
    t.prepare = [pattern, seed](void* p, double sr, int /*max_block*/) {
        auto* s = static_cast<Instance*>(p);
        s->seq.prepare(sr);
        s->transport.prepare(sr);
        s->seq.set_seed(seed);
        for (std::size_t i = 0; i < pattern.size(); ++i) {
            const int idx = static_cast<int>(i);
            s->seq.set_stage_pitch(idx, pattern[i].pitch_v);
            s->seq.set_stage_pulse_count(idx, pattern[i].pulse_count);
            s->seq.set_stage_gate_mode(idx, pattern[i].gate_mode);
            s->seq.set_stage_slide(idx, pattern[i].slide);
            s->seq.set_stage_skip(idx, pattern[i].skip);
        }
        s->seq.reset();
    };
    t.reset = [](void* p) {
        auto* s = static_cast<Instance*>(p);
        s->seq.reset();
        s->transport.reset();
    };

    t.baked_params.push_back({kRun, 0.0f, 1.0f, 1.0f});
    t.baked_params.push_back({kNumStages, 1.0f, static_cast<float>(Seq::kMaxStages), 8.0f});
    t.baked_params.push_back({kDirection, 0.0f, 3.0f, 0.0f});
    t.baked_params.push_back({kSlideMs, 1.0f, 500.0f, static_cast<float>(Seq::kSlideMs)});
    t.baked_params.push_back(
        {kRepeatDutyPct, 10.0f, 90.0f, static_cast<float>(Seq::kRepeatDuty * 100.0)});

    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& in, int n,
                                        const BakedParamView& params) {
        auto* s = static_cast<Instance*>(p);
        const float* clock = in.channel_ptr(0);
        const float* reset_in = in.channel_ptr(1);
        float* pitch_out = out.channel_ptr(0);
        float* gate_out = out.channel_ptr(1);

        for (int k = 0; k < n; ++k) {
            const auto offset = static_cast<std::int32_t>(k);
            const auto idx = static_cast<std::size_t>(k);

            s->seq.set_num_stages(
                static_cast<int>(std::lround(params.value_at(kNumStages, offset))));
            s->seq.set_direction(static_cast<signal::SeqDirection>(std::clamp(
                static_cast<int>(std::lround(params.value_at(kDirection, offset))), 0, 3)));
            s->seq.set_slide_ms(params.value_at(kSlideMs, offset));
            s->seq.set_repeat_duty(params.value_at(kRepeatDutyPct, offset) / 100.0);

            // The kit's edge detector is the ONLY definition of an edge in the
            // library, so the node boundary uses it rather than thresholding by
            // hand — that is what `TransportEdgeT` is for. `run` arrives as a
            // param level and goes in where a run cable would.
            const float run_level = params.value_at(kRun, offset);
            const auto frame = s->transport.process(run_level, reset_in[idx], clock[idx]);

            const auto f = s->seq.process(frame.run_high, frame.reset_edge, frame.clock_edge);
            pitch_out[idx] = f.pitch_v;
            gate_out[idx] = f.gate ? kGateHigh : kGateLow;
        }
    };
    return t;
}

} // namespace pulp::host::sequencing::stage_seq
