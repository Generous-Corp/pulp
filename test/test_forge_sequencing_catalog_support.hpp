#pragma once

// Catalog nodes for the CV/trigger sequencing module — stage sequencer,
// Cartesian walk, rungler, quantizer, gate logic, probability gate.
//
// THE BAR every case here is held to: a baked param has to be shown MOVING THE
// BAKED NODE'S OUTPUT over the real production path — bake →
// `claim_param_injection` → `ParamInjector` → routed executor. A node that
// merely instantiates is not a node that works, and a param declared in the
// table but never read by `process_instance_baked_param` looks exactly like a
// working one until someone automates it. Every one of the 25 declared params
// across the six nodes is exercised below, and the assertions are DIRECTIONAL
// (this knob makes THIS happen), not "the output changed".
//
// These are CONTROL nodes, so "audio" here means the control signal: pitch CV
// in volts on the 1 V/octave standard, and gates at 0/1.
//
// ── What is measured, and where the clock comes from ──────────────────────
//
// Every input port carries a signal the node edge-detects or reads as a level.
// A "clock" is a one-sample 1.0 pulse every `period` samples — a real signal
// through the real port, not a boolean handed to the DSP directly, so these
// cases exercise the node's `TransportEdgeT` boundary as well as the DSP.
//
// Outputs are sampled AT the clock sample, because that is where a sequencer's
// contract lives ("the first clock after a reset lands on stage 0"). Gate-duty
// assertions instead count samples BETWEEN clocks, which is the only place a
// duty can live.
//
// ── Two things worth knowing before reading the assertions ───────────────
//
// **The first clock of a `repeat` stage has no measured duty.** A duty needs a
// period, and the DSP is forbidden from generating its own clock, so until one
// period has elapsed the gate stays high for the whole pulse. Duty cases
// therefore pre-roll one clock before measuring — this is the DSP module's
// documented D5, surfacing here rather than being worked around.
//
// **`TransportEdgeT` debounces the reset input** with a 0.5 ms refractory
// window (24 samples at 48 kHz), so reset pulses in these cases are spaced well
// past it. The clock is deliberately NOT debounced — a debounced clock drops
// fast subdivisions — and a case below asserts that difference.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/baked_node_fixture.hpp"
#include "harness/rt_allocation_probe.hpp"

#include <pulp/host/forge_sequencing_catalog.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <vector>

using namespace pulp::host;
namespace seqcat = pulp::host::sequencing;
namespace sig = pulp::signal;
using Catch::Matchers::WithinAbs;
using pulp::test::immediate;

namespace {

constexpr double kSr = 48000.0;
constexpr int kFrames = 512;

/// A one-sample clock pulse every `period` samples starting at `phase`.
std::vector<float> clock_line(int period, int phase = 0, int frames = kFrames) {
    std::vector<float> v(static_cast<std::size_t>(frames), 0.0f);
    for (int k = phase; k < frames; k += period) v[static_cast<std::size_t>(k)] = 1.0f;
    return v;
}

std::vector<float> flat(float level, int frames = kFrames) {
    return std::vector<float>(static_cast<std::size_t>(frames), level);
}

/// Sample indices where a clock line pulses.
std::vector<int> clock_indices(int period, int phase = 0, int frames = kFrames) {
    std::vector<int> v;
    for (int k = phase; k < frames; k += period) v.push_back(k);
    return v;
}

/// The values of `channel` sampled at each clock instant.
std::vector<float> at_clocks(const std::vector<float>& channel, int period, int phase = 0) {
    std::vector<float> v;
    for (int k : clock_indices(period, phase, static_cast<int>(channel.size())))
        v.push_back(channel[static_cast<std::size_t>(k)]);
    return v;
}

bool high(float v) { return v >= 0.5f; }

/// Distinct values in a channel, to a tolerance — how many DAC levels a rungler
/// actually visited, for instance.
std::size_t distinct_levels(const std::vector<float>& x, float quantum = 1e-4f) {
    std::set<long> s;
    for (float v : x) s.insert(std::lround(static_cast<double>(v / quantum)));
    return s.size();
}

float peak_abs(const std::vector<float>& x) {
    float m = 0.0f;
    for (float v : x) m = std::max(m, std::fabs(v));
    return m;
}

void require_finite(const std::vector<float>& x) {
    for (float v : x) REQUIRE(std::isfinite(v));
}

template <typename Fn>
void require_allocates_no_memory(Fn&& fn) {
    pulp::test::RtAllocationProbe probe;
    fn();
    REQUIRE(probe.allocation_count() == 0);
}

/// Re-initialises a baked artifact the way a host does on transport-stop-to-top
/// or a sample-rate change: `prepare()` re-runs every custom node's own
/// prepare+reset and returns injected params to their DECLARED DEFAULTS.
///
/// There is no `reset()` on `BakedGraphProcessor` — this is the production-path
/// re-init, and the fact that it also rewinds params is why every call site
/// below re-injects its operating point straight afterwards. That is not
/// ceremony: a determinism case that skipped it would be comparing two renders
/// at two different parameter settings.
template <int N>
void reinit(pulp::test::BakedNodeFixture<N>& fx) {
    pulp::format::PrepareContext pc;
    pc.sample_rate = fx.sample_rate();
    pc.max_buffer_size = fx.frames();
    pc.input_channels = N;
    pc.output_channels = N;
    fx.baked().prepare(pc);
}

}  // namespace

// ══════════════════════════════════════════════════════════════════════════
//  Stage sequencer
// ══════════════════════════════════════════════════════════════════════════

namespace {

using StageFixture = pulp::test::BakedNodeFixture<2>;

/// A pattern with a known, distinguishable pitch per stage, so a walk order is
/// readable straight off the CV output.
seqcat::stage_seq::Pattern ladder_pattern(sig::StageGateMode mode =
                                              sig::StageGateMode::repeat,
                                          int pulse_count = 1) {
    seqcat::stage_seq::Pattern p{};
    for (std::size_t i = 0; i < p.size(); ++i) {
        p[i].pitch_v = static_cast<float>(i);  // 0 V, 1 V, 2 V … one per stage
        p[i].pulse_count = pulse_count;
        p[i].gate_mode = mode;
        p[i].slide = false;
        p[i].skip = false;
    }
    return p;
}

void stage_baseline(ParamInjector& inj) {
    REQUIRE(inj.inject(immediate(seqcat::stage_seq::kRun, 1.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(seqcat::stage_seq::kNumStages, 8.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(seqcat::stage_seq::kDirection, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(seqcat::stage_seq::kSlideMs, 30.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(seqcat::stage_seq::kRepeatDutyPct, 50.0f)) == InjectStatus::Ok);
}

}  // namespace











// ══════════════════════════════════════════════════════════════════════════
//  Cartesian walk
// ══════════════════════════════════════════════════════════════════════════

namespace {

using CartFixture = pulp::test::BakedNodeFixture<3>;

/// A grid whose cell value IS its linear index, so a walk order reads straight
/// off the CV output: value 6 means the walk is at (x=2, y=0) on an 8-wide grid.
seqcat::cartesian::Grid index_grid() {
    seqcat::cartesian::Grid g{};
    for (std::size_t i = 0; i < g.size(); ++i) g[i] = static_cast<float>(i);
    return g;
}

void cart_baseline(ParamInjector& inj) {
    REQUIRE(inj.inject(immediate(seqcat::cartesian::kRun, 1.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(seqcat::cartesian::kGridW, 4.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(seqcat::cartesian::kGridH, 4.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(seqcat::cartesian::kXOffset, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(seqcat::cartesian::kYOffset, 0.0f)) == InjectStatus::Ok);
}

/// The grid is stored at the DSP's full 8-wide stride, so cell (x, y) is index
/// `y·8 + x` — the number the index grid emits.
int cell_index(int x, int y) { return y * seqcat::cartesian::Walk::kMaxDim + x; }

}  // namespace









// ══════════════════════════════════════════════════════════════════════════
//  Rungler
// ══════════════════════════════════════════════════════════════════════════

namespace {

using RunglerFixture = pulp::test::BakedNodeFixture<2>;

void rungler_baseline(ParamInjector& inj) {
    REQUIRE(inj.inject(immediate(seqcat::rungler::kRun, 1.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(seqcat::rungler::kDacBits, 3.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(seqcat::rungler::kFeedbackTap, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(seqcat::rungler::kRangeV, 2.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(seqcat::rungler::kExternalData, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(seqcat::rungler::kDataIn, 0.0f)) == InjectStatus::Ok);
}

}  // namespace











// ══════════════════════════════════════════════════════════════════════════
//  Quantizer
// ══════════════════════════════════════════════════════════════════════════

namespace {

using QuantFixture = pulp::test::BakedNodeFixture<1>;

void quant_baseline(ParamInjector& inj) {
    REQUIRE(inj.inject(immediate(seqcat::quantize::kMode, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(seqcat::quantize::kEdoN, 12.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(seqcat::quantize::kScaleMask, 2741.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(seqcat::quantize::kRootPc, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(seqcat::quantize::kHystCents, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(seqcat::quantize::kReset, 0.0f)) == InjectStatus::Ok);
}

}  // namespace









// ══════════════════════════════════════════════════════════════════════════
//  Gate logic
// ══════════════════════════════════════════════════════════════════════════



// ══════════════════════════════════════════════════════════════════════════
//  Probability gate
// ══════════════════════════════════════════════════════════════════════════

namespace {

using ProbFixture = pulp::test::BakedNodeFixture<1>;

int count_passes(const std::vector<float>& out) {
    int n = 0;
    for (float v : out)
        if (high(v)) ++n;
    return n;
}

}  // namespace



// ══════════════════════════════════════════════════════════════════════════
//  Cross-cutting
// ══════════════════════════════════════════════════════════════════════════
