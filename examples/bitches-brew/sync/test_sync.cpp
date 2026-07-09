// Sync — the anti-burst matrix, driven through a real Processor.
//
// The bug this file exists to prevent: a clock generator that accumulates phase
// has to "catch up" whenever the host moves the playhead, and the audible result
// is a burst of pulses the instant the transport starts. Downstream that is a
// fistful of spurious clock ticks into a modular system.
//
// So every assertion here checks the **exact** set of rising-edge sample offsets,
// never "an edge occurred" and never "it didn't crash". A burst is precisely
// "more edges than the math predicts", so only counting catches it.
//
// Scope note. These drive `Processor::process()` with a synthetic ProcessContext.
// They prove Sync's response to a transport, not that the transport a real DAW
// reports looks like these fixtures, and not that the pulses become real voltages
// at a jack. Both of those need a host and hardware respectively.

#include <catch2/catch_test_macros.hpp>

#include "sync_processor.hpp"

#include <pulp/format/headless.hpp>

#include <vector>

using namespace pulp;
using namespace pulp::examples::brew;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr double kTempo = 120.0;
constexpr int kFrames = 512;

/// Drives a Sync instance block by block and recovers the clock's rising edges.
///
/// The rising-edge detector carries the last sample across blocks, so a pulse
/// that spans a block boundary is not miscounted as two.
class SyncRig {
public:
    SyncRig() { host_.prepare(kSampleRate, 4096, 2, 2); }

    state::StateStore& state() { return host_.state(); }

    /// Render one block and return the clock channel's rising-edge offsets.
    std::vector<int> render(double position_beats, bool playing,
                            bool transport_started, int frames = kFrames,
                            double tempo = kTempo) {
        format::ProcessContext ctx;
        ctx.sample_rate = kSampleRate;
        ctx.num_samples = frames;
        ctx.is_playing = playing;
        ctx.transport_started = transport_started;
        ctx.tempo_bpm = tempo;
        ctx.position_beats = position_beats;
        ctx.transport_jump = force_jump_;
        ctx.time_sig_numerator = time_sig_num_;
        ctx.time_sig_denominator = time_sig_den_;
        return render(ctx);
    }

    std::vector<int> render(const format::ProcessContext& ctx) {
        const int frames = ctx.num_samples;
        audio::Buffer<float> in(2, static_cast<std::size_t>(frames));
        audio::Buffer<float> out(2, static_cast<std::size_t>(frames));
        in.clear();
        out.clear();

        const float* in_ptrs[2] = {in.channel(0).data(), in.channel(1).data()};
        audio::BufferView<const float> iv(in_ptrs, 2,
                                          static_cast<std::size_t>(frames));
        auto ov = out.view();
        host_.process(ov, iv, ctx);

        clock_.assign(frames, 0.0f);
        run_.assign(frames, 0.0f);
        for (int n = 0; n < frames; ++n) {
            clock_[static_cast<std::size_t>(n)] =
                out.channel(0)[static_cast<std::size_t>(n)];
            run_[static_cast<std::size_t>(n)] =
                out.channel(1)[static_cast<std::size_t>(n)];
        }

        std::vector<int> edges;
        for (int n = 0; n < frames; ++n) {
            const float cur = clock_[static_cast<std::size_t>(n)];
            if (prev_ <= 0.0f && cur > 0.0f) edges.push_back(n);
            prev_ = cur;
        }
        return edges;
    }

    /// Clock samples of the most recent block.
    const std::vector<float>& clock() const { return clock_; }
    /// Run/stop samples of the most recent block.
    const std::vector<float>& run() const { return run_; }

    /// Make the host lie about `transport_jump` on every block.
    void force_transport_jump(bool on) { force_jump_ = on; }
    void set_time_sig(int num, int den) {
        time_sig_num_ = num;
        time_sig_den_ = den;
    }

private:
    format::HeadlessHost host_{create_sync};
    std::vector<float> clock_;
    std::vector<float> run_;
    float prev_ = 0.0f;
    bool force_jump_ = false;
    int time_sig_num_ = 4;
    int time_sig_den_ = 4;
};

/// Width of the first pulse in a block, in samples. -1 if the block never goes
/// high, or if the pulse is still high at the block's end (so a caller cannot
/// mistake a truncated measurement for a short pulse).
int first_pulse_width(const std::vector<float>& clock) {
    int start = -1;
    for (int n = 0; n < static_cast<int>(clock.size()); ++n)
        if (clock[static_cast<std::size_t>(n)] > 0.0f) {
            start = n;
            break;
        }
    if (start < 0) return -1;
    for (int n = start; n < static_cast<int>(clock.size()); ++n)
        if (clock[static_cast<std::size_t>(n)] <= 0.0f) return n - start;
    return -1;
}

bool all_equal(const std::vector<float>& v, float expected) {
    for (float s : v)
        if (s != expected) return false;
    return !v.empty();
}

/// Sample offset of edge index `i` within a block starting at `start_beats`.
int edge_offset(std::int64_t i, double epb, double start_beats,
                double tempo = kTempo) {
    const double bps = beats_per_sample(tempo, kSampleRate);
    return static_cast<int>(
        ((static_cast<double>(i) / epb) - start_beats) / bps + 0.5);
}

}  // namespace

TEST_CASE("Sync descriptor is a clock plus a run gate", "[brew][sync]") {
    auto proc = create_sync();
    const auto desc = proc->descriptor();
    REQUIRE(desc.name == "Sync");
    REQUIRE(desc.manufacturer == "Bitches Brew");
    // aufx, deliberately: Sync reads no MIDI, so there is no reason to declare a
    // MIDI bus purely to change the AU component type.
    REQUIRE_FALSE(desc.accepts_midi);
    REQUIRE(desc.category == format::PluginCategory::Effect);
}

// ------------------------------------------------------------- the burst matrix

// Case 1: play from the top. The pulse on the downbeat lands at sample 0, and
// nothing precedes it.
TEST_CASE("Sync puts the downbeat pulse at sample 0", "[brew][sync][anti-burst]") {
    SyncRig rig;
    const auto edges = rig.render(0.0, true, true);
    REQUIRE_FALSE(edges.empty());
    REQUIRE(edges[0] == 0);

    // 24 ppqn at 120 BPM = one edge per 1000 samples; a 512-sample block holds one.
    REQUIRE(edges.size() == 1);
}

// Case 2: the burst case. Park at beat 3.7, hit play. Exactly the edges inside
// the block — never a catch-up from beat 0.
TEST_CASE("Sync starting mid-timeline emits only that block's edges",
          "[brew][sync][anti-burst]") {
    SyncRig rig;
    const auto edges = rig.render(3.7, true, true);
    // Beat 3.7 is edge index 88.8, so index 89 is the first inside the block.
    REQUIRE(edges == std::vector<int>{edge_offset(89, 24.0, 3.7)});
}

// Case 3: stop, play, stop, play at the same position. Identical every time —
// nothing accumulates across runs.
TEST_CASE("Sync repeats identically across stop/play cycles",
          "[brew][sync][anti-burst]") {
    SyncRig rig;
    const auto first = rig.render(3.7, true, true);
    (void)rig.render(3.7, false, false);
    const auto second = rig.render(3.7, true, true);
    (void)rig.render(3.7, false, false);
    const auto third = rig.render(3.7, true, true);

    REQUIRE(first == second);
    REQUIRE(second == third);
    REQUIRE(first.size() == 1);
}

// Case 4: a loop wrap moves the playhead backwards. Edges continue on the grid;
// nothing bursts at the seam.
TEST_CASE("Sync does not burst at a loop wrap", "[brew][sync][anti-burst]") {
    SyncRig rig;
    (void)rig.render(0.0, true, true);
    const auto mid = rig.render(1.0, true, false);
    REQUIRE(mid.size() == 1);

    // Host wraps back to the loop start. One block's worth of edges, not a
    // rewind's worth.
    const auto wrapped = rig.render(0.0, true, false);
    REQUIRE(wrapped == std::vector<int>{0});
}

// Case 5: a forward locate mid-play. Edges resume at the new position; none
// "catch up" across the gap.
TEST_CASE("Sync resumes cleanly after a forward locate",
          "[brew][sync][anti-burst]") {
    SyncRig rig;
    (void)rig.render(0.0, true, true);
    const auto located = rig.render(64.0, true, false);
    REQUIRE(located == std::vector<int>{0});
}

// Case 6: tempo change mid-stream. Edges re-derive from the host position, so
// there is no drift to accumulate.
TEST_CASE("Sync re-derives its grid after a tempo change",
          "[brew][sync][anti-burst]") {
    SyncRig rig;
    (void)rig.render(0.0, true, true, kFrames, 120.0);

    // At 140 BPM a 512-sample block spans a different number of beats, but the
    // edge at beat 1/24 is still at beat 1/24.
    const double beat = 1.0 / 24.0;
    const auto edges = rig.render(beat, true, false, kFrames, 140.0);
    REQUIRE(edges == std::vector<int>{0});
}

// Case 7: wait for the bar line. Starting at beat 3.7 with Wait For Bar on, the
// clock stays silent until beat 4.
TEST_CASE("Sync waits for the bar line when asked", "[brew][sync][run-segment]") {
    SyncRig rig;
    rig.state().set_value(SyncProcessor::kWaitForBar, 1.0f);

    // Beat 3.7, one block (512 samples ≈ 0.0213 beats): still short of beat 4.
    REQUIRE(rig.render(3.7, true, true).empty());

    // Jump the playhead to just before the bar and render across it.
    const auto edges = rig.render(3.99, true, false, 2048);
    REQUIRE_FALSE(edges.empty());
    // The first pulse is the one at beat 4.0 exactly (edge index 96), not the
    // grid edges at 3.9917 that fall inside the block but before the bar.
    REQUIRE(edges[0] == edge_offset(96, 24.0, 3.99));

    SECTION("with Wait For Bar off, the same block starts immediately") {
        SyncRig plain;
        REQUIRE_FALSE(plain.render(3.7, true, true).empty());
    }
}

// Case 8: some hosts render the same block twice (around loop points, and when a
// plug-in is inserted mid-playback). The edge is emitted once.
TEST_CASE("Sync emits an edge once when the host repeats a block",
          "[brew][sync][anti-burst]") {
    SyncRig rig;
    const auto first = rig.render(0.0, true, true);
    REQUIRE(first == std::vector<int>{0});

    // Same position, same block. The dedupe suppresses it — and because the
    // level never fell, the rising-edge detector agrees.
    REQUIRE(rig.render(0.0, true, false).empty());
}

// Case 9: the one that earns the design. Hosts set `transport_jump` spuriously.
// Sync reads only the position, so a lie about *how* the position changed cannot
// alter the output at all.
TEST_CASE("Sync is immune to a spurious transport_jump",
          "[brew][sync][anti-burst]") {
    std::vector<std::vector<int>> honest, lying;

    SyncRig a;
    honest.push_back(a.render(0.0, true, true));
    honest.push_back(a.render(1.0, true, false));
    honest.push_back(a.render(3.7, true, false));

    SyncRig b;
    b.force_transport_jump(true);
    lying.push_back(b.render(0.0, true, true));
    lying.push_back(b.render(1.0, true, false));
    lying.push_back(b.render(3.7, true, false));

    REQUIRE(honest == lying);
}

// Case 10: no transport at all. No clock, and the run gate sits at its stop level.
TEST_CASE("Sync is silent with the transport stopped", "[brew][sync][safety]") {
    SyncRig rig;
    for (int block = 0; block < 4; ++block) {
        REQUIRE(rig.render(0.0, false, false).empty());
        REQUIRE(all_equal(rig.clock(), 0.0f));
        REQUIRE(all_equal(rig.run(), 0.0f));
    }
}

// -------------------------------------------------------------- run/stop gate

TEST_CASE("Sync raises the run gate while the transport runs", "[brew][sync]") {
    SyncRig rig;
    (void)rig.render(0.0, true, true);
    REQUIRE(all_equal(rig.run(), 1.0f));

    (void)rig.render(0.5, false, false);
    REQUIRE(all_equal(rig.run(), 0.0f));

    SECTION("run level and polarity follow the output stage") {
        SyncRig r2;
        r2.state().set_value(SyncProcessor::kRunLevel, 0.5f);
        r2.state().set_value(SyncProcessor::kInvert, 1.0f);
        (void)r2.render(0.0, true, true);
        REQUIRE(all_equal(r2.run(), -0.5f));
    }
}

// ------------------------------------------------------------------ parameters

TEST_CASE("Sync multiplier and divisor scale the edge density", "[brew][sync]") {
    SECTION("one pulse per beat") {
        SyncRig rig;
        rig.state().set_value(SyncProcessor::kPulsesPerBeat, 1.0f);
        // One beat of samples at 120 BPM / 48 kHz is 24000 samples: one edge.
        REQUIRE(rig.render(0.0, true, true, 24000).size() == 1);
    }

    SECTION("a multiplier of 4 quadruples them") {
        SyncRig rig;
        rig.state().set_value(SyncProcessor::kPulsesPerBeat, 1.0f);
        rig.state().set_value(SyncProcessor::kMultiplier, 4.0f);
        REQUIRE(rig.render(0.0, true, true, 24000) ==
                std::vector<int>{0, 6000, 12000, 18000});
    }

    SECTION("a divisor of 2 halves them") {
        SyncRig rig;
        rig.state().set_value(SyncProcessor::kPulsesPerBeat, 4.0f);
        rig.state().set_value(SyncProcessor::kDivisor, 2.0f);
        REQUIRE(rig.render(0.0, true, true, 24000) ==
                std::vector<int>{0, 12000});
    }
}

TEST_CASE("Sync skips the first pulse of a run when asked",
          "[brew][sync][run-segment]") {
    SyncRig rig;
    rig.state().set_value(SyncProcessor::kPulsesPerBeat, 1.0f);
    rig.state().set_value(SyncProcessor::kSkipFirst, 1.0f);

    // Two beats: edges at 0 and 24000. The first is swallowed.
    REQUIRE(rig.render(0.0, true, true, 48000) == std::vector<int>{24000});

    SECTION("the allowance is per run, not per lifetime") {
        (void)rig.render(0.0, false, false, 512);
        REQUIRE(rig.render(0.0, true, true, 48000) == std::vector<int>{24000});
    }
}

TEST_CASE("Sync output scale and polarity reach the clock channel",
          "[brew][sync]") {
    SyncRig rig;
    rig.state().set_value(SyncProcessor::kOutputScale, 0.25f);
    (void)rig.render(0.0, true, true);
    REQUIRE(rig.clock()[0] == 0.25f);

    rig.state().set_value(SyncProcessor::kInvert, 1.0f);
    (void)rig.render(1.0, true, true);
    REQUIRE(rig.clock()[0] == -0.25f);
}

// ------------------------------------------------------------- pulse geometry

TEST_CASE("Sync emits a pulse wide enough for a DAC", "[brew][sync][safety]") {
    SyncRig rig;
    // The 5 ms default at 48 kHz is 240 samples, comfortably over the ~1 ms floor.
    (void)rig.render(0.0, true, true, 2048);
    REQUIRE(first_pulse_width(rig.clock()) == 240);

    SECTION("a sub-millisecond request is raised to the floor") {
        SyncRig r2;
        r2.state().set_value(SyncProcessor::kTriggerLengthMs, 0.1f);
        (void)r2.render(0.0, true, true, 2048);
        REQUIRE(first_pulse_width(r2.clock()) == 48);
    }
}

// The welded-gate case. At 24 ppqn and 300 BPM the pulse period is 400 samples,
// so an unclamped 10 ms (480-sample) trigger length would hold the gate high
// forever and the downstream module would never see a second clock.
TEST_CASE("Sync never welds the gate at high tempo", "[brew][sync][safety]") {
    SyncRig rig;
    rig.state().set_value(SyncProcessor::kTriggerLengthMs, 10.0f);

    const auto edges = rig.render(0.0, true, true, 2048, 300.0);
    const int width = first_pulse_width(rig.clock());

    REQUIRE(width == 199);            // clamped strictly below half of 400
    REQUIRE(width < 200);
    // And the gate really does fall and rise again: the clock keeps clocking.
    REQUIRE(edges.size() == 6);       // 2048 samples / 400 = 5.12 periods
}

TEST_CASE("Sync pulses survive a block boundary intact", "[brew][sync]") {
    SyncRig rig;
    rig.state().set_value(SyncProcessor::kPulsesPerBeat, 1.0f);

    // Edge at sample 0 of a 100-sample block; the 240-sample pulse must still be
    // high through the next two blocks and fall in the third.
    (void)rig.render(0.0, true, true, 100);
    REQUIRE(all_equal(rig.clock(), 1.0f));

    const double step = beats_per_sample(kTempo, kSampleRate) * 100.0;
    (void)rig.render(step, true, false, 100);
    REQUIRE(all_equal(rig.clock(), 1.0f));

    (void)rig.render(step * 2.0, true, false, 100);
    // 240 samples of pulse: 100 + 100 + 40 high, then low.
    REQUIRE(rig.clock()[39] == 1.0f);
    REQUIRE(rig.clock()[40] == 0.0f);
}
