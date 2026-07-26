#pragma once

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

#include "rt_allocation_probe.hpp"

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
[[maybe_unused]] int round_half_up(double x) { return static_cast<int>(std::floor(x + 0.5)); }

/// Independent mask snap: nearest enabled pitch class, searched outward, ties
/// upward. Written from §8's prose.
[[maybe_unused]] int ref_snap(int semitone, std::uint16_t mask, int root_pc) {
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
[[maybe_unused]] std::vector<StageObservation> run_stage_seq(StageSeq64& seq, int clocks, int period) {
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
[[maybe_unused]] void configure_walk(StageSeq64& seq, int stages, SeqDirection dir) {
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
