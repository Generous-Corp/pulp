#pragma once

#include <pulp/view/geometry.hpp>

#include <algorithm>

namespace pulp::view {

/// Accumulated repaint damage for one frame, in root/window coordinates.
///
/// Extracted so the WINDOW host and the PLUG-IN host can share one definition
/// of "what changed this frame". Before this existed the state lived only on
/// WindowHost, so `View::request_repaint(Rect)` had nowhere to put a bounded
/// rect on the plug-in path and discarded it — which meant partial repaint was
/// unreachable for every plug-in editor on every platform, however the host was
/// wired.
///
/// The invariants are what make bounded damage safe to ignore or honour:
///
///  - `full` is STICKY within a frame. A bounded mark after a full one leaves
///    the repaint full. Damage therefore only ever SHRINKS a repaint, never
///    widens one, so a host that never consults `bounds()` is always correct.
///  - An empty/degenerate rect escalates to full rather than being dropped.
///  - `full` starts true: the first frame of any surface is a full repaint.
///
/// A frame CONSUMES damage with `take()`, which reads and clears in one step.
/// Reading the three accessors and then calling `clear()` separately is how a
/// host ends up clearing a different logical state than the one it painted —
/// most importantly when the frame never reached the screen. A frame that
/// failed puts its snapshot back with `restore()`, so the damage survives the
/// retry.
class PendingDamage {
public:
    /// One frame's damage, taken atomically out of a PendingDamage.
    ///
    /// Deliberately has no mutators: it is a value the frame paints against and
    /// then either discards (the frame reached its output) or hands back to
    /// `restore()` (it did not).
    class Snapshot {
    public:
        bool is_full() const noexcept { return full_; }
        bool has_bounds() const noexcept { return have_bounds_; }
        Rect bounds() const noexcept { return bounds_; }

        /// True when this snapshot describes a clippable region — bounded
        /// damage with a real rect. The single predicate every host used to
        /// spell out as `!is_full() && has_bounds()`.
        bool is_bounded() const noexcept { return !full_ && have_bounds_; }

    private:
        friend class PendingDamage;
        bool full_ = true;
        bool have_bounds_ = false;
        Rect bounds_{};
    };

    /// Whole-surface repaint. Any structural change uses this.
    void mark_full() { full_ = true; }

    /// Union a bounded region (root coords) into this frame's damage.
    void mark(const Rect& root_rect) {
        if (root_rect.width <= 0 || root_rect.height <= 0) {
            mark_full();  // degenerate region → be conservative
            return;
        }
        if (full_) return;  // sticky: never widen back out of a full repaint
        if (!have_bounds_) {
            bounds_ = root_rect;
            have_bounds_ = true;
            return;
        }
        const float nx = std::min(bounds_.x, root_rect.x);
        const float ny = std::min(bounds_.y, root_rect.y);
        bounds_ = {nx, ny,
                   std::max(bounds_.right(), root_rect.right()) - nx,
                   std::max(bounds_.bottom(), root_rect.bottom()) - ny};
    }

    bool is_full() const { return full_; }
    bool has_bounds() const { return have_bounds_; }
    Rect bounds() const { return bounds_; }

    /// Called by the host once it has painted and submitted the frame.
    void clear() {
        full_ = false;
        have_bounds_ = false;
        bounds_ = {};
    }

    /// Read this frame's damage AND clear it, in one step.
    ///
    /// The host paints against the returned snapshot. If the frame reaches its
    /// intended output, the snapshot is simply dropped. If it does not, the
    /// host calls `restore()` so the next attempt repaints at least as much.
    Snapshot take() {
        Snapshot s;
        s.full_ = full_;
        s.have_bounds_ = have_bounds_;
        s.bounds_ = bounds_;
        clear();
        return s;
    }

    /// Put a taken snapshot back after a frame failed to reach its output.
    ///
    /// Unions rather than overwrites, so damage marked while the failed frame
    /// was in flight is preserved too, and the sticky-full rule still holds:
    /// restoring a full snapshot leaves the repaint full regardless of what
    /// arrived in the meantime.
    void restore(const Snapshot& s) {
        if (s.full_) {
            mark_full();
            return;
        }
        if (s.have_bounds_) mark(s.bounds_);
    }

private:
    bool full_ = true;  ///< first frame is always full
    bool have_bounds_ = false;
    Rect bounds_{};
};

}  // namespace pulp::view
