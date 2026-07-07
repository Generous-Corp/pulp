#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include <pulp/state/sequencer_state_channel.hpp>
#include <pulp/view/view.hpp>

/// EXPERIMENTAL — API not yet frozen. Prototype consumer for
/// SequencerStateChannel (the audio-safe non-param state transport). It renders
/// and edits one pattern of a step sequencer grid (up to kLaneCount x kStepCount
/// cells) and is the UI half that fixes the channel's API shape before we freeze
/// a public SDK primitive.
///
/// Contract with the channel (see sequencer_state_channel.hpp): the audio thread
/// owns authoritative state; this view NEVER mutates its render copy optimistically
/// — a mouse edit only SUBMITS a StepEditCommand, and the render copy advances only
/// when the audio thread's AppliedEdit echo arrives (so the UI can never drift from
/// engine-normalized state, randomize results, or rejected edits). Incremental
/// change arrives on the ordered echo stream (each echo names an exact DirtyRegion);
/// full Snapshots are consumed only for bulk/resync. The view keeps a per-cell
/// CellVisual cache rebuilt only for dirtied regions — it never derives all 12k
/// records per frame — and gates repaints on actual visible change.
namespace pulp::view {

class StepGridView : public View {
public:
    /// Precomputed per-cell visual, derived from a StepCell only when its region
    /// is dirtied. paint() reads these; it never recomputes sequencer semantics.
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

    StepGridView();
    ~StepGridView() override;

    /// Bind the channel this view reads/writes. Must outlive the view. Triggers a
    /// resync from the latest snapshot on the next pump.
    void set_channel(state::SequencerStateChannel* channel);
    state::SequencerStateChannel* channel() const { return channel_; }

    /// Which pattern is shown/edited. Independent of the engine's playing pattern:
    /// the playhead overlay only draws when the engine's active pattern matches.
    void set_displayed_pattern(std::uint8_t pattern);
    std::uint8_t displayed_pattern() const { return displayed_pattern_; }

    /// Drain the channel and update the render copy + visual cache. Called from the
    /// FrameClock activity probe every tick; public + directly callable for tests.
    /// Returns what changed so a test (or the probe) can decide to repaint.
    PumpResult pump();

    // Read access for tests / overlays.
    const CellVisual& cell_visual(std::uint8_t lane, std::uint8_t step) const {
        return visible_cache_[lane][step];
    }
    const state::Snapshot& render_snapshot() const { return render_; }
    state::EngineSequence last_engine_sequence() const { return last_engine_sequence_; }

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_down(Point pos) override;
    void on_mouse_drag(Point pos) override;
    void on_mouse_up(Point pos) override;
    void on_frame_clock_changed() override;
    bool wants_mouse_input() const override { return true; }

    // Geometry (public for tests / layout).
    struct Cell { std::uint8_t lane; std::uint8_t step; };
    std::optional<Cell> cell_at(Point pos) const;
    Rect cell_rect(std::uint8_t lane, std::uint8_t step) const;
    /// Root-of-column strip a playhead overlay occupies at `step` (all lanes),
    /// matching paint()'s overlay geometry. Public for tests.
    Rect playhead_column_rect(std::uint8_t step) const;

private:
    int rebuild_all_visible();                  // full CellVisual rebuild for the shown pattern; returns count
    int rebuild_region(const state::AppliedEdit& echo);  // replay one echo; returns visible cells rebuilt
    void resync_from_snapshot();                // replace render copy from the latest snapshot
    CellVisual visual_for(const state::StepCell& c, std::uint8_t step) const;
    void subscribe_probe();
    void unsubscribe_probe();

    state::SequencerStateChannel* channel_ = nullptr;

    // UI-owned render copy + replay bookkeeping (advanced only by audio echoes).
    state::Snapshot render_{};
    state::EngineSequence last_engine_sequence_ = 0;
    state::Epoch local_epoch_ = 0;
    bool needs_initial_resync_ = true;

    std::uint8_t displayed_pattern_ = 0;

    // Precomputed visuals for the shown pattern (active-pattern relative).
    std::array<std::array<CellVisual, state::kStepCount>, state::kLaneCount> visible_cache_{};

    // Per-pump dirty accounting for bounded invalidation. Reset at the top of
    // pump(): rebuild_region() appends each recomputed visible cell to
    // pump_dirty_cells_, and sets pump_full_ when it rebuilds every cell
    // (pattern-length change). pump() then invalidates only those cell rects
    // (plus the leaving/entering playhead columns) instead of the whole grid,
    // escalating to a full repaint when pump_full_ or a resync occurred.
    std::vector<Cell> pump_dirty_cells_;
    bool pump_full_ = false;

    // Playhead overlay state (kept separate from cell visuals).
    std::uint8_t playhead_step_ = 0;
    std::uint8_t playhead_pattern_ = 0;
    bool playhead_visible_ = false;

    // FrameClock activity subscription (must be explicitly unsubscribed).
    int probe_id_ = -1;
    FrameClock* subscribed_clock_ = nullptr;

    // Drag edit state: the value the drag paints, so a swipe sets a run coherently.
    bool dragging_ = false;
    bool drag_sets_enabled_ = true;
    std::optional<Cell> last_drag_cell_{};

    // Command sequencing: client_sequence is monotonic per command; a gesture
    // (mouse-down..up) shares one transaction_id for host-undo grouping.
    state::ClientSequence next_client_seq_ = 1;
    state::EditTransactionId next_txn_ = 1;
    state::EditTransactionId current_txn_ = 0;

    void submit_set_cell(std::uint8_t lane, std::uint8_t step, bool enabled,
                         state::GesturePhase phase);
};

} // namespace pulp::view
