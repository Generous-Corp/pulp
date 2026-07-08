#pragma once

#include <optional>
#include <type_traits>

#include <pulp/state/sequencer_state_channel.hpp>

/// The audio-side edit logic for a SequencerStateChannel, lifted out of any one
/// engine so a producer never hand-rolls the protocol.
///
/// - apply_step_edit<Config>: the reference reducer. Mutates the authoritative
///   Snapshot for one command and returns the exact AppliedEdit echo the UI
///   replays (so the UI never diffs the grid or re-runs an engine algorithm).
///   Defined for the reference StepCell; a custom cell type supplies its own
///   reducer with the same signature.
/// - drain_and_apply<Config, Reducer>: drains the command FIFO, applies each
///   command via the reducer, publishes each echo, and OWNS the overflow
///   recovery — on a full applied-FIFO it republishes the authoritative Snapshot
///   and raises the resync bar, in the order that avoids live-locking the UI.
///
/// NEUTRAL: this header includes only sequencer_state_channel.hpp (runtime
/// primitives) — nothing from <pulp/format/...>. A non-Pulp engine can use it.
namespace pulp::state {

/// Reference reducer for the StepCell grid. Returns nullopt only for a no-op.
template <class Config>
std::optional<AppliedEditT<Config>> apply_step_edit(
    SnapshotT<Config>& snapshot,
    const StepEditCommandT<Config>& cmd,
    EngineSequence& engine_seq) {
    static_assert(std::is_same_v<typename Config::cell_type, StepCell>,
                  "apply_step_edit is the reference reducer (StepCell only). A "
                  "custom cell type must supply its own reducer with the same "
                  "signature: (Snapshot&, const Command&, EngineSequence&) -> "
                  "optional<AppliedEdit>.");

    using AppliedEdit = AppliedEditT<Config>;
    using StepRangeApplied = StepRangeAppliedT<Config>;
    constexpr std::uint8_t P = static_cast<std::uint8_t>(Config::patterns);
    constexpr std::uint8_t L = static_cast<std::uint8_t>(Config::lanes);
    constexpr std::uint8_t S = static_cast<std::uint8_t>(Config::steps);

    AppliedEdit echo{};
    echo.engine_sequence = ++engine_seq;
    echo.snapshot_epoch = snapshot.epoch;
    echo.client_sequence = cmd.client_sequence;
    echo.transaction_id = cmd.transaction_id;

    switch (cmd.kind) {
    case StepEditKind::SetCell: {
        const auto& e = cmd.payload.set_cell;
        if (e.pattern >= P || e.lane >= L || e.step >= S) {
            echo.kind = AppliedEditKind::CommandRejected;
            echo.payload.reject_reason = 1;
            return echo;
        }
        snapshot.patterns[e.pattern].lanes[e.lane][e.step] = e.cell;
        echo.kind = AppliedEditKind::StepRangeChanged;
        echo.dirty = {DirtyKind::Cell, e.pattern, e.lane, e.step, 1};
        StepRangeApplied sr{};
        sr.pattern = e.pattern; sr.lane = e.lane; sr.first_step = e.step;
        sr.step_count = 1; sr.cells[0] = e.cell;
        echo.payload.step_range = sr;
        return echo;
    }
    case StepEditKind::Clear: {
        const auto& e = cmd.payload.clear;
        if (e.pattern >= P || e.lane >= L || e.step >= S) {
            echo.kind = AppliedEditKind::CommandRejected;
            echo.payload.reject_reason = 1;
            return echo;
        }
        snapshot.patterns[e.pattern].lanes[e.lane][e.step] = StepCell{};
        echo.kind = AppliedEditKind::StepRangeChanged;
        echo.dirty = {DirtyKind::Cell, e.pattern, e.lane, e.step, 1};
        StepRangeApplied sr{};
        sr.pattern = e.pattern; sr.lane = e.lane; sr.first_step = e.step;
        sr.step_count = 1; sr.cells[0] = StepCell{};
        echo.payload.step_range = sr;
        return echo;
    }
    case StepEditKind::RandomizeLane: {
        const auto& e = cmd.payload.randomize_lane;
        if (e.pattern >= P || e.lane >= L) {
            echo.kind = AppliedEditKind::CommandRejected;
            echo.payload.reject_reason = 1;
            return echo;
        }
        // Deterministic PRNG so the echo carries the exact cells the UI replays
        // — it never re-runs this algorithm.
        std::uint32_t s = e.seed ? e.seed : 1u;
        auto next = [&s]() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; };
        StepRangeApplied sr{};
        sr.pattern = e.pattern; sr.lane = e.lane; sr.first_step = 0;
        sr.step_count = S;
        const std::uint8_t span =
            static_cast<std::uint8_t>(1u + e.max_velocity - e.min_velocity);
        for (std::uint8_t st = 0; st < S; ++st) {
            StepCell c{};
            const bool on = (next() % 128u) < e.density;
            c.flags = on ? StepCell::kEnabledBit : 0;
            c.velocity = static_cast<std::uint8_t>(
                e.min_velocity + (span ? next() % span : 0u));
            snapshot.patterns[e.pattern].lanes[e.lane][st] = c;
            sr.cells[st] = c;
        }
        echo.kind = AppliedEditKind::StepRangeChanged;
        echo.dirty = {DirtyKind::Lane, e.pattern, e.lane, 0, S};
        echo.payload.step_range = sr;
        return echo;
    }
    case StepEditKind::SetPatternLength: {
        const auto& e = cmd.payload.set_pattern_length;
        if (e.pattern >= P) {
            echo.kind = AppliedEditKind::CommandRejected;
            echo.payload.reject_reason = 1;
            return echo;
        }
        // Active step count. Normalize 0 (or an over-capacity value) to full here
        // so the view (muted = step >= length) and playback see one consistent
        // value — never a snapshot where 0 means "full" to one and "all muted" to
        // the other.
        const std::uint8_t len = (e.length == 0 || e.length > S) ? S : e.length;
        snapshot.patterns[e.pattern].length = len;
        echo.kind = AppliedEditKind::PatternLengthChanged;
        echo.dirty = {DirtyKind::Pattern, e.pattern, 0, 0, 0};
        echo.payload.pattern_length = {e.pattern, len};
        return echo;
    }
    case StepEditKind::SwitchPattern: {
        const auto& e = cmd.payload.switch_pattern;
        if (e.pattern >= P) {
            echo.kind = AppliedEditKind::CommandRejected;
            echo.payload.reject_reason = 1;
            return echo;
        }
        snapshot.active_pattern = e.pattern;
        echo.kind = AppliedEditKind::ActivePatternChanged;
        echo.dirty = {DirtyKind::FullSnapshot, 0, 0, 0, 0};
        echo.payload.active_pattern = e;
        return echo;
    }
    }
    return std::nullopt;
}

/// Drain the command FIFO and apply each command with @p reduce, publishing the
/// echoes. OWNS the overflow-recovery invariant: if the applied FIFO is full,
/// the UI would miss incremental echoes, so republish the authoritative snapshot
/// and raise the resync bar — snapshot BEFORE the resync mark (the order the
/// channel requires). @p reduce has signature
///   std::optional<AppliedEditT<Config>>(SnapshotT<Config>&, const Command&, EngineSequence&)
/// and must bump @p engine_seq for every applied edit (apply_step_edit does).
template <class Config, class Reducer>
void drain_and_apply(SequencerStateChannelT<Config>& channel,
                     SnapshotT<Config>& snapshot,
                     Epoch& epoch, EngineSequence& engine_seq,
                     Reducer&& reduce) {
    while (auto cmd = channel.audio_try_pop_command()) {
        auto echo = reduce(snapshot, *cmd, engine_seq);
        if (!echo) continue;
        if (!channel.audio_try_publish_applied(*echo)) {
            // Applied FIFO overflow: fold the latest engine_sequence into a fresh
            // snapshot, bump the epoch, publish it, THEN raise the resync bar.
            snapshot.engine_sequence = engine_seq;
            snapshot.epoch = ++epoch;
            channel.audio_publish_snapshot(snapshot);
            channel.audio_mark_resync_required(snapshot.epoch);
        }
    }
}

/// Convenience overload using the reference StepCell reducer.
template <class Config>
void drain_and_apply(SequencerStateChannelT<Config>& channel,
                     SnapshotT<Config>& snapshot,
                     Epoch& epoch, EngineSequence& engine_seq) {
    drain_and_apply(channel, snapshot, epoch, engine_seq,
                    [](SnapshotT<Config>& s, const StepEditCommandT<Config>& c,
                       EngineSequence& es) { return apply_step_edit<Config>(s, c, es); });
}

} // namespace pulp::state
