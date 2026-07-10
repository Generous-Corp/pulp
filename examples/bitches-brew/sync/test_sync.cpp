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

#include <cmath>
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
class SyncRig;

/// Point the clock at the `Pulses Per Beat` knob.
///
/// The knob is inert unless `Clock Type` selects `Custom` — the two named rates,
/// 24 and 48 ppqn, ignore it. Every rate-dependent test therefore has to say so
/// explicitly, which is the point: a test that quietly relied on the knob would
/// have gone on passing after the menu was added and asserted nothing.
void set_custom_ppqn(SyncRig& rig, float ppqn);

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
        if (input_level_ != 0.0f)
            for (std::size_t c = 0; c < 2; ++c)
                for (int n = 0; n < frames; ++n)
                    in.channel(c)[static_cast<std::size_t>(n)] = input_level_;

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

    /// Present a constant level on both input channels.
    void set_input_level(float v) { input_level_ = v; }

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
    float input_level_ = 0.0f;
    bool force_jump_ = false;
    int time_sig_num_ = 4;
    int time_sig_den_ = 4;
};

void set_custom_ppqn(SyncRig& rig, float ppqn) {
    rig.state().set_value(SyncProcessor::kClockType,
                          static_cast<float>(static_cast<int>(ClockType::custom)));
    rig.state().set_value(SyncProcessor::kPulsesPerBeat, ppqn);
}

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

// Bypass means stop driving the patch. A clock that keeps pulsing after the user
// pressed Bypass keeps clocking the modular, and un-bypassing must not resume a
// gate that was left mid-high.
TEST_CASE("Sync emits nothing while bypassed", "[brew][sync][safety][bypass]") {
    SyncRig rig;
    (void)rig.render(0.0, true, true);  // a 240-sample pulse is now in flight

    format::ProcessContext ctx;
    ctx.sample_rate = kSampleRate;
    ctx.num_samples = kFrames;
    ctx.is_playing = true;
    ctx.tempo_bpm = kTempo;
    ctx.position_beats = beats_per_sample(kTempo, kSampleRate) * kFrames;
    ctx.is_bypassed = true;

    REQUIRE(rig.render(ctx).empty());
    REQUIRE(all_equal(rig.clock(), 0.0f));
    REQUIRE(all_equal(rig.run(), 0.0f));

    SECTION("un-bypassing resumes on the grid, not mid-pulse") {
        ctx.is_bypassed = false;
        ctx.position_beats = 1.0;  // an edge sits exactly here
        REQUIRE(rig.render(ctx) == std::vector<int>{0});
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
        set_custom_ppqn(rig, 1.0f);
        // One beat of samples at 120 BPM / 48 kHz is 24000 samples: one edge.
        REQUIRE(rig.render(0.0, true, true, 24000).size() == 1);
    }

    SECTION("a multiplier of 4 quadruples them") {
        SyncRig rig;
        set_custom_ppqn(rig, 1.0f);
        rig.state().set_value(SyncProcessor::kMultiplier, 4.0f);
        REQUIRE(rig.render(0.0, true, true, 24000) ==
                std::vector<int>{0, 6000, 12000, 18000});
    }

    SECTION("a divisor of 2 halves them") {
        SyncRig rig;
        set_custom_ppqn(rig, 4.0f);
        rig.state().set_value(SyncProcessor::kDivisor, 2.0f);
        REQUIRE(rig.render(0.0, true, true, 24000) ==
                std::vector<int>{0, 12000});
    }
}

TEST_CASE("Sync skips the first pulse of a run when asked",
          "[brew][sync][run-segment]") {
    SyncRig rig;
    set_custom_ppqn(rig, 1.0f);
    rig.state().set_value(SyncProcessor::kSkipFirst, 1.0f);

    // Two beats: edges at 0 and 24000. The first is swallowed.
    REQUIRE(rig.render(0.0, true, true, 48000) == std::vector<int>{24000});

    SECTION("the allowance is per run, not per lifetime") {
        (void)rig.render(0.0, false, false, 512);
        REQUIRE(rig.render(0.0, true, true, 48000) == std::vector<int>{24000});
    }
}

// The delay is measured from the run origin, so two runs starting at different
// beats behave identically — that is the whole point of having an origin.
TEST_CASE("Sync holds the clock off for the first delay",
          "[brew][sync][run-segment]") {
    SyncRig rig;
    // 24 ppqn at 120 BPM: one edge every 1000 samples, i.e. every 20.8333 ms.
    // 15 ms swallows the edge at beat 0 and lets the next one through. The delay
    // is deliberately not an exact multiple of the edge period: a gate that lands
    // exactly on an edge is decided by the last bit of a float parameter, and
    // asserting on that would be asserting on rounding.
    rig.state().set_value(SyncProcessor::kFirstDelayMs, 15.0f);
    REQUIRE(rig.render(0.0, true, true, 2048) == std::vector<int>{1000, 2000});

    SECTION("zero delay is the unchanged clock") {
        SyncRig plain;
        REQUIRE(plain.render(0.0, true, true, 2048) ==
                std::vector<int>{0, 1000, 2000});
    }

    SECTION("a delay longer than one period swallows two edges") {
        SyncRig r4;
        r4.state().set_value(SyncProcessor::kFirstDelayMs, 25.0f);  // > 20.83 ms
        REQUIRE(r4.render(0.0, true, true, 3072) == std::vector<int>{2000, 3000});
    }

    SECTION("the delay is relative to the run, not the timeline") {
        SyncRig r2;
        r2.state().set_value(SyncProcessor::kFirstDelayMs, 15.0f);
        // Start at beat 4 instead of 0: the same edge is swallowed.
        const auto edges = r2.render(4.0, true, true, 2048);
        REQUIRE(edges == std::vector<int>{1000, 2000});
    }

    SECTION("delay and Skip First compose: the delay gates, then skip eats one") {
        SyncRig r3;
        r3.state().set_value(SyncProcessor::kFirstDelayMs, 15.0f);
        r3.state().set_value(SyncProcessor::kSkipFirst, 1.0f);
        // Edge at 0 is gated (never reaches skip); edge at 1000 is skipped.
        REQUIRE(r3.render(0.0, true, true, 3072) == std::vector<int>{2000, 3000});
    }
}

// A DAC, its reconstruction filter, and the receiving gate input all add latency,
// so a clock that is sample-accurate in software arrives late at the hardware.
TEST_CASE("Sync offset slides the whole pulse train", "[brew][sync]") {
    SECTION("a positive offset moves every pulse later by the same amount") {
        SyncRig rig;
        rig.state().set_value(SyncProcessor::kOffsetMs, 5.0f);  // 240 samples
        REQUIRE(rig.render(0.0, true, true, 2048) ==
                std::vector<int>{240, 1240});
    }

    SECTION("a negative offset pulls them ahead of the beat") {
        SyncRig rig;
        rig.state().set_value(SyncProcessor::kOffsetMs, -5.0f);
        // The edge that would sit at 1000 now lands at 760; the one at 0 has
        // already gone by, before the run started, so it is gated away.
        REQUIRE(rig.render(0.0, true, true, 2048) ==
                std::vector<int>{760, 1760});
    }

    // The gate lives in host-position time; the grid does not. With a positive
    // offset, an edge whose *grid* beat falls just before the run origin still
    // emits just after it, and must not be gated away. Starting at beat 0.005
    // with a +5 ms (0.01-beat) offset puts edge 0 exactly in that window.
    SECTION("an edge that emits after the run start is kept, even if its grid "
            "beat precedes it") {
        SyncRig rig;
        rig.state().set_value(SyncProcessor::kOffsetMs, 5.0f);
        REQUIRE(rig.render(0.005, true, true, 2048) == std::vector<int>{120, 1120});
    }

    SECTION("offset adds and removes no edges across contiguous blocks") {
        SyncRig a, b;
        b.state().set_value(SyncProcessor::kOffsetMs, 3.0f);
        int na = 0, nb = 0;
        double beat = 0.0;
        const double step = beats_per_sample(kTempo, kSampleRate) * 512.0;
        for (int i = 0; i < 40; ++i, beat += step) {
            na += static_cast<int>(a.render(beat, true, i == 0).size());
            nb += static_cast<int>(b.render(beat, true, i == 0).size());
        }
        REQUIRE(na == nb);
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
    set_custom_ppqn(rig, 1.0f);

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

// One beat is 24000 samples at 120 BPM / 48 kHz, so a swung edge lands on a
// sample number that can be predicted by hand rather than by rerunning the code.
TEST_CASE("Sync's swing moves the off-beat pulse to the sample it should",
          "[brew][sync][swing]") {
    constexpr int kBeatSamples = 24000;
    constexpr int kLongBlock = 20000;

    SyncRig rig;
    set_custom_ppqn(rig, 2.0f);  // an edge per eighth

    SECTION("straight puts it halfway") {
        rig.state().set_value(SyncProcessor::kSwingPercent, 50.0f);
        const auto e = rig.render(0.0, true, true, kLongBlock);
        REQUIRE(e == std::vector<int>{0, kBeatSamples / 2});
    }

    SECTION("66% pushes it back to two-thirds of the beat") {
        rig.state().set_value(SyncProcessor::kSwingPercent, 66.0f);
        const auto e = rig.render(0.0, true, true, kLongBlock);
        // 0.66 of a beat = 15840 samples. The downbeat does not move.
        REQUIRE(e == std::vector<int>{0, 15840});
    }

    SECTION("below 50% it rushes instead") {
        rig.state().set_value(SyncProcessor::kSwingPercent, 40.0f);
        const auto e = rig.render(0.0, true, true, kLongBlock);
        REQUIRE(e == std::vector<int>{0, static_cast<int>(0.4 * kBeatSamples)});
    }

    SECTION("the sixteenth unit swings a different note") {
        set_custom_ppqn(rig, 4.0f);
        rig.state().set_value(SyncProcessor::kSwingUnit, 1.0f);
        rig.state().set_value(SyncProcessor::kSwingPercent, 66.0f);
        const auto e = rig.render(0.0, true, true, kLongBlock);
        // Pairs are eighths now: the off-sixteenth at 0.25 beats moves to 0.33,
        // the eighth at 0.5 is a pair boundary and stays put, and the sixteenth
        // after it moves to 0.5 + 0.33 = 0.83.
        REQUIRE(e == std::vector<int>{0, 7920, kBeatSamples / 2, 19920});
    }
}

TEST_CASE("a swung pulse just after the play edge is not swallowed by the gate",
          "[brew][sync][swing][safety]") {
    // The run gate compares an edge's *sounding* beat against the beat the
    // transport started at. Comparing its straight beat instead would drop any
    // pulse the swing has carried across the play edge — a missing downstream
    // clock tick, which reads as a dead cable rather than a rounding bug.
    constexpr int kBeatSamples = 24000;
    SyncRig rig;
    set_custom_ppqn(rig, 2.0f);
    rig.state().set_value(SyncProcessor::kSwingPercent, 66.0f);

    // Start playing at beat 0.55, for a block that ends at 0.967 — so the only
    // edge it can contain is the swung off-eighth. Straight, that edge sits at
    // 0.5, behind us. Swung, it sounds at 0.66, ahead of us, and must be emitted.
    const auto e = rig.render(0.55, true, true, 10000);
    const int expected = static_cast<int>((0.66 - 0.55) * kBeatSamples + 0.5);
    REQUIRE(e.size() == 1);
    REQUIRE(std::abs(e.front() - expected) <= 1);
}

// ------------------------------------------------------------ the clock's type

namespace {

float as_param(ClockType t) { return static_cast<float>(static_cast<int>(t)); }
float as_param(RunSignal r) { return static_cast<float>(static_cast<int>(r)); }
float as_param(NoteUnit u) { return static_cast<float>(static_cast<int>(u)); }

/// Rising edges within one block. Unlike SyncRig::render's clock detector this
/// carries no state across blocks, which is what the run channel wants: a run
/// *level* is high from sample zero, and a carried `prev_` would hide its edge.
std::vector<int> rising_edges(const std::vector<float>& v) {
    std::vector<int> e;
    for (std::size_t n = 1; n < v.size(); ++n)
        if (v[n - 1] <= 0.0f && v[n] > 0.0f) e.push_back(static_cast<int>(n));
    if (!v.empty() && v[0] > 0.0f) e.insert(e.begin(), 0);
    return e;
}

}  // namespace

TEST_CASE("Sync's clock type selects the rate, and Off silences the clock",
          "[brew][sync][clock-type]") {
    constexpr int kBeatSamples = 24000;  // 120 BPM at 48 kHz

    SECTION("Off emits nothing, however the ppqn knob is set") {
        SyncRig rig;
        rig.state().set_value(SyncProcessor::kClockType, as_param(ClockType::off));
        rig.state().set_value(SyncProcessor::kPulsesPerBeat, 4.0f);
        REQUIRE(rig.render(0.0, true, true, kBeatSamples).empty());
        REQUIRE(all_equal(rig.clock(), 0.0f));
    }

    SECTION("24 ppqn puts 24 edges in a beat and ignores the knob") {
        SyncRig rig;
        rig.state().set_value(SyncProcessor::kClockType, as_param(ClockType::ppqn24));
        rig.state().set_value(SyncProcessor::kPulsesPerBeat, 1.0f);
        REQUIRE(rig.render(0.0, true, true, kBeatSamples).size() == 24);
    }

    SECTION("48 ppqn puts 48 edges in a beat and ignores the knob") {
        SyncRig rig;
        rig.state().set_value(SyncProcessor::kClockType, as_param(ClockType::ppqn48));
        rig.state().set_value(SyncProcessor::kPulsesPerBeat, 1.0f);
        REQUIRE(rig.render(0.0, true, true, kBeatSamples).size() == 48);
    }

    SECTION("Custom hands the rate to the knob") {
        SyncRig rig;
        set_custom_ppqn(rig, 2.0f);
        REQUIRE(rig.render(0.0, true, true, kBeatSamples).size() == 2);
    }
}

TEST_CASE("a Trigger Length of zero makes the clock a 50% duty square wave",
          "[brew][sync][clock-type]") {
    constexpr int kBeatSamples = 24000;
    SyncRig rig;
    set_custom_ppqn(rig, 1.0f);  // one edge per beat: a 24000-sample period

    SECTION("zero means half the period, not a zero-width pulse") {
        rig.state().set_value(SyncProcessor::kTriggerLengthMs, 0.0f);
        rig.render(0.0, true, true, kBeatSamples);
        REQUIRE(first_pulse_width(rig.clock()) == kBeatSamples / 2);
    }

    SECTION("a positive length is still a fixed-width trigger") {
        rig.state().set_value(SyncProcessor::kTriggerLengthMs, 5.0f);
        rig.render(0.0, true, true, kBeatSamples);
        // 5 ms at 48 kHz, well under the ceiling of half a 24000-sample period.
        REQUIRE(first_pulse_width(rig.clock()) == 240);
    }
}

// -------------------------------------------------------------- the run signal

TEST_CASE("Sync's run signal type decides what the run output carries",
          "[brew][sync][run-signal]") {
    SECTION("Run holds a level for the whole run") {
        SyncRig rig;
        rig.state().set_value(SyncProcessor::kRunType, as_param(RunSignal::run));
        rig.render(0.0, true, true);
        REQUIRE(all_equal(rig.run(), 1.0f));
        rig.render(1.0, true, false);
        REQUIRE(all_equal(rig.run(), 1.0f));
        rig.render(2.0, false, false);
        REQUIRE(all_equal(rig.run(), 0.0f));
    }

    SECTION("Start pulses once at the play edge and never again") {
        SyncRig rig;
        rig.state().set_value(SyncProcessor::kRunType, as_param(RunSignal::start));
        rig.render(0.0, true, true);
        REQUIRE(rising_edges(rig.run()) == std::vector<int>{0});
        // 5 ms is 240 samples, so the pulse has finished inside this 512-frame block.
        REQUIRE(first_pulse_width(rig.run()) == 240);

        rig.render(0.25, true, false);
        REQUIRE(all_equal(rig.run(), 0.0f));
    }

    SECTION("Stop pulses when the transport stops, not when it starts") {
        SyncRig rig;
        rig.state().set_value(SyncProcessor::kRunType, as_param(RunSignal::stop));
        rig.render(0.0, true, true);
        REQUIRE(all_equal(rig.run(), 0.0f));
        rig.render(0.25, false, false);
        REQUIRE(rising_edges(rig.run()) == std::vector<int>{0});
    }

    SECTION("Start/Stop pulses at both edges") {
        SyncRig rig;
        rig.state().set_value(SyncProcessor::kRunType, as_param(RunSignal::start_stop));
        rig.render(0.0, true, true);
        REQUIRE(rising_edges(rig.run()) == std::vector<int>{0});
        rig.render(0.25, true, false);
        REQUIRE(all_equal(rig.run(), 0.0f));
        rig.render(0.5, false, false);
        REQUIRE(rising_edges(rig.run()) == std::vector<int>{0});
    }

    SECTION("a stop pulse wider than the block finishes on the stopped path") {
        SyncRig rig;
        rig.state().set_value(SyncProcessor::kRunType, as_param(RunSignal::stop));
        // 20 ms is 960 samples: it cannot fit in one 512-frame block.
        rig.state().set_value(SyncProcessor::kRunPulseMs, 20.0f);
        rig.render(0.0, true, true);
        rig.render(0.25, false, false);
        REQUIRE(all_equal(rig.run(), 1.0f));  // still high at the block's end
        rig.render(0.25, false, false);
        REQUIRE(rig.run().front() > 0.0f);
        REQUIRE(rig.run().back() == 0.0f);  // and it falls in the next one
    }
}

// ----------------------------------------------------------- the periodic reset

TEST_CASE("Sync's periodic reset fires at the interval, measured from the run",
          "[brew][sync][periodic-reset]") {
    constexpr int kBeatSamples = 24000;

    SECTION("one pulse per beat, and the play edge is not double-triggered") {
        SyncRig rig;
        rig.state().set_value(SyncProcessor::kRunType, as_param(RunSignal::start));
        rig.state().set_value(SyncProcessor::kResetBeats, 1.0f);
        rig.state().set_value(SyncProcessor::kResetUnit, as_param(NoteUnit::quarter));
        rig.render(0.0, true, true, 2 * kBeatSamples);
        // Beat 0 is the play edge's own pulse; beat 1 is the first periodic one.
        REQUIRE(rising_edges(rig.run()) == std::vector<int>{0, kBeatSamples});
    }

    // The reset grid's index 0 sits exactly on the run origin. With `Start` that
    // is invisible — the play edge has already pulsed at that sample, and firing
    // again only retriggers a pulse that is already high. With `Stop` nothing has
    // pulsed there, so an unguarded index 0 emits a reset the moment the user
    // presses play. That is the bug the guard exists for, and this is the only
    // configuration that can see it.
    SECTION("Stop's periodic resets do not put a pulse on the play edge") {
        SyncRig rig;
        rig.state().set_value(SyncProcessor::kRunType, as_param(RunSignal::stop));
        rig.state().set_value(SyncProcessor::kResetBeats, 1.0f);
        rig.state().set_value(SyncProcessor::kResetUnit, as_param(NoteUnit::quarter));
        rig.render(0.0, true, true, 2 * kBeatSamples);
        REQUIRE(rising_edges(rig.run()) == std::vector<int>{kBeatSamples});
    }

    SECTION("the interval is counted from the run origin, not the timeline") {
        SyncRig rig;
        rig.state().set_value(SyncProcessor::kRunType, as_param(RunSignal::start));
        rig.state().set_value(SyncProcessor::kResetBeats, 2.0f);
        rig.state().set_value(SyncProcessor::kResetUnit, as_param(NoteUnit::quarter));
        // Start at beat 3.5, which is not a multiple of the two-beat interval.
        rig.render(3.5, true, true, 3 * kBeatSamples);
        REQUIRE(rising_edges(rig.run()) == std::vector<int>{0, 2 * kBeatSamples});
    }

    SECTION("the note unit scales the interval") {
        SyncRig rig;
        rig.state().set_value(SyncProcessor::kRunType, as_param(RunSignal::start));
        rig.state().set_value(SyncProcessor::kResetBeats, 1.0f);
        rig.state().set_value(SyncProcessor::kResetUnit, as_param(NoteUnit::half));
        rig.render(0.0, true, true, 3 * kBeatSamples);
        REQUIRE(rising_edges(rig.run()) == std::vector<int>{0, 2 * kBeatSamples});
    }

    SECTION("zero beats disables it") {
        SyncRig rig;
        rig.state().set_value(SyncProcessor::kRunType, as_param(RunSignal::start));
        rig.state().set_value(SyncProcessor::kResetBeats, 0.0f);
        rig.render(0.0, true, true, 2 * kBeatSamples);
        REQUIRE(rising_edges(rig.run()) == std::vector<int>{0});
    }

    SECTION("a level run output has no periodic reset to give") {
        SyncRig rig;
        rig.state().set_value(SyncProcessor::kRunType, as_param(RunSignal::run));
        rig.state().set_value(SyncProcessor::kResetBeats, 1.0f);
        rig.render(0.0, true, true, 2 * kBeatSamples);
        REQUIRE(all_equal(rig.run(), 1.0f));
    }
}

// ------------------------------------------------------------- signal routing

TEST_CASE("Sync sums its inputs into its outputs", "[brew][sync][routing]") {
    SECTION("the input rides on top of the clock and the run gate") {
        SyncRig rig;
        rig.state().set_value(SyncProcessor::kClockType, as_param(ClockType::off));
        rig.set_input_level(0.25f);
        rig.render(0.0, true, true);
        REQUIRE(all_equal(rig.clock(), 0.25f));
        REQUIRE(all_equal(rig.run(), 1.0f));  // 1.0 + 0.25, clamped at the jack
    }

    SECTION("bypass is a wire, not a mute") {
        SyncRig rig;
        rig.set_input_level(0.25f);
        format::ProcessContext ctx;
        ctx.sample_rate = kSampleRate;
        ctx.num_samples = kFrames;
        ctx.is_playing = true;
        ctx.transport_started = true;
        ctx.tempo_bpm = kTempo;
        ctx.position_beats = 0.0;
        ctx.is_bypassed = true;
        rig.render(ctx);
        REQUIRE(all_equal(rig.clock(), 0.25f));
        REQUIRE(all_equal(rig.run(), 0.25f));
    }
}
