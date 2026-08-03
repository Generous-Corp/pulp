#pragma once

/// @file arranger_view.hpp
/// The arranger canvas: track lanes, clip rectangles, and the gestures that
/// edit them.

#include <pulp/timeline/model.hpp>
#include <pulp/timeline_editor/edit_intent.hpp>
#include <pulp/view/hit_metrics.hpp>
#include <pulp/view/view.hpp>

#include <cstddef>
#include <functional>
#include <optional>

namespace pulp::timeline_view {

/** @addtogroup timeline_view
 * @{
 */

/// A resolved tick/pixel mapping, supplied by whoever owns the viewport.
///
/// The arranger CONSUMES a projection and never computes one. Zoom policy,
/// scroll clamping, snapping and legibility thresholds all resolve to these
/// scalars before the view sees them — the same shape as the resolved
/// `tolerance_px` a front-end hands the hit tests instead of a pointer type.
/// Keeping the policy out means the arranger, a piano roll and an automation
/// lane can be driven from one projection rather than each growing its own.
struct ArrangerLayout {
    /// Document tick drawn at the left edge of the clip lanes.
    timebase::TickPosition origin_tick{};
    /// Horizontal scale. Zero collapses the timeline onto a single column,
    /// which is degenerate but not invalid: every clip hit-tests at the lane
    /// origin and no drag can change a tick.
    double px_per_tick = 0.0;
    /// Row pitch, and the height of a clip rectangle within its row.
    float track_height_px = 64.0f;
    /// Clip lanes begin here; `[0, lane_left_px)` is the track-header column.
    float lane_left_px = 0.0f;

    /// Lane-space x of a document tick. Values left of `lane_left_px` are
    /// returned unclamped so a partially scrolled-out clip keeps a truthful
    /// rectangle for the caller to clip against the lane bounds.
    float x_for_tick(timebase::TickPosition tick) const noexcept;
    /// Document tick at a lane-space x. Inverse of `x_for_tick` wherever
    /// `px_per_tick` is nonzero; returns `origin_tick` when it is not.
    timebase::TickPosition tick_for_x(float x) const noexcept;
    /// Top edge of the row at an authored track index.
    float y_for_track_index(std::size_t index) const noexcept;
};

/// Why the arranger declined to emit an intent a gesture would otherwise
/// have produced. Recorded rather than thrown: a refused gesture is an
/// ordinary outcome a view stays usable after.
enum class ArrangerRefusal : std::uint8_t {
    /// A clip factory returned content the arranger will not author. See
    /// `set_clip_factory`.
    NestedSequenceContent,
};

/// Arranger canvas over a `timeline::Project`.
///
/// Holds nothing durable: the project is borrowed for painting and hit
/// testing, and every edit leaves as a `timeline_editor::EditIntent` submitted
/// to a host. The view never builds a `timeline::Command`, never owns a
/// revision, and never learns what applies its intents.
class ArrangerView : public view::View {
  public:
    /// Builds the payload for a create-clip gesture at a lane position.
    ///
    /// Supplied by the caller because a clip's identity comes from the
    /// document's id domain, which a view does not own. Returning `nullopt`
    /// declines the gesture, which is what a read-only arranger does.
    using ClipFactory = std::function<std::optional<timeline::Clip>(timebase::TickPosition start)>;

    ArrangerView();

    /// Borrows the project to paint and hit-test. Neither pointer is retained
    /// past the next call; the caller re-points the view after every commit.
    void set_project(const timeline::Project* project, timeline::ItemId sequence_id);
    void set_layout(const ArrangerLayout& layout);
    /// Where emitted intents go. Null leaves the view fully interactive and
    /// silent, which is what a detached or read-only arranger is.
    void set_host(timeline_editor::EditIntentHost* host);
    /// Pointer geometry, resolved once per gesture rather than per hit test.
    void set_hit_metrics(const view::HitMetrics& metrics);
    /// Enables the create-clip gesture. Unset, a click on empty lane space
    /// does nothing.
    void set_clip_factory(ClipFactory factory);

    const ArrangerLayout& layout() const noexcept { return layout_; }

    /// Rectangle a clip occupies, in this view's local coordinates, or
    /// `nullopt` when the identity is not in the bound sequence.
    std::optional<view::Rect> clip_rect(timeline::ItemId track_id,
                                        timeline::ItemId clip_id) const;

    /// Refusals recorded since the last `clear_refusals()`, in order.
    const std::vector<ArrangerRefusal>& refusals() const noexcept { return refusals_; }
    void clear_refusals() { refusals_.clear(); }

    /// Whether a drag is mid-flight. Undo and redo are rejected by a session
    /// while a gesture is open, so a host greys them out on this.
    bool gesture_open() const noexcept { return drag_.has_value(); }

    // ── View ─────────────────────────────────────────────────────────────

    void paint(canvas::Canvas& canvas) override;
    bool wants_mouse_input() const override { return true; }
    void on_mouse_down(view::Point position) override;
    void on_mouse_drag(view::Point position) override;
    void on_mouse_up(view::Point position) override;
    void on_mouse_cancel(view::Point position) override;

  private:
    /// One clip drag in flight.
    struct Drag {
        timeline::ItemId track_id;
        timeline::ItemId clip_id;
        /// Distance from the clip's start to where the pointer grabbed it, so
        /// a drag moves the clip rather than snapping its start to the cursor.
        std::int64_t grab_offset_ticks = 0;
        timebase::TickDuration duration{};
        /// The range the view believes the clip currently occupies. Each
        /// emission gates on this value and then replaces it, so a stream of
        /// Begin/Update/End steps chains instead of every step claiming the
        /// range the drag started from — which the second step's optimistic
        /// gate would reject.
        timeline::MusicalTimeRange believed{};
        /// False until a step actually changed the tick. A click that never
        /// moves must not open an undo group.
        bool opened = false;
    };

    const timeline::Sequence* sequence() const;
    const timeline::Track* track_at(view::Point position, std::size_t& index_out) const;
    /// Clip under a lane-space point, resolved against the pointer tolerance.
    const timeline::Clip* clip_at(const timeline::Track& track, view::Point position) const;

    void emit_move(const Drag& drag, timeline::GesturePhase phase,
                   timeline::MusicalTimeRange replacement);
    void submit(const timeline_editor::EditIntent& intent);

    const timeline::Project* project_ = nullptr;
    timeline::ItemId sequence_id_{};
    ArrangerLayout layout_{};
    timeline_editor::EditIntentHost* host_ = nullptr;
    view::HitMetrics metrics_ = view::HitMetrics::for_pointer(view::PointerType::mouse);
    ClipFactory clip_factory_;
    std::optional<Drag> drag_;
    std::vector<ArrangerRefusal> refusals_;
};

/// @}

} // namespace pulp::timeline_view
