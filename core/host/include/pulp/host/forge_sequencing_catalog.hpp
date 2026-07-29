#pragma once

// Sequencing — bake-layer catalog nodes for the CV/trigger domain.
//
// The home for `modular_sequencing.hpp`: the pulse-count stage sequencer, the
// 2-D Cartesian walk, the Hordijk rungler, the scale quantizer, and the two
// gate combinators. These are CONTROL nodes — every port carries CV or a gate,
// not sound — so they belong upstream of the audio nodes, feeding a `vca`, a
// `filter_cv`, or an oscillator's pitch. The `lofi` catalog's `lfo` node
// established that an audio port may carry CV; this file is the rest of that
// layer.
//
// ── PORTS carry patch cables; PARAMS carry transport ──────────────────────
//
// The one rule that decides every port list here. An input port exists so
// ANOTHER NODE can drive it — a clock from a divider, a reset from a second
// sequencer's end-of-cycle, a CV to quantize, a gate to combine. Those are
// signals with a source inside the graph.
//
// `run` and `reset` are TRANSPORT, and in a baked artifact transport comes from
// the host: a `run` level is a sample-accurate automation lane, which is exactly
// what a baked param is, and a `reset` is a momentary action a user or an
// automation lane fires. So `run` is a param on every node here.
//
// `reset` is the deliberate exception: it is a param on the two pure PROCESSORS
// (`quantize_scale`, `prob_gate`), where it only clears a latch, and an input
// PORT on all three SEQUENCERS, because the classic patch — one sequencer's
// gate resetting another to build a polymeter — needs a cable, not an
// automation lane. That asymmetry is the topology talking, not an oversight.
//
// ── REALIZATION vs INJECTABLE PARAM, applied per node ─────────────────────
//
// The series rule is that anything moving `latency_samples()` must be frozen at
// registration. **That lever is inert in this module**: every block reports 0
// unconditionally, a slide is a continuous smoother rather than a delay, and no
// setting can change it. So the realizations here are forced by the other two
// reasons, and it is worth being explicit about which:
//
//   - **Seeds are realizations, always** (series law 2: "seeds never
//     automated/macro-exposed"). A seed is not a knob — it is which performance
//     you are playing. `stage_seq`'s random-direction seed, `rungler`'s seed
//     pattern and `prob_gate`'s seed are all construction arguments. NOTE that
//     the spec's own §10 tables list them as baked-param rows *annotated* "not
//     automatable", which is self-contradictory: a baked-param row IS an
//     automation lane. Resolved in favour of law 2.
//   - **Pattern data is a realization** — per-stage pitch/pulse/mode/slide/skip,
//     and the Cartesian grid. The spec says so directly ("array config, not
//     automatable params"), and the shape is right: a baked artifact plays a
//     composed pattern and automates how it is traversed.
//   - **A genuine topology change is a realization.** Two here:
//       * `rungler`'s REGISTER LENGTH. Not a stylistic call — the DSP reloads
//         the seed pattern when the length changes, because a shift register
//         whose length moves mid-orbit has no meaningful "same state" to keep.
//         As a param it would therefore re-pin the sequence every sample and
//         the node would emit one constant. `dac_bits` by contrast only
//         re-quantizes the same register, so it injects.
//       * `cartesian_walk`'s ACCESS MODE, split into two registered type ids
//         exactly as the diode-bridge member splits on detection topology. In
//         row-major an X wrap carries into Y and INPUT PORT 1 IS NOT READ AT
//         ALL. A node whose port silently stops being a port is a different
//         node, not a mode of one.
//   - **Everything else injects**, including the stepped switches: direction,
//     gate op, quantizer mode, grid dimensions, CV offsets. They are front-panel
//     selectors over an unchanged signal path with invariant latency.
//
// ── Levels ────────────────────────────────────────────────────────────────
//
// Gates in and out are 0/1, which sits inside the trigger kit's default
// hysteresis window (rising above 0.5, falling below 0.25) — so any gate output
// here drives any clock or reset input here with no level negotiation, which is
// what makes the chaining patches work. Pitch CV is volts on the 1 V/octave
// standard, so `stage_seq` → `quantize_scale` → an oscillator needs no
// conversion.

#include <pulp/host/signal_graph.hpp>

#include <pulp/signal/modular_sequencing.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace pulp::host::sequencing {

/// Level a gate/clock output is driven to. Chosen to sit above the trigger
/// kit's rising threshold (0.5) so any output here clocks any input here.
inline constexpr float kGateHigh = 1.0f;
inline constexpr float kGateLow = 0.0f;

/// Reads a 0/1 param as a boolean, at the kit's rising threshold so the
/// convention is the same one the ports use.
inline bool param_high(float v) { return v >= static_cast<float>(signal::kTriggerHighThreshold); }

// ══════════════════════════════════════════════════════════════════════════
//  Stage sequencer — the pulse-count (M-185 / Metropolis lineage) sequencer
// ══════════════════════════════════════════════════════════════════════════
namespace stage_seq {

inline constexpr const char* kTypeId = "sequencing.stage_seq";

// Node-local ids; the framework namespaces per node, so these restart at 1 in
// every namespace below without colliding.
inline constexpr state::ParamID kRun = 1;             // stepped 0/1 — transport level
inline constexpr state::ParamID kNumStages = 2;       // count
inline constexpr state::ParamID kDirection = 3;       // stepped 0 fwd/1 rev/2 ping/3 rand
inline constexpr state::ParamID kSlideMs = 4;         // ms
inline constexpr state::ParamID kRepeatDutyPct = 5;   // %

using Seq = signal::StageSeq;

/// One stage of the composed pattern — the realization payload.
struct Stage {
    float pitch_v = 0.0f;
    int pulse_count = 1;
    signal::StageGateMode gate_mode = signal::StageGateMode::single;
    bool slide = false;
    bool skip = false;
};

using Pattern = std::array<Stage, static_cast<std::size_t>(Seq::kMaxStages)>;

/// The default composed pattern: a rising major scale, one clock per stage,
/// one re-articulated gate per clock.
///
/// The major scale specifically, so the default `stage_seq` → `quantize_scale`
/// chain is already in key with that node's own default mask and a graph author
/// who places both and turns nothing gets something musical rather than
/// something silent.
///
/// `repeat` rather than `single` for the gate, which is the non-obvious part: at
/// a pulse count of 1 those two modes differ completely. `single` holds the gate
/// high for the whole of its one pulse, so consecutive stages produce ONE
/// unbroken gate and a downstream envelope never re-triggers — the sequence
/// plays but every note is tied. `repeat` re-pulses at `repeat_duty`, which is
/// what a step sequencer's gate output is for.
inline Pattern default_pattern() {
    Pattern p{};
    const int degrees[] = {0, 2, 4, 5, 7, 9, 11, 12};
    for (std::size_t i = 0; i < p.size(); ++i) {
        const int semitone = degrees[i % 8] + 12 * static_cast<int>(i / 8);
        p[i].pitch_v = static_cast<float>(semitone) /
                       static_cast<float>(signal::units::kSemitonesPerOctave);
        p[i].pulse_count = 1;
        p[i].gate_mode = signal::StageGateMode::repeat;
        p[i].slide = false;
        p[i].skip = false;
    }
    return p;
}

struct Instance {
    signal::StageSeq seq;
    signal::TransportEdge transport;
};

/// Worst-case linear gain for the Forge registry (series law 8).
///
/// There is no input-to-output signal path at all: the clock and reset ports are
/// consumed as EDGES and the outputs are synthesised from the composed pattern,
/// so no input amplitude can reach an output. The gate output is a boolean level
/// bounded by `kGateHigh`, and the pitch output is bounded by the largest pitch
/// in the baked pattern — a registration-time constant, reported here so the
/// registry states a number the artifact cannot exceed rather than a ratio that
/// does not apply.
inline float stage_seq_worst_case_gain() { return 1.0f; }

/// Largest absolute pitch, in volts, the node can emit for a given pattern.
/// The slide is a `SlewLimiterT` between two pattern pitches and cannot
/// overshoot either, so the pattern's extreme IS the bound.
inline float stage_seq_pitch_bound_v(const Pattern& pattern) {
    float m = 0.0f;
    for (const auto& s : pattern)
        if (std::isfinite(s.pitch_v)) m = std::max(m, std::fabs(s.pitch_v));
    return m;
}

/// `pattern` and `seed` are registration-time: the composed notes and which
/// random performance this artifact plays. Everything a user turns injects.
inline CustomNodeType make_stage_seq_node(Pattern pattern = default_pattern(),
                                          std::uint32_t seed = Seq::kRandomSeed) {
    CustomNodeType t;
    t.type_id = kTypeId;
    t.version = 1;
    t.num_input_ports = 2;   // 0 = clock, 1 = reset
    t.num_output_ports = 2;  // 0 = pitch CV (volts), 1 = gate
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
    t.baked_params.push_back({kRepeatDutyPct, 10.0f, 90.0f,
                              static_cast<float>(Seq::kRepeatDuty * 100.0)});

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

}  // namespace stage_seq

// ══════════════════════════════════════════════════════════════════════════
//  Cartesian walk — the 2-D (René lineage) sequencer
// ══════════════════════════════════════════════════════════════════════════
namespace cartesian {

/// Independent X and Y — the documented René behaviour, and the default.
inline constexpr const char* kTypeId = "sequencing.cartesian_walk";
/// Row-major — an X wrap carries into Y, and the Y clock port is not read.
inline constexpr const char* kRowMajorTypeId = "sequencing.cartesian_walk_row_major";

inline constexpr state::ParamID kRun = 1;       // stepped 0/1
inline constexpr state::ParamID kGridW = 2;     // cells
inline constexpr state::ParamID kGridH = 3;     // cells
inline constexpr state::ParamID kXOffset = 4;   // cells, added to the X counter
inline constexpr state::ParamID kYOffset = 5;   // cells, added to the Y counter

using Walk = signal::CartesianWalk;

using Grid = std::array<float, static_cast<std::size_t>(Walk::kMaxDim * Walk::kMaxDim)>;

/// The default grid: a rising chromatic ramp in volts, so an unconfigured node
/// walks an audible, obviously-ordered sequence rather than silence — which
/// makes "is this thing clocked at all" answerable by ear.
inline Grid default_grid() {
    Grid g{};
    for (std::size_t i = 0; i < g.size(); ++i)
        g[i] = static_cast<float>(i) / static_cast<float>(signal::units::kSemitonesPerOctave);
    return g;
}

struct Instance {
    signal::CartesianWalk walk;
    signal::TransportEdge transport;          // run + reset + the X clock
    signal::HystereticTriggerDetectT<float> y_detect;   // the second clock, same edge definition
};

/// Worst-case linear gain for the Forge registry (series law 8).
///
/// As with `stage_seq`: the clock and reset ports are consumed as edges, the CV
/// output comes from the baked grid, and the gate and end-of-cycle outputs are
/// boolean levels. No input amplitude reaches an output.
inline float cartesian_walk_worst_case_gain() { return 1.0f; }

/// Largest absolute CV, in volts, the node can emit for a given grid.
inline float cartesian_walk_cv_bound_v(const Grid& grid) {
    float m = 0.0f;
    for (float v : grid)
        if (std::isfinite(v)) m = std::max(m, std::fabs(v));
    return m;
}

/// `grid` is the composed pattern and `row_major` is the access topology — the
/// latter picks the registered type id, because it changes whether input port 1
/// is read at all.
inline CustomNodeType make_cartesian_walk_node(Grid grid = default_grid(),
                                               bool row_major = false) {
    CustomNodeType t;
    t.type_id = row_major ? kRowMajorTypeId : kTypeId;
    t.version = 1;
    t.num_input_ports = 3;   // 0 = X clock, 1 = Y clock, 2 = reset
    t.num_output_ports = 3;  // 0 = CV (volts), 1 = gate, 2 = end of cycle
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
                s->walk.set_value(x, y,
                                  grid[static_cast<std::size_t>(y * Walk::kMaxDim + x)]);
        s->walk.reset();
    };
    t.reset = [](void* p) {
        auto* s = static_cast<Instance*>(p);
        s->walk.reset();
        s->transport.reset();
        s->y_detect.reset();
    };

    t.baked_params.push_back({kRun, 0.0f, 1.0f, 1.0f});
    t.baked_params.push_back({kGridW, 1.0f, static_cast<float>(Walk::kMaxDim),
                              static_cast<float>(Walk::kDefaultDim)});
    t.baked_params.push_back({kGridH, 1.0f, static_cast<float>(Walk::kMaxDim),
                              static_cast<float>(Walk::kDefaultDim)});
    t.baked_params.push_back({kXOffset, -static_cast<float>(Walk::kMaxDim),
                              static_cast<float>(Walk::kMaxDim), 0.0f});
    t.baked_params.push_back({kYOffset, -static_cast<float>(Walk::kMaxDim),
                              static_cast<float>(Walk::kMaxDim), 0.0f});

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
            s->walk.set_offsets(
                static_cast<int>(std::lround(params.value_at(kXOffset, offset))),
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

}  // namespace cartesian

// ══════════════════════════════════════════════════════════════════════════
//  Rungler — the Hordijk shift-register / DAC chaos source
// ══════════════════════════════════════════════════════════════════════════
namespace rungler {

inline constexpr const char* kTypeId = "sequencing.rungler";

inline constexpr state::ParamID kRun = 1;            // stepped 0/1
inline constexpr state::ParamID kDacBits = 2;        // bits → 2^D levels
inline constexpr state::ParamID kFeedbackTap = 3;    // register index
inline constexpr state::ParamID kRangeV = 4;         // volts
inline constexpr state::ParamID kExternalData = 5;   // stepped 0/1
inline constexpr state::ParamID kDataIn = 6;         // stepped 0/1 — the steering bit

using Rung = signal::Rungler;

struct Instance {
    signal::Rungler rungler;
    signal::TransportEdge transport;
};

/// Worst-case linear gain for the Forge registry (series law 8).
///
/// This is the one block in the module with a feedback path, so law 8 bites
/// here — and the answer is a PROVEN bound rather than a measurement. The
/// feedback is a single-bit XOR into a shift register: there is no
/// gain-carrying analog nonlinearity, so no small-signal-gain compensation
/// applies. The output is a `D`-bit DAC code mapped affinely onto
/// `[−range_v, +range_v]`, so `|y| ≤ range_v` BY CONSTRUCTION for any clock or
/// data sequence whatsoever. The DSP suite asserts exactly that over 10⁶ clocks
/// of adversarial data (all-ones, alternating, xorshift) at three
/// configurations, and asserts the bound is TIGHT — both extremes are reached.
/// Reported as 1.0 because there is no input-to-output amplitude path at all;
/// the absolute output bound is `range_v`, whose param ceiling is below.
inline float rungler_worst_case_gain() { return 1.0f; }

/// The largest absolute output the node can emit, in volts: the ceiling of the
/// `range_v` param, because a baked param can be automated anywhere in range.
inline float rungler_output_bound_v() { return 5.0f; }

/// `reg_bits` and `seed_pattern` are registration-time.
///
/// `seed_pattern` because a seed is which performance you are playing (law 2).
/// `reg_bits` because the DSP reloads the seed when the register length
/// changes — a register whose length moves mid-orbit has no meaningful "same
/// state" — so as an injectable param it would re-pin the sequence on every
/// sample and the node would emit one frozen level instead of a line.
inline CustomNodeType make_rungler_node(int reg_bits = Rung::kDefaultBits,
                                        std::uint32_t seed_pattern = Rung::kSeedPattern) {
    const int normalized_reg_bits = std::clamp(reg_bits, Rung::kMinBits, Rung::kMaxBits);
    CustomNodeType t;
    t.type_id = kTypeId;
    t.version = 1;
    t.num_input_ports = 2;   // 0 = clock, 1 = reset
    t.num_output_ports = 2;  // 0 = CV (volts), 1 = serial bit as a gate
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
    t.baked_params.push_back({kFeedbackTap, 0.0f,
                              static_cast<float>(normalized_reg_bits - 2),
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

}  // namespace rungler

// ══════════════════════════════════════════════════════════════════════════
//  Quantizer — CV to scale, on the 1 V/octave standard
// ══════════════════════════════════════════════════════════════════════════
namespace quantize {

inline constexpr const char* kTypeId = "sequencing.quantize_scale";

inline constexpr state::ParamID kMode = 1;        // stepped 0 = EDO-N, 1 = scale mask
inline constexpr state::ParamID kEdoN = 2;        // steps per octave
inline constexpr state::ParamID kScaleMask = 3;   // 12-bit pitch-class mask
inline constexpr state::ParamID kRootPc = 4;      // pitch class
inline constexpr state::ParamID kHystCents = 5;   // cents
inline constexpr state::ParamID kReset = 6;       // stepped 0/1 — clears the latch

using Quant = signal::QuantizeScale;

struct Instance {
    signal::QuantizeScale quant;
    signal::HystereticTriggerDetectT<float> reset_detect;
};

/// Worst-case linear gain for the Forge registry (series law 8).
///
/// The only node here with a real input-to-output amplitude path, so the only
/// one whose number is a genuine ratio — and it is derived, not measured, at the
/// node's PARAMETER CEILINGS, because a baked param can be automated anywhere in
/// its declared range.
///
/// The memoryless answer is 2: `out = round(in·N)/N` is 0 below half a step, and
/// the worst ratio is at `|in| = 0.5/N`, where `(1/N)/(0.5/N) = 2`. **That is
/// not this node's bound**, because the block is not memoryless — the
/// hysteresis latch is its entire reason for existing. The latch holds a step
/// while the input stays within `0.5 + window` of it, so an output of one step
/// survives down to an input of `0.5 − window` steps, and the ratio is
/// `1/(0.5 − window)`. The window is capped at `kMaxHystSteps` (0.45), which the
/// 50-cent ceiling of `hyst_cents` reaches at every division from EDO-30 up. So
/// the bound is `1/(0.5 − 0.45) = 20`, and the suite constructs exactly that
/// operating point rather than assuming it is unreachable.
///
/// Scale-mask mode is NOT multiplicatively bounded at all: with a one-note mask
/// an input arbitrarily near 0 V is snapped up to six semitones away, so the
/// ratio is unbounded as the input approaches zero. Its invariant is ADDITIVE —
/// `|out| ≤ |in| + (0.5 + kMaxHystSteps + 6)/12 V` — and reporting that as a
/// gain would be a fabrication, so it is stated and asserted separately below.
inline float quantize_scale_worst_case_gain() {
    return static_cast<float>(1.0 / (0.5 - signal::QuantizeScale::kMaxHystSteps));
}

/// The additive ceiling of scale-mask mode, in volts: the rounding boundary
/// plus the widest hysteresis window plus the furthest a snap can travel (six
/// semitones, from a mask with a single bit set).
inline float quantize_scale_mask_offset_bound_v() {
    return static_cast<float>((0.5 + signal::QuantizeScale::kMaxHystSteps + 6.0) /
                              signal::units::kSemitonesPerOctave);
}

inline CustomNodeType make_quantize_scale_node() {
    CustomNodeType t;
    t.type_id = kTypeId;
    t.version = 1;
    t.num_input_ports = 1;   // 0 = CV in (volts)
    t.num_output_ports = 1;  // 0 = CV out (volts)
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
    t.baked_params.push_back({kEdoN, 1.0f, static_cast<float>(Quant::kMaxEdo),
                              static_cast<float>(Quant::kDefaultEdo)});
    t.baked_params.push_back({kScaleMask, 0.0f, 4095.0f,
                              static_cast<float>(Quant::kMajorMask)});
    t.baked_params.push_back({kRootPc, 0.0f, 11.0f, 0.0f});
    t.baked_params.push_back({kHystCents, 0.0f, 50.0f,
                              static_cast<float>(Quant::kHystCents)});
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
            s->quant.set_scale_mask(static_cast<std::uint16_t>(
                std::clamp(static_cast<int>(std::lround(params.value_at(kScaleMask, offset))), 0,
                           4095)));
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

}  // namespace quantize

// ══════════════════════════════════════════════════════════════════════════
//  Gate logic — the Boolean combinator
// ══════════════════════════════════════════════════════════════════════════
namespace gate_logic {

inline constexpr const char* kTypeId = "sequencing.gate_logic";

inline constexpr state::ParamID kOp = 1;  // stepped 0 AND/1 OR/2 XOR/3 NAND/4 NOR/5 XNOR

using Logic = signal::GateLogic;

struct Instance {
    signal::GateLogic logic;
};

/// Worst-case linear gain for the Forge registry (series law 8).
///
/// No feedback path, and no amplitude path either: both outputs are boolean
/// levels of exactly `kGateHigh` or `kGateLow` regardless of how large the
/// inputs are. A 100 V input produces a 1 V gate.
inline float gate_logic_worst_case_gain() { return 1.0f; }

/// Two outputs, because these operations come in complementary pairs and the
/// complement is free: with `op` set to AND, output 1 IS the NAND of the same
/// inputs. A patch that needs both — the usual case when building a rhythm from
/// coincidences and anti-coincidences — would otherwise place a second node and
/// keep its `op` param in sync with this one by hand.
inline CustomNodeType make_gate_logic_node() {
    CustomNodeType t;
    t.type_id = kTypeId;
    t.version = 1;
    t.num_input_ports = 2;   // 0 = A, 1 = B
    t.num_output_ports = 2;  // 0 = result, 1 = complement
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
            s->logic.set_op(static_cast<signal::GateOp>(std::clamp(
                static_cast<int>(std::lround(params.value_at(kOp, offset))), 0, 5)));
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

}  // namespace gate_logic

// ══════════════════════════════════════════════════════════════════════════
//  Probability gate — deterministic chance
// ══════════════════════════════════════════════════════════════════════════
namespace prob_gate {

inline constexpr const char* kTypeId = "sequencing.prob_gate";

inline constexpr state::ParamID kProbabilityPct = 1;  // %
inline constexpr state::ParamID kReset = 2;           // stepped 0/1 — clears the latch

using Prob = signal::ProbGate;

struct Instance {
    signal::ProbGate gate;
    signal::HystereticTriggerDetectT<float> reset_detect;
};

/// Worst-case linear gain for the Forge registry (series law 8).
///
/// No feedback path. The output is a boolean level: a trigger is passed at
/// `kGateHigh` or blocked at `kGateLow`, whatever the input amplitude was.
inline float prob_gate_worst_case_gain() { return 1.0f; }

/// `seed` is registration-time (law 2): changing the seed is how you roll
/// again, and that is a composition decision rather than an automation lane. A
/// baked artifact therefore plays the SAME probabilistic performance on every
/// render, which is the whole point of a seeded chance gate.
inline CustomNodeType make_prob_gate_node(std::uint32_t seed = Prob::kProbSeed) {
    CustomNodeType t;
    t.type_id = kTypeId;
    t.version = 1;
    t.num_input_ports = 1;   // 0 = trigger in
    t.num_output_ports = 1;  // 0 = trigger out
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

    t.baked_params.push_back({kProbabilityPct, 0.0f, 100.0f,
                              static_cast<float>(Prob::kDefaultProbability * 100.0)});
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

}  // namespace prob_gate

}  // namespace pulp::host::sequencing

#include <pulp/host/detail/forge_sequencing_descriptors.hpp>
