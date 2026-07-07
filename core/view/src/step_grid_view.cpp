#include <pulp/view/step_grid_view.hpp>

#include <pulp/canvas/canvas.hpp>
#include <pulp/view/frame_clock.hpp>

#include <algorithm>

namespace pulp::view {

using state::AppliedEdit;
using state::AppliedEditKind;
using state::DirtyRegion;
using state::PlayheadState;
using state::Snapshot;
using state::StepCell;
using state::kLaneCount;
using state::kStepCount;

StepGridView::StepGridView() { set_focusable(true); }

StepGridView::~StepGridView() { unsubscribe_probe(); }

void StepGridView::set_channel(state::SequencerStateChannel* channel) {
    channel_ = channel;
    needs_initial_resync_ = true;
}

void StepGridView::set_displayed_pattern(std::uint8_t pattern) {
    if (pattern >= state::kPatternCount || pattern == displayed_pattern_) return;
    displayed_pattern_ = pattern;
    rebuild_all_visible();
    request_repaint();
}

StepGridView::CellVisual StepGridView::visual_for(const StepCell& c, std::uint8_t step) const {
    CellVisual v;
    v.enabled = c.enabled();
    v.intensity = std::clamp(c.velocity / 127.0f, 0.0f, 1.0f);
    v.muted = step >= render_.patterns[displayed_pattern_].length;
    return v;
}

int StepGridView::rebuild_all_visible() {
    const auto& pat = render_.patterns[displayed_pattern_];
    int n = 0;
    for (std::uint8_t lane = 0; lane < kLaneCount; ++lane)
        for (std::uint8_t step = 0; step < kStepCount; ++step) {
            visible_cache_[lane][step] = visual_for(pat.lanes[lane][step], step);
            ++n;
        }
    return n;
}

void StepGridView::resync_from_snapshot() {
    const Snapshot& s = channel_->ui_read_latest_snapshot();
    render_ = s;
    last_engine_sequence_ = s.engine_sequence;
    local_epoch_ = s.epoch;
    needs_initial_resync_ = false;
}

// Replay one applied echo into the render copy and rebuild the affected visible
// cells. Returns the number of *visible* CellVisuals recomputed (0 if the echo
// only touched a pattern other than the one on screen). Bounds are validated —
// the channel is transport, not validation.
int StepGridView::rebuild_region(const AppliedEdit& e) {
    switch (e.kind) {
    case AppliedEditKind::StepRangeChanged: {
        const auto& sr = e.payload.step_range;
        if (sr.pattern >= state::kPatternCount || sr.lane >= kLaneCount) return 0;
        auto& lane = render_.patterns[sr.pattern].lanes[sr.lane];
        for (std::uint8_t i = 0; i < sr.step_count; ++i) {
            std::uint8_t step = static_cast<std::uint8_t>(sr.first_step + i);
            if (step >= kStepCount) break;
            lane[step] = sr.cells[i];
        }
        if (sr.pattern != displayed_pattern_) return 0;
        int n = 0;
        for (std::uint8_t i = 0; i < sr.step_count; ++i) {
            std::uint8_t step = static_cast<std::uint8_t>(sr.first_step + i);
            if (step >= kStepCount) break;
            visible_cache_[sr.lane][step] = visual_for(lane[step], step);
            pump_dirty_cells_.push_back(Cell{sr.lane, step});  // bounded-invalidate just this cell
            ++n;
        }
        return n;
    }
    case AppliedEditKind::PatternLengthChanged: {
        const auto& pl = e.payload.pattern_length;
        if (pl.pattern >= state::kPatternCount) return 0;
        render_.patterns[pl.pattern].length = pl.length;
        // Length changes the muted flag across every step of the shown pattern.
        if (pl.pattern != displayed_pattern_) return 0;
        pump_full_ = true;  // every cell's muted flag may flip → repaint the grid
        return rebuild_all_visible();
    }
    case AppliedEditKind::ActivePatternChanged:
        // Playback selection only; the edited/displayed pattern is independent, so
        // no cell rebuild. Playhead-overlay visibility is handled after the drain.
        render_.active_pattern = e.payload.active_pattern.pattern;
        return 0;
    case AppliedEditKind::CommandRejected:
        return 0;
    }
    return 0;
}

StepGridView::PumpResult StepGridView::pump() {
    PumpResult r;
    if (!channel_) return r;

    // Reset per-pump dirty accounting; rebuild_region()/rebuild_all_visible()
    // repopulate it as they replay this tick's echoes.
    pump_dirty_cells_.clear();
    pump_full_ = false;

    // 1. Initial or resync-bar-driven resync (full rebuild of the shown pattern).
    if (needs_initial_resync_ || channel_->ui_resync_required_epoch() > local_epoch_) {
        resync_from_snapshot();
        r.resynced = true;
        r.full_rebuild = true;
        r.rebuilt_cells += rebuild_all_visible();
    }

    // 2-3. Drain the ordered applied-echo stream; skip stale, replay the rest.
    while (auto e = channel_->ui_try_pop_applied()) {
        if (e->engine_sequence <= last_engine_sequence_) continue;  // stale
        last_engine_sequence_ = e->engine_sequence;
        ++r.echoes_applied;
        r.rebuilt_cells += rebuild_region(*e);
    }

    // 4. The resync bar may have risen mid-drain (echo FIFO overflowed). Recover
    // now and drop any stale queued echoes so they can't overwrite the snapshot.
    if (channel_->ui_resync_required_epoch() > local_epoch_) {
        resync_from_snapshot();
        r.resynced = true;
        r.full_rebuild = true;
        r.rebuilt_cells += rebuild_all_visible();
        while (auto e = channel_->ui_try_pop_applied()) {
            if (e->engine_sequence <= last_engine_sequence_) continue;
            last_engine_sequence_ = e->engine_sequence;
            ++r.echoes_applied;
            r.rebuilt_cells += rebuild_region(*e);
        }
    }

    // 5. Read the playhead LAST — after any ActivePatternChanged echo — so the
    // overlay reflects the current active pattern, not a stale one. Capture the
    // outgoing overlay position first so a bounded repaint can invalidate the
    // column the playhead is LEAVING as well as the one it is entering.
    const bool was_visible = playhead_visible_;
    const std::uint8_t prev_step = playhead_step_;
    const PlayheadState ph = channel_->ui_read_playhead();
    const bool now_visible = ph.playing != 0 && ph.active_pattern == displayed_pattern_;
    const bool playhead_changed =
        now_visible != playhead_visible_ ||
        (now_visible && ph.active_step != playhead_step_);
    playhead_visible_ = now_visible;
    playhead_step_ = ph.active_step;
    playhead_pattern_ = ph.active_pattern;

    // 6. Repaint iff something *visible* changed (an echo on an off-screen pattern
    // updates the render copy but rebuilds no visible cell -> no repaint). Prefer
    // bounded invalidation: only the recomputed cells and the two playhead
    // columns, so the static grid chrome does not re-composite every tick. A full
    // rebuild (resync / pattern-length change) escalates to a whole-view repaint.
    const bool full = r.full_rebuild || pump_full_;
    r.requested_repaint = full || !pump_dirty_cells_.empty() || playhead_changed;
    if (r.requested_repaint) {
        if (full) {
            request_repaint();
        } else {
            for (const Cell& c : pump_dirty_cells_)
                request_repaint(cell_rect(c.lane, c.step));
            if (playhead_changed) {
                if (was_visible && prev_step < kStepCount)
                    request_repaint(playhead_column_rect(prev_step));
                if (playhead_visible_ && playhead_step_ < kStepCount)
                    request_repaint(playhead_column_rect(playhead_step_));
            }
        }
    }
    return r;
}

// ── Geometry ────────────────────────────────────────────────────────────────
Rect StepGridView::cell_rect(std::uint8_t lane, std::uint8_t step) const {
    const Rect b = local_bounds();
    const float cw = b.width / static_cast<float>(kStepCount);
    const float ch = b.height / static_cast<float>(kLaneCount);
    constexpr float gap = 1.0f;
    return Rect{static_cast<float>(step) * cw + gap,
                static_cast<float>(lane) * ch + gap,
                std::max(0.0f, cw - 2 * gap),
                std::max(0.0f, ch - 2 * gap)};
}

Rect StepGridView::playhead_column_rect(std::uint8_t step) const {
    // Matches paint()'s overlay: a vertical strip spanning lane 0's top to the
    // last lane's bottom at this step's column.
    const Rect top = cell_rect(0, step);
    const Rect bot = cell_rect(static_cast<std::uint8_t>(kLaneCount - 1), step);
    return Rect{top.x, top.y, top.width, (bot.y + bot.height) - top.y};
}

std::optional<StepGridView::Cell> StepGridView::cell_at(Point pos) const {
    const Rect b = local_bounds();
    if (b.width <= 0 || b.height <= 0) return std::nullopt;
    if (pos.x < 0 || pos.y < 0 || pos.x >= b.width || pos.y >= b.height) return std::nullopt;
    const int step = static_cast<int>(pos.x / (b.width / static_cast<float>(kStepCount)));
    const int lane = static_cast<int>(pos.y / (b.height / static_cast<float>(kLaneCount)));
    if (lane < 0 || lane >= kLaneCount || step < 0 || step >= kStepCount) return std::nullopt;
    return Cell{static_cast<std::uint8_t>(lane), static_cast<std::uint8_t>(step)};
}

// ── Painting ────────────────────────────────────────────────────────────────
void StepGridView::paint(canvas::Canvas& canvas) {
    using canvas::Color;
    const Rect b = local_bounds();
    canvas.set_fill_color(resolve_color("bg.surface", Color::rgba8(24, 24, 32)));
    canvas.fill_rect(b.x, b.y, b.width, b.height);

    for (std::uint8_t lane = 0; lane < kLaneCount; ++lane) {
        for (std::uint8_t step = 0; step < kStepCount; ++step) {
            const CellVisual& v = visible_cache_[lane][step];
            const Rect c = cell_rect(lane, step);
            Color fill;
            if (v.muted) {
                fill = resolve_color("bg.primary", Color::rgba8(18, 18, 24));
            } else if (v.enabled) {
                // Accent brightened by velocity (token supplies hue; velocity drives alpha).
                fill = resolve_color("accent.primary", Color::rgba8(99, 181, 255));
                fill.a = 0.35f + 0.65f * v.intensity;
            } else {
                fill = resolve_color("bg.elevated", Color::rgba8(44, 44, 56));
            }
            canvas.set_fill_color(fill);
            canvas.fill_rounded_rect(c.x, c.y, c.width, c.height, 2.0f);
        }
    }

    // Playhead overlay column (drawn on top; never baked into cell visuals).
    // Re-check the pattern here: set_displayed_pattern() can change the shown
    // pattern after the last pump, and a synchronous repaint would otherwise draw
    // the previous pattern's playhead over the new one.
    if (playhead_visible_ && playhead_pattern_ == displayed_pattern_ &&
        playhead_step_ < kStepCount) {
        const Rect col = playhead_column_rect(playhead_step_);
        Color ph = resolve_color("overlay.bg", Color::rgba8(255, 255, 255));
        ph.a = 0.14f;
        canvas.set_fill_color(ph);
        canvas.fill_rect(col.x, col.y, col.width, col.height);
    }
}

// ── Interaction (submit-only; render copy advances from echoes) ──────────────
void StepGridView::submit_set_cell(std::uint8_t lane, std::uint8_t step, bool enabled,
                                   state::GesturePhase phase) {
    if (!channel_ || lane >= kLaneCount || step >= kStepCount) return;
    state::StepEditCommand cmd;
    cmd.client_sequence = next_client_seq_++;
    cmd.transaction_id = current_txn_;
    cmd.gesture_phase = phase;
    cmd.kind = state::StepEditKind::SetCell;
    state::SetCellEdit e;
    e.pattern = displayed_pattern_;
    e.lane = lane;
    e.step = step;
    // Carry the existing cell forward, flipping only the enabled bit; a freshly
    // enabled cell gets a sensible default velocity if it had none.
    e.cell = render_.patterns[displayed_pattern_].lanes[lane][step];
    if (enabled) {
        e.cell.flags = static_cast<std::uint8_t>(e.cell.flags | StepCell::kEnabledBit);
        if (e.cell.velocity == 0) e.cell.velocity = 100;
    } else {
        e.cell.flags = static_cast<std::uint8_t>(e.cell.flags & ~StepCell::kEnabledBit);
    }
    cmd.payload.set_cell = e;
    channel_->ui_try_submit(cmd);  // failure is fine: no authoritative change occurs
}

void StepGridView::on_mouse_down(Point pos) {
    auto cell = cell_at(pos);
    if (!cell) return;
    current_txn_ = next_txn_++;
    dragging_ = true;
    // Toggle relative to current authoritative state; the drag then paints that
    // value across swept cells.
    drag_sets_enabled_ = !render_.patterns[displayed_pattern_].lanes[cell->lane][cell->step].enabled();
    last_drag_cell_ = cell;
    submit_set_cell(cell->lane, cell->step, drag_sets_enabled_, state::GesturePhase::Begin);
}

void StepGridView::on_mouse_drag(Point pos) {
    if (!dragging_) return;
    auto cell = cell_at(pos);
    if (!cell) return;
    if (last_drag_cell_ && last_drag_cell_->lane == cell->lane &&
        last_drag_cell_->step == cell->step)
        return;  // same cell, no repeat command
    last_drag_cell_ = cell;
    submit_set_cell(cell->lane, cell->step, drag_sets_enabled_, state::GesturePhase::Update);
}

void StepGridView::on_mouse_up(Point /*pos*/) {
    if (!dragging_) return;
    dragging_ = false;
    // Close the gesture with an IDEMPOTENT End marker on the last cell the drag
    // actually touched, re-applying the SAME value the drag set. Crucially this
    // must not read the pre-echo render copy for a cell that changed (that would
    // queue a revert), and must not fall back to a default cell 0,0 on a
    // release-outside-grid — hence gating on last_drag_cell_.
    if (channel_ && last_drag_cell_)
        submit_set_cell(last_drag_cell_->lane, last_drag_cell_->step,
                        drag_sets_enabled_, state::GesturePhase::End);
    last_drag_cell_.reset();
    current_txn_ = 0;
}

// ── FrameClock activity probe ────────────────────────────────────────────────
void StepGridView::on_frame_clock_changed() { subscribe_probe(); }

void StepGridView::subscribe_probe() {
    FrameClock* clock = frame_clock();
    if (clock == subscribed_clock_) return;
    unsubscribe_probe();
    if (clock) {
        probe_id_ = clock->subscribe_activity([this](float) { pump(); });
        subscribed_clock_ = clock;
    }
}

void StepGridView::unsubscribe_probe() {
    if (subscribed_clock_ && probe_id_ >= 0)
        subscribed_clock_->unsubscribe_activity(probe_id_);
    probe_id_ = -1;
    subscribed_clock_ = nullptr;
}

} // namespace pulp::view
