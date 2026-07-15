// Freeze coverage for the parametric SequencerStateChannel: a second
// (non-square) config, a custom cell type + reducer, the shared step_edit_reducer
// (apply + overflow recovery), and a NEUTRAL non-Processor producer driving the
// channel end to end. The reference-config behavior is covered by
// test_sequencer_state_channel.cpp; this file proves generalization + the SDK
// reducer contract.
#include <catch2/catch_test_macros.hpp>

#include <pulp/state/sequencer_state_channel.hpp>
#include <pulp/state/step_edit_reducer.hpp>

#include <cstdint>
#include <optional>
#include <type_traits>

using namespace pulp::state;

namespace {

// A deliberately non-square config (Lanes != Steps) to catch transposed
// lane/step indexing — the exact bug class templating can introduce.
using YssConfig = SequencerConfig<5, 16, 8, StepCell>;
using YssChannel = SequencerStateChannelT<YssConfig>;

// A custom cell whose field names are DISJOINT from StepCell, so any accidental
// dependency on StepCell field names fails to compile rather than hiding.
struct VineCell {
    std::uint16_t growth = 0;
    std::uint8_t active = 0;
    std::uint8_t hue = 0;
};
using VineConfig = SequencerConfig<8, 12, 4, VineCell>;
using VineChannel = SequencerStateChannelT<VineConfig>;

// Frozen operational contract (Codex): the composed transport types — not just
// the cell — must be trivially copyable, copy-assignable, and default-constructible.
template <class Ch>
constexpr bool transport_conforms() {
    return std::is_trivially_copyable_v<typename Ch::Snapshot> &&
           std::is_copy_assignable_v<typename Ch::Snapshot> &&
           std::is_default_constructible_v<typename Ch::Snapshot> &&
           std::is_trivially_copyable_v<typename Ch::Command> &&
           std::is_trivially_copyable_v<typename Ch::AppliedEdit>;
}
static_assert(transport_conforms<SequencerStateChannel>(), "reference config");
static_assert(transport_conforms<YssChannel>(), "second config");
static_assert(transport_conforms<VineChannel>(), "custom-cell config");

// Config dims are MAXIMUM CAPACITY; active extent is explicit in the snapshot.
static_assert(YssChannel::Snapshot{}.active_lane_count == 5);
static_assert(YssChannel::Snapshot{}.active_pattern_count == 8);
static_assert(VineChannel::Snapshot{}.active_lane_count == 8);

// SetCell carries the config's cell by value.
static_assert(std::is_same_v<decltype(YssChannel::SetCellEdit{}.cell), StepCell>);
static_assert(std::is_same_v<decltype(VineChannel::SetCellEdit{}.cell), VineCell>);

template <class Config>
StepEditCommandT<Config> set_cell_cmd(std::uint8_t pat, std::uint8_t lane,
                                      std::uint8_t step,
                                      typename Config::cell_type cell,
                                      ClientSequence cs) {
    StepEditCommandT<Config> c{};
    c.client_sequence = cs;
    c.kind = StepEditKind::SetCell;
    SetCellEditT<Config> e{};
    e.pattern = pat; e.lane = lane; e.step = step; e.cell = cell;
    c.payload.set_cell = e;
    return c;
}

} // namespace

TEST_CASE("Reducer drain_and_apply round-trips a set-cell on the reference config",
          "[state][sequencer][freeze]") {
    SequencerStateChannel ch;
    Snapshot snap; Epoch epoch = 0; EngineSequence seq = 0;

    StepCell cell{}; cell.flags = StepCell::kEnabledBit; cell.velocity = 88;
    REQUIRE(ch.ui_try_submit(set_cell_cmd<ReferenceSequencerConfig>(1, 2, 3, cell, 1)));
    drain_and_apply(ch, snap, epoch, seq);

    REQUIRE(snap.patterns[1].lanes[2][3].velocity == 88);
    REQUIRE(seq == 1);
    auto echo = ch.ui_try_pop_applied();
    REQUIRE(echo.has_value());
    REQUIRE(echo->engine_sequence == 1);
    REQUIRE(echo->payload.step_range.cells[0].velocity == 88);
}

TEST_CASE("Second non-square config round-trips a set-cell without transposing indices",
          "[state][sequencer][freeze]") {
    YssChannel ch;
    YssChannel::Snapshot snap; Epoch epoch = 0; EngineSequence seq = 0;

    // lane 4 (< 5), step 15 (< 16): valid only if indexing is [lane][step].
    StepCell cell{}; cell.flags = StepCell::kEnabledBit; cell.velocity = 77;
    REQUIRE(ch.ui_try_submit(set_cell_cmd<YssConfig>(7, 4, 15, cell, 1)));
    drain_and_apply(ch, snap, epoch, seq);

    REQUIRE(snap.patterns[7].lanes[4][15].velocity == 77);
    auto echo = ch.ui_try_pop_applied();
    REQUIRE(echo.has_value());
    REQUIRE(echo->kind == AppliedEditKind::StepRangeChanged);

    // An out-of-range lane (>= 5) is rejected, not written OOB.
    REQUIRE(ch.ui_try_submit(set_cell_cmd<YssConfig>(0, 5, 0, cell, 2)));
    drain_and_apply(ch, snap, epoch, seq);
    auto reject = ch.ui_try_pop_applied();
    REQUIRE(reject.has_value());
    REQUIRE(reject->kind == AppliedEditKind::CommandRejected);
}

TEST_CASE("Custom-cell config round-trips through a caller-supplied reducer",
          "[state][sequencer][freeze]") {
    VineChannel ch;
    VineChannel::Snapshot snap; Epoch epoch = 0; EngineSequence seq = 0;

    // A custom reducer for VineCell (apply_step_edit is StepCell-only by design).
    auto vine_reduce = [](VineChannel::Snapshot& s,
                          const VineChannel::Command& cmd,
                          EngineSequence& es) -> std::optional<VineChannel::AppliedEdit> {
        VineChannel::AppliedEdit echo{};
        echo.engine_sequence = ++es;
        echo.client_sequence = cmd.client_sequence;
        if (cmd.kind != StepEditKind::SetCell) return std::nullopt;
        const auto& e = cmd.payload.set_cell;
        s.patterns[e.pattern].lanes[e.lane][e.step] = e.cell;
        echo.kind = AppliedEditKind::StepRangeChanged;
        echo.dirty = {DirtyKind::Cell, e.pattern, e.lane, e.step, 1};
        VineChannel::StepRangeApplied sr{};
        sr.pattern = e.pattern; sr.lane = e.lane; sr.first_step = e.step;
        sr.step_count = 1; sr.cells[0] = e.cell;
        echo.payload.step_range = sr;
        return echo;
    };

    VineCell cell{}; cell.growth = 4096; cell.active = 1; cell.hue = 200;
    REQUIRE(ch.ui_try_submit(set_cell_cmd<VineConfig>(3, 6, 11, cell, 1)));
    drain_and_apply(ch, snap, epoch, seq, vine_reduce);

    REQUIRE(snap.patterns[3].lanes[6][11].growth == 4096);
    REQUIRE(snap.patterns[3].lanes[6][11].hue == 200);
    auto echo = ch.ui_try_pop_applied();
    REQUIRE(echo.has_value());
    REQUIRE(echo->payload.step_range.cells[0].growth == 4096);
}

TEST_CASE("Reducer overflow recovery republishes the snapshot and raises the resync bar",
          "[state][sequencer][freeze]") {
    SequencerStateChannel ch;
    Snapshot snap; Epoch epoch = 0; EngineSequence seq = 0;

    // Flood far past the applied-FIFO capacity WITHOUT the UI draining: the
    // reducer must republish the snapshot and raise the resync bar rather than
    // silently dropping echoes.
    StepCell cell{}; cell.flags = StepCell::kEnabledBit; cell.velocity = 100;
    for (std::size_t i = 0; i < kAppliedQueueCapacity + 64; ++i) {
        // Submit + drain one at a time so commands never overflow.
        ch.ui_try_submit(set_cell_cmd<ReferenceSequencerConfig>(
            0, 0, static_cast<std::uint8_t>(i % kStepCount), cell,
            static_cast<ClientSequence>(i + 1)));
        drain_and_apply(ch, snap, epoch, seq);
    }
    REQUIRE(ch.ui_applied_telemetry().overflow_count > 0);
    REQUIRE(ch.ui_resync_required_epoch() > 0);
    // The republished snapshot carries the latest engine_sequence (UI can resync).
    REQUIRE(ch.ui_read_latest_snapshot().engine_sequence == seq);
    REQUIRE(ch.ui_read_latest_snapshot().epoch == ch.ui_resync_required_epoch());
}

TEST_CASE("Reducer normalizes pattern-length 0 to full so view and playback agree",
          "[state][sequencer][freeze]") {
    SequencerStateChannel ch;
    Snapshot snap; Epoch epoch = 0; EngineSequence seq = 0;

    // length 0 must mean "full" (kStepCount), not "all steps muted".
    StepEditCommand cmd{};
    cmd.client_sequence = 1;
    cmd.kind = StepEditKind::SetPatternLength;
    cmd.payload.set_pattern_length = SetPatternLengthEdit{2, 0};
    ch.ui_try_submit(cmd);
    drain_and_apply(ch, snap, epoch, seq);

    REQUIRE(snap.patterns[2].length == kStepCount);
    auto echo = ch.ui_try_pop_applied();
    REQUIRE(echo.has_value());
    REQUIRE(echo->kind == AppliedEditKind::PatternLengthChanged);
    REQUIRE(echo->payload.pattern_length.length == kStepCount);  // view sees full, not 0
}

TEST_CASE("A neutral non-Processor producer drives the channel and the UI converges",
          "[state][sequencer][freeze]") {
    // The whole point of Q3/Q5: the authoritative writer need NOT be a
    // pulp::format::Processor. This "engine" is a bare struct that owns a channel
    // and drives it via the shared reducer — no <pulp/format/...> anywhere.
    struct NeutralEngine {
        SequencerStateChannel channel;
        Snapshot snapshot{};
        Epoch epoch = 0;
        EngineSequence seq = 0;
        void tick() { drain_and_apply(channel, snapshot, epoch, seq); }
    };

    NeutralEngine engine;

    // A UI render copy replaying echoes (mirrors what StepGridView does).
    Snapshot ui{}; EngineSequence ui_last = 0;
    auto ui_replay = [&](const AppliedEdit& e) {
        if (e.engine_sequence <= ui_last) return;
        ui_last = e.engine_sequence;
        if (e.kind == AppliedEditKind::StepRangeChanged) {
            const auto& sr = e.payload.step_range;
            for (std::uint8_t i = 0; i < sr.step_count; ++i)
                ui.patterns[sr.pattern].lanes[sr.lane][sr.first_step + i] = sr.cells[i];
        }
    };

    StepCell cell{}; cell.flags = StepCell::kEnabledBit;
    for (std::uint8_t s = 0; s < 20; ++s) {
        cell.velocity = static_cast<std::uint8_t>(10 + s);
        engine.channel.ui_try_submit(
            set_cell_cmd<ReferenceSequencerConfig>(0, 1, s, cell, s + 1));
    }
    engine.tick();  // neutral engine applies all + echoes
    while (auto e = engine.channel.ui_try_pop_applied()) ui_replay(*e);

    for (std::uint8_t s = 0; s < 20; ++s)
        REQUIRE(ui.patterns[0].lanes[1][s].velocity == engine.snapshot.patterns[0].lanes[1][s].velocity);
    REQUIRE(ui.patterns[0].lanes[1][19].velocity == 29);
}

TEST_CASE("Undo transaction grouping survives the echo round trip",
          "[state][sequencer][freeze]") {
    SequencerStateChannel ch;
    Snapshot snap; Epoch epoch = 0; EngineSequence seq = 0;

    // A gesture: Begin / Update / End sharing one transaction_id.
    const EditTransactionId txn = 42;
    StepCell cell{}; cell.flags = StepCell::kEnabledBit; cell.velocity = 64;
    for (auto phase : {GesturePhase::Begin, GesturePhase::Update, GesturePhase::End}) {
        auto cmd = set_cell_cmd<ReferenceSequencerConfig>(0, 0, 0, cell, static_cast<ClientSequence>(phase));
        cmd.transaction_id = txn;
        cmd.gesture_phase = phase;
        ch.ui_try_submit(cmd);
    }
    drain_and_apply(ch, snap, epoch, seq);

    int echoes = 0;
    while (auto e = ch.ui_try_pop_applied()) {
        REQUIRE(e->transaction_id == txn);  // grouping preserved through apply
        ++echoes;
    }
    REQUIRE(echoes == 3);
}
