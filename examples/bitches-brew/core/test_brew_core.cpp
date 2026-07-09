// brew-core: the CV output stage, the position-derived clock grid, and the
// pulse-width rules that keep a gate from welding shut.
//
// The clock-grid cases here are the unit-level half of the suite's anti-burst
// guard. The other half — the same scenarios driven through a real Processor and
// a real ProcessContext — lives in the Sync tests.

#include <catch2/catch_test_macros.hpp>

#include <brew/clock.hpp>
#include <brew/cv.hpp>
#include <brew/pulse.hpp>
#include <brew/run_segment.hpp>

#include <cstdint>
#include <vector>

using namespace pulp::examples::brew;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr double kTempo = 120.0;

/// Collect the edge sample-offsets a block produces. The whole point of the
/// anti-burst design is that this is an exact multiset, not "at least one edge".
std::vector<int> edges_of(ClockGrid& grid, double start_beats, double epb,
                          int frames, double tempo = kTempo,
                          double sr = kSampleRate) {
    std::vector<int> out;
    grid.advance(start_beats, epb, beats_per_sample(tempo, sr), frames,
                 [&](int offset, std::int64_t) { out.push_back(offset); });
    return out;
}

/// Beats spanned by one block.
double block_beats(int frames, double tempo = kTempo, double sr = kSampleRate) {
    return beats_per_sample(tempo, sr) * frames;
}

}  // namespace

// ---------------------------------------------------------------- output stage

TEST_CASE("CV output stage scales, inverts, and clamps", "[brew][core][cv]") {
    SECTION("scale attenuates toward zero") {
        REQUIRE(resolve_output(1.0f, 0.5f, false) == 0.5f);
        REQUIRE(resolve_output(-1.0f, 0.5f, false) == -0.5f);
        REQUIRE(resolve_output(0.5f, 0.0f, false) == 0.0f);
    }

    // Some interfaces wire their outputs with reversed polarity. Without this the
    // suite is unusable on them.
    SECTION("invert flips polarity") {
        REQUIRE(resolve_output(0.5f, 1.0f, true) == -0.5f);
        REQUIRE(resolve_output(-0.5f, 1.0f, true) == 0.5f);
        REQUIRE(resolve_output(0.0f, 1.0f, true) == 0.0f);
    }

    // A CV above the interface's rail is a clipped voltage, not a louder one.
    SECTION("value and scale are clamped, never wrapped") {
        REQUIRE(resolve_output(2.0f, 1.0f, false) == 1.0f);
        REQUIRE(resolve_output(-2.0f, 1.0f, false) == -1.0f);
        REQUIRE(resolve_output(1.0f, 2.0f, false) == 1.0f);
        // A negative scale would be a second polarity inversion fighting `invert`.
        REQUIRE(resolve_output(1.0f, -1.0f, false) == 0.0f);
    }

    // A host may hand back 0.499998 for what the UI shows as "off". Every plug-in
    // must round it the same way or two of them disagree about one saved value.
    SECTION("toggles round at the midpoint") {
        REQUIRE_FALSE(as_toggle(0.0f));
        REQUIRE_FALSE(as_toggle(0.4999f));
        REQUIRE(as_toggle(0.5f));
        REQUIRE(as_toggle(1.0f));
    }
}

// ------------------------------------------------------------------ edge count

TEST_CASE("edges_per_beat applies multiplier and divisor", "[brew][core][clock]") {
    REQUIRE(edges_per_beat(24.0, 1, 1) == 24.0);
    REQUIRE(edges_per_beat(24.0, 2, 1) == 48.0);
    REQUIRE(edges_per_beat(24.0, 1, 2) == 12.0);
    REQUIRE(edges_per_beat(24.0, 3, 2) == 36.0);

    SECTION("degenerate settings yield no grid rather than a division by zero") {
        REQUIRE(edges_per_beat(0.0, 1, 1) == 0.0);
        REQUIRE(edges_per_beat(24.0, 0, 1) == 0.0);
        REQUIRE(edges_per_beat(24.0, 1, 0) == 0.0);
    }
}

// ------------------------------------------------------------------ clock grid

// 120 BPM, 48 kHz, 1 pulse per beat: one edge every 24000 samples.
TEST_CASE("clock grid places edges from position, starting at the block start",
          "[brew][core][clock]") {
    ClockGrid grid;
    // A block of exactly one beat, starting on the beat: the edge is at sample 0,
    // and the edge one beat later belongs to the *next* block.
    REQUIRE(edges_of(grid, 0.0, 1.0, 24000) == std::vector<int>{0});
    REQUIRE(edges_of(grid, 1.0, 1.0, 24000) == std::vector<int>{0});
}

TEST_CASE("clock grid emits several edges per block in ascending order",
          "[brew][core][clock]") {
    ClockGrid grid;
    // 4 edges per beat, one beat of samples: edges at 0, 6000, 12000, 18000.
    REQUIRE(edges_of(grid, 0.0, 4.0, 24000) ==
            std::vector<int>{0, 6000, 12000, 18000});
}

// This is the burst case. Parking the playhead at 3.7 and hitting play must emit
// only the edges that live inside the block, never a catch-up from beat 0.
TEST_CASE("starting mid-timeline emits only that block's edges",
          "[brew][core][clock][anti-burst]") {
    ClockGrid grid;
    grid.reset();  // what the play edge does

    const int frames = 512;
    const auto edges = edges_of(grid, 3.7, 24.0, frames);

    // 24 ppqn at 120 BPM = one edge per 1000 samples. Beat 3.7 sits at edge index
    // 88.8, so the block's first edge is index 89 (beat 3.708333...).
    const double bps = beats_per_sample(kTempo, kSampleRate);
    const int expected_first = static_cast<int>(((89.0 / 24.0) - 3.7) / bps + 0.5);
    REQUIRE(edges.size() == 1);
    REQUIRE(edges[0] == expected_first);
    REQUIRE(edges[0] < frames);
}

TEST_CASE("clock grid is immune to a host that re-renders the same block",
          "[brew][core][clock][anti-burst]") {
    ClockGrid grid;
    const auto first = edges_of(grid, 0.0, 4.0, 24000);
    REQUIRE(first == std::vector<int>{0, 6000, 12000, 18000});

    // Same block start: every index was already emitted, so nothing repeats.
    REQUIRE(edges_of(grid, 0.0, 4.0, 24000).empty());
}

TEST_CASE("a loop wrap re-arms the grid without bursting",
          "[brew][core][clock][anti-burst]") {
    ClockGrid grid;
    // Play through beats 0..2, then the host wraps back to beat 0.
    (void)edges_of(grid, 0.0, 4.0, 24000);
    (void)edges_of(grid, 1.0, 4.0, 24000);

    // Strictly-backwards block start: those edges are new again, and there is
    // exactly one block's worth of them — not a catch-up.
    REQUIRE(edges_of(grid, 0.0, 4.0, 24000) ==
            std::vector<int>{0, 6000, 12000, 18000});
}

TEST_CASE("a forward locate resumes at the new position",
          "[brew][core][clock][anti-burst]") {
    ClockGrid grid;
    (void)edges_of(grid, 0.0, 1.0, 24000);  // edge index 0

    // Jump forward to beat 64. Exactly the one edge in the block, not 64 of them.
    REQUIRE(edges_of(grid, 64.0, 1.0, 24000) == std::vector<int>{0});
}

TEST_CASE("contiguous blocks tile the edge grid with no gaps or repeats",
          "[brew][core][clock]") {
    ClockGrid grid;
    const int frames = 512;
    const double epb = 24.0;
    const double step = block_beats(frames);

    int total = 0;
    double beat = 0.0;
    // Walk 4 beats. At 24 ppqn that is exactly 96 edges, and the edge at beat 4
    // belongs to the block after the last one we render.
    const int blocks = static_cast<int>(4.0 / step);
    for (int b = 0; b < blocks; ++b, beat += step)
        total += static_cast<int>(edges_of(grid, beat, epb, frames).size());

    // 4 beats at 24 ppqn. The edge at beat 4 belongs to the block after the last
    // one rendered, so it is not counted.
    REQUIRE(total == 96);
}

// A host whose reported position wobbles in the last ulp must not straddle an
// integer edge boundary and emit twice, or skip.
TEST_CASE("clock grid tolerates last-ulp jitter in the reported position",
          "[brew][core][clock]") {
    const double epb = 24.0;
    const int frames = 1000;  // exactly one edge period at 120 BPM / 48 kHz

    // A block starting one edge period in *should* start exactly on beat 1/24. Nudge it a hair either
    // way; the edge must land once, at sample 0, in both cases.
    ClockGrid low;
    low.reset();
    REQUIRE(edges_of(low, (1.0 / 24.0) - 1e-12, epb, frames) ==
            std::vector<int>{0});

    ClockGrid high;
    high.reset();
    REQUIRE(edges_of(high, (1.0 / 24.0) + 1e-12, epb, frames) ==
            std::vector<int>{0});
}

TEST_CASE("clock grid emits nothing for a degenerate block",
          "[brew][core][clock]") {
    ClockGrid grid;
    REQUIRE(edges_of(grid, 0.0, 24.0, 0).empty());       // no samples
    REQUIRE(edges_of(grid, 0.0, 0.0, 512).empty());      // no grid
    REQUIRE(edges_of(grid, 0.0, 24.0, 512, 0.0).empty());  // stopped clock
}

// -------------------------------------------------------------- pulse geometry

TEST_CASE("pulse period follows tempo and edge density", "[brew][core][pulse]") {
    // 120 BPM, 24 ppqn, 48 kHz: 24000 samples per beat / 24 edges = 1000.
    REQUIRE(pulse_period_samples(48000.0, 120.0, 24.0) == 1000.0);
    REQUIRE(pulse_period_samples(48000.0, 240.0, 24.0) == 500.0);
    REQUIRE(pulse_period_samples(48000.0, 120.0, 0.0) == 0.0);
}

TEST_CASE("trigger width honors the DAC floor", "[brew][core][pulse]") {
    const double period = pulse_period_samples(48000.0, 120.0, 24.0);  // 1000

    // A one-sample pulse does not survive a reconstruction filter; ask for a
    // hair and get the ~1 ms floor (48 samples at 48 kHz).
    REQUIRE(trigger_width_samples(0.0, 48000.0, period) == 48);
    REQUIRE(trigger_width_samples(0.1, 48000.0, period) == 48);

    SECTION("a reasonable request passes through untouched") {
        REQUIRE(trigger_width_samples(5.0, 48000.0, period) == 240);
    }
}

// The welded-gate case. At 24 ppqn and 300 BPM the period is 8.3 ms, so a
// perfectly reasonable-looking 10 ms trigger length would hold the gate high
// forever. The clamp is what makes the plug-in safe to hand to hardware.
TEST_CASE("trigger width never welds the gate", "[brew][core][pulse][safety]") {
    const double period = pulse_period_samples(48000.0, 300.0, 24.0);  // 400
    REQUIRE(period == 400.0);

    const std::int64_t width = trigger_width_samples(10.0, 48000.0, period);
    REQUIRE(static_cast<double>(width) < period * 0.5);
    REQUIRE(width == 199);

    SECTION("even an absurd request stays strictly under half the period") {
        REQUIRE(trigger_width_samples(10000.0, 48000.0, period) == 199);
    }

    // When the period is so short that even the 1 ms floor would weld the gate,
    // the ceiling wins: a weak trigger beats a stuck one.
    SECTION("the ceiling outranks the floor at extreme edge densities") {
        const double fast = pulse_period_samples(48000.0, 300.0, 24.0 * 16.0);
        REQUIRE(fast == 25.0);
        const std::int64_t w = trigger_width_samples(5.0, 48000.0, fast);
        REQUIRE(w == 12);
        // Compared in double: half of a 25-sample period is 12.5, and truncating
        // that to 12 would let a welded gate pass this assertion.
        REQUIRE(static_cast<double>(w) < fast * 0.5);
    }

    SECTION("a pulse is never zero-width") {
        REQUIRE(trigger_width_samples(0.0, 48000.0, 1.0) >= 1);
        REQUIRE(trigger_width_samples(0.0, 48000.0, 0.0) >= 1);
    }
}

TEST_CASE("pulse shaper counts down across block boundaries",
          "[brew][core][pulse]") {
    PulseShaper pulse;
    REQUIRE_FALSE(pulse.high());

    pulse.trigger(3);
    REQUIRE(pulse.high());
    REQUIRE(pulse.tick());
    REQUIRE(pulse.tick());
    REQUIRE(pulse.tick());
    REQUIRE_FALSE(pulse.tick());
    REQUIRE_FALSE(pulse.high());

    SECTION("reset drops an in-flight pulse") {
        pulse.trigger(100);
        pulse.reset();
        REQUIRE_FALSE(pulse.high());
    }

    SECTION("a zero-width trigger still emits one sample") {
        pulse.trigger(0);
        REQUIRE(pulse.tick());
        REQUIRE_FALSE(pulse.tick());
    }
}

// ----------------------------------------------------------------- run segment

TEST_CASE("bar length follows the time signature", "[brew][core][run-segment]") {
    REQUIRE(beats_per_bar(4, 4) == 4.0);
    REQUIRE(beats_per_bar(3, 4) == 3.0);
    REQUIRE(beats_per_bar(6, 8) == 3.0);   // in quarter notes, not eighths
    REQUIRE(beats_per_bar(7, 8) == 3.5);
    REQUIRE(beats_per_bar(0, 4) == 4.0);   // degenerate: fall back to common time
}

TEST_CASE("next bar line lands on, not past, an exact bar",
          "[brew][core][run-segment]") {
    REQUIRE(next_bar_at_or_after(0.0, 4.0) == 0.0);
    REQUIRE(next_bar_at_or_after(4.0, 4.0) == 4.0);
    REQUIRE(next_bar_at_or_after(0.1, 4.0) == 4.0);
    REQUIRE(next_bar_at_or_after(3.9, 4.0) == 4.0);
    REQUIRE(next_bar_at_or_after(4.1, 4.0) == 8.0);
    REQUIRE(next_bar_at_or_after(2.0, 0.0) == 2.0);  // degenerate bar length
}

TEST_CASE("run segment gates edges before its origin", "[brew][core][run-segment]") {
    RunSegment run;
    run.begin(3.7, 4.0);  // started at 3.7, waiting for the bar at 4.0

    REQUIRE_FALSE(run.passes_gate(3.75));
    REQUIRE(run.passes_gate(4.0));
    REQUIRE(run.passes_gate(4.25));
    REQUIRE(run.origin_beats == 3.7);
    REQUIRE_FALSE(run.skip_consumed);

    SECTION("begin() re-arms the skip allowance") {
        run.skip_consumed = true;
        run.begin(8.0, 8.0);
        REQUIRE_FALSE(run.skip_consumed);
        REQUIRE(run.passes_gate(8.0));
    }
}
