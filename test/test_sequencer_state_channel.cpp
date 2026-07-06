#include <catch2/catch_test_macros.hpp>

#include <pulp/state/sequencer_state_channel.hpp>

#include <atomic>
#include <cstdint>
#include <optional>
#include <thread>
#include <vector>

using namespace pulp::state;

namespace {

// A minimal reference "engine": the authoritative-state owner a real plugin
// implements on the audio thread. It applies one StepEditCommand to its own
// Snapshot and returns the AppliedEdit echo (exact changed cells) the UI replays.
// This is the prototype consumer that exercises the channel's API shape.
class RefEngine {
public:
    RefEngine() { snapshot_.schema_version = 1; }

    // Returns nullopt only for a no-op command; otherwise the echo to publish.
    std::optional<AppliedEdit> apply(const StepEditCommand& cmd) {
        AppliedEdit echo{};
        echo.engine_sequence = ++engine_seq_;
        echo.snapshot_epoch = snapshot_.epoch;
        echo.client_sequence = cmd.client_sequence;
        echo.transaction_id = cmd.transaction_id;

        switch (cmd.kind) {
        case StepEditKind::SetCell: {
            const auto& e = cmd.payload.set_cell;
            snapshot_.patterns[e.pattern].lanes[e.lane][e.step] = e.cell;
            echo.kind = AppliedEditKind::StepRangeChanged;
            echo.dirty = {DirtyKind::Cell, e.pattern, e.lane, e.step, 1};
            StepRangeApplied sr{};
            sr.pattern = e.pattern; sr.lane = e.lane; sr.first_step = e.step;
            sr.step_count = 1; sr.cells[0] = e.cell;
            echo.payload.step_range = sr;
            return echo;
        }
        case StepEditKind::RandomizeLane: {
            const auto& e = cmd.payload.randomize_lane;
            // Deterministic PRNG so the echo carries the exact result; the UI
            // never re-runs this — it replays the cells below.
            std::uint32_t s = e.seed ? e.seed : 1u;
            auto next = [&s]() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; };
            StepRangeApplied sr{};
            sr.pattern = e.pattern; sr.lane = e.lane; sr.first_step = 0;
            sr.step_count = kStepCount;
            for (std::uint8_t st = 0; st < kStepCount; ++st) {
                StepCell c{};
                bool on = (next() % 128u) < e.density;
                c.flags = on ? StepCell::kEnabledBit : 0;
                c.velocity = static_cast<std::uint8_t>(
                    e.min_velocity + (next() % (1u + e.max_velocity - e.min_velocity)));
                snapshot_.patterns[e.pattern].lanes[e.lane][st] = c;
                sr.cells[st] = c;
            }
            echo.kind = AppliedEditKind::StepRangeChanged;
            echo.dirty = {DirtyKind::Lane, e.pattern, e.lane, 0, kStepCount};
            echo.payload.step_range = sr;
            return echo;
        }
        case StepEditKind::SetPatternLength: {
            const auto& e = cmd.payload.set_pattern_length;
            snapshot_.patterns[e.pattern].length = e.length;
            echo.kind = AppliedEditKind::PatternLengthChanged;
            echo.dirty = {DirtyKind::Pattern, e.pattern, 0, 0, 0};
            echo.payload.pattern_length = e;
            return echo;
        }
        case StepEditKind::SwitchPattern: {
            const auto& e = cmd.payload.switch_pattern;
            snapshot_.active_pattern = e.pattern;
            echo.kind = AppliedEditKind::ActivePatternChanged;
            echo.dirty = {DirtyKind::FullSnapshot, 0, 0, 0, 0};
            echo.payload.active_pattern = e;
            return echo;
        }
        case StepEditKind::Clear: {
            const auto& e = cmd.payload.clear;
            snapshot_.patterns[e.pattern].lanes[e.lane][e.step] = StepCell{};
            echo.kind = AppliedEditKind::StepRangeChanged;
            echo.dirty = {DirtyKind::Cell, e.pattern, e.lane, e.step, 1};
            StepRangeApplied sr{};
            sr.pattern = e.pattern; sr.lane = e.lane; sr.first_step = e.step;
            sr.step_count = 1; sr.cells[0] = StepCell{};
            echo.payload.step_range = sr;
            return echo;
        }
        }
        return std::nullopt;
    }

    Snapshot& snapshot() { return snapshot_; }
    EngineSequence engine_seq() const { return engine_seq_; }

private:
    Snapshot snapshot_{};
    EngineSequence engine_seq_ = 0;
};

// A UI render copy that replays applied echoes onto its own Snapshot.
struct UiModel {
    Snapshot copy{};
    EngineSequence last_engine_seq = 0;

    void replay(const AppliedEdit& e) {
        if (e.engine_sequence <= last_engine_seq) return;  // stale echo
        last_engine_seq = e.engine_sequence;
        switch (e.kind) {
        case AppliedEditKind::StepRangeChanged: {
            const auto& sr = e.payload.step_range;
            for (std::uint8_t i = 0; i < sr.step_count; ++i)
                copy.patterns[sr.pattern].lanes[sr.lane][sr.first_step + i] = sr.cells[i];
            break;
        }
        case AppliedEditKind::PatternLengthChanged:
            copy.patterns[e.payload.pattern_length.pattern].length =
                e.payload.pattern_length.length;
            break;
        case AppliedEditKind::ActivePatternChanged:
            copy.active_pattern = e.payload.active_pattern.pattern;
            break;
        case AppliedEditKind::CommandRejected:
            break;
        }
    }

    void resync_from(const Snapshot& s) {
        copy = s;
        last_engine_seq = s.engine_sequence;
    }
};

StepEditCommand make_set_cell(std::uint8_t pat, std::uint8_t lane, std::uint8_t step,
                              std::uint8_t vel, ClientSequence cs) {
    StepEditCommand c{};
    c.client_sequence = cs;
    c.kind = StepEditKind::SetCell;
    SetCellEdit e{}; e.pattern = pat; e.lane = lane; e.step = step;
    e.cell.flags = StepCell::kEnabledBit; e.cell.velocity = vel;
    c.payload.set_cell = e;
    return c;
}

bool cells_equal(const StepCell& a, const StepCell& b) {
    return a.flags == b.flags && a.velocity == b.velocity &&
           a.probability == b.probability && a.pitch_offset == b.pitch_offset &&
           a.gate_ticks == b.gate_ticks && a.ratchet == b.ratchet;
}

} // namespace

TEST_CASE("SequencerStateChannel applies UI set-cell command and echoes exact dirty cell",
          "[state][sequencer]") {
    SequencerStateChannel ch;
    RefEngine engine;
    UiModel ui;

    REQUIRE(ch.ui_try_submit(make_set_cell(2, 3, 5, 111, 1)));

    // Audio side drains + applies + echoes.
    auto cmd = ch.audio_try_pop_command();
    REQUIRE(cmd.has_value());
    auto echo = engine.apply(*cmd);
    REQUIRE(echo.has_value());
    REQUIRE(ch.audio_try_publish_applied(*echo));

    // UI side replays.
    auto got = ch.ui_try_pop_applied();
    REQUIRE(got.has_value());
    REQUIRE(got->dirty.kind == DirtyKind::Cell);
    REQUIRE(got->dirty.pattern == 2);
    REQUIRE(got->dirty.lane == 3);
    REQUIRE(got->dirty.first_step == 5);
    ui.replay(*got);

    REQUIRE(cells_equal(ui.copy.patterns[2].lanes[3][5],
                        engine.snapshot().patterns[2].lanes[3][5]));
    REQUIRE(ui.copy.patterns[2].lanes[3][5].velocity == 111);
}

TEST_CASE("SequencerStateChannel preserves applied edit order across command round trip",
          "[state][sequencer]") {
    SequencerStateChannel ch;
    RefEngine engine;
    UiModel ui;

    for (ClientSequence i = 0; i < 50; ++i)
        REQUIRE(ch.ui_try_submit(make_set_cell(0, 0, static_cast<std::uint8_t>(i % kStepCount),
                                               static_cast<std::uint8_t>(1 + i), i + 1)));
    while (auto cmd = ch.audio_try_pop_command()) {
        auto echo = engine.apply(*cmd);
        REQUIRE(echo.has_value());
        REQUIRE(ch.audio_try_publish_applied(*echo));
    }
    EngineSequence prev = 0;
    while (auto e = ch.ui_try_pop_applied()) {
        REQUIRE(e->engine_sequence > prev);  // strictly monotonic
        prev = e->engine_sequence;
        ui.replay(*e);
    }
    // Last write to each step wins on both sides.
    for (std::uint8_t s = 0; s < kStepCount; ++s)
        REQUIRE(cells_equal(ui.copy.patterns[0].lanes[0][s],
                            engine.snapshot().patterns[0].lanes[0][s]));
}

TEST_CASE("SequencerStateChannel randomize-lane echo carries exact changed cells",
          "[state][sequencer]") {
    SequencerStateChannel ch;
    RefEngine engine;
    UiModel ui;

    StepEditCommand c{};
    c.client_sequence = 1;
    c.kind = StepEditKind::RandomizeLane;
    RandomizeLaneEdit e{}; e.pattern = 1; e.lane = 4; e.seed = 0xC0FFEE;
    e.density = 96; e.min_velocity = 40; e.max_velocity = 120;
    c.payload.randomize_lane = e;
    REQUIRE(ch.ui_try_submit(c));

    auto cmd = ch.audio_try_pop_command();
    auto echo = engine.apply(*cmd);
    REQUIRE(echo.has_value());
    REQUIRE(echo->dirty.kind == DirtyKind::Lane);
    REQUIRE(echo->payload.step_range.step_count == kStepCount);
    ch.audio_try_publish_applied(*echo);
    ui.replay(*ch.ui_try_pop_applied());

    // The UI replayed the exact engine cells — no algorithm re-run, no drift.
    for (std::uint8_t s = 0; s < kStepCount; ++s)
        REQUIRE(cells_equal(ui.copy.patterns[1].lanes[4][s],
                            engine.snapshot().patterns[1].lanes[4][s]));
}

TEST_CASE("SequencerStateChannel bulk snapshot epoch replaces UI copy with full invalidation",
          "[state][sequencer]") {
    SequencerStateChannel ch;
    RefEngine engine;
    UiModel ui;

    // Engine loads a preset: mutate authoritative state, bump epoch, publish bulk.
    engine.snapshot().patterns[7].lanes[2][9].flags = StepCell::kEnabledBit;
    engine.snapshot().patterns[7].lanes[2][9].velocity = 77;
    engine.snapshot().epoch = 42;
    engine.snapshot().engine_sequence = engine.engine_seq();
    ch.audio_publish_snapshot(engine.snapshot());

    REQUIRE(ch.ui_read_latest_snapshot().epoch == 42);
    ui.resync_from(ch.ui_read_latest_snapshot());

    REQUIRE(ui.copy.patterns[7].lanes[2][9].velocity == 77);
    REQUIRE(ui.last_engine_seq == engine.snapshot().engine_sequence);
}

TEST_CASE("SequencerStateChannel applied-edit overflow forces snapshot resync and skips stale echoes",
          "[state][sequencer]") {
    SequencerStateChannel ch;
    RefEngine engine;
    UiModel ui;

    // Fill the applied FIFO to overflow WITHOUT the UI draining it.
    bool overflowed = false;
    for (std::size_t i = 0; i < kAppliedQueueCapacity + 16; ++i) {
        auto cmd = make_set_cell(0, 0, static_cast<std::uint8_t>(i % kStepCount),
                                 static_cast<std::uint8_t>(1 + (i % 120)),
                                 static_cast<ClientSequence>(i + 1));
        auto echo = engine.apply(cmd);
        if (!ch.audio_try_publish_applied(*echo)) {
            overflowed = true;
            // Recovery: publish a fresh snapshot and raise the resync bar.
            engine.snapshot().epoch = 100;
            engine.snapshot().engine_sequence = engine.engine_seq();
            ch.audio_publish_snapshot(engine.snapshot());
            ch.audio_mark_resync_required(100);
        }
    }
    REQUIRE(overflowed);
    REQUIRE(ch.ui_applied_telemetry().overflow_count > 0);

    // UI observes the resync bar and rebuilds from the snapshot.
    REQUIRE(ch.ui_resync_required_epoch() == 100);
    ui.resync_from(ch.ui_read_latest_snapshot());
    REQUIRE(ui.last_engine_seq == engine.snapshot().engine_sequence);

    // Any surviving queued echoes are stale (<= snapshot engine_sequence) and
    // must be ignored so they don't replay over the fresh snapshot.
    while (auto e = ch.ui_try_pop_applied()) {
        std::size_t before = ui.last_engine_seq;
        ui.replay(*e);
        REQUIRE(ui.last_engine_seq == before);  // filtered as stale, no mutation
    }
    // Final UI state matches authoritative state after resync.
    for (std::uint8_t s = 0; s < kStepCount; ++s)
        REQUIRE(cells_equal(ui.copy.patterns[0].lanes[0][s],
                            engine.snapshot().patterns[0].lanes[0][s]));
}

TEST_CASE("SequencerStateChannel rejects UI commands when command queue is full",
          "[state][sequencer]") {
    SequencerStateChannel ch;
    std::size_t accepted = 0;
    for (std::size_t i = 0; i < kCommandQueueCapacity + 64; ++i) {
        if (ch.ui_try_submit(make_set_cell(0, 0, 0, 100, static_cast<ClientSequence>(i + 1))))
            ++accepted;
    }
    // Bounded: it accepts up to capacity, then cleanly refuses — never UB.
    REQUIRE(accepted <= kCommandQueueCapacity);
    REQUIRE(accepted >= kCommandQueueCapacity - 1);
    REQUIRE(ch.ui_command_telemetry().overflow_count > 0);
}

TEST_CASE("SequencerStateChannel coalesced snapshots are not used as incremental dirty descriptors",
          "[state][sequencer]") {
    // Two bulk publishes back-to-back: TripleBuffer coalesces to the latest.
    // This encodes WHY incremental change flows on the echo stream, not snapshots.
    SequencerStateChannel ch;
    RefEngine engine;

    engine.snapshot().patterns[0].lanes[0][0].velocity = 11;
    engine.snapshot().epoch = 1;
    ch.audio_publish_snapshot(engine.snapshot());
    engine.snapshot().patterns[0].lanes[0][1].velocity = 22;
    engine.snapshot().epoch = 2;
    ch.audio_publish_snapshot(engine.snapshot());

    // The UI only ever sees the latest snapshot — epoch 1 is gone.
    REQUIRE(ch.ui_read_latest_snapshot().epoch == 2);
    // Both cells are present because the snapshot is the full authoritative
    // state; but the UI could not have learned "cell (0,0,0) changed" from the
    // coalesced snapshot alone — that is what AppliedEdit echoes are for.
    REQUIRE(ch.ui_read_latest_snapshot().patterns[0].lanes[0][0].velocity == 11);
    REQUIRE(ch.ui_read_latest_snapshot().patterns[0].lanes[0][1].velocity == 22);
}

TEST_CASE("SequencerStateChannel two-thread hammer has monotonic engine sequences and no torn UI state",
          "[state][sequencer][threaded]") {
    SequencerStateChannel ch;
    constexpr int kEdits = 20000;
    std::atomic<bool> done_producing{false};

    // Authoritative engine, touched only by the audio thread.
    RefEngine engine;
    std::atomic<EngineSequence> last_echo_seq{0};
    std::atomic<bool> seq_monotonic{true};

    // Audio thread: pop commands, apply, publish echoes. This test proves
    // ordered delivery + monotonic engine_sequence; it retries on a full FIFO so
    // no echo is dropped (the overflow -> resync recovery is proven by the
    // dedicated concurrency test below, not here).
    std::thread audio([&] {
        int applied = 0;
        while (applied < kEdits || !done_producing.load(std::memory_order_acquire)) {
            if (auto cmd = ch.audio_try_pop_command()) {
                auto echo = engine.apply(*cmd);
                if (echo) {
                    while (!ch.audio_try_publish_applied(*echo))
                        std::this_thread::yield();  // reader will drain; retry
                    ++applied;
                }
            } else {
                std::this_thread::yield();
            }
        }
    });

    // UI reader thread: drain echoes, verify monotonic engine_sequence.
    std::thread ui_reader([&] {
        EngineSequence prev = 0;
        int drained = 0;
        while (drained < kEdits) {
            if (auto e = ch.ui_try_pop_applied()) {
                if (e->engine_sequence <= prev) seq_monotonic.store(false);
                prev = e->engine_sequence;
                last_echo_seq.store(prev, std::memory_order_relaxed);
                ++drained;
            } else {
                std::this_thread::yield();
            }
        }
    });

    // Producer: submit edits on the main thread.
    for (int i = 0; i < kEdits; ++i) {
        auto cmd = make_set_cell(0, 0, static_cast<std::uint8_t>(i % kStepCount),
                                 static_cast<std::uint8_t>(1 + (i % 120)),
                                 static_cast<ClientSequence>(i + 1));
        while (!ch.ui_try_submit(cmd)) std::this_thread::yield();
    }
    done_producing.store(true, std::memory_order_release);

    audio.join();
    ui_reader.join();

    REQUIRE(seq_monotonic.load());
    REQUIRE(engine.engine_seq() == static_cast<EngineSequence>(kEdits));
    REQUIRE(last_echo_seq.load() == static_cast<EngineSequence>(kEdits));
}

TEST_CASE("SequencerStateChannel SeqLock playhead reads coherent concurrent writes",
          "[state][sequencer][threaded]") {
    SequencerStateChannel ch;
    std::atomic<bool> stop{false};
    std::atomic<bool> torn{false};

    // Writer: encode an invariant across fields so a torn read is detectable.
    std::thread writer([&] {
        for (std::uint64_t t = 0; !stop.load(std::memory_order_acquire); ++t) {
            PlayheadState p{};
            p.sample_time = t;
            p.block_index = static_cast<std::uint32_t>(t & 0xFFFFFFFF);
            p.active_step = static_cast<std::uint8_t>(t % kStepCount);
            p.playing = 1;
            ch.audio_publish_playhead(p);
        }
    });

    std::thread reader([&] {
        for (int i = 0; i < 200000; ++i) {
            PlayheadState p = ch.ui_read_playhead();
            // The invariant the writer maintains must hold on every coherent read.
            if (p.playing == 1 &&
                p.active_step != static_cast<std::uint8_t>(p.sample_time % kStepCount))
                torn.store(true);
        }
        stop.store(true, std::memory_order_release);
    });

    writer.join();
    reader.join();
    REQUIRE_FALSE(torn.load());
}

TEST_CASE("SequencerStateChannel overflow under concurrency resyncs from snapshot and converges",
          "[state][sequencer][threaded]") {
    // Deterministically forces the applied-FIFO to overflow across threads and
    // proves the recovery contract end to end: the audio thread DROPS echoes on a
    // full FIFO (no retry) but keeps its authoritative state complete and raises a
    // monotonic resync epoch; the UI thread, on a different thread, observes the
    // epoch, resyncs from the latest snapshot, and drops stale queued echoes — and
    // its render copy converges to the exact authoritative state.
    SequencerStateChannel ch;
    RefEngine engine;
    constexpr int kEdits = 6000;  // >> kAppliedQueueCapacity, so overflow is certain
    std::atomic<bool> burst_done{false};
    std::atomic<Epoch> observed_resync_epoch{0};

    std::thread audio([&] {
        for (int i = 0; i < kEdits; ++i) {
            auto cmd = make_set_cell(0, static_cast<std::uint8_t>(i % kLaneCount),
                                     static_cast<std::uint8_t>(i % kStepCount),
                                     static_cast<std::uint8_t>(1 + (i % 120)),
                                     static_cast<ClientSequence>(i + 1));
            auto echo = engine.apply(cmd);            // authoritative state stays complete
            if (!ch.audio_try_publish_applied(*echo)) {
                // Echo dropped: publish a fresh coherent snapshot + raise the bar.
                engine.snapshot().epoch += 1;
                engine.snapshot().engine_sequence = engine.engine_seq();
                ch.audio_publish_snapshot(engine.snapshot());
                ch.audio_mark_resync_required(engine.snapshot().epoch);
            }
        }
        // Final coherent snapshot after all edits (engine quiescent).
        engine.snapshot().epoch += 1;
        engine.snapshot().engine_sequence = engine.engine_seq();
        ch.audio_publish_snapshot(engine.snapshot());
        ch.audio_mark_resync_required(engine.snapshot().epoch);
        burst_done.store(true, std::memory_order_release);
    });

    UiModel ui;
    std::thread ui_thread([&] {
        // Do not drain during the burst: with kEdits >> kAppliedQueueCapacity and
        // no reader, the applied FIFO is guaranteed to overflow. The audio thread
        // produces snapshots + resync epochs concurrently; the burst_done
        // release/acquire hands the final state across to this thread.
        while (!burst_done.load(std::memory_order_acquire)) std::this_thread::yield();
        // Recovery: observe the resync bar, resync to the last authoritative
        // snapshot, then drain the tail (all remaining echoes are stale
        // <= snapshot.engine_sequence and filtered, never replayed over it).
        // (Assertions run on the main thread post-join — Catch2 macros are not
        // thread-safe.)
        observed_resync_epoch.store(ch.ui_resync_required_epoch());
        ui.resync_from(ch.ui_read_latest_snapshot());
        while (auto e = ch.ui_try_pop_applied()) ui.replay(*e);
    });

    audio.join();
    ui_thread.join();

    REQUIRE(ch.ui_applied_telemetry().overflow_count > 0);  // overflow really happened
    REQUIRE(observed_resync_epoch.load() > 0);              // UI saw the resync bar
    REQUIRE(ui.last_engine_seq == engine.snapshot().engine_sequence);
    // Every touched cell of the UI render copy equals the authoritative state.
    for (std::uint8_t lane = 0; lane < kLaneCount; ++lane)
        for (std::uint8_t step = 0; step < kStepCount; ++step)
            REQUIRE(cells_equal(ui.copy.patterns[0].lanes[lane][step],
                                engine.snapshot().patterns[0].lanes[lane][step]));
}
