// pointer_coalescer.hpp — collapse a burst of pointer motion into one
// dispatch per presented frame.
//
// A host receives pointer motion at the input device's rate; it presents
// frames at the display's rate, or slower when a frame is expensive. Those two
// rates are independent, and when motion outruns presentation every extra
// sample costs a full delivery — hit test, handler invocation (which for a
// scripted UI means entering the JS engine), and an invalidation whose only
// effect is to re-dirty a surface that is already dirty. None of that work
// reaches the screen: only the last sample before the next presented frame is
// visible.
//
// Measured on Spectr Native Preview before this existed: at 60 input events
// per second — an ordinary hand-drag rate, not a synthetic one — the app
// presented 2.7 frames per second while issuing ~997 repaint requests per
// second, i.e. ~363 repaint requests per presented frame. Holding the button
// still (no motion) stayed at 7.5 fps, so it is the motion, not the gesture
// state, that costs.
//
// ── What this deliberately does NOT decide ──────────────────────────────────
//
// This type coalesces a STREAM. It knows nothing about views, hit testing, or
// dispatch, so it can be unit-tested without a window and reused by any host.
// The caller decides what a flushed sample means.
//
// It also does not assume the consumer can reconstruct skipped motion. Some
// consumers can: Spectr's band paint sends (start_band, current_band) and
// repaints that whole span every message, so dropping intermediate samples
// provably cannot leave gaps. Others cannot — anything that stamps one value
// per sample would draw a dotted line instead of a stroke. So a coalesced move
// carries the full merged `path` alongside the surviving position, and a
// consumer that needs the intermediate points can walk it. Nothing here forces
// the span interpretation into shared code.

#pragma once

#include "pulp/view/input_events.hpp"
#include "pulp/view/pointer_dispatch.hpp"

#include <cstddef>
#include <vector>

namespace pulp::view {

/// One pointer sample as the host received it, before any coalescing.
struct PointerSample {
    Point position{};
    std::uint16_t modifiers = 0;
    int click_count = 0;
    MousePhase phase = MousePhase::automatic;
    MouseButton button = MouseButton::left;
    PointerAttributes pointer{};

    /// Motion is mergeable; a button transition is not. `automatic` is treated
    /// as terminal: it carries no promise about what it means, so merging it
    /// could silently reorder a press or a release.
    bool is_motion() const {
        return phase == MousePhase::drag || phase == MousePhase::hover;
    }
};

/// Collapses runs of motion into one sample per presented frame while leaving
/// button transitions untouched and in order.
///
/// Ownership of timing sits with the caller: `submit` returns what must be
/// dispatched immediately, and `flush_frame` is called once per presented
/// frame. There is no timer and no clock here.
class PointerCoalescer {
public:
    /// Feed one raw sample.
    ///
    /// Returns the samples the caller must dispatch RIGHT NOW, in order:
    ///   * motion  -> empty. It is held until the next `flush_frame`.
    ///   * press / release / anything not motion -> the held motion first (if
    ///     any), then the transition itself.
    ///
    /// Flushing held motion ahead of a transition is what keeps a gesture's
    /// end from arriving before its last movement, and it is why a release can
    /// never swallow the motion that preceded it. Transitions dispatch
    /// immediately rather than waiting for the frame because a click deferred
    /// to the next presented frame is a click delayed by a whole frame — 370ms
    /// on the build that motivated this, which would trade a drag problem for
    /// a click problem.
    std::vector<PointerSample> submit(const PointerSample& sample);

    /// Call once per PRESENTED frame. Returns at most one motion sample,
    /// carrying the newest position and the merged path.
    std::vector<PointerSample> flush_frame();

    /// True when motion is held and waiting for a flush.
    bool has_pending() const { return has_pending_; }

    /// Positions merged into the currently-held motion, oldest first, with the
    /// surviving position last. Empty once flushed.
    const std::vector<Point>& pending_path() const { return path_; }

    /// Raw motion samples discarded by the most recent flush or transition —
    /// the samples that would have been dispatched without coalescing, minus
    /// the one that survived. Diagnostics only.
    std::size_t last_merged_count() const { return last_merged_; }

    /// Total motion samples dropped since construction. Diagnostics only.
    std::size_t total_merged() const { return total_merged_; }

    /// Drop any held motion without dispatching it. For a host tearing down a
    /// window mid-gesture, where the target may already be gone.
    void reset();

private:
    std::vector<PointerSample> take_pending_();

    PointerSample pending_{};
    std::vector<Point> path_;
    bool has_pending_ = false;
    std::size_t merged_in_pending_ = 0;
    std::size_t last_merged_ = 0;
    std::size_t total_merged_ = 0;
};

}  // namespace pulp::view
