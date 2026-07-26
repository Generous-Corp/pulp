// modular_sequencing.hpp — the triggers / resets / esoteric-sequencing
// acceptance suite (spec tests 1–12).
//
// Expected values are COMPUTED from the shipped constants or from an
// independent reference written out here, never restated as bare literals. The
// two places the spec hands over a fully worked example — the rungler's first
// shift and the quantizer's hysteresis boundary — are checked in both
// directions: the reference implementation in this file is first asserted
// against the spec's worked numbers, and the header is then asserted against
// the reference. A single agreeing implementation would only prove the header
// agrees with itself.
//
// ── Measurement recipe ────────────────────────────────────────────────────
//
// Everything here is control-rate and exact. Clocks are one-sample booleans at
// a stated period; gates are read as a boolean per sample; pitch is read in
// volts on the 1 V/octave standard. `kSr` is 48 kHz so the shipped 30 ms slide
// is an exact 1440 samples and the timing assertions have no rounding slack to
// hide in. There is no FFT, no window and no tolerance-by-taste in this suite:
// where a number is not exact it is because a `double` accumulated, and the
// tolerance says so.
//
// Sequence assertions sample the outputs AT the clock sample, because that is
// where the spec's gate-mode tables are written ("over the three clocks of that
// stage the gate is high, low, low"). Duty assertions instead count samples
// between clocks, which is the only place a duty can live.
//
// ── Six spec defects, with the arithmetic ─────────────────────────────────
//
// **D1 — §3's general rule contradicts its own `RunglerT` row.** The "single
// rule that removes the ambiguity" says a reset edge "leaves every continuous
// output (pitch CV, DAC voltage) holding its last value until the next clock".
// The `RunglerT` row two lines above says the reset edge sets "register→seed
// pattern, out=DAC(seed)" and calls it "the only tool where it does". Both
// cannot hold. The specific row wins here, and the suite asserts it: the
// general rule's rationale is "so no click", which is about a pitch CV feeding
// an oscillator, whereas §7 states the rungler's stepping IS the sound. Holding
// the old voltage would also make the row's stated purpose — re-pinning a
// rungler line live — impossible, since the first clock after the reset already
// shifts the register once.
//
// **D2 — `Xorshift32::next_unipolar()` does not exist.** §9 specifies the
// `ProbGateT` draw as `u = rng.next_unipolar()`; the shipped `rng.hpp` spells
// the `[0,1)` draw `next_unit()` (`next_bipolar()` is the `[-1,1)` one). Naming
// only — the semantics match — but a literal implementation does not compile.
//
// **D3 — the kit's `TriggerDetectT` has no refractory.** §3's row for it says
// `reset()` sets "armed=false, refractory=0" and §4 gives `TransportEdgeT` a
// refractory "via an embedded `TriggerDetectT`". The shipped detector has a
// hysteresis window and nothing else; `reset()` sets armed=TRUE (so the next
// high sample is an edge), and there is no refractory state to zero. The
// refractory therefore lives in `TransportEdgeT` itself, layered on top of the
// kit's edge, and this suite asserts it there. Pushing it down into
// `TriggerDetectT` would be wrong: a debounced CLOCK silently drops fast
// subdivisions.
//
// **D4 — pingpong's stated period `2N−2` is degenerate at N = 1.** `2·1−2 = 0`
// is not a period. Guarded (a one-stage pattern stays on its stage) and
// asserted; at N = 2 the formula gives 2, which is correct and coincides with
// forward.
//
// **D5 — `repeat` mode's duty is unmeasurable on the first clock after a
// reset** (law 6, physical achievability). Test 3 asks for "3 pulses at
// `repeat_duty_pct`/100 high fraction ±1 sample", but a duty needs a period and
// §4 rule 5 forbids the block from generating its own clock — so on the first
// clock it has never seen a period. This is the constraint `ClockMultT` already
// documents. The block holds the gate high for the whole first pulse and is
// exact from the second clock on; the suite asserts both halves.
//
// **D6 — §6 leaves `access_mode`'s row-major behaviour open** ("X advances, Y
// on wrap-guard optional"), which §0 defines as a spec bug. Closed here: in
// row-major an X wrap carries into Y and the Y clock input is IGNORED. Carrying
// *and* clocking Y would advance the row twice per wrap, and there is no third
// reading that leaves both inputs meaningful.
//
// **D7 — two shipped quantizer constants are mutually inconsistent above
// EDO-30.** §8 gives the hysteresis a 20-cent default and the EDO mode a range
// of 1–48. One EDO-31 step is 1200/31 = 38.71 cents and its boundary sits 19.35
// cents away, so the travel needed to change step is 19.35 + 20 = 39.35 cents —
// MORE than the 38.71 cents an adjacent step is away. Above EDO-30 the spec as
// written makes the next step unreachable and the output lags a monotone input
// by a step forever, which also makes §11 test 7's "round-trip of any exact step
// voltage is identity" unachievable (law 6). Found by that very test failing at
// EDO-31 and EDO-48. Resolved in the header by capping the window at
// `kMaxHystSteps` (0.45 of a step) instead of weakening the test; the cap is
// inactive at every division at or below EDO-24, so the spec's worked example
// and the 12-TET and quarter-tone defaults are untouched.
//
// **D8 — a landing on a skipped stage 0.** Not a spec defect but a real bug
// this suite caught: §5 says skipped stages are stepped over "in every mode",
// and the first implementation validated the playhead only at reset time, so a
// pattern whose stage 0 was skipped played it once, on its downbeat. The
// landing is now re-validated at the clock. Kept as a named case rather than
// folded into the skip test, because it is the only path where a skip flag is
// read at a moment other than an advance.
//
// ── One place this suite extends the spec rather than following it ────────
//
// §5 gives `StageSeqT` the rule "the first clock after a reset lands on stage
// 0's first pulse"; §6 states no equivalent for `CartesianWalkT`. Applying §4
// rule 4 uniformly, the Cartesian walk's first clock after a reset lands ON the
// home cell rather than one past it. Without that, "reset then raise run"
// (§4 rule 3, "play from the top") would start the two sequencers in this
// header on different beats of their own patterns.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/modular_sequencing.hpp>
#include <pulp/signal/slew_limiter.hpp>
#include <pulp/signal/trigger_kit.hpp>
#include <pulp/signal/units.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;

namespace {

constexpr double kSr = 48000.0;

/// Float-comparison epsilon for quantizer voltages (spec test 7's
/// `kFloatCompareEps`). At the ~0.0417 V magnitude of an EDO-24 step a `float`
/// ULP is about 4 nV, so 1 µV is ~250× the representable rounding floor: it
/// absorbs accumulation across a round trip without masking a mistuning, since
/// the smallest musically meaningful error (one cent at that pitch) is five
/// orders of magnitude larger.
/// [design parameter] default 1e-6 V, range 1e-7 .. 1e-5 V.
constexpr double kFloatCompareEps = 1e-6;

template <typename Fn>
void require_allocates_no_memory(Fn&& fn) {
    pulp::test::RtAllocationProbe probe;
    fn();
    REQUIRE(probe.allocation_count() == 0);
}

/// A one-sample clock edge every `period` samples, starting at sample 0.
struct ClockLine {
    int period = 1;
    std::int64_t n = 0;

    bool tick() {
        const bool edge = (n % period) == 0;
        ++n;
        return edge;
    }
};

/// Marsaglia's (13, 17, 5) xorshift written out independently of `rng.hpp`, so
/// a determinism assertion checks the header's use of the generator rather than
/// calling the same code twice and agreeing with itself.
struct RefXorshift {
    std::uint32_t s;

    explicit RefXorshift(std::uint32_t seed) : s(seed == 0u ? 0x9E3779B9u : seed) {}

    std::uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }

    double unit() { return static_cast<double>(next()) * (1.0 / 4294967296.0); }
};

/// The rungler's shift/DAC topology, written out from §7's prose rather than
/// from the header.
struct RefRungler {
    int bits = 8;
    int dac = 3;
    int tap = 0;
    double range = 2.0;
    std::uint32_t reg = 0;

    std::uint32_t mask() const { return (1u << bits) - 1u; }
    std::uint32_t dac_mask() const { return (1u << dac) - 1u; }
    int code() const { return static_cast<int>(reg & dac_mask()); }

    double out() const {
        return range * (2.0 * static_cast<double>(code()) /
                            static_cast<double>(dac_mask()) -
                        1.0);
    }

    void clock(bool data) {
        const std::uint32_t last = (reg >> (bits - 1)) & 1u;
        const std::uint32_t t = (reg >> tap) & 1u;
        const std::uint32_t new_bit = (last ^ t) ^ (data ? 1u : 0u);
        reg = ((reg << 1) | new_bit) & mask();
    }
};

/// Round half up, the rounding §8 specifies for the quantizer.
int round_half_up(double x) { return static_cast<int>(std::floor(x + 0.5)); }

/// Independent mask snap: nearest enabled pitch class, searched outward, ties
/// upward. Written from §8's prose.
int ref_snap(int semitone, std::uint16_t mask, int root_pc) {
    if (mask == 0u) return semitone;
    const auto allowed = [&](int st) {
        int pc = (st - root_pc) % 12;
        if (pc < 0) pc += 12;
        return ((mask >> pc) & 1u) != 0u;
    };
    for (int d = 0; d <= 12; ++d) {
        if (allowed(semitone + d)) return semitone + d;
        if (d > 0 && allowed(semitone - d)) return semitone - d;
    }
    return semitone;
}

/// One clock's worth of `StageSeqT` output, sampled at the clock sample.
struct StageObservation {
    int stage = 0;
    int pulse = 0;
    bool gate = false;
    double pitch = 0.0;
};

/// Runs `clocks` clock edges at `period` samples apart and records the state at
/// each clock sample.
std::vector<StageObservation> run_stage_seq(StageSeq64& seq, int clocks, int period) {
    std::vector<StageObservation> out;
    out.reserve(static_cast<std::size_t>(clocks));
    ClockLine clk{period};
    const std::int64_t total = static_cast<std::int64_t>(clocks) * period;
    for (std::int64_t i = 0; i < total; ++i) {
        const bool edge = clk.tick();
        const auto f = seq.process(true, false, edge);
        if (edge) out.push_back({seq.stage(), seq.pulse(), f.gate, f.pitch_v});
    }
    return out;
}

/// A four-stage, one-pulse-per-stage sequencer — the shape every direction test
/// wants, so the walk order is the only thing under test.
void configure_walk(StageSeq64& seq, int stages, SeqDirection dir) {
    seq.prepare(kSr);
    seq.set_num_stages(stages);
    seq.set_direction(dir);
    for (int s = 0; s < stages; ++s) {
        seq.set_stage_pulse_count(s, 1);
        seq.set_stage_gate_mode(s, StageGateMode::hold);
        seq.set_stage_pitch(s, static_cast<double>(s));
    }
}

}  // namespace

// ── Test 1: reset-matrix conformance ──────────────────────────────────────

TEST_CASE("Reset matrix: StageSeq reset edge zeroes position and holds pitch",
          "[signal][sequencing][reset]") {
    StageSeq64 seq;
    configure_walk(seq, 4, SeqDirection::forward);
    seq.set_stage_pitch(2, 0.5);

    // Walk to stage 2 so there is a position worth zeroing.
    auto obs = run_stage_seq(seq, 3, 100);
    REQUIRE(obs.back().stage == 2);
    REQUIRE(seq.gate());
    const double held = seq.pitch_v();
    REQUIRE_THAT(held, WithinAbs(0.5, 1e-12));

    seq.apply_reset_edge();

    // Position to the top of the pattern, gate low.
    REQUIRE(seq.stage() == 0);
    REQUIRE(seq.pulse() == 0);
    REQUIRE_FALSE(seq.gate());
    REQUIRE_FALSE(seq.started());

    // Pitch CV holds its last value — the reset edge must not click.
    REQUIRE_THAT(seq.pitch_v(), WithinAbs(held, 1e-12));

    // And it still holds through un-clocked samples after the reset.
    for (int i = 0; i < 64; ++i) {
        const auto f = seq.process(true, false, false);
        REQUIRE_FALSE(f.gate);
        REQUIRE_THAT(f.pitch_v, WithinAbs(held, 1e-12));
    }
}

TEST_CASE("Reset matrix: StageSeq reset edge does not advance or rewind the RNG",
          "[signal][sequencing][reset][determinism]") {
    constexpr int kStages = 4;

    // Ground truth: the stage index drawn on each ADVANCING clock is
    // `next_uint() % N`. A landing clock (the first after a reset) consumes no
    // draw, because it lands on stage 0 rather than drawing.
    RefXorshift ref(StageSeq64::kRandomSeed);

    StageSeq64 seq;
    configure_walk(seq, kStages, SeqDirection::random);

    auto first = run_stage_seq(seq, 11, 32);
    REQUIRE(first.front().stage == 0);  // landing clock
    for (std::size_t i = 1; i < first.size(); ++i)
        REQUIRE(first[i].stage == static_cast<int>(ref.next() % kStages));

    // A reset edge mid-pattern: the NEXT clock lands on stage 0 without a draw,
    // and the stream then continues from exactly where it was.
    seq.apply_reset_edge();
    auto second = run_stage_seq(seq, 11, 32);
    REQUIRE(second.front().stage == 0);
    for (std::size_t i = 1; i < second.size(); ++i)
        REQUIRE(second[i].stage == static_cast<int>(ref.next() % kStages));

    // `reset()` (verb 1), by contrast, rewinds the stream.
    seq.reset();
    RefXorshift rewound(StageSeq64::kRandomSeed);
    auto third = run_stage_seq(seq, 11, 32);
    for (std::size_t i = 1; i < third.size(); ++i)
        REQUIRE(third[i].stage == static_cast<int>(rewound.next() % kStages));
}

TEST_CASE("Reset matrix: CartesianWalk reset edge homes both counters and holds CV",
          "[signal][sequencing][reset]") {
    CartesianWalk64 walk;
    walk.prepare(kSr);
    walk.set_size(4, 4);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) walk.set_value(x, y, 0.1 * (y * 4 + x));

    ClockLine xc{16};
    ClockLine yc{64};
    double last = 0.0;
    for (int i = 0; i < 16 * 6; ++i) {
        const auto f = walk.process(true, false, xc.tick(), yc.tick());
        last = f.cv;
    }
    REQUIRE(walk.cell_x() != 0);

    walk.apply_reset_edge();
    REQUIRE(walk.x() == 0);
    REQUIRE(walk.y() == 0);
    REQUIRE_FALSE(walk.gate());
    REQUIRE_THAT(walk.cv(), WithinAbs(last, 1e-12));

    // CV keeps holding across un-clocked samples.
    for (int i = 0; i < 32; ++i) {
        const auto f = walk.process(true, false, false, false);
        REQUIRE_FALSE(f.gate);
        REQUIRE_THAT(f.cv, WithinAbs(last, 1e-12));
    }
}

TEST_CASE("Reset matrix: Rungler reset edge restores the seed pattern and its DAC level",
          "[signal][sequencing][reset]") {
    // D1: this is the documented exception to "continuous outputs hold".
    Rungler64 r;
    r.prepare(kSr);

    RefRungler ref{Rungler64::kDefaultBits, Rungler64::kDefaultDacBits,
                   Rungler64::kFeedbackTap, Rungler64::kRangeV,
                   Rungler64::kSeedPattern};
    const double seed_level = ref.out();

    REQUIRE(r.register_bits() == Rungler64::kSeedPattern);
    REQUIRE_THAT(r.value(), WithinAbs(seed_level, 1e-12));

    for (int i = 0; i < 37; ++i) (void)r.process(true, false, true);
    REQUIRE(r.register_bits() != Rungler64::kSeedPattern);

    r.apply_reset_edge();
    REQUIRE(r.register_bits() == Rungler64::kSeedPattern);
    REQUIRE_THAT(r.value(), WithinAbs(seed_level, 1e-12));
}

TEST_CASE("Reset matrix: quantizer reset edge clears the hysteresis latch",
          "[signal][sequencing][reset]") {
    QuantizeScale64 q;
    q.set_mode(QuantizeMode::edo);
    q.set_edo(12);

    // Latch onto semitone 4, then move just inside the window so the latch holds.
    (void)q.process(4.0 / 12.0);
    REQUIRE(q.latched_step() == 4);
    const double inside = (4.0 - 0.6) / 12.0;  // rounds to 3, inside 0.5 + 0.2
    REQUIRE_THAT(q.process(inside), WithinAbs(4.0 / 12.0, kFloatCompareEps));
    REQUIRE(q.latched_step() == 4);

    // Cleared: the same input now quantizes on its own merits.
    q.apply_reset_edge();
    REQUIRE_THAT(q.process(inside), WithinAbs(3.0 / 12.0, kFloatCompareEps));
    REQUIRE(q.latched_step() == 3);
}

TEST_CASE("Reset matrix: GateLogic reset is a no-op on a combinational block",
          "[signal][sequencing][reset]") {
    GateLogic64 g;
    g.set_op(GateOp::logic_xor);
    const bool before = g.process(true, false);
    g.apply_reset_edge();
    REQUIRE(g.process(true, false) == before);
    g.reset();
    REQUIRE(g.process(true, false) == before);
    REQUIRE(g.op() == GateOp::logic_xor);  // configuration survives
}

TEST_CASE("Reset matrix: ProbGate reset edge does not rewind randomness",
          "[signal][sequencing][reset][determinism]") {
    const auto run = [](ProbGate64& p, int n) {
        std::vector<char> out;
        out.reserve(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) out.push_back(p.process_edge(true) ? 1 : 0);
        return out;
    };

    ProbGate64 a;
    const auto reference = run(a, 10);

    ProbGate64 b;
    auto head = run(b, 5);
    b.apply_reset_edge();  // verb 2: latch only
    const auto tail = run(b, 5);
    for (std::size_t i = 0; i < 5; ++i) REQUIRE(tail[i] == reference[5 + i]);
    REQUIRE(b.draw_count() == 10u);

    ProbGate64 c;
    (void)run(c, 5);
    c.reset();  // verb 1: rewind
    const auto rewound = run(c, 5);
    for (std::size_t i = 0; i < 5; ++i) REQUIRE(rewound[i] == reference[i]);
    REQUIRE(c.draw_count() == 5u);
}

TEST_CASE("Reset matrix: the trigger-kit rows this header composes",
          "[signal][sequencing][reset]") {
    // §3 also tabulates the kit blocks these sequencers are built from. Their
    // reset edge IS the kit's `reset()`; each row is asserted here because this
    // header depends on the behaviour.

    SECTION("TriggerDetectT never emits a trigger from a reset") {
        TriggerDetectT<double> d;
        REQUIRE(d.process(1.0));    // armed → edge
        REQUIRE_FALSE(d.process(1.0));
        d.reset();
        // The reset itself emits nothing; the next high sample is an edge, even
        // though the input never fell.
        REQUIRE(d.process(1.0));
    }

    SECTION("GateGenT reset forces a hung gate low") {
        GateGenT<double> g;
        g.prepare(kSr);
        g.set_length_ms(100.0);
        REQUIRE_THAT(g.process(1.0), WithinAbs(1.0, 1e-12));
        REQUIRE(g.open());
        g.reset();
        REQUIRE_FALSE(g.open());
        REQUIRE_THAT(g.process(0.0), WithinAbs(0.0, 1e-12));
    }

    SECTION("ClockDividerT reset makes the next clock the downbeat") {
        ClockDividerT<double> d;
        d.set_division(4);
        REQUIRE(d.process(1.0));  // the "1"
        (void)d.process(0.0);
        REQUIRE_FALSE(d.process(1.0));
        (void)d.process(0.0);
        d.reset();
        REQUIRE(d.process(1.0));  // downbeat again, not 2 edges later
    }

    SECTION("SignalClockMultT reset aborts the in-flight subdivision burst") {
        SignalClockMultT<double> m;
        m.prepare(kSr);
        m.set_multiple(4);
        // Two edges 400 samples apart establish a period.
        REQUIRE(m.process(1.0));
        for (int i = 0; i < 399; ++i) (void)m.process(0.0);
        REQUIRE(m.process(1.0));
        m.reset();
        int emitted = 0;
        for (int i = 0; i < 400; ++i)
            if (m.process(0.0)) ++emitted;
        REQUIRE(emitted == 0);  // no orphan ticks
    }

    SECTION("BurstGenT reset aborts the burst") {
        BurstGenT<double> b;
        b.prepare(kSr);
        b.set_count(8);
        b.set_interval_ms(1.0);
        REQUIRE(b.process(1.0));
        REQUIRE(b.busy());
        b.reset();
        REQUIRE_FALSE(b.busy());
        int emitted = 0;
        for (int i = 0; i < static_cast<int>(kSr / 100.0); ++i)
            if (b.process(0.0)) ++emitted;
        REQUIRE(emitted == 0);
    }

    SECTION("TrigDelayT reset flushes the queued trigger rather than firing it") {
        TrigDelayT<double> t;
        t.prepare(kSr);
        t.set_delay_ms(10.0);
        REQUIRE_FALSE(t.process(1.0));
        t.reset();
        int emitted = 0;
        for (int i = 0; i < static_cast<int>(kSr / 10.0); ++i)
            if (t.process(0.0)) ++emitted;
        REQUIRE(emitted == 0);
    }

    SECTION("SampleHoldT reset zeroes the held value") {
        SampleHold64 sh;
        REQUIRE_THAT(sh.process_signal(0.75, 1.0), WithinAbs(0.75, 1e-12));
        sh.reset();
        REQUIRE_THAT(sh.value(), WithinAbs(0.0, 1e-12));
    }
}

// ── Test 2: transport order of operations ─────────────────────────────────

TEST_CASE("Transport: reset and clock in the same sample fire the downbeat on that clock",
          "[signal][sequencing][transport]") {
    StageSeq64 seq;
    configure_walk(seq, 4, SeqDirection::forward);
    seq.set_stage_pitch(0, -0.25);

    // Walk away from the top first, so landing on stage 0 is a real assertion.
    (void)run_stage_seq(seq, 3, 8);
    REQUIRE(seq.stage() == 2);

    // Rule 4: reset wins, then the clock advances from the top — so the very
    // sample carrying both is stage 0's first pulse.
    const auto f = seq.process(true, true, true);
    REQUIRE(seq.stage() == 0);
    REQUIRE(seq.pulse() == 0);
    REQUIRE(f.gate);
    REQUIRE_THAT(f.pitch_v, WithinAbs(-0.25, 1e-12));
}

TEST_CASE("Transport: run is a level — clocks are ignored while stopped, gate forced low",
          "[signal][sequencing][transport]") {
    StageSeq64 seq;
    configure_walk(seq, 4, SeqDirection::forward);

    auto obs = run_stage_seq(seq, 3, 8);
    REQUIRE(obs.back().stage == 2);
    REQUIRE(seq.gate());
    const double held = seq.pitch_v();

    // Stopped: clock edges do nothing, the gate is low, position survives.
    for (int i = 0; i < 40; ++i) {
        const auto f = seq.process(false, false, true);
        REQUIRE_FALSE(f.gate);
        REQUIRE_THAT(f.pitch_v, WithinAbs(held, 1e-12));
    }
    REQUIRE(seq.stage() == 2);
    REQUIRE(seq.pulse() == 0);

    // Continue (not restart): the next clock resumes from stage 2.
    const auto resumed = seq.process(true, false, true);
    REQUIRE(seq.stage() == 3);
    REQUIRE(resumed.gate);
}

TEST_CASE("Transport: a reset while stopped arms the pattern for the next run",
          "[signal][sequencing][transport]") {
    StageSeq64 seq;
    configure_walk(seq, 4, SeqDirection::forward);
    (void)run_stage_seq(seq, 3, 8);

    (void)seq.process(false, true, false);  // reset while stopped
    REQUIRE(seq.stage() == 0);
    REQUIRE_FALSE(seq.started());

    const auto f = seq.process(true, false, true);
    REQUIRE(seq.stage() == 0);
    REQUIRE(seq.pulse() == 0);
    REQUIRE(f.gate);
}

TEST_CASE("Transport: TransportEdgeT decodes a level pair into run and a debounced edge",
          "[signal][sequencing][transport]") {
    TransportEdge64 t;
    t.prepare(kSr);

    // Run is a level.
    REQUIRE_FALSE(t.process(0.0, 0.0, 0.0).run_high);
    REQUIRE(t.process(1.0, 0.0, 0.0).run_high);
    REQUIRE(t.process(1.0, 0.0, 0.0).run_high);
    REQUIRE_FALSE(t.process(0.0, 0.0, 0.0).run_high);

    // Reset is an edge, and only one per crossing.
    REQUIRE(t.process(1.0, 1.0, 0.0).reset_edge);
    REQUIRE_FALSE(t.process(1.0, 1.0, 0.0).reset_edge);

    // D3: the refractory is this block's, not the kit detector's. Inside the
    // window a fresh crossing is swallowed; outside it, it is honoured.
    const int window =
        static_cast<int>(std::llround(units::ms_to_samples(TransportEdge64::kRefractoryMs, kSr)));
    REQUIRE(window > 1);
    (void)t.process(1.0, 0.0, 0.0);
    REQUIRE_FALSE(t.process(1.0, 1.0, 0.0).reset_edge);

    t.reset();
    REQUIRE(t.process(1.0, 1.0, 0.0).reset_edge);
    for (int i = 0; i < window + 2; ++i) (void)t.process(1.0, 0.0, 0.0);
    REQUIRE(t.process(1.0, 1.0, 0.0).reset_edge);

    // The clock is NOT debounced: consecutive crossings all pass.
    t.reset();
    int clocks = 0;
    for (int i = 0; i < 8; ++i) {
        if (t.process(1.0, 0.0, 1.0).clock_edge) ++clocks;
        if (t.process(1.0, 0.0, 0.0).clock_edge) ++clocks;
    }
    REQUIRE(clocks == 8);
}

// ── Test 3: StageSeq gate modes and directions ────────────────────────────

TEST_CASE("StageSeq gate modes over a three-pulse stage", "[signal][sequencing][stageseq]") {
    const auto pattern = [](StageGateMode mode) {
        StageSeq64 seq;
        seq.prepare(kSr);
        seq.set_num_stages(1);
        seq.set_stage_pulse_count(0, 3);
        seq.set_stage_gate_mode(0, mode);
        auto obs = run_stage_seq(seq, 3, 200);
        return std::vector<bool>{obs[0].gate, obs[1].gate, obs[2].gate};
    };

    REQUIRE(pattern(StageGateMode::hold) == std::vector<bool>{true, true, true});
    REQUIRE(pattern(StageGateMode::single) == std::vector<bool>{true, false, false});
    REQUIRE(pattern(StageGateMode::rest) == std::vector<bool>{false, false, false});
    // `repeat` re-pulses on every clock, so it too reads high AT each clock.
    REQUIRE(pattern(StageGateMode::repeat) == std::vector<bool>{true, true, true});
}

TEST_CASE("StageSeq rest consumes its clocks — silence with duration",
          "[signal][sequencing][stageseq]") {
    StageSeq64 seq;
    seq.prepare(kSr);
    seq.set_num_stages(2);
    seq.set_stage_pulse_count(0, 3);
    seq.set_stage_gate_mode(0, StageGateMode::rest);
    seq.set_stage_pulse_count(1, 1);
    seq.set_stage_gate_mode(1, StageGateMode::hold);

    auto obs = run_stage_seq(seq, 5, 64);
    REQUIRE(obs[0].stage == 0);
    REQUIRE(obs[1].stage == 0);
    REQUIRE(obs[2].stage == 0);
    REQUIRE(obs[3].stage == 1);  // three clocks of silence, then the next stage
    REQUIRE_FALSE(obs[0].gate);
    REQUIRE_FALSE(obs[2].gate);
    REQUIRE(obs[3].gate);
}

TEST_CASE("StageSeq repeat duty is exact once a period has been measured",
          "[signal][sequencing][stageseq]") {
    constexpr int kPeriod = 480;
    StageSeq64 seq;
    seq.prepare(kSr);
    seq.set_num_stages(1);
    seq.set_stage_pulse_count(0, 4);
    seq.set_stage_gate_mode(0, StageGateMode::repeat);

    ClockLine clk{kPeriod};
    // D5: the first pulse has no measured period, so the gate stays high for
    // the whole of it. Asserted rather than glossed.
    int first_high = 0;
    for (int i = 0; i < kPeriod; ++i)
        if (seq.process(true, false, clk.tick()).gate) ++first_high;
    REQUIRE(first_high == kPeriod);

    // From the second clock on the duty is exact: `since_clock < duty · period`
    // is high on offsets 0 .. ceil(duty·period) − 1.
    const int expected_high =
        static_cast<int>(std::ceil(StageSeq64::kRepeatDuty * static_cast<double>(kPeriod)));
    for (int pulse = 0; pulse < 3; ++pulse) {
        int high = 0;
        for (int i = 0; i < kPeriod; ++i)
            if (seq.process(true, false, clk.tick()).gate) ++high;
        REQUIRE(std::abs(high - expected_high) <= 1);
    }
}

TEST_CASE("StageSeq walk orders are exact in every direction",
          "[signal][sequencing][stageseq]") {
    SECTION("forward") {
        StageSeq64 seq;
        configure_walk(seq, 4, SeqDirection::forward);
        auto obs = run_stage_seq(seq, 9, 8);
        const std::vector<int> want{0, 1, 2, 3, 0, 1, 2, 3, 0};
        for (std::size_t i = 0; i < want.size(); ++i) REQUIRE(obs[i].stage == want[i]);
    }

    SECTION("reverse") {
        StageSeq64 seq;
        configure_walk(seq, 4, SeqDirection::reverse);
        auto obs = run_stage_seq(seq, 9, 8);
        const std::vector<int> want{0, 3, 2, 1, 0, 3, 2, 1, 0};
        for (std::size_t i = 0; i < want.size(); ++i) REQUIRE(obs[i].stage == want[i]);
    }

    SECTION("pingpong reflects without repeating the endpoints, period 2N−2") {
        for (int n = 2; n <= 6; ++n) {
            StageSeq64 seq;
            configure_walk(seq, n, SeqDirection::pingpong);
            const int period = 2 * n - 2;
            auto obs = run_stage_seq(seq, 3 * period + 1, 4);

            // Period computed from N, not restated.
            for (std::size_t i = 0; i + static_cast<std::size_t>(period) < obs.size(); ++i)
                REQUIRE(obs[i].stage == obs[i + static_cast<std::size_t>(period)].stage);

            // Endpoints appear exactly once per period — the reflection does not
            // sit on them for two clocks.
            int at_low = 0;
            int at_high = 0;
            for (int i = 0; i < period; ++i) {
                if (obs[static_cast<std::size_t>(i)].stage == 0) ++at_low;
                if (obs[static_cast<std::size_t>(i)].stage == n - 1) ++at_high;
            }
            REQUIRE(at_low == 1);
            REQUIRE(at_high == 1);
        }
    }

    SECTION("pingpong at N = 1 is guarded — D4") {
        // 2N − 2 = 0 is not a period. A one-stage pattern stays put.
        StageSeq64 seq;
        configure_walk(seq, 1, SeqDirection::pingpong);
        auto obs = run_stage_seq(seq, 8, 4);
        for (const auto& o : obs) REQUIRE(o.stage == 0);
    }

    SECTION("random is reproducible and matches the reference stream") {
        StageSeq64 seq;
        configure_walk(seq, 8, SeqDirection::random);
        auto first = run_stage_seq(seq, 33, 4);

        RefXorshift ref(StageSeq64::kRandomSeed);
        for (std::size_t i = 1; i < first.size(); ++i)
            REQUIRE(first[i].stage == static_cast<int>(ref.next() % 8u));

        seq.reset();
        auto second = run_stage_seq(seq, 33, 4);
        for (std::size_t i = 0; i < first.size(); ++i)
            REQUIRE(first[i].stage == second[i].stage);
    }
}

TEST_CASE("StageSeq steps over skipped stages in every direction",
          "[signal][sequencing][stageseq]") {
    SECTION("forward") {
        StageSeq64 seq;
        configure_walk(seq, 4, SeqDirection::forward);
        seq.set_stage_skip(1, true);
        seq.set_stage_skip(2, true);
        auto obs = run_stage_seq(seq, 5, 8);
        const std::vector<int> want{0, 3, 0, 3, 0};
        for (std::size_t i = 0; i < want.size(); ++i) REQUIRE(obs[i].stage == want[i]);
    }

    SECTION("a skipped stage 0 moves the reset landing forward") {
        StageSeq64 seq;
        configure_walk(seq, 4, SeqDirection::forward);
        seq.set_stage_skip(0, true);
        auto obs = run_stage_seq(seq, 4, 8);
        const std::vector<int> want{1, 2, 3, 1};
        for (std::size_t i = 0; i < want.size(); ++i) REQUIRE(obs[i].stage == want[i]);
    }

    SECTION("random rejection-samples past skips") {
        StageSeq64 seq;
        configure_walk(seq, 8, SeqDirection::random);
        for (int s = 0; s < 8; ++s) seq.set_stage_skip(s, s % 2 == 1);
        auto obs = run_stage_seq(seq, 64, 4);
        for (const auto& o : obs) REQUIRE(o.stage % 2 == 0);
    }
}

TEST_CASE("StageSeq all-skip guard: advance is a no-op and the gate stays low",
          "[signal][sequencing][stageseq]") {
    StageSeq64 seq;
    configure_walk(seq, 4, SeqDirection::forward);
    for (int s = 0; s < 4; ++s) {
        seq.set_stage_skip(s, true);
        seq.set_stage_pitch(s, 1.0 + s);
    }

    auto obs = run_stage_seq(seq, 200, 4);
    for (const auto& o : obs) {
        REQUIRE(o.stage == 0);
        REQUIRE_FALSE(o.gate);
        REQUIRE_THAT(o.pitch, WithinAbs(0.0, 1e-12));  // never entered a stage
    }
    REQUIRE_FALSE(seq.started());

    // The same guard holds in every direction, including random (which would
    // otherwise reject-sample forever).
    for (auto dir : {SeqDirection::reverse, SeqDirection::pingpong, SeqDirection::random}) {
        StageSeq64 s2;
        configure_walk(s2, 4, dir);
        for (int s = 0; s < 4; ++s) s2.set_stage_skip(s, true);
        auto o2 = run_stage_seq(s2, 100, 4);
        for (const auto& o : o2) REQUIRE_FALSE(o.gate);
    }
}

TEST_CASE("StageSeq: the first clock after a reset is stage 0's first pulse",
          "[signal][sequencing][stageseq]") {
    for (auto dir : {SeqDirection::forward, SeqDirection::reverse, SeqDirection::pingpong,
                     SeqDirection::random}) {
        StageSeq64 seq;
        configure_walk(seq, 5, dir);
        (void)run_stage_seq(seq, 7, 4);
        seq.apply_reset_edge();
        auto obs = run_stage_seq(seq, 1, 4);
        REQUIRE(obs.front().stage == 0);
        REQUIRE(obs.front().pulse == 0);
    }
}

// ── Test 4: StageSeq slide ────────────────────────────────────────────────

namespace {

/// Drives a two-stage sequencer whose second stage slides to `target_v`, and
/// returns the number of samples after the entering clock at which the pitch
/// first arrives.
int slide_arrival_samples(double target_v, bool slide_on) {
    constexpr int kPeriod = 8000;  // longer than any slide under test
    StageSeq64 seq;
    seq.prepare(kSr);
    seq.set_num_stages(2);
    for (int s = 0; s < 2; ++s) {
        seq.set_stage_pulse_count(s, 1);
        seq.set_stage_gate_mode(s, StageGateMode::hold);
    }
    seq.set_stage_pitch(0, 0.0);
    seq.set_stage_pitch(1, target_v);
    seq.set_stage_slide(1, slide_on);

    ClockLine clk{kPeriod};
    for (int i = 0; i < kPeriod; ++i) (void)seq.process(true, false, clk.tick());

    int arrival = -1;
    for (int i = 0; i < kPeriod; ++i) {
        const auto f = seq.process(true, false, clk.tick());
        if (arrival < 0 && std::abs(f.pitch_v - target_v) <= 1e-12) arrival = i;
    }
    return arrival;
}

}  // namespace

TEST_CASE("StageSeq slide is constant-time and interval-independent",
          "[signal][sequencing][stageseq][slide]") {
    // Expected duration computed from the shipped constant, not restated.
    const double slide_samples = units::ms_to_samples(StageSeq64::kSlideMs, kSr);
    const int expected = static_cast<int>(std::llround(slide_samples)) - 1;

    const int one_semitone = slide_arrival_samples(1.0 / 12.0, true);
    const int one_octave = slide_arrival_samples(1.0, true);

    REQUIRE(std::abs(one_semitone - expected) <= 1);
    REQUIRE(std::abs(one_octave - expected) <= 1);
    // The whole point of constant TIME: the two take the same time.
    REQUIRE(std::abs(one_semitone - one_octave) <= 1);

    // A non-slide stage steps within one sample.
    REQUIRE(slide_arrival_samples(1.0, false) == 0);
}

TEST_CASE("StageSeq slide freezes while stopped and resumes on run",
          "[signal][sequencing][stageseq][slide]") {
    StageSeq64 seq;
    seq.prepare(kSr);
    seq.set_num_stages(2);
    for (int s = 0; s < 2; ++s) {
        seq.set_stage_pulse_count(s, 1);
        seq.set_stage_gate_mode(s, StageGateMode::hold);
    }
    seq.set_stage_pitch(0, 0.0);
    seq.set_stage_pitch(1, 1.0);
    seq.set_stage_slide(1, true);

    (void)seq.process(true, false, true);   // land on stage 0
    (void)seq.process(true, false, true);   // enter stage 1, slide begins
    for (int i = 0; i < 200; ++i) (void)seq.process(true, false, false);
    const double mid = seq.pitch_v();
    REQUIRE(mid > 0.0);
    REQUIRE(mid < 1.0);

    // Order of operations rule (b) returns before the smoother updates, so a
    // stopped transport freezes the glide where it stood.
    for (int i = 0; i < 500; ++i) {
        const auto f = seq.process(false, false, false);
        REQUIRE_THAT(f.pitch_v, WithinAbs(mid, 1e-12));
    }
    const auto resumed = seq.process(true, false, false);
    REQUIRE(resumed.pitch_v > mid);
}

// ── Test 5: Cartesian walk ────────────────────────────────────────────────

TEST_CASE("Cartesian: an X-only clock loops row 0 in exact cell order",
          "[signal][sequencing][cartesian]") {
    CartesianWalk64 walk;
    walk.prepare(kSr);
    walk.set_size(4, 4);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) walk.set_value(x, y, y + 0.1 * x);

    ClockLine xc{10};
    std::vector<int> cells;
    int gates = 0;
    for (int i = 0; i < 10 * 9; ++i) {
        const bool edge = xc.tick();
        const auto f = walk.process(true, false, edge, false);
        if (edge) {
            cells.push_back(walk.cell_x());
            REQUIRE(walk.cell_y() == 0);
            REQUIRE_THAT(f.cv, WithinAbs(0.1 * walk.cell_x(), 1e-12));
        }
        if (f.gate) ++gates;
    }
    const std::vector<int> want{0, 1, 2, 3, 0, 1, 2, 3, 0};
    REQUIRE(cells == want);
    REQUIRE(gates == 9);  // one per position-changing clock, including the downbeat
}

TEST_CASE("Cartesian: independent X and Y produce the closed-form counter walk",
          "[signal][sequencing][cartesian]") {
    const auto walk_cells = [](int x_period, int y_period, int clocks) {
        CartesianWalk64 w;
        w.prepare(kSr);
        w.set_size(4, 4);
        ClockLine xc{x_period};
        ClockLine yc{y_period};
        std::vector<std::pair<int, int>> cells;
        for (int i = 0; i < x_period * clocks; ++i) {
            const bool xe = xc.tick();
            const bool ye = yc.tick();
            (void)w.process(true, false, xe, ye);
            if (xe) cells.emplace_back(w.cell_x(), w.cell_y());
        }
        return cells;
    };

    SECTION("Y at four times the X period gives a 16-cell super-cycle") {
        const auto cells = walk_cells(10, 40, 33);
        for (std::size_t k = 0; k < cells.size(); ++k) {
            const int kk = static_cast<int>(k);
            REQUIRE(cells[k].first == kk % 4);
            REQUIRE(cells[k].second == (kk / 4) % 4);
        }
        // Period 16 and all 16 cells visited.
        for (std::size_t k = 0; k + 16 < cells.size(); ++k) REQUIRE(cells[k] == cells[k + 16]);
        bool seen[4][4] = {};
        for (int k = 0; k < 16; ++k)
            seen[cells[static_cast<std::size_t>(k)].second][cells[static_cast<std::size_t>(k)].first] =
                true;
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) REQUIRE(seen[y][x]);
    }

    SECTION("coprime periods give an lcm super-cycle in a non-obvious order") {
        const auto cells = walk_cells(10, 30, 37);
        for (std::size_t k = 0; k < cells.size(); ++k) {
            const int kk = static_cast<int>(k);
            REQUIRE(cells[k].first == kk % 4);
            REQUIRE(cells[k].second == (kk / 3) % 4);
        }
        // X repeats every 4 clocks, Y every 12 → lcm(4, 12) = 12.
        for (std::size_t k = 0; k + 12 < cells.size(); ++k) REQUIRE(cells[k] == cells[k + 12]);
        REQUIRE(cells[0] != cells[4]);  // not simply the X loop
    }
}

TEST_CASE("Cartesian: a simultaneous X and Y edge is one gate",
          "[signal][sequencing][cartesian]") {
    CartesianWalk64 w;
    w.prepare(kSr);
    w.set_size(4, 4);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) w.set_value(x, y, y * 4 + x);

    (void)w.process(true, false, true, true);  // downbeat, home cell
    REQUIRE(w.cell_x() == 0);
    REQUIRE(w.cell_y() == 0);

    int gates = 0;
    for (int i = 0; i < 8; ++i) {
        if (w.process(true, false, true, true).gate) ++gates;
        (void)w.process(true, false, false, false);
    }
    REQUIRE(gates == 8);           // eight clocks, eight gates — not sixteen
    REQUIRE(w.cell_x() == 8 % 4);  // both counters advanced once per clock
    REQUIRE(w.cell_y() == 8 % 4);
}

TEST_CASE("Cartesian: a 1x1 grid never changes position, so it never gates",
          "[signal][sequencing][cartesian]") {
    CartesianWalk64 w;
    w.prepare(kSr);
    w.set_size(1, 1);
    w.set_value(0, 0, 0.5);

    REQUIRE(w.process(true, false, true, true).gate);  // the downbeat still fires
    int gates = 0;
    for (int i = 0; i < 16; ++i) {
        if (w.process(true, false, true, true).gate) ++gates;
        (void)w.process(true, false, false, false);
    }
    REQUIRE(gates == 0);
    REQUIRE_THAT(w.cv(), WithinAbs(0.5, 1e-12));
}

TEST_CASE("Cartesian: CV offsets are read at clock time, not continuously",
          "[signal][sequencing][cartesian]") {
    CartesianWalk64 w;
    w.prepare(kSr);
    w.set_size(4, 4);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) w.set_value(x, y, y * 4 + x);

    (void)w.process(true, false, true, false);
    REQUIRE_THAT(w.cv(), WithinAbs(0.0, 1e-12));

    w.set_offsets(2, 1);
    // No clock yet: the output must not zipper to the new cell.
    for (int i = 0; i < 16; ++i) {
        const auto f = w.process(true, false, false, false);
        REQUIRE_THAT(f.cv, WithinAbs(0.0, 1e-12));
    }

    // The next clock reads the offset: counters (1,0) plus offset (2,1) = (3,1).
    const auto f = w.process(true, false, true, false);
    REQUIRE(w.cell_x() == 3);
    REQUIRE(w.cell_y() == 1);
    REQUIRE_THAT(f.cv, WithinAbs(7.0, 1e-12));

    // Negative offsets wrap rather than index out of range.
    w.set_offsets(-1, -1);
    (void)w.process(true, false, true, false);
    REQUIRE(w.cell_x() >= 0);
    REQUIRE(w.cell_x() < 4);
    REQUIRE(w.cell_y() >= 0);
    REQUIRE(w.cell_y() < 4);
}

TEST_CASE("Cartesian: row-major carries an X wrap into Y and ignores the Y clock — D6",
          "[signal][sequencing][cartesian]") {
    CartesianWalk64 w;
    w.prepare(kSr);
    w.set_size(4, 4);
    w.set_access(CartesianAccess::row_major);

    std::vector<std::pair<int, int>> cells;
    for (int k = 0; k < 9; ++k) {
        // A Y clock on every edge too: in row-major it must have no effect.
        (void)w.process(true, false, true, true);
        cells.emplace_back(w.cell_x(), w.cell_y());
        (void)w.process(true, false, false, false);
    }
    const std::vector<std::pair<int, int>> want{{0, 0}, {1, 0}, {2, 0}, {3, 0}, {0, 1},
                                                {1, 1}, {2, 1}, {3, 1}, {0, 2}};
    REQUIRE(cells == want);
}

// ── Test 6: Rungler determinism and bound ─────────────────────────────────

TEST_CASE("Rungler reproduces the worked shift sequence bit-exactly",
          "[signal][sequencing][rungler]") {
    // First: the reference in this file is checked against the spec's worked
    // example, so the header is then compared with independently-verified
    // ground truth rather than with a copy of itself.
    RefRungler ref{8, 3, 0, 2.0, 0b10110100u};
    REQUIRE(ref.code() == 4);
    REQUIRE_THAT(ref.out(), WithinAbs(2.0 * (2.0 * 4.0 / 7.0 - 1.0), 1e-12));
    ref.clock(false);
    REQUIRE(ref.reg == 0b01101001u);
    REQUIRE(ref.code() == 1);
    REQUIRE_THAT(ref.out(), WithinAbs(2.0 * (2.0 * 1.0 / 7.0 - 1.0), 1e-12));

    // Now the header, configured from its own shipped constants.
    Rungler64 r;
    r.prepare(kSr);
    RefRungler mirror{Rungler64::kDefaultBits, Rungler64::kDefaultDacBits,
                      Rungler64::kFeedbackTap, Rungler64::kRangeV, Rungler64::kSeedPattern};

    REQUIRE(r.register_bits() == mirror.reg);
    REQUIRE(r.dac_code() == mirror.code());
    REQUIRE_THAT(r.value(), WithinAbs(mirror.out(), 1e-12));

    for (int i = 0; i < 512; ++i) {
        const double y = r.process(true, false, true);
        mirror.clock(false);
        REQUIRE(r.register_bits() == mirror.reg);
        REQUIRE_THAT(y, WithinAbs(mirror.out(), 1e-12));
    }
}

TEST_CASE("Rungler output is bounded by construction over adversarial data",
          "[signal][sequencing][rungler][bound]") {
    // Law 8: the Forge `worst_case_gain` cites THIS assertion. |y| <= range_v
    // holds for any clock/data sequence because the output is a D-bit DAC code
    // mapped affinely onto [-range_v, +range_v] — there is no accumulating
    // state that could exceed it.
    constexpr int kClocks = 1000000;

    struct Config {
        int bits;
        int dac;
        int tap;
        double range;
    };
    const Config configs[] = {
        {Rungler64::kDefaultBits, Rungler64::kDefaultDacBits, Rungler64::kFeedbackTap,
         Rungler64::kRangeV},
        {Rungler64::kMinBits, Rungler64::kMinDacBits, 0, 0.5},
        {Rungler64::kMaxBits, Rungler64::kMaxDacBits, Rungler64::kMaxBits - 2, 5.0},
    };

    for (const auto& cfg : configs) {
        Rungler64 r;
        r.prepare(kSr);
        r.set_reg_bits(cfg.bits);
        r.set_dac_bits(cfg.dac);
        r.set_feedback_tap(cfg.tap);
        r.set_range_v(cfg.range);
        r.set_external_data(true);

        RefXorshift noise(0xC0FFEEu);
        double peak = 0.0;
        bool saw_positive_extreme = false;
        bool saw_negative_extreme = false;

        for (int i = 0; i < kClocks; ++i) {
            // Three adversarial data streams interleaved: all-ones, alternating,
            // and xorshift.
            const int which = i % 3;
            const bool data = which == 0 ? true : (which == 1 ? (i & 1) != 0 : noise.next() & 1u);
            const double y = r.process(true, false, true, data);
            peak = std::max(peak, std::abs(y));
            if (y >= cfg.range - 1e-12) saw_positive_extreme = true;
            if (y <= -cfg.range + 1e-12) saw_negative_extreme = true;
            REQUIRE(std::abs(y) <= cfg.range);
        }

        // The bound is TIGHT, not merely satisfied: both DAC extremes are hit,
        // so a passing test is not passing because the output stayed small.
        REQUIRE(saw_positive_extreme);
        REQUIRE(saw_negative_extreme);
        REQUIRE_THAT(peak, WithinAbs(cfg.range, 1e-12));
    }
}

TEST_CASE("Rungler: a zero seed is an absorbing state, by documented design",
          "[signal][sequencing][rungler]") {
    Rungler64 r;
    r.prepare(kSr);
    r.set_seed_pattern(0);
    const double floor_v = -Rungler64::kRangeV;
    for (int i = 0; i < 256; ++i)
        REQUIRE_THAT(r.process(true, false, true), WithinAbs(floor_v, 1e-12));

    // External data kicks it out — the documented escape.
    r.set_external_data(true);
    REQUIRE_THAT(r.process(true, false, true, true), WithinAbs(floor_v + 2.0 * Rungler64::kRangeV /
                                                                             7.0,
                                                              1e-12));
}

TEST_CASE("Rungler holds its output while the transport is stopped",
          "[signal][sequencing][rungler]") {
    Rungler64 r;
    r.prepare(kSr);
    for (int i = 0; i < 5; ++i) (void)r.process(true, false, true);
    const std::uint32_t reg = r.register_bits();
    const double held = r.value();
    for (int i = 0; i < 64; ++i) REQUIRE_THAT(r.process(false, false, true), WithinAbs(held, 1e-12));
    REQUIRE(r.register_bits() == reg);
}

// ── Test 7: quantizer ─────────────────────────────────────────────────────

TEST_CASE("Quantizer: 12-TET nearest-step rounding", "[signal][sequencing][quantizer]") {
    QuantizeScale64 q;
    q.set_mode(QuantizeMode::edo);
    q.set_edo(QuantizeScale64::kDefaultEdo);

    const double cv = 0.30;
    const int expected_step = round_half_up(cv * QuantizeScale64::kDefaultEdo);
    REQUIRE(expected_step == 4);  // the spec's worked example, re-derived
    REQUIRE_THAT(q.process(cv),
                 WithinAbs(static_cast<double>(expected_step) / QuantizeScale64::kDefaultEdo,
                           kFloatCompareEps));
}

TEST_CASE("Quantizer: a scale mask snaps to the nearest enabled pitch class",
          "[signal][sequencing][quantizer]") {
    QuantizeScale64 q;
    q.set_mode(QuantizeMode::scale_mask);
    q.set_scale_mask(QuantizeScale64::kMajorMask);
    q.set_root_pc(0);

    const double cv = 0.26;
    const int chromatic = round_half_up(cv * 12.0);
    REQUIRE(chromatic == 3);  // D#, not in C major
    const int snapped = ref_snap(chromatic, QuantizeScale64::kMajorMask, 0);
    REQUIRE(snapped == 4);  // tie between 2 and 4 resolves upward → E
    REQUIRE_THAT(q.process(cv), WithinAbs(snapped / 12.0, kFloatCompareEps));

    // The shipped mask really is the major scale, derived rather than asserted
    // as a magic number.
    const int major_degrees[] = {0, 2, 4, 5, 7, 9, 11};
    std::uint16_t rebuilt = 0;
    for (int d : major_degrees) rebuilt = static_cast<std::uint16_t>(rebuilt | (1u << d));
    REQUIRE(rebuilt == QuantizeScale64::kMajorMask);

    // Every chromatic input maps onto an enabled class.
    for (int st = -24; st <= 24; ++st) {
        const double out = q.process(st / 12.0);
        q.apply_reset_edge();
        const int out_step = round_half_up(out * 12.0);
        int pc = out_step % 12;
        if (pc < 0) pc += 12;
        REQUIRE(((QuantizeScale64::kMajorMask >> pc) & 1u) != 0u);
    }
}

TEST_CASE("Quantizer: the root rotates the mask", "[signal][sequencing][quantizer]") {
    QuantizeScale64 q;
    q.set_mode(QuantizeMode::scale_mask);
    q.set_scale_mask(QuantizeScale64::kMajorMask);
    q.set_root_pc(2);  // D major: F# and C# enabled, F and C not

    for (int st = 0; st < 12; ++st) {
        q.apply_reset_edge();
        const double out = q.process(st / 12.0);
        const int out_step = round_half_up(out * 12.0);
        REQUIRE(out_step == ref_snap(st, QuantizeScale64::kMajorMask, 2));
    }
}

TEST_CASE("Quantizer: hysteresis holds a step until the input crosses by the stated cents",
          "[signal][sequencing][quantizer]") {
    QuantizeScale64 q;
    q.set_mode(QuantizeMode::edo);
    q.set_edo(12);

    // Window derived from the shipped constants: cents → steps at 12 per
    // octave, capped at `kMaxHystSteps` (inactive here — see D7).
    const double window_steps =
        std::min(QuantizeScale64::kHystCents * 12.0 / 1200.0, QuantizeScale64::kMaxHystSteps);
    REQUIRE_THAT(window_steps, WithinAbs(0.2, 1e-12));

    // Latch onto semitone 4, then ramp down slowly.
    (void)q.process(4.0 / 12.0);
    const double boundary = (4.0 - 0.5 - window_steps) / 12.0;  // = 3.3/12 V
    REQUIRE_THAT(boundary, WithinAbs(3.3 / 12.0, 1e-12));

    double last_out = 4.0 / 12.0;
    int changed_at_or_below_boundary = 0;
    for (int i = 0; i <= 2000; ++i) {
        const double cv = 4.0 / 12.0 - (i / 2000.0) * (1.0 / 12.0);
        const double out = q.process(cv);
        if (out != last_out) {
            // The only step change on this ramp must happen at the boundary.
            REQUIRE(cv <= boundary + 1e-9);
            REQUIRE(cv >= boundary - (1.0 / 12.0) / 2000.0 - 1e-9);
            ++changed_at_or_below_boundary;
            last_out = out;
        }
    }
    REQUIRE(changed_at_or_below_boundary == 1);

    // With hysteresis disabled the same ramp changes at the plain boundary.
    QuantizeScale64 q0;
    q0.set_mode(QuantizeMode::edo);
    q0.set_edo(12);
    q0.set_hysteresis_cents(0.0);
    (void)q0.process(4.0 / 12.0);
    REQUIRE_THAT(q0.process(3.6 / 12.0), WithinAbs(4.0 / 12.0, kFloatCompareEps));
    REQUIRE_THAT(q0.process(3.4 / 12.0), WithinAbs(3.0 / 12.0, kFloatCompareEps));
}

TEST_CASE("Quantizer: the hysteresis cap keeps adjacent steps reachable at every EDO — D7",
          "[signal][sequencing][quantizer]") {
    // Ground truth, computed rather than asserted: an adjacent step is one step
    // of travel away, and the latch releases after 0.5 + window steps. Without a
    // cap the shipped 20-cent window exceeds that above EDO-30 (one EDO-31 step
    // is 1200/31 = 38.71 cents, the boundary is 19.35 cents, and 19.35 + 20 >
    // 38.71), so the next step becomes unreachable and the output lags forever.
    for (int n = 1; n <= QuantizeScale64::kMaxEdo; ++n) {
        const double uncapped = QuantizeScale64::kHystCents * n / 1200.0;
        const double window = std::min(uncapped, QuantizeScale64::kMaxHystSteps);
        REQUIRE(0.5 + window < 1.0);  // an adjacent step is always reachable
        if (n > 30) REQUIRE(uncapped > QuantizeScale64::kMaxHystSteps);  // cap engages
        if (n <= 24) REQUIRE(uncapped <= QuantizeScale64::kMaxHystSteps);  // and only there

        // Behavioural check: a monotone one-step-at-a-time ramp tracks exactly,
        // with no accumulated lag, at every division of the octave.
        QuantizeScale64 q;
        q.set_mode(QuantizeMode::edo);
        q.set_edo(n);
        for (int step = 0; step <= 3 * n; ++step) {
            const double v = static_cast<double>(step) / n;
            REQUIRE_THAT(q.process(v), WithinAbs(v, kFloatCompareEps));
        }
    }
}

TEST_CASE("Quantizer: exact step voltages round-trip to themselves",
          "[signal][sequencing][quantizer]") {
    for (int n : {12, 19, 24, 31, QuantizeScale64::kMaxEdo}) {
        QuantizeScale64 q;
        q.set_mode(QuantizeMode::edo);
        q.set_edo(n);
        for (int step = -2 * n; step <= 2 * n; ++step) {
            const double v = static_cast<double>(step) / n;
            REQUIRE_THAT(q.process(v), WithinAbs(v, kFloatCompareEps));
        }
    }
}

TEST_CASE("Quantizer: EDO-24 steps are quarter tones", "[signal][sequencing][quantizer]") {
    QuantizeScale64 q;
    q.set_mode(QuantizeMode::edo);
    q.set_edo(24);

    const double expected_spacing = 0.5 / 12.0;  // half a semitone
    for (int step = 0; step < 24; ++step) {
        q.apply_reset_edge();
        const double a = q.process(static_cast<double>(step) / 24.0);
        q.apply_reset_edge();
        const double b = q.process(static_cast<double>(step + 1) / 24.0);
        REQUIRE_THAT(b - a, WithinAbs(expected_spacing, kFloatCompareEps));
    }
}

TEST_CASE("Quantizer: an empty mask passes the chromatic step through",
          "[signal][sequencing][quantizer]") {
    QuantizeScale64 q;
    q.set_mode(QuantizeMode::scale_mask);
    q.set_scale_mask(0);
    for (int st = -12; st <= 12; ++st) {
        q.apply_reset_edge();
        REQUIRE_THAT(q.process(st / 12.0), WithinAbs(st / 12.0, kFloatCompareEps));
    }
}

TEST_CASE("Quantizer survives the wild CV it exists to tame",
          "[signal][sequencing][quantizer]") {
    // A quantizer's whole job is to take a rungler or a runaway envelope, so it
    // is the block most likely to be handed a value no musician would send.
    // Casting an unbounded or non-finite double to int is undefined behaviour,
    // not a wrong note, so the bound is asserted rather than assumed.
    QuantizeScale64 q;
    q.set_mode(QuantizeMode::edo);
    q.set_edo(12);

    const double hostile[] = {1e30,
                              -1e30,
                              std::numeric_limits<double>::infinity(),
                              -std::numeric_limits<double>::infinity(),
                              std::numeric_limits<double>::quiet_NaN(),
                              1e300,
                              0.0};
    for (double cv : hostile) {
        q.apply_reset_edge();
        const double out = q.process(cv);
        REQUIRE(std::isfinite(out));
        REQUIRE(std::abs(out) <= QuantizeScale64::kMaxAbsSteps / 12.0);
        // Still a valid pitch: an exact multiple of one step.
        REQUIRE_THAT(out * 12.0 - std::floor(out * 12.0 + 0.5), WithinAbs(0.0, 1e-9));
    }

    // A hostile input does not poison the latch for the next real one.
    q.process(std::numeric_limits<double>::quiet_NaN());
    q.apply_reset_edge();
    REQUIRE_THAT(q.process(0.25), WithinAbs(3.0 / 12.0, kFloatCompareEps));

    // Non-finite parameter values are clamped rather than propagated.
    QuantizeScale64 p;
    p.set_hysteresis_cents(std::numeric_limits<double>::quiet_NaN());
    REQUIRE(std::isfinite(p.hysteresis_cents()));
    REQUIRE(p.hysteresis_cents() >= 0.0);
}

TEST_CASE("Cartesian offsets stay in range under a runaway CV",
          "[signal][sequencing][cartesian]") {
    CartesianWalk64 w;
    w.prepare(kSr);
    w.set_size(4, 4);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) w.set_value(x, y, y * 4 + x);

    for (int off : {2147483647, -2147483647 - 1, 999999999, -999999999}) {
        w.set_offsets(off, off);
        for (int i = 0; i < 8; ++i) {
            (void)w.process(true, false, true, true);
            REQUIRE(w.cell_x() >= 0);
            REQUIRE(w.cell_x() < 4);
            REQUIRE(w.cell_y() >= 0);
            REQUIRE(w.cell_y() < 4);
            REQUIRE(std::isfinite(w.cv()));
        }
    }
}

TEST_CASE("StageSeq and Rungler clamp non-finite parameter values",
          "[signal][sequencing][stageseq][rungler]") {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();

    StageSeq64 seq;
    seq.prepare(kSr);
    seq.set_slide_ms(nan);
    REQUIRE(std::isfinite(seq.slide_ms()));
    seq.set_slide_ms(inf);
    REQUIRE(std::isfinite(seq.slide_ms()));
    seq.set_repeat_duty(nan);
    REQUIRE(std::isfinite(seq.repeat_duty()));
    REQUIRE(seq.repeat_duty() >= 0.0);
    REQUIRE(seq.repeat_duty() <= 1.0);
    seq.set_num_stages(2);
    seq.set_stage_slide(1, true);
    for (int i = 0; i < 512; ++i) REQUIRE(std::isfinite(seq.process(true, false, i % 64 == 0).pitch_v));

    Rungler64 r;
    r.prepare(kSr);
    r.set_range_v(nan);
    REQUIRE(std::isfinite(r.range_v()));
    r.set_range_v(inf);
    REQUIRE(std::isfinite(r.range_v()));
    for (int i = 0; i < 64; ++i) REQUIRE(std::isfinite(r.process(true, false, true)));

    TransportEdge64 t;
    t.prepare(kSr);
    t.set_refractory_ms(inf);
    for (int i = 0; i < 64; ++i) (void)t.process(1.0, i % 8 == 0 ? 1.0 : 0.0, 0.0);
}

// ── Test 8: gate logic ────────────────────────────────────────────────────

TEST_CASE("GateLogic truth tables are exhaustive", "[signal][sequencing][gatelogic]") {
    struct Row {
        GateOp op;
        bool expect[4];  // (F,F) (F,T) (T,F) (T,T)
    };
    const Row rows[] = {
        {GateOp::logic_and, {false, false, false, true}},
        {GateOp::logic_or, {false, true, true, true}},
        {GateOp::logic_xor, {false, true, true, false}},
        {GateOp::logic_nand, {true, true, true, false}},
        {GateOp::logic_nor, {true, false, false, false}},
        {GateOp::logic_xnor, {true, false, false, true}},
    };

    GateLogic64 g;
    for (const auto& row : rows) {
        g.set_op(row.op);
        int i = 0;
        for (bool a : {false, true})
            for (bool b : {false, true}) {
                REQUIRE(g.process(a, b) == row.expect[i]);
                // The level-domain form agrees with the boolean form.
                const double la = a ? 1.0 : 0.0;
                const double lb = b ? 1.0 : 0.0;
                REQUIRE_THAT(g.process_levels(la, lb),
                             WithinAbs(row.expect[i] ? 1.0 : 0.0, 1e-12));
                ++i;
            }
    }
}

TEST_CASE("GateLogic N-input form is the N-input gate, not a pairwise fold",
          "[signal][sequencing][gatelogic]") {
    GateLogic64 g;
    const bool three[3] = {true, false, true};

    g.set_op(GateOp::logic_and);
    REQUIRE_FALSE(g.process(three, 3));
    g.set_op(GateOp::logic_or);
    REQUIRE(g.process(three, 3));
    g.set_op(GateOp::logic_xor);
    REQUIRE_FALSE(g.process(three, 3));  // parity of two trues
    g.set_op(GateOp::logic_nand);
    REQUIRE(g.process(three, 3));
    g.set_op(GateOp::logic_nor);
    REQUIRE_FALSE(g.process(three, 3));
    g.set_op(GateOp::logic_xnor);
    REQUIRE(g.process(three, 3));

    // A pairwise fold of NAND would give NAND(NAND(T,F),T) = NAND(T,T) = F,
    // which is NOT the 3-input NAND. This is the case that distinguishes them.
    g.set_op(GateOp::logic_nand);
    const bool fold = g.process(g.process(three[0], three[1]), three[2]);
    REQUIRE(fold != g.process(three, 3));

    // The two-input path and the vector path agree on two inputs.
    for (auto op : {GateOp::logic_and, GateOp::logic_or, GateOp::logic_xor, GateOp::logic_nand,
                    GateOp::logic_nor, GateOp::logic_xnor}) {
        g.set_op(op);
        for (bool a : {false, true})
            for (bool b : {false, true}) {
                const bool pair[2] = {a, b};
                REQUIRE(g.process(pair, 2) == g.process(a, b));
            }
    }
}

// ── Test 9: ProbGate ──────────────────────────────────────────────────────

TEST_CASE("ProbGate decisions match the reference xorshift stream",
          "[signal][sequencing][probgate][determinism]") {
    ProbGate64 p;
    p.set_probability(ProbGate64::kDefaultProbability);
    RefXorshift ref(ProbGate64::kProbSeed);

    for (int i = 0; i < 4096; ++i) {
        const bool got = p.process_edge(true);
        REQUIRE(got == (ref.unit() < ProbGate64::kDefaultProbability));
    }
    REQUIRE(p.draw_count() == 4096u);
}

TEST_CASE("ProbGate pass rate matches its probability", "[signal][sequencing][probgate]") {
    constexpr int kTriggers = 1000000;
    ProbGate64 p;
    p.set_probability(0.5);
    int passed = 0;
    for (int i = 0; i < kTriggers; ++i)
        if (p.process_edge(true)) ++passed;

    const double rate = static_cast<double>(passed) / kTriggers;
    // Acceptance tolerance: ±0.002 absolute, which is 4σ of the binomial
    // standard error (sqrt(0.25/1e6) = 5e-4) for a uniform source.
    REQUIRE_THAT(rate, WithinAbs(0.5, 0.002));
}

TEST_CASE("ProbGate extremes and stream discipline", "[signal][sequencing][probgate]") {
    SECTION("p = 0 blocks everything, p = 1 passes everything") {
        ProbGate64 zero;
        zero.set_probability(0.0);
        ProbGate64 one;
        one.set_probability(1.0);
        for (int i = 0; i < 1000; ++i) {
            REQUIRE_FALSE(zero.process_edge(true));
            REQUIRE(one.process_edge(true));
        }
    }

    SECTION("a draw is consumed per trigger regardless of p") {
        // Two instances see the same triggers with different probabilities; the
        // stream position must be identical, so switching p mid-take cannot
        // shift every later decision.
        ProbGate64 a;
        ProbGate64 b;
        a.set_probability(0.0);
        b.set_probability(1.0);
        for (int i = 0; i < 500; ++i) {
            (void)a.process_edge(true);
            (void)b.process_edge(true);
        }
        REQUIRE(a.draw_count() == b.draw_count());
        REQUIRE(a.draw_count() == 500u);

        // And a non-trigger sample consumes nothing.
        (void)a.process_edge(false);
        REQUIRE(a.draw_count() == 500u);
    }

    SECTION("the signal-domain path detects its own edges") {
        ProbGate64 p;
        p.set_probability(1.0);
        int passes = 0;
        for (int i = 0; i < 100; ++i) {
            if (p.process(1.0)) ++passes;
            if (p.process(1.0)) ++passes;  // still high: not a second edge
            if (p.process(0.0)) ++passes;
        }
        REQUIRE(passes == 100);
        REQUIRE(p.draw_count() == 100u);
    }
}

// ── Test 10: determinism, block-size independence, alias parity ───────────

namespace {

/// A fixed pseudo-arbitrary transport drive, so every determinism assertion
/// exercises resets, stops and irregular clocks rather than a metronome.
struct DriveStep {
    bool run;
    bool reset_edge;
    bool clock;
    bool clock_b;
    bool data;
};

std::vector<DriveStep> make_drive(int samples) {
    std::vector<DriveStep> drive;
    drive.reserve(static_cast<std::size_t>(samples));
    RefXorshift r(0xBEEF01u);
    for (int i = 0; i < samples; ++i) {
        const std::uint32_t w = r.next();
        drive.push_back({(w & 0xFFu) > 24u, (i % 977) == 976, (i % 53) == 0, (i % 31) == 0,
                         (w & 0x10000u) != 0u});
    }
    return drive;
}

}  // namespace

TEST_CASE("Determinism: render, reset, re-render is bit-identical for every block",
          "[signal][sequencing][determinism]") {
    const auto drive = make_drive(20000);

    SECTION("StageSeqT") {
        StageSeq64 seq;
        configure_walk(seq, 6, SeqDirection::random);
        seq.set_stage_slide(3, true);
        seq.set_stage_gate_mode(2, StageGateMode::repeat);
        seq.set_stage_gate_mode(4, StageGateMode::rest);
        seq.set_stage_pulse_count(1, 3);

        const auto render = [&] {
            std::vector<double> out;
            out.reserve(drive.size() * 2);
            for (const auto& d : drive) {
                const auto f = seq.process(d.run, d.reset_edge, d.clock);
                out.push_back(f.pitch_v);
                out.push_back(f.gate ? 1.0 : 0.0);
            }
            return out;
        };
        seq.reset();
        const auto a = render();
        seq.reset();
        const auto b = render();
        REQUIRE(a == b);
    }

    SECTION("CartesianWalkT") {
        CartesianWalk64 w;
        w.prepare(kSr);
        w.set_size(4, 3);
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < 4; ++x) w.set_value(x, y, 0.07 * (y * 4 + x));

        const auto render = [&] {
            std::vector<double> out;
            for (const auto& d : drive) {
                const auto f = w.process(d.run, d.reset_edge, d.clock, d.clock_b);
                out.push_back(f.cv);
                out.push_back(f.gate ? 1.0 : 0.0);
            }
            return out;
        };
        w.reset();
        const auto a = render();
        w.reset();
        const auto b = render();
        REQUIRE(a == b);
    }

    SECTION("RunglerT") {
        Rungler64 r;
        r.prepare(kSr);
        r.set_external_data(true);
        const auto render = [&] {
            std::vector<double> out;
            for (const auto& d : drive) out.push_back(r.process(d.run, d.reset_edge, d.clock, d.data));
            return out;
        };
        r.reset();
        const auto a = render();
        r.reset();
        const auto b = render();
        REQUIRE(a == b);
    }

    SECTION("QuantizeScaleT") {
        QuantizeScale64 q;
        const auto render = [&] {
            std::vector<double> out;
            RefXorshift src(0x5150u);
            for (const auto& d : drive) {
                if (d.reset_edge) q.apply_reset_edge();
                out.push_back(q.process(src.unit() * 4.0 - 2.0));
            }
            return out;
        };
        q.reset();
        const auto a = render();
        q.reset();
        const auto b = render();
        REQUIRE(a == b);
    }

    SECTION("ProbGateT") {
        ProbGate64 p;
        p.set_probability(0.37);
        const auto render = [&] {
            std::vector<double> out;
            for (const auto& d : drive) {
                if (d.reset_edge) p.apply_reset_edge();
                out.push_back(p.process_edge(d.clock) ? 1.0 : 0.0);
            }
            return out;
        };
        p.reset();
        const auto a = render();
        p.reset();
        const auto b = render();
        REQUIRE(a == b);
    }
}

TEST_CASE("Determinism: a fresh instance renders identically to a reset one",
          "[signal][sequencing][determinism]") {
    const auto drive = make_drive(4000);

    const auto render_stage = [&](StageSeq64& seq) {
        std::vector<double> out;
        for (const auto& d : drive) {
            const auto f = seq.process(d.run, d.reset_edge, d.clock);
            out.push_back(f.pitch_v);
            out.push_back(f.gate ? 1.0 : 0.0);
        }
        return out;
    };

    StageSeq64 fresh;
    configure_walk(fresh, 5, SeqDirection::pingpong);
    fresh.set_stage_slide(2, true);
    const auto a = render_stage(fresh);

    StageSeq64 used;
    configure_walk(used, 5, SeqDirection::pingpong);
    used.set_stage_slide(2, true);
    (void)render_stage(used);
    used.reset();
    const auto b = render_stage(used);
    REQUIRE(a == b);
}

TEST_CASE("Determinism: output depends only on the drive sequence, not on how it is chunked",
          "[signal][sequencing][determinism]") {
    // These blocks have no block-scoped state and no block API to get wrong, so
    // this is a structural assertion: the same call sequence delivered in
    // different groupings must produce the same samples. It is cheap and it is
    // the thing a future `process_block` overload would break first.
    const auto drive = make_drive(6000);
    const auto render = [&](const std::vector<int>& chunks) {
        StageSeq64 seq;
        configure_walk(seq, 7, SeqDirection::forward);
        seq.set_stage_gate_mode(3, StageGateMode::repeat);
        seq.set_stage_slide(5, true);
        std::vector<double> out;
        std::size_t i = 0;
        std::size_t c = 0;
        while (i < drive.size()) {
            const std::size_t n =
                std::min(static_cast<std::size_t>(chunks[c % chunks.size()]), drive.size() - i);
            for (std::size_t k = 0; k < n; ++k, ++i) {
                const auto f = seq.process(drive[i].run, drive[i].reset_edge, drive[i].clock);
                out.push_back(f.pitch_v);
                out.push_back(f.gate ? 1.0 : 0.0);
            }
            ++c;
        }
        return out;
    };

    REQUIRE(render({1}) == render({97, 13, 512, 3}));
}

TEST_CASE("Double-alias parity: the float and double instantiations agree",
          "[signal][sequencing][determinism]") {
    const auto drive = make_drive(3000);

    StageSeq f32;
    StageSeq64 f64;
    f32.prepare(kSr);
    f64.prepare(kSr);
    f32.set_num_stages(5);
    f64.set_num_stages(5);
    for (int s = 0; s < 5; ++s) {
        f32.set_stage_pitch(s, static_cast<float>(s) * 0.25f);
        f64.set_stage_pitch(s, s * 0.25);
        f32.set_stage_pulse_count(s, 1 + (s % 3));
        f64.set_stage_pulse_count(s, 1 + (s % 3));
    }
    f32.set_stage_slide(2, true);
    f64.set_stage_slide(2, true);

    for (const auto& d : drive) {
        const auto a = f32.process(d.run, d.reset_edge, d.clock);
        const auto b = f64.process(d.run, d.reset_edge, d.clock);
        REQUIRE(a.gate == b.gate);
        REQUIRE_THAT(static_cast<double>(a.pitch_v), WithinAbs(b.pitch_v, 1e-6));
    }

    Rungler r32;
    Rungler64 r64;
    r32.prepare(kSr);
    r64.prepare(kSr);
    r32.set_external_data(true);
    r64.set_external_data(true);
    for (const auto& d : drive) {
        const double a = static_cast<double>(r32.process(d.run, d.reset_edge, d.clock, d.data));
        const double b = r64.process(d.run, d.reset_edge, d.clock, d.data);
        REQUIRE(r32.register_bits() == r64.register_bits());
        REQUIRE_THAT(a, WithinAbs(b, 1e-6));
    }
}

TEST_CASE("Double-alias parity: every float alias instantiates and behaves",
          "[signal][sequencing][determinism]") {
    // The `Foo` aliases are the ones a caller reaches for first, so each must be
    // exercised at least once — a template that only ever gets instantiated at
    // `double` can carry a `float`-only compile error indefinitely.
    CartesianWalk w;
    QuantizeScale q;
    GateLogic g;
    ProbGate p;
    TransportEdge t;

    w.prepare(static_cast<float>(kSr));
    q.prepare(static_cast<float>(kSr));
    g.prepare(static_cast<float>(kSr));
    p.prepare(static_cast<float>(kSr));
    t.prepare(static_cast<float>(kSr));

    w.set_size(2, 2);
    w.set_value(1, 0, 0.5f);
    (void)w.process(true, false, true, false);
    const auto wf = w.process(true, false, true, false);
    REQUIRE(wf.gate);
    REQUIRE_THAT(static_cast<double>(wf.cv), WithinAbs(0.5, 1e-6));

    q.set_mode(QuantizeMode::edo);
    q.set_edo(12);
    REQUIRE_THAT(static_cast<double>(q.process(0.30f)), WithinAbs(4.0 / 12.0, 1e-6));

    g.set_op(GateOp::logic_xor);
    REQUIRE(g.process(true, false));
    REQUIRE_THAT(static_cast<double>(g.process_levels(1.0f, 0.0f)), WithinAbs(1.0, 1e-6));

    p.set_probability(1.0);
    REQUIRE(p.process(1.0f));

    REQUIRE(t.process(1.0f, 1.0f, 1.0f).reset_edge);
}

TEST_CASE("GateLogic returns each operation's identity on an empty input list",
          "[signal][sequencing][gatelogic]") {
    // Documented behaviour, so a tree that loses a branch degrades predictably
    // rather than collapsing every op to false.
    GateLogic64 g;
    const bool* none = nullptr;
    g.set_op(GateOp::logic_and);
    REQUIRE(g.process(none, 0));
    g.set_op(GateOp::logic_or);
    REQUIRE_FALSE(g.process(none, 0));
    g.set_op(GateOp::logic_xor);
    REQUIRE_FALSE(g.process(none, 0));
    g.set_op(GateOp::logic_nand);
    REQUIRE_FALSE(g.process(none, 0));
    g.set_op(GateOp::logic_nor);
    REQUIRE(g.process(none, 0));
    g.set_op(GateOp::logic_xnor);
    REQUIRE(g.process(none, 0));
}

// ── Test 11: RT allocation probe roster ───────────────────────────────────

TEST_CASE("RT safety: no allocation in process, set_* or reset after prepare",
          "[signal][sequencing][rt]") {
    StageSeq64 seq;
    CartesianWalk64 walk;
    Rungler64 rungler;
    QuantizeScale64 quant;
    GateLogic64 logic;
    ProbGate64 prob;
    TransportEdge64 transport;

    seq.prepare(kSr);
    walk.prepare(kSr);
    rungler.prepare(kSr);
    quant.prepare(kSr);
    logic.prepare(kSr);
    prob.prepare(kSr);
    transport.prepare(kSr);

    require_allocates_no_memory([&] {
        for (int i = 0; i < 4096; ++i) {
            const double t = i / 4096.0;
            const bool clock = (i % 7) == 0;
            const bool reset_edge = (i % 601) == 600;
            const bool run = (i % 997) < 900;

            // set_* on the audio thread must be as safe as process.
            seq.set_num_stages(1 + (i % StageSeq64::kMaxStages));
            seq.set_direction(static_cast<SeqDirection>(i % 4));
            seq.set_stage_pitch(i % StageSeq64::kMaxStages, t * 4.0 - 2.0);
            seq.set_stage_pulse_count(i % StageSeq64::kMaxStages, 1 + (i % 8));
            seq.set_stage_gate_mode(i % StageSeq64::kMaxStages,
                                    static_cast<StageGateMode>(i % 4));
            seq.set_stage_slide(i % StageSeq64::kMaxStages, (i % 5) == 0);
            seq.set_stage_skip(i % StageSeq64::kMaxStages, (i % 11) == 0);
            seq.set_slide_ms(1.0 + 400.0 * t);
            seq.set_repeat_duty(0.1 + 0.8 * t);
            (void)seq.process(run, reset_edge, clock);

            walk.set_size(1 + (i % CartesianWalk64::kMaxDim), 1 + ((i / 3) % CartesianWalk64::kMaxDim));
            walk.set_value(i % CartesianWalk64::kMaxDim, (i / 5) % CartesianWalk64::kMaxDim, t);
            walk.set_access(static_cast<CartesianAccess>(i % 2));
            walk.set_offsets(i % 9 - 4, i % 7 - 3);
            (void)walk.process(run, reset_edge, clock, (i % 5) == 0);

            rungler.set_reg_bits(Rungler64::kMinBits + (i % 13));
            rungler.set_dac_bits(Rungler64::kMinDacBits + (i % 4));
            rungler.set_feedback_tap(i % 15);
            rungler.set_range_v(0.5 + 4.5 * t);
            rungler.set_external_data((i % 2) == 0);
            rungler.set_seed_pattern(static_cast<std::uint32_t>(i * 2654435761u) & 0xFFFFu);
            (void)rungler.process(run, reset_edge, clock, (i % 3) == 0);

            quant.set_mode(static_cast<QuantizeMode>(i % 2));
            quant.set_edo(1 + (i % QuantizeScale64::kMaxEdo));
            quant.set_scale_mask(static_cast<std::uint16_t>(i & 0x0FFF));
            quant.set_root_pc(i % 12);
            quant.set_hysteresis_cents(50.0 * t);
            if (reset_edge) quant.apply_reset_edge();
            (void)quant.process(t * 6.0 - 3.0);

            logic.set_op(static_cast<GateOp>(i % 6));
            (void)logic.process((i % 2) == 0, (i % 3) == 0);
            const bool three[3] = {(i % 2) == 0, (i % 3) == 0, (i % 5) == 0};
            (void)logic.process(three, 3);
            (void)logic.process_levels(t, 1.0 - t);

            prob.set_probability(t);
            if (reset_edge) prob.apply_reset_edge();
            (void)prob.process_edge(clock);
            (void)prob.process(clock ? 1.0 : 0.0);

            transport.set_refractory_ms(0.1 + 4.9 * t);
            transport.set_thresholds(0.5, 0.25);
            (void)transport.process(run ? 1.0 : 0.0, reset_edge ? 1.0 : 0.0, clock ? 1.0 : 0.0);

            (void)seq.latency_samples();
            (void)walk.latency_samples();
            (void)rungler.latency_samples();
            (void)quant.latency_samples();
            (void)logic.latency_samples();
            (void)prob.latency_samples();
            (void)transport.latency_samples();
        }

        seq.reset();
        walk.reset();
        rungler.reset();
        quant.reset();
        logic.reset();
        prob.reset();
        transport.reset();
    });
}

// ── Test 12: latency ──────────────────────────────────────────────────────

TEST_CASE("Latency is zero and effects land on the clock sample",
          "[signal][sequencing][latency]") {
    static_assert(StageSeq64::latency_samples() == 0);
    static_assert(CartesianWalk64::latency_samples() == 0);
    static_assert(Rungler64::latency_samples() == 0);
    static_assert(QuantizeScale64::latency_samples() == 0);
    static_assert(GateLogic64::latency_samples() == 0);
    static_assert(ProbGate64::latency_samples() == 0);
    static_assert(TransportEdge64::latency_samples() == 0);

    SECTION("StageSeq gate and CV appear on the clock sample") {
        StageSeq64 seq;
        seq.prepare(kSr);
        seq.set_num_stages(1);
        seq.set_stage_pitch(0, 0.75);
        seq.set_stage_gate_mode(0, StageGateMode::hold);

        for (int i = 0; i < 100; ++i) {
            const auto f = seq.process(true, false, false);
            REQUIRE_FALSE(f.gate);
        }
        const auto f = seq.process(true, false, true);
        REQUIRE(f.gate);                                    // same sample
        REQUIRE_THAT(f.pitch_v, WithinAbs(0.75, 1e-12));    // no slide, no delay
    }

    SECTION("Cartesian CV appears on the clock sample") {
        CartesianWalk64 w;
        w.prepare(kSr);
        w.set_size(2, 1);
        w.set_value(0, 0, 0.25);
        const auto f = w.process(true, false, true, false);
        REQUIRE(f.gate);
        REQUIRE_THAT(f.cv, WithinAbs(0.25, 1e-12));
    }

    SECTION("Rungler steps on the clock sample") {
        Rungler64 r;
        r.prepare(kSr);
        RefRungler ref{Rungler64::kDefaultBits, Rungler64::kDefaultDacBits,
                       Rungler64::kFeedbackTap, Rungler64::kRangeV, Rungler64::kSeedPattern};
        ref.clock(false);
        REQUIRE_THAT(r.process(true, false, true), WithinAbs(ref.out(), 1e-12));
    }
}
