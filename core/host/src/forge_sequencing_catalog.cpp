// Forge sequencing catalog factory implementations.

#include <pulp/host/forge_sequencing_catalog.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

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

namespace pulp::host::sequencing::cartesian {

float cartesian_walk_worst_case_gain() {
    return 1.0f;
}

} // namespace pulp::host::sequencing::cartesian

namespace pulp::host::sequencing::cartesian {

float cartesian_walk_cv_bound_v(const Grid& grid) {
    float m = 0.0f;
    for (float v : grid)
        if (std::isfinite(v))
            m = std::max(m, std::fabs(v));
    return m;
}

} // namespace pulp::host::sequencing::cartesian

namespace pulp::host::sequencing::cartesian {

CustomNodeType make_cartesian_walk_node(Grid grid, bool row_major) {
    CustomNodeType t;
    t.type_id = row_major ? kRowMajorTypeId : kTypeId;
    t.version = 1;
    t.num_input_ports = 3;  // 0 = X clock, 1 = Y clock, 2 = reset
    t.num_output_ports = 3; // 0 = CV (volts), 1 = gate, 2 = end of cycle
    t.default_name = row_major ? "Cartesian Walk (Row-Major)" : "Cartesian Walk";
    t.lowerable = true;

    t.create = []() -> void* { return new Instance{}; };
    t.destroy = [](void* p) { delete static_cast<Instance*>(p); };
    t.prepare = [grid, row_major](void* p, double sr, int /*max_block*/) {
        auto* s = static_cast<Instance*>(p);
        s->walk.prepare(sr);
        s->transport.prepare(sr);
        s->walk.set_access(row_major ? signal::CartesianAccess::row_major
                                     : signal::CartesianAccess::independent);
        for (int y = 0; y < Walk::kMaxDim; ++y)
            for (int x = 0; x < Walk::kMaxDim; ++x)
                s->walk.set_value(x, y, grid[static_cast<std::size_t>(y * Walk::kMaxDim + x)]);
        s->walk.reset();
    };
    t.reset = [](void* p) {
        auto* s = static_cast<Instance*>(p);
        s->walk.reset();
        s->transport.reset();
        s->y_detect.reset();
    };

    t.baked_params.push_back({kRun, 0.0f, 1.0f, 1.0f});
    t.baked_params.push_back(
        {kGridW, 1.0f, static_cast<float>(Walk::kMaxDim), static_cast<float>(Walk::kDefaultDim)});
    t.baked_params.push_back(
        {kGridH, 1.0f, static_cast<float>(Walk::kMaxDim), static_cast<float>(Walk::kDefaultDim)});
    t.baked_params.push_back(
        {kXOffset, -static_cast<float>(Walk::kMaxDim), static_cast<float>(Walk::kMaxDim), 0.0f});
    t.baked_params.push_back(
        {kYOffset, -static_cast<float>(Walk::kMaxDim), static_cast<float>(Walk::kMaxDim), 0.0f});

    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& in, int n,
                                        const BakedParamView& params) {
        auto* s = static_cast<Instance*>(p);
        const float* x_clock = in.channel_ptr(0);
        const float* y_clock = in.channel_ptr(1);
        const float* reset_in = in.channel_ptr(2);
        float* cv_out = out.channel_ptr(0);
        float* gate_out = out.channel_ptr(1);
        float* eoc_out = out.channel_ptr(2);

        for (int k = 0; k < n; ++k) {
            const auto offset = static_cast<std::int32_t>(k);
            const auto idx = static_cast<std::size_t>(k);

            s->walk.set_size(static_cast<int>(std::lround(params.value_at(kGridW, offset))),
                             static_cast<int>(std::lround(params.value_at(kGridH, offset))));
            s->walk.set_offsets(static_cast<int>(std::lround(params.value_at(kXOffset, offset))),
                                static_cast<int>(std::lround(params.value_at(kYOffset, offset))));

            const float run_level = params.value_at(kRun, offset);
            const auto frame = s->transport.process(run_level, reset_in[idx], x_clock[idx]);
            const bool y_edge = s->y_detect.process(y_clock[idx]);

            const auto f =
                s->walk.process(frame.run_high, frame.reset_edge, frame.clock_edge, y_edge);
            cv_out[idx] = f.cv;
            gate_out[idx] = f.gate ? kGateHigh : kGateLow;

            // End of cycle: the COUNTERS are home. Read from the counters rather
            // than the offset cell, so a CV offset shifts which notes play
            // without moving where the cycle boundary is — which is the whole
            // point of being able to chain one of these into another's reset.
            const bool home = f.gate && s->walk.x() == 0 && s->walk.y() == 0;
            eoc_out[idx] = home ? kGateHigh : kGateLow;
        }
    };
    return t;
}

} // namespace pulp::host::sequencing::cartesian

namespace pulp::host::sequencing::rungler {

float rungler_worst_case_gain() {
    return 1.0f;
}

} // namespace pulp::host::sequencing::rungler

namespace pulp::host::sequencing::rungler {

float rungler_output_bound_v() {
    return 5.0f;
}

} // namespace pulp::host::sequencing::rungler

namespace pulp::host::sequencing::rungler {

CustomNodeType make_rungler_node(int reg_bits, std::uint32_t seed_pattern) {
    const int normalized_reg_bits = std::clamp(reg_bits, Rung::kMinBits, Rung::kMaxBits);
    CustomNodeType t;
    t.type_id = kTypeId;
    t.version = 1;
    t.num_input_ports = 2;  // 0 = clock, 1 = reset
    t.num_output_ports = 2; // 0 = CV (volts), 1 = serial bit as a gate
    t.default_name = "Rungler";
    t.lowerable = true;

    t.create = []() -> void* { return new Instance{}; };
    t.destroy = [](void* p) { delete static_cast<Instance*>(p); };
    t.prepare = [normalized_reg_bits, seed_pattern](void* p, double sr, int /*max_block*/) {
        auto* s = static_cast<Instance*>(p);
        s->rungler.prepare(sr);
        s->transport.prepare(sr);
        s->rungler.set_reg_bits(normalized_reg_bits);
        s->rungler.set_seed_pattern(seed_pattern);
    };
    t.reset = [](void* p) {
        auto* s = static_cast<Instance*>(p);
        s->rungler.reset();
        s->transport.reset();
    };

    t.baked_params.push_back({kRun, 0.0f, 1.0f, 1.0f});
    t.baked_params.push_back({kDacBits, static_cast<float>(Rung::kMinDacBits),
                              static_cast<float>(Rung::kMaxDacBits),
                              static_cast<float>(Rung::kDefaultDacBits)});
    t.baked_params.push_back({kFeedbackTap, 0.0f, static_cast<float>(normalized_reg_bits - 2),
                              static_cast<float>(Rung::kFeedbackTap)});
    t.baked_params.push_back({kRangeV, 0.5f, 5.0f, static_cast<float>(Rung::kRangeV)});
    t.baked_params.push_back({kExternalData, 0.0f, 1.0f, 0.0f});
    t.baked_params.push_back({kDataIn, 0.0f, 1.0f, 0.0f});

    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& in, int n,
                                        const BakedParamView& params) {
        auto* s = static_cast<Instance*>(p);
        const float* clock = in.channel_ptr(0);
        const float* reset_in = in.channel_ptr(1);
        float* cv_out = out.channel_ptr(0);
        float* bit_out = out.channel_ptr(1);

        for (int k = 0; k < n; ++k) {
            const auto offset = static_cast<std::int32_t>(k);
            const auto idx = static_cast<std::size_t>(k);

            s->rungler.set_dac_bits(
                static_cast<int>(std::lround(params.value_at(kDacBits, offset))));
            s->rungler.set_feedback_tap(
                static_cast<int>(std::lround(params.value_at(kFeedbackTap, offset))));
            s->rungler.set_range_v(params.value_at(kRangeV, offset));
            s->rungler.set_external_data(param_high(params.value_at(kExternalData, offset)));

            const float run_level = params.value_at(kRun, offset);
            const auto frame = s->transport.process(run_level, reset_in[idx], clock[idx]);
            const bool data = param_high(params.value_at(kDataIn, offset));

            cv_out[idx] =
                s->rungler.process(frame.run_high, frame.reset_edge, frame.clock_edge, data);
            // The register's newest serial bit, as a gate. This is a tap of
            // existing state (`register_bits()`), not new DSP: in the Blippoo
            // the register's bits ARE signals, and exposing bit 0 lets a rungler
            // clock or gate something else with the same chaos that is steering
            // its own CV.
            bit_out[idx] = (s->rungler.register_bits() & 1u) != 0u ? kGateHigh : kGateLow;
        }
    };
    return t;
}

} // namespace pulp::host::sequencing::rungler

namespace pulp::host::sequencing::quantize {

float quantize_scale_worst_case_gain() {
    return static_cast<float>(1.0 / (0.5 - signal::QuantizeScale::kMaxHystSteps));
}

} // namespace pulp::host::sequencing::quantize

namespace pulp::host::sequencing::quantize {

float quantize_scale_mask_offset_bound_v() {
    return static_cast<float>((0.5 + signal::QuantizeScale::kMaxHystSteps + 6.0) /
                              signal::units::kSemitonesPerOctave);
}

} // namespace pulp::host::sequencing::quantize

namespace pulp::host::sequencing::quantize {

CustomNodeType make_quantize_scale_node() {
    CustomNodeType t;
    t.type_id = kTypeId;
    t.version = 1;
    t.num_input_ports = 1;  // 0 = CV in (volts)
    t.num_output_ports = 1; // 0 = CV out (volts)
    t.default_name = "Quantizer";
    t.lowerable = true;

    t.create = []() -> void* { return new Instance{}; };
    t.destroy = [](void* p) { delete static_cast<Instance*>(p); };
    t.prepare = [](void* p, double sr, int /*max_block*/) {
        static_cast<Instance*>(p)->quant.prepare(sr);
    };
    t.reset = [](void* p) {
        auto* s = static_cast<Instance*>(p);
        s->quant.reset();
        s->reset_detect.reset();
    };

    t.baked_params.push_back({kMode, 0.0f, 1.0f, 1.0f});
    t.baked_params.push_back(
        {kEdoN, 1.0f, static_cast<float>(Quant::kMaxEdo), static_cast<float>(Quant::kDefaultEdo)});
    t.baked_params.push_back({kScaleMask, 0.0f, 4095.0f, static_cast<float>(Quant::kMajorMask)});
    t.baked_params.push_back({kRootPc, 0.0f, 11.0f, 0.0f});
    t.baked_params.push_back({kHystCents, 0.0f, 50.0f, static_cast<float>(Quant::kHystCents)});
    t.baked_params.push_back({kReset, 0.0f, 1.0f, 0.0f});

    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& in, int n,
                                        const BakedParamView& params) {
        auto* s = static_cast<Instance*>(p);
        const float* cv_in = in.channel_ptr(0);
        float* cv_out = out.channel_ptr(0);

        for (int k = 0; k < n; ++k) {
            const auto offset = static_cast<std::int32_t>(k);
            const auto idx = static_cast<std::size_t>(k);

            s->quant.set_mode(param_high(params.value_at(kMode, offset))
                                  ? signal::QuantizeMode::scale_mask
                                  : signal::QuantizeMode::edo);
            s->quant.set_edo(static_cast<int>(std::lround(params.value_at(kEdoN, offset))));
            s->quant.set_scale_mask(static_cast<std::uint16_t>(std::clamp(
                static_cast<int>(std::lround(params.value_at(kScaleMask, offset))), 0, 4095)));
            s->quant.set_root_pc(static_cast<int>(std::lround(params.value_at(kRootPc, offset))));
            s->quant.set_hysteresis_cents(params.value_at(kHystCents, offset));

            // The reset param is a momentary ACTION, so it is edge-detected with
            // the kit's detector rather than read as a level — holding it high
            // would otherwise clear the latch on every sample and disable the
            // hysteresis entirely.
            if (s->reset_detect.process(params.value_at(kReset, offset)))
                s->quant.apply_reset_edge();

            cv_out[idx] = s->quant.process(cv_in[idx]);
        }
    };
    return t;
}

} // namespace pulp::host::sequencing::quantize

namespace pulp::host::sequencing::gate_logic {

float gate_logic_worst_case_gain() {
    return 1.0f;
}

} // namespace pulp::host::sequencing::gate_logic

namespace pulp::host::sequencing::gate_logic {

CustomNodeType make_gate_logic_node() {
    CustomNodeType t;
    t.type_id = kTypeId;
    t.version = 1;
    t.num_input_ports = 2;  // 0 = A, 1 = B
    t.num_output_ports = 2; // 0 = result, 1 = complement
    t.default_name = "Gate Logic";
    t.lowerable = true;

    t.create = []() -> void* { return new Instance{}; };
    t.destroy = [](void* p) { delete static_cast<Instance*>(p); };
    t.prepare = [](void* p, double sr, int /*max_block*/) {
        static_cast<Instance*>(p)->logic.prepare(sr);
    };
    t.reset = [](void* p) { static_cast<Instance*>(p)->logic.reset(); };

    t.baked_params.push_back({kOp, 0.0f, 5.0f, 0.0f});

    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& in, int n,
                                        const BakedParamView& params) {
        auto* s = static_cast<Instance*>(p);
        const float* a = in.channel_ptr(0);
        const float* b = in.channel_ptr(1);
        float* q = out.channel_ptr(0);
        float* nq = out.channel_ptr(1);

        for (int k = 0; k < n; ++k) {
            const auto offset = static_cast<std::int32_t>(k);
            const auto idx = static_cast<std::size_t>(k);
            s->logic.set_op(static_cast<signal::GateOp>(
                std::clamp(static_cast<int>(std::lround(params.value_at(kOp, offset))), 0, 5)));
            // Combinational, so the inputs are thresholded rather than
            // edge-detected: hysteresis is memory, and memory here would make
            // the answer depend on the order the two inputs happened to move.
            const bool result = s->logic.process(param_high(a[idx]), param_high(b[idx]));
            q[idx] = result ? kGateHigh : kGateLow;
            nq[idx] = result ? kGateLow : kGateHigh;
        }
    };
    return t;
}

} // namespace pulp::host::sequencing::gate_logic

namespace pulp::host::sequencing::prob_gate {

float prob_gate_worst_case_gain() {
    return 1.0f;
}

} // namespace pulp::host::sequencing::prob_gate

namespace pulp::host::sequencing::prob_gate {

CustomNodeType make_prob_gate_node(std::uint32_t seed) {
    CustomNodeType t;
    t.type_id = kTypeId;
    t.version = 1;
    t.num_input_ports = 1;  // 0 = trigger in
    t.num_output_ports = 1; // 0 = trigger out
    t.default_name = "Probability Gate";
    t.lowerable = true;

    t.create = []() -> void* { return new Instance{}; };
    t.destroy = [](void* p) { delete static_cast<Instance*>(p); };
    t.prepare = [seed](void* p, double sr, int /*max_block*/) {
        auto* s = static_cast<Instance*>(p);
        s->gate.prepare(sr);
        s->gate.set_seed(seed);
        s->gate.reset();
    };
    t.reset = [](void* p) {
        auto* s = static_cast<Instance*>(p);
        s->gate.reset();
        s->reset_detect.reset();
    };

    t.baked_params.push_back(
        {kProbabilityPct, 0.0f, 100.0f, static_cast<float>(Prob::kDefaultProbability * 100.0)});
    t.baked_params.push_back({kReset, 0.0f, 1.0f, 0.0f});

    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& in, int n,
                                        const BakedParamView& params) {
        auto* s = static_cast<Instance*>(p);
        const float* trig_in = in.channel_ptr(0);
        float* trig_out = out.channel_ptr(0);

        for (int k = 0; k < n; ++k) {
            const auto offset = static_cast<std::int32_t>(k);
            const auto idx = static_cast<std::size_t>(k);
            s->gate.set_probability(params.value_at(kProbabilityPct, offset) / 100.0);

            // Momentary action, edge-detected — and note it does NOT rewind the
            // RNG stream. A live reset that rewound randomness would make every
            // reset sound identical; only `reset()` reseeds (series law 2).
            if (s->reset_detect.process(params.value_at(kReset, offset)))
                s->gate.apply_reset_edge();

            trig_out[idx] = s->gate.process(trig_in[idx]) ? kGateHigh : kGateLow;
        }
    };
    return t;
}

} // namespace pulp::host::sequencing::prob_gate
