#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <optional>
#include <vector>

#include <pulp/state/sequencer_state_channel.hpp>
#include <pulp/view/view.hpp>

/// StepGridView renders and edits one pattern of a step-sequencer grid backed by
/// a SequencerStateChannel (the audio-safe non-param state transport). It is the
/// blessed base for sequencer-style grids whose per-step data fits the channel's
/// cell model. (Free-form procedural controls — arbitrary multi-value gestures on
/// a painted canvas — are a different surface: DesignFrameView::Kind::custom.)
///
/// Structure (so the view generalizes over SequencerConfig without templating the
/// expensive paint/geometry/interaction code):
///   - StepGridViewBase — NON-template. Owns paint, geometry, hit-testing, the
///     pump() orchestration, the flat visual cache, dirty accounting, playhead
///     state, drag state, and the FrameClock probe. Config-independent.
///   - StepGridViewT<Config, CellPolicy> — thin templated shim. Owns the typed
///     channel pointer + typed render copy, and implements the handful of hooks
///     that touch Config-typed state (apply an echo, resync, project a visual/
///     playhead, submit an edit). CellPolicy maps the cell type to a visual AND
///     to the edit a click applies.
///
/// Contract with the channel: the engine owns authoritative state; this view
/// NEVER mutates its render copy optimistically — a mouse edit only SUBMITS a
/// command, and the render copy advances only when the engine's applied echo
/// arrives (so the UI can never drift from engine-normalized state, randomize
/// results, or rejected edits). Stale echoes are filtered (by engine_sequence)
/// BEFORE they touch the typed render copy. Exactly one StepGridView* may bind a
/// given channel (the applied-echo FIFO is single-consumer); debug builds assert
/// on a second binding via the channel's consumer-claim token.
namespace pulp::view {

// ── Non-template base ────────────────────────────────────────────────────────
class StepGridViewBase : public View {
public:
    /// Precomputed per-cell visual, recomputed only for dirtied regions.
    struct CellVisual {
        bool enabled = false;
        float intensity = 0.0f;  // velocity / 127, drives fill alpha/height
        bool muted = false;      // step index >= pattern length
    };

    /// Result of one pump(), surfaced for headless tests (a hostless view cannot
    /// observe request_repaint()).
    struct PumpResult {
        bool requested_repaint = false;
        bool full_rebuild = false;   // a resync or active-pattern change rebuilt all visible cells
        int rebuilt_cells = 0;       // visible cells whose CellVisual was recomputed
        int echoes_applied = 0;      // non-stale echoes replayed into the render copy
        bool resynced = false;       // a snapshot resync occurred this pump
    };

    struct Cell { std::uint8_t lane; std::uint8_t step; };

    ~StepGridViewBase() override;

    /// Which pattern is shown/edited. Independent of the engine's playing pattern:
    /// the playhead overlay only draws when the engine's active pattern matches.
    void set_displayed_pattern(std::uint8_t pattern);
    std::uint8_t displayed_pattern() const { return displayed_pattern_; }

    /// Drain the channel and update the render copy + visual cache. Called from the
    /// FrameClock activity probe every tick; public + directly callable for tests.
    PumpResult pump();

    const CellVisual& cell_visual(std::uint8_t lane, std::uint8_t step) const {
        return visible_cache_[index(lane, step)];
    }
    state::EngineSequence last_engine_sequence() const { return last_engine_sequence_; }

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_down(Point pos) override;
    void on_mouse_drag(Point pos) override;
    void on_mouse_up(Point pos) override;
    void on_frame_clock_changed() override;
    bool wants_mouse_input() const override { return true; }

    // Geometry (public for tests / layout).
    std::optional<Cell> cell_at(Point pos) const;
    Rect cell_rect(std::uint8_t lane, std::uint8_t step) const;
    Rect playhead_column_rect(std::uint8_t step) const;

protected:
    StepGridViewBase(std::uint8_t lanes, std::uint8_t steps, std::uint8_t patterns);

    // ── Hooks the templated shim implements (the only Config-typed surface) ──
    enum class EchoStatus { Applied, SkippedStale, Drained };
    struct EchoResult {
        EchoStatus status = EchoStatus::Drained;
        state::EngineSequence engine_sequence = 0;
        bool full_rebuild = false;   // e.g. pattern-length change rebuilt all visible cells
        int rebuilt_cells = 0;
    };
    struct ResyncPoint {
        state::Epoch epoch = 0;
        state::EngineSequence engine_sequence = 0;
        int rebuilt_cells = 0;
    };
    struct PlayheadProjection {
        std::uint8_t active_pattern = 0;
        std::uint8_t active_step = 0;
        bool playing = false;
    };

    virtual bool channel_bound() const = 0;
    virtual state::Epoch channel_resync_required_epoch() const = 0;
    /// Replace the render copy from the channel's latest snapshot and rebuild the
    /// whole visible cache; return the reached epoch + engine_sequence.
    virtual ResyncPoint resync_from_snapshot() = 0;
    /// Pop the next applied echo. Skip it if engine_sequence <= @p min_exclusive
    /// (returns SkippedStale WITHOUT mutating the render copy — stale filtering
    /// happens before any typed mutation). Otherwise apply it to the typed render
    /// copy, rebuild the affected visible cells (via set_visible/record_dirty),
    /// and return Applied. Drained when the FIFO is empty.
    virtual EchoResult apply_next_echo(state::EngineSequence min_exclusive) = 0;
    virtual PlayheadProjection read_playhead() const = 0;
    /// Rebuild the whole visible cache for the displayed pattern; return the count.
    virtual int rebuild_visible() = 0;
    virtual void submit_set_cell(std::uint8_t lane, std::uint8_t step, bool enabled,
                                 state::GesturePhase) = 0;

    // ── Shared state the shim reads/writes through these accessors ──
    std::size_t index(std::uint8_t lane, std::uint8_t step) const {
        return static_cast<std::size_t>(lane) * steps_ + step;
    }
    void set_visible(std::uint8_t lane, std::uint8_t step, const CellVisual& v) {
        visible_cache_[index(lane, step)] = v;
    }
    void record_dirty(std::uint8_t lane, std::uint8_t step) {
        pump_dirty_cells_.push_back(Cell{lane, step});
    }
    std::uint8_t lanes() const { return lanes_; }
    std::uint8_t steps() const { return steps_; }
    std::uint8_t patterns() const { return patterns_; }
    state::EditTransactionId current_txn() const { return current_txn_; }
    state::ClientSequence next_client_sequence() { return next_client_seq_++; }
    void request_initial_resync() { needs_initial_resync_ = true; }
    /// The shim MUST call this in its destructor (before its typed members die) so
    /// the FrameClock probe can't invoke a pure-virtual on a half-destroyed object.
    void detach_probe();

    std::uint8_t displayed_pattern_ = 0;

private:
    std::uint8_t lanes_, steps_, patterns_;

    void subscribe_probe();
    void unsubscribe_probe();

    // UI-owned visual cache + replay bookkeeping (advanced only by engine echoes).
    std::vector<CellVisual> visible_cache_;          // lanes_ * steps_
    state::EngineSequence last_engine_sequence_ = 0;
    state::Epoch local_epoch_ = 0;
    bool needs_initial_resync_ = true;

    // Per-pump dirty accounting for bounded invalidation.
    std::vector<Cell> pump_dirty_cells_;
    bool pump_full_ = false;

    // Playhead overlay state (kept separate from cell visuals).
    std::uint8_t playhead_step_ = 0;
    std::uint8_t playhead_pattern_ = 0;
    bool playhead_visible_ = false;

    // Drag edit state: the value the drag paints, so a swipe sets a run coherently.
    bool dragging_ = false;
    bool drag_sets_enabled_ = true;
    std::optional<Cell> last_drag_cell_{};

    // Command sequencing.
    state::ClientSequence next_client_seq_ = 1;
    state::EditTransactionId next_txn_ = 1;
    state::EditTransactionId current_txn_ = 0;

    // FrameClock activity subscription (must be explicitly unsubscribed).
    int probe_id_ = -1;
    FrameClock* subscribed_clock_ = nullptr;
};

// ── Cell policy: cell type -> visual AND the edit a click applies ────────────
/// Primary template is deliberately undefined-usable: a custom cell type must
/// supply its own CellPolicy (visual + make_set_cell), else it is a compile error
/// rather than a silent blank / uneditable grid.
template <class Cell>
struct DefaultCellPolicy {
    static_assert(sizeof(Cell) == 0,
                  "Provide a CellPolicy<YourCell> with static visual(const Cell&, "
                  "bool muted) -> StepGridViewBase::CellVisual and static "
                  "make_set_cell(const Cell&, bool enabled) -> Cell.");
};

template <>
struct DefaultCellPolicy<state::StepCell> {
    static StepGridViewBase::CellVisual visual(const state::StepCell& c, bool muted) {
        StepGridViewBase::CellVisual v;
        v.enabled = c.enabled();
        v.intensity = std::clamp(c.velocity / 127.0f, 0.0f, 1.0f);
        v.muted = muted;
        return v;
    }
    static state::StepCell make_set_cell(const state::StepCell& old, bool enabled) {
        state::StepCell c = old;
        if (enabled) {
            c.flags = static_cast<std::uint8_t>(c.flags | state::StepCell::kEnabledBit);
            if (c.velocity == 0) c.velocity = 100;
        } else {
            c.flags = static_cast<std::uint8_t>(c.flags & ~state::StepCell::kEnabledBit);
        }
        return c;
    }
};

// ── Templated shim ───────────────────────────────────────────────────────────
template <class Config,
          class CellPolicy = DefaultCellPolicy<typename Config::cell_type>>
class StepGridViewT final : public StepGridViewBase {
public:
    using Channel = state::SequencerStateChannelT<Config>;
    using Snapshot = typename Channel::Snapshot;

    StepGridViewT()
        : StepGridViewBase(static_cast<std::uint8_t>(Config::lanes),
                           static_cast<std::uint8_t>(Config::steps),
                           static_cast<std::uint8_t>(Config::patterns)) {}

    ~StepGridViewT() override {
        // Stop the probe BEFORE typed members die (it would else pure-virtual-call
        // pump() on a half-destroyed object), then release the consumer claim.
        detach_probe();
        if (channel_) channel_->debug_release_ui_consumer();
    }

    /// Bind the channel this view reads/writes. Must outlive the view. Triggers a
    /// resync from the latest snapshot on the next pump.
    void set_channel(Channel* channel) {
        if (channel_ == channel) return;
        if (channel_) channel_->debug_release_ui_consumer();
        channel_ = channel;
        if (channel_) {
            const bool claimed = channel_->debug_claim_ui_consumer();
            (void)claimed;
            // Exactly one UI-side consumer per channel (single-consumer echo FIFO).
            assert(claimed && "another StepGridView already consumes this channel");
        }
        request_initial_resync();
    }
    Channel* channel() const { return channel_; }

    const Snapshot& render_snapshot() const { return render_; }

protected:
    bool channel_bound() const override { return channel_ != nullptr; }

    state::Epoch channel_resync_required_epoch() const override {
        return channel_->ui_resync_required_epoch();
    }

    ResyncPoint resync_from_snapshot() override {
        render_ = channel_->ui_read_latest_snapshot();
        const int n = rebuild_visible();
        return {render_.epoch, render_.engine_sequence, n};
    }

    EchoResult apply_next_echo(state::EngineSequence min_exclusive) override {
        auto e = channel_->ui_try_pop_applied();
        if (!e) return {EchoStatus::Drained, 0, false, 0};
        if (e->engine_sequence <= min_exclusive)
            return {EchoStatus::SkippedStale, e->engine_sequence, false, 0};

        EchoResult res;
        res.status = EchoStatus::Applied;
        res.engine_sequence = e->engine_sequence;

        switch (e->kind) {
        case state::AppliedEditKind::StepRangeChanged: {
            const auto& sr = e->payload.step_range;
            if (sr.pattern >= patterns() || sr.lane >= lanes()) break;
            auto& lane = render_.patterns[sr.pattern].lanes[sr.lane];
            for (std::uint8_t i = 0; i < sr.step_count; ++i) {
                std::uint8_t step = static_cast<std::uint8_t>(sr.first_step + i);
                if (step >= steps()) break;
                lane[step] = sr.cells[i];
            }
            if (sr.pattern != displayed_pattern_) break;
            for (std::uint8_t i = 0; i < sr.step_count; ++i) {
                std::uint8_t step = static_cast<std::uint8_t>(sr.first_step + i);
                if (step >= steps()) break;
                set_visible(sr.lane, step, visual_for(lane[step], step));
                record_dirty(sr.lane, step);
                ++res.rebuilt_cells;
            }
            break;
        }
        case state::AppliedEditKind::PatternLengthChanged: {
            const auto& pl = e->payload.pattern_length;
            if (pl.pattern >= patterns()) break;
            render_.patterns[pl.pattern].length = pl.length;
            if (pl.pattern != displayed_pattern_) break;
            res.full_rebuild = true;  // every cell's muted flag may flip
            res.rebuilt_cells += rebuild_visible();
            break;
        }
        case state::AppliedEditKind::ActivePatternChanged:
            render_.active_pattern = e->payload.active_pattern.pattern;
            break;
        case state::AppliedEditKind::CommandRejected:
            break;
        }
        return res;
    }

    PlayheadProjection read_playhead() const override {
        // The reference/default playhead exposes active_pattern/active_step/playing.
        // A custom Config::playhead_type must expose the same three fields.
        const auto ph = channel_->ui_read_playhead();
        return {ph.active_pattern, ph.active_step, ph.playing != 0};
    }

    int rebuild_visible() override {
        const auto& pat = render_.patterns[displayed_pattern_];
        int n = 0;
        for (std::uint8_t lane = 0; lane < lanes(); ++lane)
            for (std::uint8_t step = 0; step < steps(); ++step) {
                set_visible(lane, step, visual_for(pat.lanes[lane][step], step));
                ++n;
            }
        return n;
    }

    void submit_set_cell(std::uint8_t lane, std::uint8_t step, bool enabled,
                         state::GesturePhase phase) override {
        if (!channel_ || lane >= lanes() || step >= steps()) return;
        typename Channel::Command cmd;
        cmd.client_sequence = next_client_sequence();
        cmd.transaction_id = current_txn();
        cmd.gesture_phase = phase;
        cmd.kind = state::StepEditKind::SetCell;
        typename Channel::SetCellEdit e;
        e.pattern = displayed_pattern_;
        e.lane = lane;
        e.step = step;
        e.cell = CellPolicy::make_set_cell(
            render_.patterns[displayed_pattern_].lanes[lane][step], enabled);
        cmd.payload.set_cell = e;
        channel_->ui_try_submit(cmd);  // failure is fine: no authoritative change occurs
    }

private:
    CellVisual visual_for(const typename Config::cell_type& c, std::uint8_t step) const {
        const bool muted = step >= render_.patterns[displayed_pattern_].length;
        return CellPolicy::visual(c, muted);
    }

    Channel* channel_ = nullptr;
    Snapshot render_{};
};

// The blessed reference grid: 12x32x32 StepCell.
using StepGridView = StepGridViewT<state::ReferenceSequencerConfig>;

} // namespace pulp::view
