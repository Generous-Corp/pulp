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

TEST_CASE("Forge sequencing stage_seq: the node bakes with two CV ports each way",
          "[host][baked][forge][forge-sequencing][stageseq]") {
    const auto type = seqcat::stage_seq::make_stage_seq_node();
    REQUIRE(type.type_id == std::string("sequencing.stage_seq"));
    REQUIRE(type.lowerable);
    REQUIRE(type.num_input_ports == 2);   // clock, reset
    REQUIRE(type.num_output_ports == 2);  // pitch CV, gate
    REQUIRE(type.baked_params.size() == 5);

    StageFixture fx(type, kSr, kFrames);
    auto inj = fx.claim_injector();
    REQUIRE(inj.valid());
    stage_baseline(inj);

    const auto out = fx.render({clock_line(64), flat(0.0f)});
    require_finite(out[0]);
    require_finite(out[1]);

    // The default pattern is a rising major scale; the walk visits it in order.
    const auto pitches = at_clocks(out[0], 64);
    REQUIRE(pitches.size() == 8);
    for (std::size_t i = 1; i < pitches.size(); ++i) REQUIRE(pitches[i] > pitches[i - 1]);
    REQUIRE_THAT(pitches[0], WithinAbs(0.0, 1e-6));
    REQUIRE_THAT(pitches[7], WithinAbs(12.0 / 12.0, 1e-6));  // the octave
}

TEST_CASE("Forge sequencing stage_seq: run gates the transport",
          "[host][baked][forge][forge-sequencing][stageseq]") {
    StageFixture fx(seqcat::stage_seq::make_stage_seq_node(ladder_pattern()), kSr, kFrames);
    auto inj = fx.claim_injector();
    stage_baseline(inj);

    const auto running = fx.render({clock_line(64), flat(0.0f)});
    const auto run_pitches = at_clocks(running[0], 64);
    REQUIRE(run_pitches.back() > run_pitches.front());  // it advanced

    // run = 0: clocks are ignored, the gate is forced low, the pitch holds.
    REQUIRE(inj.inject(immediate(seqcat::stage_seq::kRun, 0.0f)) == InjectStatus::Ok);
    const auto stopped = fx.render({clock_line(64), flat(0.0f)});
    const float held = stopped[0].front();
    for (std::size_t k = 0; k < stopped[0].size(); ++k) {
        REQUIRE_THAT(stopped[0][k], WithinAbs(held, 1e-6));
        REQUIRE_THAT(stopped[1][k], WithinAbs(0.0, 1e-9));  // no hung gate across a stop
    }

    // Raising run continues from where it stopped rather than restarting — the
    // ladder's pitch IS its stage index, so "one stage on, modulo the pattern"
    // is the whole assertion. The eight-stage walk had reached stage 7 when the
    // transport stopped, so continuing wraps to 0; a RESTART would also land on
    // 0, so the case above (position held while stopped) is what separates them.
    REQUIRE_THAT(static_cast<double>(held), WithinAbs(7.0, 1e-6));
    REQUIRE(inj.inject(immediate(seqcat::stage_seq::kRun, 1.0f)) == InjectStatus::Ok);
    const auto resumed = fx.render({clock_line(64), flat(0.0f)});
    const auto resumed_pitches = at_clocks(resumed[0], 64);
    REQUIRE_THAT(static_cast<double>(resumed_pitches[0]),
                 WithinAbs(std::fmod(static_cast<double>(held) + 1.0, 8.0), 1e-6));
    REQUIRE_THAT(static_cast<double>(resumed_pitches[1]), WithinAbs(1.0, 1e-6));
}

TEST_CASE("Forge sequencing stage_seq: num_stages sets the loop length",
          "[host][baked][forge][forge-sequencing][stageseq]") {
    StageFixture fx(seqcat::stage_seq::make_stage_seq_node(ladder_pattern()), kSr, kFrames);
    auto inj = fx.claim_injector();
    stage_baseline(inj);

    for (int stages : {2, 3, 5, 8}) {
        REQUIRE(inj.inject(immediate(seqcat::stage_seq::kNumStages,
                                     static_cast<float>(stages))) == InjectStatus::Ok);
        // A reset pulse on the PORT so each length starts from the top.
        auto reset = flat(0.0f);
        reset[0] = 1.0f;
        const auto out = fx.render({clock_line(32), reset});
        const auto pitches = at_clocks(out[0], 32);

        // The ladder's pitch IS its stage index, so the walk order is readable
        // directly and the loop length is asserted rather than inferred.
        for (std::size_t i = 0; i < pitches.size(); ++i)
            REQUIRE_THAT(static_cast<double>(pitches[i]),
                         WithinAbs(static_cast<double>(i % static_cast<std::size_t>(stages)),
                                   1e-6));
    }
}

TEST_CASE("Forge sequencing stage_seq: direction picks the walk order",
          "[host][baked][forge][forge-sequencing][stageseq]") {
    StageFixture fx(seqcat::stage_seq::make_stage_seq_node(ladder_pattern()), kSr, kFrames);
    auto inj = fx.claim_injector();
    stage_baseline(inj);
    REQUIRE(inj.inject(immediate(seqcat::stage_seq::kNumStages, 4.0f)) == InjectStatus::Ok);

    const auto walk = [&](int direction) {
        REQUIRE(inj.inject(immediate(seqcat::stage_seq::kDirection,
                                     static_cast<float>(direction))) == InjectStatus::Ok);
        auto reset = flat(0.0f);
        reset[0] = 1.0f;
        const auto out = fx.render({clock_line(32), reset});
        std::vector<int> order;
        for (float v : at_clocks(out[0], 32)) order.push_back(static_cast<int>(std::lround(v)));
        return order;
    };

    const auto forward = walk(0);
    const auto reverse = walk(1);
    const auto pingpong = walk(2);
    const auto random_walk = walk(3);

    REQUIRE(forward[0] == 0);
    REQUIRE(forward[1] == 1);
    REQUIRE(forward[2] == 2);
    REQUIRE(forward[3] == 3);
    REQUIRE(forward[4] == 0);

    REQUIRE(reverse[0] == 0);
    REQUIRE(reverse[1] == 3);
    REQUIRE(reverse[2] == 2);
    REQUIRE(reverse[3] == 1);

    // Reflects at the ends without repeating them: 0,1,2,3,2,1,0,…
    REQUIRE(pingpong[0] == 0);
    REQUIRE(pingpong[3] == 3);
    REQUIRE(pingpong[4] == 2);
    REQUIRE(pingpong[6] == 0);

    // The random walk is a walk, not a constant, and stays inside the pattern.
    for (int s : random_walk) {
        REQUIRE(s >= 0);
        REQUIRE(s <= 3);
    }
    REQUIRE(random_walk != forward);
}

TEST_CASE("Forge sequencing stage_seq: slide_ms sets the glide time",
          "[host][baked][forge][forge-sequencing][stageseq]") {
    // A two-stage pattern with a slide onto stage 1, so the glide is the only
    // thing moving between the two clocks.
    auto pattern = ladder_pattern(sig::StageGateMode::hold);
    pattern[0].pitch_v = 0.0f;
    pattern[1].pitch_v = 1.0f;
    pattern[1].slide = true;

    StageFixture fx(seqcat::stage_seq::make_stage_seq_node(pattern), kSr, kFrames);
    auto inj = fx.claim_injector();
    stage_baseline(inj);
    REQUIRE(inj.inject(immediate(seqcat::stage_seq::kNumStages, 2.0f)) == InjectStatus::Ok);

    // How far the glide has travelled 128 samples after entering stage 1.
    const auto travelled_after = [&](float slide_ms) {
        REQUIRE(inj.inject(immediate(seqcat::stage_seq::kSlideMs, slide_ms)) == InjectStatus::Ok);
        auto reset = flat(0.0f);
        reset[0] = 1.0f;
        const auto out = fx.render({clock_line(256), reset});
        return out[0][static_cast<std::size_t>(256 + 128)];
    };

    // Constant TIME: a shorter slide has covered more of the same distance at
    // the same instant. Directional, and monotone across the declared range.
    const float fast = travelled_after(1.0f);
    const float mid = travelled_after(30.0f);
    const float slow = travelled_after(500.0f);
    REQUIRE(fast > mid);
    REQUIRE(mid > slow);
    REQUIRE_THAT(static_cast<double>(fast), WithinAbs(1.0, 1e-4));  // 1 ms is long done

    // 30 ms at 48 kHz is 1440 samples, so at 128 samples the glide is ~8.9 %
    // of the way. Computed from the injected value, not restated.
    const double expected_mid = 128.0 / (30.0 * kSr / 1000.0);
    REQUIRE_THAT(static_cast<double>(mid), WithinAbs(expected_mid, 0.02));
}

TEST_CASE("Forge sequencing stage_seq: repeat_duty_pct sets the gate's high fraction",
          "[host][baked][forge][forge-sequencing][stageseq]") {
    constexpr int kPeriod = 64;
    StageFixture fx(seqcat::stage_seq::make_stage_seq_node(
                        ladder_pattern(sig::StageGateMode::repeat)),
                    kSr, kFrames);
    auto inj = fx.claim_injector();
    stage_baseline(inj);

    const auto high_fraction = [&](float duty_pct) {
        REQUIRE(inj.inject(immediate(seqcat::stage_seq::kRepeatDutyPct, duty_pct)) ==
                InjectStatus::Ok);
        const auto out = fx.render({clock_line(kPeriod), flat(0.0f)});
        // Count over ONE pulse well past the first — the first clock after a
        // reset has no measured period yet (the DSP module's D5), so its gate
        // is high for the whole pulse by design.
        int highs = 0;
        for (int k = kPeriod * 4; k < kPeriod * 5; ++k)
            if (high(out[1][static_cast<std::size_t>(k)])) ++highs;
        return static_cast<double>(highs) / kPeriod;
    };

    for (float duty : {10.0f, 25.0f, 50.0f, 75.0f, 90.0f})
        REQUIRE_THAT(high_fraction(duty), WithinAbs(duty / 100.0, 1.5 / kPeriod));
}

TEST_CASE("Forge sequencing stage_seq: the reset port returns the walk to the top",
          "[host][baked][forge][forge-sequencing][stageseq]") {
    StageFixture fx(seqcat::stage_seq::make_stage_seq_node(ladder_pattern()), kSr, kFrames);
    auto inj = fx.claim_injector();
    stage_baseline(inj);

    // Clock at 32; fire a reset midway, spaced clear of the 0.5 ms refractory.
    auto reset = flat(0.0f);
    reset[200] = 1.0f;
    const auto out = fx.render({clock_line(32), reset});
    const auto pitches = at_clocks(out[0], 32);

    // Clocks land at 0,32,…; the reset at 200 falls between the clocks at 192
    // and 224, so the clock at 224 (index 7) is the first after it.
    REQUIRE_THAT(static_cast<double>(pitches[6]), WithinAbs(6.0, 1e-6));
    REQUIRE_THAT(static_cast<double>(pitches[7]), WithinAbs(0.0, 1e-6));  // back to the top
    REQUIRE_THAT(static_cast<double>(pitches[8]), WithinAbs(1.0, 1e-6));
}

TEST_CASE("Forge sequencing stage_seq: the seed is a realization, not a param",
          "[host][baked][forge][forge-sequencing][stageseq][determinism]") {
    // Series law 2: a seed is which performance the artifact plays, so it is
    // frozen at registration. Two nodes, two seeds, two different random walks —
    // and each is reproducible.
    const auto walk_with_seed = [](std::uint32_t seed) {
        StageFixture fx(seqcat::stage_seq::make_stage_seq_node(ladder_pattern(), seed), kSr,
                        kFrames);
        auto inj = fx.claim_injector();
        stage_baseline(inj);
        REQUIRE(inj.inject(immediate(seqcat::stage_seq::kNumStages, 8.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(seqcat::stage_seq::kDirection, 3.0f)) == InjectStatus::Ok);
        return at_clocks(fx.render({clock_line(16), flat(0.0f)})[0], 16);
    };

    const auto a = walk_with_seed(0x2A3Bu);
    const auto b = walk_with_seed(0x2A3Bu);
    const auto c = walk_with_seed(0xBEEFu);
    REQUIRE(a == b);  // same seed, same performance
    REQUIRE(a != c);  // a different seed really is a different one
}

TEST_CASE("Forge sequencing stage_seq: the baked node is bit-reproducible across resets",
          "[host][baked][forge][forge-sequencing][stageseq][determinism]") {
    // The module's headline claim, asserted at the NODE level rather than only
    // in the DSP suite: a baked artifact renders the same sequence every time.
    StageFixture fx(seqcat::stage_seq::make_stage_seq_node(ladder_pattern()), kSr, kFrames);
    auto inj = fx.claim_injector();
    stage_baseline(inj);
    REQUIRE(inj.inject(immediate(seqcat::stage_seq::kDirection, 3.0f)) == InjectStatus::Ok);

    const auto clock = clock_line(16);
    const auto first = fx.render({clock, flat(0.0f)});
    const auto second = fx.render({clock, flat(0.0f)});

    reinit(fx);
    stage_baseline(inj);
    REQUIRE(inj.inject(immediate(seqcat::stage_seq::kDirection, 3.0f)) == InjectStatus::Ok);
    const auto after_reset = fx.render({clock, flat(0.0f)});
    const auto after_reset_2 = fx.render({clock, flat(0.0f)});

    // Bit-identical, not "close": these are the same samples or the claim is
    // false. The second block after a reset must match the second block before
    // it too, so the reset restores the RNG rather than only the position.
    REQUIRE(after_reset[0] == first[0]);
    REQUIRE(after_reset[1] == first[1]);
    REQUIRE(after_reset_2[0] == second[0]);
    REQUIRE(after_reset_2[1] == second[1]);
}

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

TEST_CASE("Forge sequencing cartesian: the node bakes with three CV ports each way",
          "[host][baked][forge][forge-sequencing][cartesian]") {
    const auto type = seqcat::cartesian::make_cartesian_walk_node();
    REQUIRE(type.type_id == std::string("sequencing.cartesian_walk"));
    REQUIRE(type.num_input_ports == 3);   // X clock, Y clock, reset
    REQUIRE(type.num_output_ports == 3);  // CV, gate, end of cycle
    REQUIRE(type.baked_params.size() == 5);

    CartFixture fx(type, kSr, kFrames);
    auto inj = fx.claim_injector();
    cart_baseline(inj);

    // X only: the bottom row of four values, looping.
    const auto out = fx.render({clock_line(32), flat(0.0f), flat(0.0f)});
    require_finite(out[0]);
    // The DEFAULT grid is a chromatic ramp in VOLTS, so cell (x, y) holds
    // `index/12` — the other cases below bake an index grid instead, where the
    // cell value is the index itself and a walk order reads off directly.
    const auto cv = at_clocks(out[0], 32);
    for (std::size_t i = 0; i < cv.size(); ++i)
        REQUIRE_THAT(static_cast<double>(cv[i]),
                     WithinAbs(cell_index(static_cast<int>(i % 4), 0) / 12.0, 1e-6));
}

TEST_CASE("Forge sequencing cartesian: the two clocks are independent",
          "[host][baked][forge][forge-sequencing][cartesian]") {
    CartFixture fx(seqcat::cartesian::make_cartesian_walk_node(index_grid()), kSr, kFrames);
    auto inj = fx.claim_injector();
    cart_baseline(inj);

    // Y at four times the X period: the closed-form 16-cell super-cycle.
    const auto out = fx.render({clock_line(32), clock_line(128), flat(0.0f)});
    const auto cv = at_clocks(out[0], 32);
    for (std::size_t i = 0; i < cv.size(); ++i) {
        const int k = static_cast<int>(i);
        REQUIRE_THAT(static_cast<double>(cv[i]),
                     WithinAbs(cell_index(k % 4, (k / 4) % 4), 1e-6));
    }
}

TEST_CASE("Forge sequencing cartesian: grid width and height set the walk's extent",
          "[host][baked][forge][forge-sequencing][cartesian]") {
    CartFixture fx(seqcat::cartesian::make_cartesian_walk_node(index_grid()), kSr, kFrames);
    auto inj = fx.claim_injector();
    cart_baseline(inj);

    SECTION("grid_w sets the X loop length") {
        for (int w : {2, 3, 5, 8}) {
            REQUIRE(inj.inject(immediate(seqcat::cartesian::kGridW,
                                         static_cast<float>(w))) == InjectStatus::Ok);
            auto reset = flat(0.0f);
            reset[0] = 1.0f;
            const auto cv = at_clocks(fx.render({clock_line(32), flat(0.0f), reset})[0], 32);
            for (std::size_t i = 0; i < cv.size(); ++i)
                REQUIRE_THAT(static_cast<double>(cv[i]),
                             WithinAbs(cell_index(static_cast<int>(i) % w, 0), 1e-6));
        }
    }

    SECTION("grid_h sets the Y loop length") {
        REQUIRE(inj.inject(immediate(seqcat::cartesian::kGridW, 1.0f)) == InjectStatus::Ok);
        for (int h : {2, 4, 7}) {
            REQUIRE(inj.inject(immediate(seqcat::cartesian::kGridH,
                                         static_cast<float>(h))) == InjectStatus::Ok);
            auto reset = flat(0.0f);
            reset[0] = 1.0f;
            // Clock Y only; X is one cell wide so the walk is a pure column.
            const auto cv = at_clocks(fx.render({flat(0.0f), clock_line(32), reset})[0], 32);
            for (std::size_t i = 0; i < cv.size(); ++i)
                REQUIRE_THAT(static_cast<double>(cv[i]),
                             WithinAbs(cell_index(0, static_cast<int>(i) % h), 1e-6));
        }
    }
}

TEST_CASE("Forge sequencing cartesian: the CV offsets shift which cell is read",
          "[host][baked][forge][forge-sequencing][cartesian]") {
    CartFixture fx(seqcat::cartesian::make_cartesian_walk_node(index_grid()), kSr, kFrames);
    auto inj = fx.claim_injector();
    cart_baseline(inj);

    const auto walk_with = [&](int x_off, int y_off) {
        REQUIRE(inj.inject(immediate(seqcat::cartesian::kXOffset,
                                     static_cast<float>(x_off))) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(seqcat::cartesian::kYOffset,
                                     static_cast<float>(y_off))) == InjectStatus::Ok);
        auto reset = flat(0.0f);
        reset[0] = 1.0f;
        return at_clocks(fx.render({clock_line(32), flat(0.0f), reset})[0], 32);
    };

    // The offset is added to the counter modulo the axis length, so it rotates
    // the sequence rather than transposing the values.
    for (auto [x_off, y_off] : {std::pair{1, 0}, std::pair{2, 1}, std::pair{-1, 0}}) {
        const auto cv = walk_with(x_off, y_off);
        for (std::size_t i = 0; i < cv.size(); ++i) {
            const int x = ((static_cast<int>(i) + x_off) % 4 + 4) % 4;
            const int y = ((y_off % 4) + 4) % 4;
            REQUIRE_THAT(static_cast<double>(cv[i]), WithinAbs(cell_index(x, y), 1e-6));
        }
    }
}

TEST_CASE("Forge sequencing cartesian: run and the reset port work as on the stage sequencer",
          "[host][baked][forge][forge-sequencing][cartesian]") {
    CartFixture fx(seqcat::cartesian::make_cartesian_walk_node(index_grid()), kSr, kFrames);
    auto inj = fx.claim_injector();
    cart_baseline(inj);

    (void)fx.render({clock_line(32), flat(0.0f), flat(0.0f)});

    REQUIRE(inj.inject(immediate(seqcat::cartesian::kRun, 0.0f)) == InjectStatus::Ok);
    const auto stopped = fx.render({clock_line(32), flat(0.0f), flat(0.0f)});
    const float held = stopped[0].front();
    for (std::size_t k = 0; k < stopped[0].size(); ++k) {
        REQUIRE_THAT(stopped[0][k], WithinAbs(held, 1e-6));
        REQUIRE_THAT(stopped[1][k], WithinAbs(0.0, 1e-9));
        REQUIRE_THAT(stopped[2][k], WithinAbs(0.0, 1e-9));
    }

    REQUIRE(inj.inject(immediate(seqcat::cartesian::kRun, 1.0f)) == InjectStatus::Ok);
    auto reset = flat(0.0f);
    reset[200] = 1.0f;
    const auto out = fx.render({clock_line(32), flat(0.0f), reset});
    const auto cv = at_clocks(out[0], 32);
    // The clock at 224 is the first after the reset at 200, and it lands home.
    REQUIRE_THAT(static_cast<double>(cv[7]), WithinAbs(cell_index(0, 0), 1e-6));
    REQUIRE_THAT(static_cast<double>(cv[8]), WithinAbs(cell_index(1, 0), 1e-6));
}

TEST_CASE("Forge sequencing cartesian: the end-of-cycle output fires only at the home cell",
          "[host][baked][forge][forge-sequencing][cartesian]") {
    CartFixture fx(seqcat::cartesian::make_cartesian_walk_node(index_grid()), kSr, kFrames);
    auto inj = fx.claim_injector();
    cart_baseline(inj);
    REQUIRE(inj.inject(immediate(seqcat::cartesian::kGridH, 1.0f)) == InjectStatus::Ok);

    const auto out = fx.render({clock_line(32), flat(0.0f), flat(0.0f)});
    const auto eoc = at_clocks(out[2], 32);
    const auto cv = at_clocks(out[0], 32);
    for (std::size_t i = 0; i < eoc.size(); ++i) {
        const bool at_home = std::lround(static_cast<double>(cv[i])) == cell_index(0, 0);
        REQUIRE(high(eoc[i]) == at_home);  // exactly the cycle boundary, no more
    }
    // On a 4-wide row that is one pulse in four.
    int fired = 0;
    for (float v : eoc)
        if (high(v)) ++fired;
    REQUIRE(fired == static_cast<int>(eoc.size()) / 4);

    // The EOC reads the COUNTERS, so a CV offset changes which notes play
    // without moving the cycle boundary.
    REQUIRE(inj.inject(immediate(seqcat::cartesian::kXOffset, 2.0f)) == InjectStatus::Ok);
    auto reset = flat(0.0f);
    reset[0] = 1.0f;
    const auto shifted = fx.render({clock_line(32), flat(0.0f), reset});
    const auto shifted_eoc = at_clocks(shifted[2], 32);
    REQUIRE(high(shifted_eoc[0]));
    REQUIRE_FALSE(high(shifted_eoc[1]));
    REQUIRE(high(shifted_eoc[4]));  // still every fourth clock
    // …but the cell it plays there has moved.
    REQUIRE_THAT(static_cast<double>(at_clocks(shifted[0], 32)[0]),
                 WithinAbs(cell_index(2, 0), 1e-6));
}

TEST_CASE("Forge sequencing cartesian: access mode is a realization, and row-major ignores Y",
          "[host][baked][forge][forge-sequencing][cartesian]") {
    // Two registered type ids, exactly as the diode-bridge member splits on
    // detection topology — because in row-major, INPUT PORT 1 IS NOT READ.
    const auto independent = seqcat::cartesian::make_cartesian_walk_node(index_grid(), false);
    const auto row_major = seqcat::cartesian::make_cartesian_walk_node(index_grid(), true);
    REQUIRE(independent.type_id != row_major.type_id);
    REQUIRE(row_major.type_id == std::string("sequencing.cartesian_walk_row_major"));

    SECTION("independent: a Y clock alone moves the walk") {
        CartFixture fx(independent, kSr, kFrames);
        auto inj = fx.claim_injector();
        cart_baseline(inj);
        const auto cv = at_clocks(fx.render({flat(0.0f), clock_line(32), flat(0.0f)})[0], 32);
        REQUIRE_THAT(static_cast<double>(cv[1]), WithinAbs(cell_index(0, 1), 1e-6));
    }

    SECTION("row-major: a Y clock alone moves nothing, and an X wrap carries") {
        CartFixture fx(row_major, kSr, kFrames);
        auto inj = fx.claim_injector();
        cart_baseline(inj);

        const auto y_only = fx.render({flat(0.0f), clock_line(32), flat(0.0f)});
        for (float v : y_only[0]) REQUIRE_THAT(static_cast<double>(v), WithinAbs(0.0, 1e-9));

        auto reset = flat(0.0f);
        reset[0] = 1.0f;
        const auto cv = at_clocks(fx.render({clock_line(32), clock_line(32), reset})[0], 32);
        // 0,1,2,3 then the wrap carries into the next row: (0,1) = index 8.
        REQUIRE_THAT(static_cast<double>(cv[3]), WithinAbs(cell_index(3, 0), 1e-6));
        REQUIRE_THAT(static_cast<double>(cv[4]), WithinAbs(cell_index(0, 1), 1e-6));
        REQUIRE_THAT(static_cast<double>(cv[5]), WithinAbs(cell_index(1, 1), 1e-6));
    }
}

TEST_CASE("Forge sequencing cartesian: the baked node is bit-reproducible across resets",
          "[host][baked][forge][forge-sequencing][cartesian][determinism]") {
    CartFixture fx(seqcat::cartesian::make_cartesian_walk_node(index_grid()), kSr, kFrames);
    auto inj = fx.claim_injector();
    cart_baseline(inj);

    const auto x = clock_line(32);
    const auto y = clock_line(96);
    const auto first = fx.render({x, y, flat(0.0f)});
    const auto second = fx.render({x, y, flat(0.0f)});
    reinit(fx);
    cart_baseline(inj);
    REQUIRE(fx.render({x, y, flat(0.0f)})[0] == first[0]);
    REQUIRE(fx.render({x, y, flat(0.0f)})[0] == second[0]);
}

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

TEST_CASE("Forge sequencing rungler: the node bakes and reproduces the DSP's worked sequence",
          "[host][baked][forge][forge-sequencing][rungler]") {
    const auto type = seqcat::rungler::make_rungler_node();
    REQUIRE(type.type_id == std::string("sequencing.rungler"));
    REQUIRE(type.num_input_ports == 2);   // clock, reset
    REQUIRE(type.num_output_ports == 2);  // CV, serial bit
    REQUIRE(type.baked_params.size() == 6);

    RunglerFixture fx(type, kSr, kFrames);
    auto inj = fx.claim_injector();
    rungler_baseline(inj);

    const auto out = fx.render({clock_line(32), flat(0.0f)});
    const auto cv = at_clocks(out[0], 32);

    // The module's worked example, re-derived from the shipped constants rather
    // than restated: seed 0b10110100 has low three bits 100 → code 4 → the first
    // clock shifts to 0b01101001 → code 1.
    const double range = seqcat::rungler::Rung::kRangeV;
    const double levels = 7.0;  // 2^3 − 1
    REQUIRE_THAT(static_cast<double>(cv[0]),
                 WithinAbs(range * (2.0 * 1.0 / levels - 1.0), 1e-5));
    // Eight DAC levels, and the line really moves through them.
    REQUIRE(distinct_levels(cv) > 3);
}

TEST_CASE("Forge sequencing rungler: feedback-tap range follows the registered length",
          "[host][baked][forge][forge-sequencing][rungler]") {
    const auto type = seqcat::rungler::make_rungler_node(4);
    const auto it = std::find_if(type.baked_params.begin(), type.baked_params.end(),
                                 [](const auto& p) {
                                     return p.id == seqcat::rungler::kFeedbackTap;
                                 });
    REQUIRE(it != type.baked_params.end());
    REQUIRE(it->min_value == 0.0f);
    REQUIRE(it->max_value == 2.0f);
}

TEST_CASE("Forge sequencing rungler: dac_bits sets the number of output levels",
          "[host][baked][forge][forge-sequencing][rungler]") {
    RunglerFixture fx(seqcat::rungler::make_rungler_node(), kSr, kFrames);
    auto inj = fx.claim_injector();
    rungler_baseline(inj);

    // A long enough run for the orbit to visit its levels.
    const auto levels_for = [&](int dac_bits) {
        reinit(fx);
        rungler_baseline(inj);
        REQUIRE(inj.inject(immediate(seqcat::rungler::kDacBits,
                                     static_cast<float>(dac_bits))) == InjectStatus::Ok);
        std::vector<float> all;
        for (int b = 0; b < 8; ++b) {
            const auto cv = at_clocks(fx.render({clock_line(8), flat(0.0f)})[0], 8);
            all.insert(all.end(), cv.begin(), cv.end());
        }
        return distinct_levels(all);
    };

    // A D-bit DAC can emit at most 2^D levels, and the count grows with D.
    const auto one = levels_for(1);
    const auto two = levels_for(2);
    const auto three = levels_for(3);
    const auto four = levels_for(4);
    REQUIRE(one <= 2u);
    REQUIRE(two <= 4u);
    REQUIRE(three <= 8u);
    REQUIRE(four <= 16u);
    REQUIRE(one < three);
    REQUIRE(three <= four);
}

TEST_CASE("Forge sequencing rungler: feedback_tap picks a different orbit",
          "[host][baked][forge][forge-sequencing][rungler]") {
    RunglerFixture fx(seqcat::rungler::make_rungler_node(), kSr, kFrames);
    auto inj = fx.claim_injector();
    rungler_baseline(inj);

    const auto orbit_for = [&](int tap) {
        reinit(fx);
        rungler_baseline(inj);
        REQUIRE(inj.inject(immediate(seqcat::rungler::kFeedbackTap,
                                     static_cast<float>(tap))) == InjectStatus::Ok);
        return at_clocks(fx.render({clock_line(8), flat(0.0f)})[0], 8);
    };

    // The tap IS the recurrence, so a different tap is a different sequence —
    // and each one is still reproducible.
    const auto tap0 = orbit_for(0);
    const auto tap3 = orbit_for(3);
    const auto tap0_again = orbit_for(0);
    REQUIRE(tap0 != tap3);
    REQUIRE(tap0 == tap0_again);
}

TEST_CASE("Forge sequencing rungler: range_v scales the output and bounds it",
          "[host][baked][forge][forge-sequencing][rungler]") {
    RunglerFixture fx(seqcat::rungler::make_rungler_node(), kSr, kFrames);
    auto inj = fx.claim_injector();
    rungler_baseline(inj);

    for (float range : {0.5f, 1.0f, 2.0f, 5.0f}) {
        reinit(fx);
        rungler_baseline(inj);
        REQUIRE(inj.inject(immediate(seqcat::rungler::kRangeV, range)) == InjectStatus::Ok);
        const auto out = fx.render({clock_line(8), flat(0.0f)});
        // The registry's invariant, over the production path: |y| <= range_v by
        // construction, because the output is a D-bit DAC code mapped affinely
        // onto [-range, +range]. Not "close to" — never above.
        for (float v : out[0]) REQUIRE(std::fabs(v) <= range + 1e-6f);
        // And the bound is TIGHT: the orbit reaches an extreme, so a passing
        // test is not passing because the output stayed small.
        REQUIRE_THAT(static_cast<double>(peak_abs(out[0])),
                     WithinAbs(static_cast<double>(range), 1e-5));
    }
    REQUIRE_THAT(static_cast<double>(seqcat::rungler::rungler_output_bound_v()),
                 WithinAbs(5.0, 1e-9));
}

TEST_CASE("Forge sequencing rungler: external_data and data_in steer the chaos",
          "[host][baked][forge][forge-sequencing][rungler]") {
    RunglerFixture fx(seqcat::rungler::make_rungler_node(), kSr, kFrames);
    auto inj = fx.claim_injector();
    rungler_baseline(inj);

    const auto run_with = [&](float external, float data) {
        reinit(fx);
        rungler_baseline(inj);
        REQUIRE(inj.inject(immediate(seqcat::rungler::kExternalData, external)) ==
                InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(seqcat::rungler::kDataIn, data)) == InjectStatus::Ok);
        return at_clocks(fx.render({clock_line(8), flat(0.0f)})[0], 8);
    };

    const auto plain = run_with(0.0f, 0.0f);
    const auto data_ignored = run_with(0.0f, 1.0f);
    const auto steered = run_with(1.0f, 1.0f);

    // The switch is what makes the data bit matter: with it off, the data bit
    // is ignored entirely.
    REQUIRE(plain == data_ignored);
    // With it on, the bit perturbs the state every clock.
    REQUIRE(steered != plain);
}

TEST_CASE("Forge sequencing rungler: run holds, and the reset port re-pins to the seed",
          "[host][baked][forge][forge-sequencing][rungler]") {
    RunglerFixture fx(seqcat::rungler::make_rungler_node(), kSr, kFrames);
    auto inj = fx.claim_injector();
    rungler_baseline(inj);

    (void)fx.render({clock_line(8), flat(0.0f)});
    REQUIRE(inj.inject(immediate(seqcat::rungler::kRun, 0.0f)) == InjectStatus::Ok);
    const auto stopped = fx.render({clock_line(8), flat(0.0f)});
    const float held = stopped[0].front();
    for (float v : stopped[0]) REQUIRE_THAT(static_cast<double>(v),
                                            WithinAbs(static_cast<double>(held), 1e-9));

    // This is the module's one documented exception to "a reset edge holds the
    // continuous output": the rungler's reset re-pins the register AND the DAC
    // level immediately, so a wandering line can be caught live.
    reinit(fx);
    rungler_baseline(inj);
    const auto fresh = at_clocks(fx.render({clock_line(32), flat(0.0f)})[0], 32);

    reinit(fx);
    rungler_baseline(inj);
    auto reset = flat(0.0f);
    reset[200] = 1.0f;
    const auto repinned = at_clocks(fx.render({clock_line(32), reset})[0], 32);
    // The clock at 224 is the first after the reset, and it plays what the first
    // clock after a fresh start plays.
    REQUIRE_THAT(static_cast<double>(repinned[7]),
                 WithinAbs(static_cast<double>(fresh[0]), 1e-6));
}

TEST_CASE("Forge sequencing rungler: the serial-bit output tracks the register",
          "[host][baked][forge][forge-sequencing][rungler]") {
    RunglerFixture fx(seqcat::rungler::make_rungler_node(), kSr, kFrames);
    auto inj = fx.claim_injector();
    rungler_baseline(inj);
    REQUIRE(inj.inject(immediate(seqcat::rungler::kDacBits, 1.0f)) == InjectStatus::Ok);

    const auto out = fx.render({clock_line(16), flat(0.0f)});
    // At D = 1 the DAC code IS bit 0, so the CV and the bit output must agree
    // sample for sample: CV at +range means bit 0 is set.
    for (std::size_t k = 0; k < out[0].size(); ++k) {
        const bool bit = high(out[1][k]);
        REQUIRE(bit == (out[0][k] > 0.0f));
        REQUIRE((out[1][k] == 0.0f || out[1][k] == 1.0f));  // a gate, not a level
    }
}

TEST_CASE("Forge sequencing rungler: register length and seed are realizations",
          "[host][baked][forge][forge-sequencing][rungler][determinism]") {
    const auto run_node = [](int reg_bits, std::uint32_t seed) {
        RunglerFixture fx(seqcat::rungler::make_rungler_node(reg_bits, seed), kSr, kFrames);
        auto inj = fx.claim_injector();
        rungler_baseline(inj);
        return at_clocks(fx.render({clock_line(8), flat(0.0f)})[0], 8);
    };

    // The seed is which performance the artifact plays (law 2).
    REQUIRE(run_node(8, 0b10110100u) == run_node(8, 0b10110100u));
    REQUIRE(run_node(8, 0b10110100u) != run_node(8, 0b11001010u));

    // The register length is a realization because the DSP reloads the seed when
    // it changes: as a param it would re-pin the sequence on every sample and
    // the node would emit one frozen level instead of a line. Different lengths
    // are different orbits, and each is a line rather than a constant.
    const auto n8 = run_node(8, 0b10110100u);
    const auto n16 = run_node(16, 0b10110100u);
    REQUIRE(n8 != n16);
    REQUIRE(distinct_levels(n8) > 1);
    REQUIRE(distinct_levels(n16) > 1);
}

TEST_CASE("Forge sequencing rungler: the baked node is bit-reproducible across resets",
          "[host][baked][forge][forge-sequencing][rungler][determinism]") {
    RunglerFixture fx(seqcat::rungler::make_rungler_node(), kSr, kFrames);
    auto inj = fx.claim_injector();
    rungler_baseline(inj);
    REQUIRE(inj.inject(immediate(seqcat::rungler::kExternalData, 1.0f)) == InjectStatus::Ok);

    const auto clock = clock_line(8);
    const auto first = fx.render({clock, flat(0.0f)});
    const auto second = fx.render({clock, flat(0.0f)});
    reinit(fx);
    rungler_baseline(inj);
    REQUIRE(inj.inject(immediate(seqcat::rungler::kExternalData, 1.0f)) == InjectStatus::Ok);
    REQUIRE(fx.render({clock, flat(0.0f)})[0] == first[0]);
    REQUIRE(fx.render({clock, flat(0.0f)})[0] == second[0]);
}

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

TEST_CASE("Forge sequencing quantizer: the node bakes and snaps to 12-TET",
          "[host][baked][forge][forge-sequencing][quantizer]") {
    const auto type = seqcat::quantize::make_quantize_scale_node();
    REQUIRE(type.type_id == std::string("sequencing.quantize_scale"));
    REQUIRE(type.num_input_ports == 1);
    REQUIRE(type.num_output_ports == 1);
    REQUIRE(type.baked_params.size() == 6);

    QuantFixture fx(type, kSr, kFrames);
    auto inj = fx.claim_injector();
    quant_baseline(inj);

    const auto out = fx.render({flat(0.30f)});
    // The module's worked example over the production path: 0.30 V → step 4.
    for (float v : out[0]) REQUIRE_THAT(static_cast<double>(v), WithinAbs(4.0 / 12.0, 1e-6));
}

TEST_CASE("Forge sequencing quantizer: mode picks between EDO and the scale mask",
          "[host][baked][forge][forge-sequencing][quantizer]") {
    QuantFixture fx(seqcat::quantize::make_quantize_scale_node(), kSr, kFrames);
    auto inj = fx.claim_injector();
    quant_baseline(inj);

    // 0.26 V is chromatic step 3 (D#), which C major does not contain.
    REQUIRE(inj.inject(immediate(seqcat::quantize::kMode, 0.0f)) == InjectStatus::Ok);
    REQUIRE_THAT(static_cast<double>(fx.render({flat(0.26f)})[0].front()),
                 WithinAbs(3.0 / 12.0, 1e-6));

    REQUIRE(inj.inject(immediate(seqcat::quantize::kMode, 1.0f)) == InjectStatus::Ok);
    REQUIRE_THAT(static_cast<double>(fx.render({flat(0.26f)})[0].front()),
                 WithinAbs(4.0 / 12.0, 1e-6));  // snapped up to E
}

TEST_CASE("Forge sequencing quantizer: edo_n sets the step grid",
          "[host][baked][forge][forge-sequencing][quantizer]") {
    QuantFixture fx(seqcat::quantize::make_quantize_scale_node(), kSr, kFrames);
    auto inj = fx.claim_injector();
    quant_baseline(inj);

    for (int n : {12, 19, 24, 31, 48}) {
        // Every output must be an exact multiple of one step of THIS division.
        for (float cv : {0.07f, 0.23f, 0.41f, 0.77f}) {
            reinit(fx);
            quant_baseline(inj);
            REQUIRE(inj.inject(immediate(seqcat::quantize::kEdoN, static_cast<float>(n))) ==
                    InjectStatus::Ok);
            const float out = fx.render({flat(cv)})[0].front();
            const double steps = static_cast<double>(out) * n;
            REQUIRE_THAT(steps - std::round(steps), WithinAbs(0.0, 1e-4));
            REQUIRE(std::fabs(out - cv) <= 0.5 / n + 1e-5);  // nearest step
        }
    }
}

TEST_CASE("Forge sequencing quantizer: scale_mask and root_pc pick the allowed classes",
          "[host][baked][forge][forge-sequencing][quantizer]") {
    QuantFixture fx(seqcat::quantize::make_quantize_scale_node(), kSr, kFrames);
    auto inj = fx.claim_injector();
    quant_baseline(inj);
    REQUIRE(inj.inject(immediate(seqcat::quantize::kMode, 1.0f)) == InjectStatus::Ok);

    // The mask and root are injected INSIDE the probe, after the re-init that
    // clears the hysteresis latch — `reinit` rewinds every param to its declared
    // default, so a probe that set them outside would be measuring the defaults.
    const auto pitch_class_of = [&](int mask, int root_pc, float cv) {
        reinit(fx);
        quant_baseline(inj);
        REQUIRE(inj.inject(immediate(seqcat::quantize::kMode, 1.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(seqcat::quantize::kScaleMask,
                                     static_cast<float>(mask))) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(seqcat::quantize::kRootPc,
                                     static_cast<float>(root_pc))) == InjectStatus::Ok);
        const float out = fx.render({flat(cv)})[0].front();
        int pc = static_cast<int>(std::lround(static_cast<double>(out) * 12.0)) % 12;
        return pc < 0 ? pc + 12 : pc;
    };

    SECTION("a mask restricts the output to its own pitch classes") {
        // A pentatonic mask: {0, 2, 4, 7, 9}.
        const int degrees[] = {0, 2, 4, 7, 9};
        int mask = 0;
        for (int d : degrees) mask |= 1 << d;
        for (int st = 0; st < 12; ++st) {
            const int pc = pitch_class_of(mask, 0, static_cast<float>(st) / 12.0f);
            REQUIRE(((mask >> pc) & 1) != 0);
        }
    }

    SECTION("the root rotates the mask") {
        const int major = static_cast<int>(sig::QuantizeScale::kMajorMask);
        // C major contains E (pc 4) but not D# (pc 3), so 3 snaps up to 4.
        REQUIRE(pitch_class_of(major, 0, 3.0f / 12.0f) == 4);
        // Rooted on D# the same mask contains pc 3 itself, so the input stands.
        REQUIRE(pitch_class_of(major, 3, 3.0f / 12.0f) == 3);
    }

    SECTION("an empty mask falls through to chromatic") {
        for (int st = 0; st < 12; ++st)
            REQUIRE(pitch_class_of(0, 0, static_cast<float>(st) / 12.0f) == st);
    }
}

TEST_CASE("Forge sequencing quantizer: hyst_cents widens the step boundary",
          "[host][baked][forge][forge-sequencing][quantizer]") {
    QuantFixture fx(seqcat::quantize::make_quantize_scale_node(), kSr, kFrames);
    auto inj = fx.claim_injector();
    quant_baseline(inj);

    // Latch onto semitone 4, then present an input just below the plain
    // boundary. With no hysteresis it follows; with hysteresis it holds.
    const auto follows_at = [&](float hyst_cents, float probe_semitones) {
        reinit(fx);
        quant_baseline(inj);
        REQUIRE(inj.inject(immediate(seqcat::quantize::kHystCents, hyst_cents)) ==
                InjectStatus::Ok);
        std::vector<float> ramp(static_cast<std::size_t>(kFrames), probe_semitones / 12.0f);
        for (std::size_t k = 0; k < 64; ++k) ramp[k] = 4.0f / 12.0f;  // establish the latch
        const auto out = fx.render({ramp});
        return static_cast<double>(out[0].back()) * 12.0;
    };

    // 3.4 semitones rounds to 3 on its own.
    REQUIRE_THAT(follows_at(0.0f, 3.4f), WithinAbs(3.0, 1e-4));
    // With a 20-cent window the release point moves to 3.3, so 3.4 still holds.
    REQUIRE_THAT(follows_at(20.0f, 3.4f), WithinAbs(4.0, 1e-4));
    // Past the widened boundary it follows again.
    REQUIRE_THAT(follows_at(20.0f, 3.2f), WithinAbs(3.0, 1e-4));
    // A wider window holds further still.
    REQUIRE_THAT(follows_at(50.0f, 3.2f), WithinAbs(4.0, 1e-4));
}

TEST_CASE("Forge sequencing quantizer: the reset param is an edge, not a level",
          "[host][baked][forge][forge-sequencing][quantizer]") {
    QuantFixture fx(seqcat::quantize::make_quantize_scale_node(), kSr, kFrames);
    auto inj = fx.claim_injector();
    quant_baseline(inj);
    REQUIRE(inj.inject(immediate(seqcat::quantize::kHystCents, 20.0f)) == InjectStatus::Ok);

    // Latch on 4, then sit at 3.4 semitones — inside the window, so it holds.
    std::vector<float> input(static_cast<std::size_t>(kFrames), 3.4f / 12.0f);
    for (std::size_t k = 0; k < 64; ++k) input[k] = 4.0f / 12.0f;
    REQUIRE_THAT(static_cast<double>(fx.render({input})[0].back()) * 12.0,
                 WithinAbs(4.0, 1e-4));

    // A reset edge mid-block clears the latch, so the same input re-quantizes on
    // its own merits from that sample on.
    reinit(fx);
    // ONE queue carrying the whole operating point plus the two reset events:
    // the queue form of `inject` REPLACES the pending batch rather than merging
    // into it, so a baseline published as separate singles beforehand would be
    // silently discarded here and the node would run on its declared defaults.
    pulp::state::ParameterEventQueue q;
    q.push(immediate(seqcat::quantize::kMode, 0.0f, 0));
    q.push(immediate(seqcat::quantize::kEdoN, 12.0f, 0));
    q.push(immediate(seqcat::quantize::kScaleMask, 2741.0f, 0));
    q.push(immediate(seqcat::quantize::kRootPc, 0.0f, 0));
    q.push(immediate(seqcat::quantize::kHystCents, 20.0f, 0));
    q.push(immediate(seqcat::quantize::kReset, 0.0f, 0));
    q.push(immediate(seqcat::quantize::kReset, 1.0f, 256));
    REQUIRE(inj.inject(q) == InjectStatus::Ok);
    const auto out = fx.render({input});
    REQUIRE_THAT(static_cast<double>(out[0][200]) * 12.0, WithinAbs(4.0, 1e-4));
    REQUIRE_THAT(static_cast<double>(out[0][300]) * 12.0, WithinAbs(3.0, 1e-4));

    // Holding it high does NOT keep clearing the latch — it is an edge. If it
    // were read as a level the hysteresis would be disabled for the whole block
    // and the first assertion below would read 3.
    reinit(fx);
    quant_baseline(inj);
    REQUIRE(inj.inject(immediate(seqcat::quantize::kHystCents, 20.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(seqcat::quantize::kReset, 1.0f)) == InjectStatus::Ok);
    const auto held_high = fx.render({input});
    REQUIRE_THAT(static_cast<double>(held_high[0].back()) * 12.0, WithinAbs(4.0, 1e-4));
}

TEST_CASE("Forge sequencing quantizer: the registry gain bound holds and is attained",
          "[host][baked][forge][forge-sequencing][quantizer]") {
    // Law 8: the registry number is a bound this suite asserts, not an estimate.
    //
    // EDO mode is multiplicatively bounded. The output is an exact multiple of
    // one step; the latch holds a step only while the input is within
    // `0.5 + window` of it, and the window is capped at `kMaxHystSteps`. So for
    // an output of one step the input is at least `0.5 − 0.45 = 0.05` steps, and
    // the worst ratio is `1 / 0.05 = 20`. Below half a step the output is 0, so
    // there is no larger ratio anywhere.
    const double bound = 1.0 / (0.5 - sig::QuantizeScale::kMaxHystSteps);
    REQUIRE_THAT(static_cast<double>(seqcat::quantize::quantize_scale_worst_case_gain()),
                 WithinAbs(bound, 1e-9));

    QuantFixture fx(seqcat::quantize::make_quantize_scale_node(), kSr, kFrames);
    auto inj = fx.claim_injector();
    quant_baseline(inj);
    REQUIRE(inj.inject(immediate(seqcat::quantize::kHystCents, 50.0f)) == InjectStatus::Ok);

    // Construct the worst case: latch on step 1, then drop the input to just
    // inside the widened window.
    const double release = 1.0 - 0.5 - sig::QuantizeScale::kMaxHystSteps;  // 0.05 steps
    std::vector<float> input(static_cast<std::size_t>(kFrames),
                             static_cast<float>((release * 1.02) / 12.0));
    for (std::size_t k = 0; k < 64; ++k) input[k] = 1.0f / 12.0f;
    const auto out = fx.render({input});
    const double ratio = static_cast<double>(out[0].back()) / static_cast<double>(input.back());
    REQUIRE(ratio <= bound + 1e-6);   // the bound holds
    REQUIRE(ratio > 0.9 * bound);     // and it is nearly attained, so it is not slack

    // Scale-mask mode is NOT multiplicatively bounded — an input near 0 V can be
    // snapped up to six semitones by a one-note mask — so the invariant there is
    // ADDITIVE, and that is the one asserted.
    REQUIRE(inj.inject(immediate(seqcat::quantize::kMode, 1.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(seqcat::quantize::kScaleMask,
                                 static_cast<float>(1 << 6))) == InjectStatus::Ok);
    const float offset_bound = seqcat::quantize::quantize_scale_mask_offset_bound_v();
    for (float cv : {0.0f, 0.001f, 0.2f, -0.35f, 1.7f, -2.4f}) {
        reinit(fx);
        quant_baseline(inj);
        REQUIRE(inj.inject(immediate(seqcat::quantize::kMode, 1.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(seqcat::quantize::kScaleMask,
                                     static_cast<float>(1 << 6))) == InjectStatus::Ok);
        const float snapped = fx.render({flat(cv)})[0].front();
        REQUIRE(std::fabs(snapped) <= std::fabs(cv) + offset_bound + 1e-5f);
    }
}

TEST_CASE("Forge sequencing quantizer: the baked node is bit-reproducible across resets",
          "[host][baked][forge][forge-sequencing][quantizer][determinism]") {
    QuantFixture fx(seqcat::quantize::make_quantize_scale_node(), kSr, kFrames);
    auto inj = fx.claim_injector();
    quant_baseline(inj);
    REQUIRE(inj.inject(immediate(seqcat::quantize::kHystCents, 20.0f)) == InjectStatus::Ok);

    std::vector<float> sweep(static_cast<std::size_t>(kFrames));
    for (std::size_t k = 0; k < sweep.size(); ++k)
        sweep[k] = static_cast<float>(std::sin(k * 0.01) * 1.5);

    const auto first = fx.render({sweep});
    reinit(fx);
    quant_baseline(inj);
    REQUIRE(inj.inject(immediate(seqcat::quantize::kHystCents, 20.0f)) == InjectStatus::Ok);
    REQUIRE(fx.render({sweep})[0] == first[0]);
}

// ══════════════════════════════════════════════════════════════════════════
//  Gate logic
// ══════════════════════════════════════════════════════════════════════════

TEST_CASE("Forge sequencing gate_logic: op selects the truth table, on both outputs",
          "[host][baked][forge][forge-sequencing][gatelogic]") {
    const auto type = seqcat::gate_logic::make_gate_logic_node();
    REQUIRE(type.type_id == std::string("sequencing.gate_logic"));
    REQUIRE(type.num_input_ports == 2);
    REQUIRE(type.num_output_ports == 2);
    REQUIRE(type.baked_params.size() == 1);

    pulp::test::BakedNodeFixture<2> fx(type, kSr, kFrames);
    auto inj = fx.claim_injector();

    // (F,F) (F,T) (T,F) (T,T) for AND / OR / XOR / NAND / NOR / XNOR.
    const bool expect[6][4] = {
        {false, false, false, true},  // AND
        {false, true, true, true},    // OR
        {false, true, true, false},   // XOR
        {true, true, true, false},    // NAND
        {true, false, false, false},  // NOR
        {true, false, false, true},   // XNOR
    };

    for (int op = 0; op < 6; ++op) {
        REQUIRE(inj.inject(immediate(seqcat::gate_logic::kOp, static_cast<float>(op))) ==
                InjectStatus::Ok);
        int row = 0;
        for (float a : {0.0f, 1.0f})
            for (float b : {0.0f, 1.0f}) {
                const auto out = fx.render({flat(a), flat(b)});
                REQUIRE(high(out[0].front()) == expect[op][row]);
                // Output 1 is the complement, always — that is what makes it
                // worth having rather than a second node kept in sync by hand.
                REQUIRE(high(out[1].front()) == !expect[op][row]);
                REQUIRE((out[0].front() == 0.0f || out[0].front() == 1.0f));
                ++row;
            }
    }
}

TEST_CASE("Forge sequencing gate_logic: the outputs are gates whatever the input amplitude",
          "[host][baked][forge][forge-sequencing][gatelogic]") {
    // The registry's worst_case_gain of 1.0 is not about a signal path — there
    // isn't one. A 100 V input still produces a 1 V gate.
    pulp::test::BakedNodeFixture<2> fx(seqcat::gate_logic::make_gate_logic_node(), kSr, kFrames);
    auto inj = fx.claim_injector();
    REQUIRE(inj.inject(immediate(seqcat::gate_logic::kOp, 0.0f)) == InjectStatus::Ok);

    for (float level : {1.0f, 5.0f, 100.0f, -100.0f}) {
        const auto out = fx.render({flat(level), flat(level)});
        for (float v : out[0]) REQUIRE(std::fabs(v) <= 1.0f);
        for (float v : out[1]) REQUIRE(std::fabs(v) <= 1.0f);
    }
    REQUIRE_THAT(static_cast<double>(seqcat::gate_logic::gate_logic_worst_case_gain()),
                 WithinAbs(1.0, 1e-9));
}

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

TEST_CASE("Forge sequencing prob_gate: probability sets the pass density",
          "[host][baked][forge][forge-sequencing][probgate]") {
    const auto type = seqcat::prob_gate::make_prob_gate_node();
    REQUIRE(type.type_id == std::string("sequencing.prob_gate"));
    REQUIRE(type.num_input_ports == 1);
    REQUIRE(type.num_output_ports == 1);
    REQUIRE(type.baked_params.size() == 2);

    ProbFixture fx(type, kSr, kFrames);
    auto inj = fx.claim_injector();
    REQUIRE(inj.inject(immediate(seqcat::prob_gate::kReset, 0.0f)) == InjectStatus::Ok);

    constexpr int kPeriod = 4;  // 128 triggers per block
    const auto trig = clock_line(kPeriod);
    const int triggers = static_cast<int>(clock_indices(kPeriod).size());

    const auto density = [&](float pct) {
        reinit(fx);
        REQUIRE(inj.inject(immediate(seqcat::prob_gate::kProbabilityPct, pct)) ==
                InjectStatus::Ok);
        int passed = 0;
        for (int b = 0; b < 16; ++b) passed += count_passes(fx.render({trig})[0]);
        return static_cast<double>(passed) / (triggers * 16);
    };

    REQUIRE_THAT(density(0.0f), WithinAbs(0.0, 1e-12));    // blocks everything
    REQUIRE_THAT(density(100.0f), WithinAbs(1.0, 1e-12));  // passes everything
    // And it is monotone in between, which is the directional claim.
    const double at25 = density(25.0f);
    const double at50 = density(50.0f);
    const double at75 = density(75.0f);
    REQUIRE(at25 < at50);
    REQUIRE(at50 < at75);
    REQUIRE_THAT(at50, WithinAbs(0.5, 0.05));
}

TEST_CASE("Forge sequencing prob_gate: the seed is a realization and the reset does not rewind it",
          "[host][baked][forge][forge-sequencing][probgate][determinism]") {
    const auto pattern_for = [](std::uint32_t seed) {
        ProbFixture fx(seqcat::prob_gate::make_prob_gate_node(seed), kSr, kFrames);
        auto inj = fx.claim_injector();
        REQUIRE(inj.inject(immediate(seqcat::prob_gate::kProbabilityPct, 50.0f)) ==
                InjectStatus::Ok);
        return fx.render({clock_line(4)})[0];
    };

    // A seeded chance gate renders the same performance every time — audition,
    // bounce, reload, same groove. Changing the seed is how you roll again.
    REQUIRE(pattern_for(0x1234567u) == pattern_for(0x1234567u));
    REQUIRE(pattern_for(0x1234567u) != pattern_for(0x7654321u));

    // A transport reset clears the edge latch but must NOT rewind the stream: a
    // live reset that rewound randomness would make every reset sound identical.
    ProbFixture fx(seqcat::prob_gate::make_prob_gate_node(), kSr, kFrames);
    auto inj = fx.claim_injector();
    REQUIRE(inj.inject(immediate(seqcat::prob_gate::kProbabilityPct, 50.0f)) ==
            InjectStatus::Ok);
    const auto trig = clock_line(4);
    const auto block1 = fx.render({trig});
    const auto block2 = fx.render({trig});

    reinit(fx);
    REQUIRE(inj.inject(immediate(seqcat::prob_gate::kProbabilityPct, 50.0f)) ==
            InjectStatus::Ok);
    (void)fx.render({trig});
    REQUIRE(inj.inject(immediate(seqcat::prob_gate::kReset, 1.0f)) == InjectStatus::Ok);
    const auto after_reset_edge = fx.render({trig});
    REQUIRE(inj.inject(immediate(seqcat::prob_gate::kReset, 0.0f)) == InjectStatus::Ok);
    // The stream carried on: block 2 of the run is unchanged by the reset edge.
    REQUIRE(after_reset_edge[0] == block2[0]);

    // Whereas the lifecycle reset DOES rewind (series law 2).
    reinit(fx);
    REQUIRE(inj.inject(immediate(seqcat::prob_gate::kProbabilityPct, 50.0f)) ==
            InjectStatus::Ok);
    REQUIRE(fx.render({trig})[0] == block1[0]);
}

// ══════════════════════════════════════════════════════════════════════════
//  Cross-cutting
// ══════════════════════════════════════════════════════════════════════════

TEST_CASE("Forge sequencing: every node reports zero latency",
          "[host][baked][forge][forge-sequencing]") {
    // The series' latency lever is INERT in this module — no setting on any
    // block can move it — which is why no realization here is forced by latency.
    // Asserted so that stops being true loudly rather than quietly.
    REQUIRE(sig::StageSeq::latency_samples() == 0);
    REQUIRE(sig::CartesianWalk::latency_samples() == 0);
    REQUIRE(sig::Rungler::latency_samples() == 0);
    REQUIRE(sig::QuantizeScale::latency_samples() == 0);
    REQUIRE(sig::GateLogic::latency_samples() == 0);
    REQUIRE(sig::ProbGate::latency_samples() == 0);

    StageFixture stage(seqcat::stage_seq::make_stage_seq_node(), kSr, kFrames);
    REQUIRE(stage.baked().latency_samples() == 0);
    QuantFixture quant(seqcat::quantize::make_quantize_scale_node(), kSr, kFrames);
    REQUIRE(quant.baked().latency_samples() == 0);
}

TEST_CASE("Forge sequencing: the registry gain rows are the asserted invariants",
          "[host][baked][forge][forge-sequencing]") {
    // Five of the six have no input-to-output amplitude path at all: the clock
    // and reset ports are consumed as edges and the outputs are synthesised.
    REQUIRE_THAT(static_cast<double>(seqcat::stage_seq::stage_seq_worst_case_gain()),
                 WithinAbs(1.0, 1e-9));
    REQUIRE_THAT(static_cast<double>(seqcat::cartesian::cartesian_walk_worst_case_gain()),
                 WithinAbs(1.0, 1e-9));
    REQUIRE_THAT(static_cast<double>(seqcat::rungler::rungler_worst_case_gain()),
                 WithinAbs(1.0, 1e-9));
    REQUIRE_THAT(static_cast<double>(seqcat::prob_gate::prob_gate_worst_case_gain()),
                 WithinAbs(1.0, 1e-9));

    // The pattern bounds are computed from the baked pattern, so a registry row
    // cannot drift from the artifact it describes.
    auto pattern = seqcat::stage_seq::default_pattern();
    // The bound covers ALL sixteen slots, not only the eight that play at the
    // default `num_stages` — that param is injectable up to the full capacity,
    // so a bound that only looked at the first eight would be wrong the moment
    // someone automated it. The default fills the upper eight an octave up.
    REQUIRE_THAT(static_cast<double>(seqcat::stage_seq::stage_seq_pitch_bound_v(pattern)),
                 WithinAbs(2.0, 1e-6));
    pattern[3].pitch_v = -4.5f;
    REQUIRE_THAT(static_cast<double>(seqcat::stage_seq::stage_seq_pitch_bound_v(pattern)),
                 WithinAbs(4.5, 1e-6));

    const auto grid = seqcat::cartesian::default_grid();
    REQUIRE_THAT(static_cast<double>(seqcat::cartesian::cartesian_walk_cv_bound_v(grid)),
                 WithinAbs(63.0 / 12.0, 1e-6));
}

TEST_CASE("Forge sequencing: the pitch bound really bounds the baked pitch output",
          "[host][baked][forge][forge-sequencing]") {
    // The registry row is only worth having if the artifact obeys it, including
    // through the slide — which is a `SlewLimiterT` between two pattern pitches
    // and therefore cannot overshoot either.
    auto pattern = ladder_pattern(sig::StageGateMode::hold);
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        pattern[i].pitch_v = (i % 2 == 0) ? 3.0f : -3.0f;  // maximal jumps
        pattern[i].slide = true;
    }
    const float bound = seqcat::stage_seq::stage_seq_pitch_bound_v(pattern);
    REQUIRE_THAT(static_cast<double>(bound), WithinAbs(3.0, 1e-6));

    StageFixture fx(seqcat::stage_seq::make_stage_seq_node(pattern), kSr, kFrames);
    auto inj = fx.claim_injector();
    stage_baseline(inj);
    REQUIRE(inj.inject(immediate(seqcat::stage_seq::kSlideMs, 1.0f)) == InjectStatus::Ok);
    for (int b = 0; b < 4; ++b) {
        const auto out = fx.render({clock_line(16), flat(0.0f)});
        for (float v : out[0]) REQUIRE(std::fabs(v) <= bound + 1e-6f);
    }
}

TEST_CASE("Forge sequencing: no allocation in the baked render path",
          "[host][baked][forge][forge-sequencing][rt]") {
    // `ReusableRenderer` rather than the convenience `render()`, whose output
    // vectors would be attributed to the node under test.
    SECTION("stage_seq") {
        StageFixture fx(seqcat::stage_seq::make_stage_seq_node(), kSr, kFrames);
        auto inj = fx.claim_injector();
        stage_baseline(inj);
        pulp::test::ReusableRenderer<2> r(fx, {clock_line(8), flat(0.0f)});
        require_allocates_no_memory([&] {
            for (int b = 0; b < 8; ++b) r.render();
        });
    }

    SECTION("cartesian_walk") {
        CartFixture fx(seqcat::cartesian::make_cartesian_walk_node(), kSr, kFrames);
        auto inj = fx.claim_injector();
        cart_baseline(inj);
        pulp::test::ReusableRenderer<3> r(fx, {clock_line(8), clock_line(24), flat(0.0f)});
        require_allocates_no_memory([&] {
            for (int b = 0; b < 8; ++b) r.render();
        });
    }

    SECTION("rungler") {
        RunglerFixture fx(seqcat::rungler::make_rungler_node(), kSr, kFrames);
        auto inj = fx.claim_injector();
        rungler_baseline(inj);
        pulp::test::ReusableRenderer<2> r(fx, {clock_line(8), flat(0.0f)});
        require_allocates_no_memory([&] {
            for (int b = 0; b < 8; ++b) r.render();
        });
    }

    SECTION("quantize_scale") {
        QuantFixture fx(seqcat::quantize::make_quantize_scale_node(), kSr, kFrames);
        auto inj = fx.claim_injector();
        quant_baseline(inj);
        pulp::test::ReusableRenderer<1> r(fx, {flat(0.37f)});
        require_allocates_no_memory([&] {
            for (int b = 0; b < 8; ++b) r.render();
        });
    }

    SECTION("gate_logic") {
        pulp::test::BakedNodeFixture<2> fx(seqcat::gate_logic::make_gate_logic_node(), kSr,
                                           kFrames);
        auto inj = fx.claim_injector();
        REQUIRE(inj.inject(immediate(seqcat::gate_logic::kOp, 2.0f)) == InjectStatus::Ok);
        pulp::test::ReusableRenderer<2> r(fx, {clock_line(8), clock_line(12)});
        require_allocates_no_memory([&] {
            for (int b = 0; b < 8; ++b) r.render();
        });
    }

    SECTION("prob_gate") {
        ProbFixture fx(seqcat::prob_gate::make_prob_gate_node(), kSr, kFrames);
        auto inj = fx.claim_injector();
        REQUIRE(inj.inject(immediate(seqcat::prob_gate::kProbabilityPct, 50.0f)) ==
                InjectStatus::Ok);
        pulp::test::ReusableRenderer<1> r(fx, {clock_line(4)});
        require_allocates_no_memory([&] {
            for (int b = 0; b < 8; ++b) r.render();
        });
    }
}

TEST_CASE("Forge sequencing: every node's params are declared with sane ranges",
          "[host][baked][forge][forge-sequencing]") {
    // A declared range is the module's canonical contract, and a default outside
    // its own range is a bug that only shows up when someone automates the knob.
    const CustomNodeType types[] = {
        seqcat::stage_seq::make_stage_seq_node(),
        seqcat::cartesian::make_cartesian_walk_node(),
        seqcat::cartesian::make_cartesian_walk_node(seqcat::cartesian::default_grid(), true),
        seqcat::rungler::make_rungler_node(),
        seqcat::quantize::make_quantize_scale_node(),
        seqcat::gate_logic::make_gate_logic_node(),
        seqcat::prob_gate::make_prob_gate_node(),
    };

    std::set<std::string> ids;
    for (const auto& t : types) {
        REQUIRE(ids.insert(t.type_id).second);  // no duplicate registrations
        REQUIRE(t.lowerable);
        REQUIRE(t.num_output_ports >= 1);
        REQUIRE_FALSE(t.baked_params.empty());
        REQUIRE(t.process_instance_baked_param);
        REQUIRE(t.create);
        REQUIRE(t.destroy);
        REQUIRE(t.prepare);
        REQUIRE(t.reset);

        std::set<pulp::state::ParamID> param_ids;
        for (const auto& p : t.baked_params) {
            REQUIRE(param_ids.insert(p.id).second);  // node-local ids are unique
            REQUIRE(p.min_value < p.max_value);
            REQUIRE(p.default_value >= p.min_value);
            REQUIRE(p.default_value <= p.max_value);
        }
    }
}
